// 小さな正規表現（バックトラック方式）
//
// パターンを構文木にしてから命令列（CHAR / SPLIT / JMP / SAVE …）に落とし、再帰で照合する。
// 対象はファイル名や URL のような短い文字列なので、深さと手数に上限を設けて暴走だけ防ぐ。

#include "obtan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { N_CHAR, N_ANY, N_CLASS, N_BOL, N_EOL, N_WB, N_NWB, N_CAT, N_ALT, N_GROUP, N_REP, N_EMPTY } NType;

typedef struct Node {
    NType t;
    int   c;          // N_CHAR: 文字, N_CLASS: クラス番号, N_GROUP: グループ番号
    int   min, max;   // N_REP（max = -1 は上限なし）
    int   greedy;
    struct Node *l, *r;
} Node;

typedef enum { I_CHAR, I_ANY, I_CLASS, I_BOL, I_EOL, I_WB, I_NWB, I_SPLIT, I_JMP, I_SAVE, I_MATCH } IOp;

typedef struct {
    IOp op;
    int c;      // CHAR: 文字, CLASS: クラス番号, SAVE: 位置の番号
    int x, y;   // SPLIT: 先に試す先 x / 次に試す先 y, JMP: x
} Inst;

struct Rx {
    Inst *prog;
    int   nprog, capprog;
    unsigned char (*cls)[32];
    int   ncls;
    int   ngroup;
    int   icase;
};

#define MAX_PROG   20000
#define MAX_DEPTH  10000
#define MAX_STEPS  2000000

typedef struct {
    const char *p;
    Rx    *rx;
    Node **nodes;
    int    nnodes, capnodes;
    char  *err;
    size_t errlen;
    int    failed;
} Parser;

static void Fail(Parser *ps, const char *msg) {
    if (!ps->failed) snprintf(ps->err, ps->errlen, "%s", msg);
    ps->failed = 1;
}

static Node *Mk(Parser *ps, NType t, Node *l, Node *r) {
    Node *n = (Node *)calloc(1, sizeof(Node));
    if (!n) { Fail(ps, "メモリ不足"); return NULL; }
    if (ps->nnodes == ps->capnodes) {
        int cap = ps->capnodes ? ps->capnodes * 2 : 64;
        Node **p = (Node **)realloc(ps->nodes, sizeof(Node *) * cap);
        if (!p) { free(n); Fail(ps, "メモリ不足"); return NULL; }
        ps->nodes = p;
        ps->capnodes = cap;
    }
    ps->nodes[ps->nnodes++] = n;
    n->t = t;
    n->l = l;
    n->r = r;
    return n;
}

