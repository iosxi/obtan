// obtan - 共通の宣言
//
// 通信・JSON・正規表現・取得元の判定は UTF-8 (char*) で扱い、画面と ini は UTF-16 (WCHAR*) で扱う。
// 境界での変換は U2W / W2U で行う。

#pragma once

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stddef.h>

#define APP_NAME    L"obtan"
#define APP_VERSION "5"

// ---------------------------------------------------------------- util.c

typedef struct {
    char  *p;
    size_t n, cap;
} Buf;

void  BufAdd(Buf *b, const void *data, size_t n);
void  BufAddStr(Buf *b, const char *s);
void  BufAddChar(Buf *b, char c);
void  BufFree(Buf *b);
char *BufTake(Buf *b);   // NUL 終端した中身を返し、b は空に戻す

char  *U8Dup(const char *s);
char  *U8NDup(const char *s, size_t n);
char  *U8Fmt(const char *fmt, ...);
WCHAR *U2W(const char *s);   // UTF-8 → UTF-16（malloc）
char  *W2U(const WCHAR *s);  // UTF-16 → UTF-8（malloc）
int    U8StartsWithI(const char *s, const char *prefix);
const char *U8FindI(const char *hay, const char *needle);
char  *PercentDecode(const char *s);

// ---------------------------------------------------------------- rx.c（小さな正規表現）
//
// 対応: . [] [^] () (?:) | * + ? {m,n} 最短一致 ^ $ \d \w \s \b とその否定、(?i)
// バイト単位で照合するが、. と否定クラスは UTF-8 の 1 文字をまとめて読む。

#define RX_MAXGROUP 10

typedef struct Rx Rx;
Rx  *RxCompile(const char *pattern, int icase, char *err, size_t errlen);
int  RxSearch(const Rx *rx, const char *s, int caps[2 * RX_MAXGROUP]);  // 一致で 1。caps は -1 で未使用
int  RxGroups(const Rx *rx);
void RxFree(Rx *rx);

// ---------------------------------------------------------------- json.c

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct J {
    JType     t;
    char     *key;     // オブジェクトの要素のときのキー
    char     *s;       // J_STR
    double    n;       // J_NUM
    int       b;       // J_BOOL
    struct J *child;   // J_ARR / J_OBJ の最初の要素
    struct J *next;
} J;

J          *JsonParse(const char *text);
void        JsonFree(J *j);
J          *JsonGet(const J *obj, const char *key);
const char *JsonStr(const J *obj, const char *key);   // 文字列でなければ NULL
int         JsonBool(const J *obj, const char *key);

// ---------------------------------------------------------------- net.c（WinHTTP）

typedef struct {
    int       status;
    Buf       body;
    char     *finalUrl;            // リダイレクト後の URL
    char      contentType[128];
    char      etag[128];
    char      lastModified[64];
    char      dispName[260];       // Content-Disposition の filename
    long long length;              // Content-Length（不明なら -1）
    int       rateRemaining;       // X-RateLimit-Remaining（無ければ -1）
    long long rateReset;           // X-RateLimit-Reset（UNIX 秒、無ければ 0）
    int       retryAfter;          // Retry-After（秒、無ければ 0）
} HttpResp;

typedef void (*ProgressFn)(void *ctx, long long got, long long total);

void NetInit(void);
int  HttpGet(const char *url, const char *headers, int headOnly, HttpResp *r, char *err, size_t errlen);
void HttpRespFree(HttpResp *r);
// path へ保存する（いったん .part に書いて置き換える）。dispName に Content-Disposition の名前を返す
int  HttpDownload(const char *url, const WCHAR *path, ProgressFn fn, void *ctx, volatile LONG *cancel,
                  char *dispName, size_t dispLen, char *err, size_t errlen);

// ---------------------------------------------------------------- source.c（最新版の判定）

// ini には番号で保存するので、新しい取得元は SRC_COUNT の直前に足す
typedef enum { SRC_AUTO, SRC_GITHUB, SRC_GITLAB, SRC_GITEA, SRC_WEB, SRC_AMO, SRC_COUNT } SourceKind;

typedef struct {
    char *name;      // ファイル名
    char *url;       // ダウンロード URL
    char *version;   // 版（取れなければ NULL）
    int   score;     // Windows 向けらしさ
} Cand;

typedef struct {
    const char *url;
    SourceKind  kind;
    const char *filter;      // ファイル名の条件（正規表現、空なら自動）
    const char *verRx;       // 版の取り出し（正規表現、空なら自動）
    int         prerelease;
    const char *token;       // GitHub トークン（任意）
    int         noApi;       // GitHub を API ではなく普通のページで調べる（回数制限に数えられない）
} SourceCfg;

typedef struct {
    int        ok;
    char       err[512];
    SourceKind kind;         // 判定した取得元
    char      *version;
    char      *fileUrl;
    char      *fileName;
    char      *title;        // 名前の候補（リポジトリ名やページの <title>）
    char      *page;         // リリースのページ
    Cand      *cands;        // 選んだリリース（ページ）のファイル一覧
    int        ncand;
    int        chosen;       // cands の何番目を選んだか（-1 なし）
    int        allLinks;     // Web ページで見つかったリンクの総数
    long long  retryAt;      // 回数制限に当たったとき、問い合わせを再開してよい時刻（UNIX 秒）
} Latest;

const char *SourceName(SourceKind k);
SourceKind  SourceDetect(const char *url);
void        SourceApiHost(const SourceCfg *cfg, char *out, size_t n);   // 回数制限を数える単位
int         SourceLatest(const SourceCfg *cfg, Latest *out);
void        LatestFree(Latest *l);

int  VersionIsNewer(const char *latest, const char *installed);   // 更新扱いにするか
int  VersionCompare(const char *a, const char *b);                // 主要な数字部分の比較
char *VersionFromName(const char *name);                          // ファイル名から版らしい部分を拾う
int  WindowsScore(const char *name);                              // -1000 以下は対象外
