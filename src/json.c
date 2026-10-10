// 最小限の JSON パーサー（API の応答を読むだけ）

#include "obtan.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    int failed;
    int depth;
} JP;

static void SkipWs(JP *jp) {
    while (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\n' || *jp->p == '\r') jp->p++;
}

static J *NewJ(JType t) {
    J *j = (J *)calloc(1, sizeof(J));
    if (j) j->t = t;
    return j;
}

static int Hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return 0;
    }
    *out = v;
    return 1;
}

static void PutU8(Buf *b, unsigned cp) {
    char t[4];
    if (cp < 0x80) { t[0] = (char)cp; BufAdd(b, t, 1); }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 0x3F)); BufAdd(b, t, 2); }
    else if (cp < 0x10000) {
        t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[2] = (char)(0x80 | (cp & 0x3F));
        BufAdd(b, t, 3);
    } else {
        t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[3] = (char)(0x80 | (cp & 0x3F));
        BufAdd(b, t, 4);
    }
}

static char *ParseString(JP *jp) {
    // jp->p は '"' を指している
    jp->p++;
    Buf b = {0};
    BufAdd(&b, "", 0);
    while (*jp->p && *jp->p != '"') {
        if (*jp->p == '\\') {
            jp->p++;
            char e = *jp->p++;
            switch (e) {
            case '"': BufAddChar(&b, '"'); break;
            case '\\': BufAddChar(&b, '\\'); break;
            case '/': BufAddChar(&b, '/'); break;
            case 'b': BufAddChar(&b, '\b'); break;
            case 'f': BufAddChar(&b, '\f'); break;
            case 'n': BufAddChar(&b, '\n'); break;
            case 'r': BufAddChar(&b, '\r'); break;
            case 't': BufAddChar(&b, '\t'); break;
            case 'u': {
                unsigned cp;
                if (!Hex4(jp->p, &cp)) { jp->failed = 1; BufFree(&b); return NULL; }
                jp->p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && jp->p[0] == '\\' && jp->p[1] == 'u') {
                    unsigned lo;
                    if (Hex4(jp->p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        jp->p += 6;
                    }
                }
                PutU8(&b, cp);
                break;
            }
            default: jp->failed = 1; BufFree(&b); return NULL;
            }
        } else {
            BufAddChar(&b, *jp->p++);
        }
    }
    if (*jp->p != '"') { jp->failed = 1; BufFree(&b); return NULL; }
    jp->p++;
    return BufTake(&b);
}

static J *ParseValue(JP *jp) {
    SkipWs(jp);
    if (++jp->depth > 200) { jp->failed = 1; return NULL; }
    J *j = NULL;
    char c = *jp->p;
    if (c == '{' || c == '[') {
        int obj = c == '{';
        j = NewJ(obj ? J_OBJ : J_ARR);
        if (!j) { jp->failed = 1; return NULL; }
        jp->p++;
        SkipWs(jp);
        J **tail = &j->child;
        if (*jp->p == (obj ? '}' : ']')) {
            jp->p++;
        } else {
            for (;;) {
                char *key = NULL;
                if (obj) {
                    SkipWs(jp);
                    if (*jp->p != '"') { jp->failed = 1; break; }
                    key = ParseString(jp);
                    if (!key) break;
                    SkipWs(jp);
                    if (*jp->p != ':') { free(key); jp->failed = 1; break; }
                    jp->p++;
                }
                J *v = ParseValue(jp);
                if (!v) { free(key); jp->failed = 1; break; }
                v->key = key;
                *tail = v;
                tail = &v->next;
                SkipWs(jp);
                if (*jp->p == ',') { jp->p++; continue; }
                if (*jp->p == (obj ? '}' : ']')) { jp->p++; break; }
                jp->failed = 1;
                break;
            }
        }
    } else if (c == '"') {
        j = NewJ(J_STR);
        if (j) j->s = ParseString(jp);
        if (!j || !j->s) jp->failed = 1;
    } else if (strncmp(jp->p, "true", 4) == 0) {
        j = NewJ(J_BOOL);
        if (j) j->b = 1;
        jp->p += 4;
    } else if (strncmp(jp->p, "false", 5) == 0) {
        j = NewJ(J_BOOL);
        jp->p += 5;
    } else if (strncmp(jp->p, "null", 4) == 0) {
        j = NewJ(J_NULL);
        jp->p += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char *end;
        j = NewJ(J_NUM);
        if (j) j->n = strtod(jp->p, &end);
        else end = (char *)jp->p + 1;
        jp->p = end;
    } else {
        jp->failed = 1;
    }
    jp->depth--;
    if (jp->failed) { JsonFree(j); return NULL; }
    return j;
}

J *JsonParse(const char *text) {
    if (!text) return NULL;
    JP jp = { text, 0, 0 };
    if ((unsigned char)jp.p[0] == 0xEF && (unsigned char)jp.p[1] == 0xBB && (unsigned char)jp.p[2] == 0xBF) jp.p += 3;
    return ParseValue(&jp);
}

void JsonFree(J *j) {
    while (j) {
        J *next = j->next;
        JsonFree(j->child);
        free(j->key);
        free(j->s);
        free(j);
        j = next;
    }
}

J *JsonGet(const J *obj, const char *key) {
    if (!obj || obj->t != J_OBJ) return NULL;
    for (J *c = obj->child; c; c = c->next)
        if (c->key && strcmp(c->key, key) == 0) return c;
    return NULL;
}

const char *JsonStr(const J *obj, const char *key) {
    J *v = JsonGet(obj, key);
    return v && v->t == J_STR ? v->s : NULL;
}

int JsonBool(const J *obj, const char *key) {
    J *v = JsonGet(obj, key);
    return v && v->t == J_BOOL && v->b;
}