static int Lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int IsWordByte(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int IsSpaceByte(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

static void SetBit(unsigned char *set, int c) { set[c >> 3] |= (unsigned char)(1 << (c & 7)); }
static int  GetBit(const unsigned char *set, int c) { return (set[c >> 3] >> (c & 7)) & 1; }

// \d \w \s とその否定をクラスに足す
static void AddShorthand(unsigned char *set, int e) {
    for (int c = 0; c < 256; c++) {
        int in;
        switch (e) {
        case 'd': in = c >= '0' && c <= '9'; break;
        case 'D': in = !(c >= '0' && c <= '9'); break;
        case 'w': in = IsWordByte(c); break;
        case 'W': in = !IsWordByte(c); break;
        case 's': in = IsSpaceByte(c); break;
        case 'S': in = !IsSpaceByte(c); break;
        default: in = 0;
        }
        if (in) SetBit(set, c);
    }
}

static int NewClass(Parser *ps) {
    Rx *rx = ps->rx;
    unsigned char (*p)[32] = realloc(rx->cls, sizeof(*rx->cls) * (rx->ncls + 1));
    if (!p) { Fail(ps, "メモリ不足"); return -1; }
    rx->cls = p;
    memset(rx->cls[rx->ncls], 0, 32);
    return rx->ncls++;
}

static int EscapeChar(int e) {
    switch (e) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    default:  return e;
    }
}

static Node *ParseClass(Parser *ps) {
    // ps->p は '[' の次
    int idx = NewClass(ps);
    if (idx < 0) return NULL;
    unsigned char set[32] = {0};
    int neg = 0;
    if (*ps->p == '^') { neg = 1; ps->p++; }
    int first = 1;
    while (*ps->p && (*ps->p != ']' || first)) {
        first = 0;
        int lo;
        if (*ps->p == '\\' && ps->p[1]) {
            int e = (unsigned char)ps->p[1];
            ps->p += 2;
            if (strchr("dDwWsS", e)) { AddShorthand(set, e); continue; }
            lo = EscapeChar(e);
        } else {
            lo = (unsigned char)*ps->p++;
        }
        int hi = lo;
        if (ps->p[0] == '-' && ps->p[1] && ps->p[1] != ']') {
            ps->p++;
            if (*ps->p == '\\' && ps->p[1]) { hi = EscapeChar((unsigned char)ps->p[1]); ps->p += 2; }
            else hi = (unsigned char)*ps->p++;
            if (hi < lo) { Fail(ps, "文字クラスの範囲が逆順"); return NULL; }
        }
        for (int c = lo; c <= hi; c++) SetBit(set, c);
    }
    if (*ps->p != ']') { Fail(ps, "] が閉じていない"); return NULL; }
    ps->p++;
    if (ps->rx->icase) {
        for (int c = 'a'; c <= 'z'; c++) {
            if (GetBit(set, c) || GetBit(set, c - 32)) { SetBit(set, c); SetBit(set, c - 32); }
        }
    }
    if (neg) for (int i = 0; i < 32; i++) set[i] = (unsigned char)~set[i];
    memcpy(ps->rx->cls[idx], set, 32);
    Node *n = Mk(ps, N_CLASS, NULL, NULL);
    if (n) n->c = idx;
    return n;
}

static Node *ParseAlt(Parser *ps);

static Node *ParseAtom(Parser *ps) {
    int c = (unsigned char)*ps->p;
    switch (c) {
    case '(': {
        ps->p++;
        int capture = 1;
        if (ps->p[0] == '?' && ps->p[1] == ':') { capture = 0; ps->p += 2; }
        else if (ps->p[0] == '?') { Fail(ps, "(? の形式は (?: と先頭の (?i) だけに対応"); return NULL; }
        int g = 0;
        if (capture) {
            g = ++ps->rx->ngroup;
            if (g >= RX_MAXGROUP) { Fail(ps, "括弧が多すぎる"); return NULL; }
        }
        Node *inner = ParseAlt(ps);
        if (ps->failed) return NULL;
        if (*ps->p != ')') { Fail(ps, ") が閉じていない"); return NULL; }
        ps->p++;
        if (!capture) return inner;
        Node *n = Mk(ps, N_GROUP, inner, NULL);
        if (n) n->c = g;
        return n;
    }
    case '[':
        ps->p++;
        return ParseClass(ps);
    case '.': ps->p++; return Mk(ps, N_ANY, NULL, NULL);
    case '^': ps->p++; return Mk(ps, N_BOL, NULL, NULL);
    case '$': ps->p++; return Mk(ps, N_EOL, NULL, NULL);
    case '*': case '+': case '?':
        Fail(ps, "繰り返し記号の前に対象がない");
        return NULL;
    case '\\': {
        int e = (unsigned char)ps->p[1];
        if (!e) { Fail(ps, "末尾の \\ が余っている"); return NULL; }
        ps->p += 2;
        if (e == 'b') return Mk(ps, N_WB, NULL, NULL);
        if (e == 'B') return Mk(ps, N_NWB, NULL, NULL);
        if (strchr("dDwWsS", e)) {
            int idx = NewClass(ps);
            if (idx < 0) return NULL;
            AddShorthand(ps->rx->cls[idx], e);
            Node *n = Mk(ps, N_CLASS, NULL, NULL);
            if (n) n->c = idx;
            return n;
        }
        Node *n = Mk(ps, N_CHAR, NULL, NULL);
        if (n) n->c = EscapeChar(e);
        return n;
    }
    default: {
        ps->p++;
        Node *n = Mk(ps, N_CHAR, NULL, NULL);
        if (n) n->c = c;
        return n;
    }
    }
}

// {m} {m,} {m,n} を読む。形が合わなければ 0 を返し、{ はただの文字として扱われる
static int ParseBraces(Parser *ps, int *mn, int *mx) {
    const char *q = ps->p + 1;
    if (*q < '0' || *q > '9') return 0;
    int a = 0, b;
    while (*q >= '0' && *q <= '9') a = a * 10 + (*q++ - '0');
    if (*q == '}') b = a;
    else if (*q == ',') {
        q++;
        if (*q == '}') b = -1;
        else {
            if (*q < '0' || *q > '9') return 0;
            b = 0;
            while (*q >= '0' && *q <= '9') b = b * 10 + (*q++ - '0');
            if (*q != '}') return 0;
        }
    } else return 0;
    ps->p = q + 1;
    *mn = a;
    *mx = b;
    return 1;
}

static Node *ParseRep(Parser *ps) {
    Node *a = ParseAtom(ps);
    while (!ps->failed && a) {
        int mn, mx;
        char c = *ps->p;
        if (c == '*') { mn = 0; mx = -1; ps->p++; }
        else if (c == '+') { mn = 1; mx = -1; ps->p++; }
        else if (c == '?') { mn = 0; mx = 1; ps->p++; }
        else if (c == '{' && ParseBraces(ps, &mn, &mx)) {
            if ((mx >= 0 && mx < mn) || mn > 1000 || mx > 1000) { Fail(ps, "{m,n} の値が不正"); return NULL; }
        } else break;
        Node *r = Mk(ps, N_REP, a, NULL);
        if (!r) return NULL;
        r->min = mn;
        r->max = mx;
        r->greedy = 1;
        if (*ps->p == '?') { r->greedy = 0; ps->p++; }
        a = r;
    }
    return a;
}

static Node *ParseCat(Parser *ps) {
    Node *l = NULL;
    while (!ps->failed && *ps->p && *ps->p != '|' && *ps->p != ')') {
        Node *a = ParseRep(ps);
        if (!a) return NULL;
        l = l ? Mk(ps, N_CAT, l, a) : a;
    }
    return l ? l : Mk(ps, N_EMPTY, NULL, NULL);
}

static Node *ParseAlt(Parser *ps) {
    Node *l = ParseCat(ps);
    while (!ps->failed && *ps->p == '|') {
        ps->p++;
        Node *r = ParseCat(ps);
        l = Mk(ps, N_ALT, l, r);
    }
    return l;
}

static int Emit(Parser *ps, IOp op, int c) {
    Rx *rx = ps->rx;
    if (rx->nprog >= MAX_PROG) { Fail(ps, "パターンが大きすぎる"); return 0; }
    if (rx->nprog == rx->capprog) {
        int cap = rx->capprog ? rx->capprog * 2 : 64;
        Inst *p = (Inst *)realloc(rx->prog, sizeof(Inst) * cap);
        if (!p) { Fail(ps, "メモリ不足"); return 0; }
        rx->prog = p;
        rx->capprog = cap;
    }
    Inst *in = &rx->prog[rx->nprog];
    in->op = op;
    in->c = c;
    in->x = in->y = 0;
    return rx->nprog++;
}

static void SetSplit(Rx *rx, int s, int body, int out, int greedy) {
    rx->prog[s].x = greedy ? body : out;
    rx->prog[s].y = greedy ? out : body;
}

static void Gen(Parser *ps, const Node *n) {
    if (ps->failed || !n) return;
    Rx *rx = ps->rx;
    switch (n->t) {
    case N_CHAR:  Emit(ps, I_CHAR, rx->icase ? Lower(n->c) : n->c); break;
    case N_ANY:   Emit(ps, I_ANY, 0); break;
    case N_CLASS: Emit(ps, I_CLASS, n->c); break;
    case N_BOL:   Emit(ps, I_BOL, 0); break;
    case N_EOL:   Emit(ps, I_EOL, 0); break;
    case N_WB:    Emit(ps, I_WB, 0); break;
    case N_NWB:   Emit(ps, I_NWB, 0); break;
    case N_EMPTY: break;
    case N_CAT:   Gen(ps, n->l); Gen(ps, n->r); break;
    case N_ALT: {
        int s = Emit(ps, I_SPLIT, 0);
        Gen(ps, n->l);
        int j = Emit(ps, I_JMP, 0);
        if (ps->failed) return;
        rx->prog[s].x = s + 1;
        rx->prog[s].y = rx->nprog;
        Gen(ps, n->r);
        if (ps->failed) return;
        rx->prog[j].x = rx->nprog;
        break;
    }
    case N_GROUP:
        Emit(ps, I_SAVE, 2 * n->c);
        Gen(ps, n->l);
        Emit(ps, I_SAVE, 2 * n->c + 1);
        break;
    case N_REP: {
        for (int i = 0; i < n->min; i++) Gen(ps, n->l);
        if (n->max < 0) {
            int s = Emit(ps, I_SPLIT, 0);
            Gen(ps, n->l);
            int j = Emit(ps, I_JMP, 0);
            if (ps->failed) return;
            rx->prog[j].x = s;
            SetSplit(rx, s, s + 1, rx->nprog, n->greedy);
        } else {
            int k = n->max - n->min;
            int *splits = (int *)malloc(sizeof(int) * (k > 0 ? k : 1));
            if (!splits) { Fail(ps, "メモリ不足"); return; }
            for (int i = 0; i < k; i++) {
                splits[i] = Emit(ps, I_SPLIT, 0);
                Gen(ps, n->l);
            }
            if (!ps->failed)
                for (int i = 0; i < k; i++) SetSplit(rx, splits[i], splits[i] + 1, rx->nprog, n->greedy);
            free(splits);
        }
        break;
    }
    }
}

Rx *RxCompile(const char *pattern, int icase, char *err, size_t errlen) {
    Rx *rx = (Rx *)calloc(1, sizeof(Rx));
    if (!rx) return NULL;
    char dummy[8];
    Parser ps = {0};
    ps.rx = rx;
    ps.err = err ? err : dummy;
    ps.errlen = err ? errlen : sizeof(dummy);
    ps.p = pattern;
    rx->icase = icase;
    if (strncmp(ps.p, "(?i)", 4) == 0) { rx->icase = 1; ps.p += 4; }

    Node *root = ParseAlt(&ps);
    if (!ps.failed && *ps.p == ')') Fail(&ps, ") が余っている");
    if (!ps.failed) {
        Emit(&ps, I_SAVE, 0);
        Gen(&ps, root);
        Emit(&ps, I_SAVE, 1);
        Emit(&ps, I_MATCH, 0);
    }
    for (int i = 0; i < ps.nnodes; i++) free(ps.nodes[i]);
    free(ps.nodes);
    if (ps.failed) { RxFree(rx); return NULL; }
    return rx;
}

void RxFree(Rx *rx) {
    if (!rx) return;
    free(rx->prog);
    free(rx->cls);
    free(rx);
}

int RxGroups(const Rx *rx) { return rx->ngroup; }

typedef struct {
    const Rx *rx;
    const unsigned char *s, *end;
    int *caps;
    long steps;
} Matcher;

// UTF-8 の 1 文字分のバイト数（壊れた並びは 1 として進む）
static int U8Len(const unsigned char *p, const unsigned char *end) {
    int n = 1;
    if (*p >= 0xF0) n = 4;
    else if (*p >= 0xE0) n = 3;
    else if (*p >= 0xC0) n = 2;
    for (int i = 1; i < n; i++)
        if (p + i >= end || (p[i] & 0xC0) != 0x80) return i;
    return n;
}

static int Bt(Matcher *m, int pc, const unsigned char *sp, int depth) {
    if (depth > MAX_DEPTH) return 0;
    for (;;) {
        if (++m->steps > MAX_STEPS) return 0;
        const Inst *in = &m->rx->prog[pc];
        switch (in->op) {
        case I_CHAR: {
            if (sp >= m->end) return 0;
            int c = *sp;
            if (m->rx->icase) c = Lower(c);
            if (c != in->c) return 0;
            sp++;
            pc++;
            break;
        }
        case I_ANY:
            if (sp >= m->end || *sp == '\n') return 0;
            sp += U8Len(sp, m->end);
            pc++;
            break;
        case I_CLASS:
            if (sp >= m->end || !GetBit(m->rx->cls[in->c], *sp)) return 0;
            sp += *sp >= 0xC0 ? U8Len(sp, m->end) : 1;
            pc++;
            break;
        case I_BOL:
            if (sp != m->s) return 0;
            pc++;
            break;
        case I_EOL:
            if (sp != m->end) return 0;
            pc++;
            break;
        case I_WB:
        case I_NWB: {
            int a = sp > m->s && IsWordByte(sp[-1]);
            int b = sp < m->end && IsWordByte(*sp);
            if ((a != b) != (in->op == I_WB)) return 0;
            pc++;
            break;
        }
        case I_JMP:
            pc = in->x;
            break;
        case I_SPLIT:
            if (Bt(m, in->x, sp, depth + 1)) return 1;
            pc = in->y;
            break;
        case I_SAVE: {
            int old = m->caps[in->c];
            m->caps[in->c] = (int)(sp - m->s);
            if (Bt(m, pc + 1, sp, depth + 1)) return 1;
            m->caps[in->c] = old;
            return 0;
        }
        case I_MATCH:
            return 1;
        }
    }
}

int RxSearch(const Rx *rx, const char *s, int caps[2 * RX_MAXGROUP]) {
    int local[2 * RX_MAXGROUP];
    if (!caps) caps = local;
    Matcher m = { rx, (const unsigned char *)s, (const unsigned char *)s + strlen(s), caps, 0 };
    int anchored = rx->nprog > 1 && rx->prog[1].op == I_BOL;
    for (const unsigned char *p = m.s; p <= m.end; p++) {
        for (int i = 0; i < 2 * RX_MAXGROUP; i++) caps[i] = -1;
        if (Bt(&m, 0, p, 0)) return 1;
        if (anchored || m.steps > MAX_STEPS) break;
    }
    for (int i = 0; i < 2 * RX_MAXGROUP; i++) caps[i] = -1;
    return 0;
}
