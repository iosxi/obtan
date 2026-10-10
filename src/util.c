// 文字列と可変長バッファの小道具

#include "obtan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

void BufAdd(Buf *b, const void *data, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1) cap *= 2;
        char *p = (char *)realloc(b->p, cap);
        if (!p) return;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
    b->p[b->n] = 0;
}

void BufAddStr(Buf *b, const char *s) { BufAdd(b, s, strlen(s)); }
void BufAddChar(Buf *b, char c) { BufAdd(b, &c, 1); }

void BufFree(Buf *b) {
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

char *BufTake(Buf *b) {
    char *p = b->p ? b->p : U8Dup("");
    b->p = NULL;
    b->n = b->cap = 0;
    return p;
}

char *U8NDup(const char *s, size_t n) {
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *U8Dup(const char *s) { return s ? U8NDup(s, strlen(s)) : NULL; }

char *U8Fmt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = _vscprintf(fmt, ap);
    va_end(ap);
    if (n < 0) return U8Dup("");
    char *p = (char *)malloc((size_t)n + 1);
    if (!p) return NULL;
    va_start(ap, fmt);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return p;
}

WCHAR *U2W(const char *s) {
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    WCHAR *w = (WCHAR *)malloc(sizeof(WCHAR) * (n > 0 ? n : 1));
    if (!w) return NULL;
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) w[0] = 0;
    return w;
}

char *W2U(const WCHAR *w) {
    if (!w) w = L"";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)malloc(n > 0 ? n : 1);
    if (!s) return NULL;
    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL)) s[0] = 0;
    return s;
}

int U8StartsWithI(const char *s, const char *prefix) {
    return _strnicmp(s, prefix, strlen(prefix)) == 0;
}

const char *U8FindI(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (_strnicmp(hay, needle, n) == 0) return hay;
    return NULL;
}

static int HexVal(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char *PercentDecode(const char *s) {
    Buf b = {0};
    for (; *s; s++) {
        if (s[0] == '%' && HexVal((unsigned char)s[1]) >= 0 && HexVal((unsigned char)s[2]) >= 0) {
            BufAddChar(&b, (char)(HexVal((unsigned char)s[1]) * 16 + HexVal((unsigned char)s[2])));
            s += 2;
        } else {
            BufAddChar(&b, *s);
        }
    }
    return BufTake(&b);
}
