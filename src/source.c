// 取得元ごとに「最新の版」と「ダウンロードするファイル」を決める
//
// GitHub / GitLab / Gitea 系（Codeberg・Forgejo）はリリースの API を読む。
// Firefox アドオン（addons.mozilla.org）は AMO の API で公開中の版と .xpi を引く。
// Web ページは HTML の href / src と本文中の URL を集め、条件に合うリンクのうち版がいちばん新しいものを選ぶ。
// リンクから版が読めない（setup.exe のような固定名）ときは、そのファイルの Last-Modified / ETag を版の代わりにする。

#include "obtan.h"
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

static const char *DEFAULT_WEB_FILTER = "\\.(exe|msi|msix|msixbundle|appx|appxbundle|zip|7z|xpi)$";

const char *SourceName(SourceKind k) {
    switch (k) {
    case SRC_GITHUB: return "GitHub";
    case SRC_GITLAB: return "GitLab";
    case SRC_GITEA:  return "Codeberg / Gitea / Forgejo";
    case SRC_WEB:    return "Web ページ";
    case SRC_AMO:    return "Firefox アドオン (AMO)";
    default:         return "自動判定";
    }
}

// ---------------------------------------------------------------- URL の分解

typedef struct {
    char scheme[16];
    char host[256];      // ポートを含む
    char *path;          // 先頭の / を含む。クエリと断片は除く
} UrlParts;

static int SplitUrl(const char *url, UrlParts *u) {
    memset(u, 0, sizeof(*u));
    const char *p = strstr(url, "://");
    if (!p || p - url >= (int)sizeof(u->scheme)) return 0;
    snprintf(u->scheme, sizeof(u->scheme), "%.*s", (int)(p - url), url);
    for (char *s = u->scheme; *s; s++) *s = (char)tolower((unsigned char)*s);
    p += 3;
    size_t hn = strcspn(p, "/?#");
    if (!hn || hn >= sizeof(u->host)) return 0;
    snprintf(u->host, sizeof(u->host), "%.*s", (int)hn, p);
    for (char *s = u->host; *s; s++) *s = (char)tolower((unsigned char)*s);
    p += hn;
    size_t pn = strcspn(p, "?#");
    u->path = pn ? U8NDup(p, pn) : U8Dup("/");
    return u->path != NULL;
}

// パスの最初の n 区切りを取り出す（例: /owner/repo/releases → seg[0]=owner, seg[1]=repo）
static int PathSegments(const char *path, char segs[][256], int max) {
    int n = 0;
    const char *p = path;
    while (*p && n < max) {
        while (*p == '/') p++;
        if (!*p) break;
        size_t len = strcspn(p, "/");
        snprintf(segs[n], 256, "%.*s", (int)(len < 255 ? len : 255), p);
        n++;
        p += len;
    }
    return n;
}

static void StripGitSuffix(char *s) {
    size_t n = strlen(s);
    if (n > 4 && _stricmp(s + n - 4, ".git") == 0) s[n - 4] = 0;
}

static int UsesGitHubWeb(const SourceCfg *cfg, SourceKind kind) {
    return kind == SRC_GITHUB && cfg->noApi && !cfg->prerelease;   // /releases/latest はプレリリースを含まない
}

void SourceApiHost(const SourceCfg *cfg, char *out, size_t n) {
    SourceKind kind = cfg->kind == SRC_AUTO ? SourceDetect(cfg->url) : cfg->kind;
    if (kind == SRC_GITHUB) { snprintf(out, n, UsesGitHubWeb(cfg, kind) ? "github.com" : "api.github.com"); return; }
    UrlParts u;
    if (SplitUrl(cfg->url, &u)) { snprintf(out, n, "%s", u.host); free(u.path); }
    else snprintf(out, n, "%s", cfg->url);
}

SourceKind SourceDetect(const char *url) {
    UrlParts u;
    if (!SplitUrl(url, &u)) return SRC_WEB;
    SourceKind k = SRC_WEB;
    char segs[2][256];
    int n = PathSegments(u.path, segs, 2);
    if (n >= 2) {
        if (strcmp(u.host, "github.com") == 0 || strcmp(u.host, "www.github.com") == 0) k = SRC_GITHUB;
        else if (strcmp(u.host, "gitlab.com") == 0 || strncmp(u.host, "gitlab.", 7) == 0) k = SRC_GITLAB;
        else if (strcmp(u.host, "codeberg.org") == 0 || strncmp(u.host, "gitea.", 6) == 0 ||
                 strncmp(u.host, "forgejo.", 8) == 0) k = SRC_GITEA;
        else if (strcmp(u.host, "addons.mozilla.org") == 0) k = SRC_AMO;
    }
    free(u.path);
    return k;
}

// ---------------------------------------------------------------- 版

static int IsAlpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int IsDigit(int c) { return c >= '0' && c <= '9'; }

// 末尾の拡張子（.zip / .tar.gz / .7z など）を除いた長さ
static size_t StemLength(const char *name) {
    size_t n = strlen(name);
    static const char *exts[] = { ".tar.gz", ".tar.xz", ".tar.bz2", ".tar.zst", ".7z", NULL };
    for (int i = 0; exts[i]; i++) {
        size_t e = strlen(exts[i]);
        if (n > e && _stricmp(name + n - e, exts[i]) == 0) return n - e;
    }
    const char *dot = strrchr(name, '.');
    if (dot && dot[1] && strlen(dot) <= 12) {
        int hasAlpha = 0, ok = 1;
        for (const char *p = dot + 1; *p; p++) {
            if (IsAlpha((unsigned char)*p)) hasAlpha = 1;
            else if (!IsDigit((unsigned char)*p)) ok = 0;
        }
        if (ok && hasAlpha) return (size_t)(dot - name);
    }
    return n;
}

// 数字の直前に付いている英字の並び（x64 の x、win32 の win）が、アーキテクチャ名の一部かどうか
static int IsArchPrefix(const char *name, size_t i) {
    static const char *words[] = { "x", "win", "arm", "amd", "i", "ia", "aarch", "x86_", "x86-", "mingw", "msvc", NULL };
    for (int k = 0; words[k]; k++) {
        size_t w = strlen(words[k]);
        if (i < w || _strnicmp(name + i - w, words[k], w) != 0) continue;
        if (i == w || !IsAlpha((unsigned char)name[i - w - 1])) return 1;   // 単語の先頭から一致している
    }
    return 0;
}

// ファイル名から版らしい数字を拾う。優先順は
//   3: 1.2.3 のように点で続く数字
//   2: v12 や -12- のように区切りに挟まれた数字
//   1: 7z2501 のように英字に挟まれた数字（長いものを採る）
// x64・win32・i386・64bit のようなアーキテクチャ名の数字は採らない。
char *VersionFromName(const char *name) {
    size_t n = StemLength(name);
    size_t bestStart = 0, bestEnd = 0;
    int bestPri = 0;
    size_t i = 0;
    while (i < n) {
        if (!IsDigit((unsigned char)name[i]) || (i > 0 && IsDigit((unsigned char)name[i - 1]))) { i++; continue; }
        int prev = i > 0 ? (unsigned char)name[i - 1] : 0;
        size_t j = i;
        while (j < n && IsDigit((unsigned char)name[j])) j++;
        size_t digitsEnd = j;
        int dots = 0;
        while (j + 1 < n && name[j] == '.' && IsDigit((unsigned char)name[j + 1])) {
            j++;
            while (j < n && IsDigit((unsigned char)name[j])) j++;
            dots++;
        }
        int next = j < n ? (unsigned char)name[j] : 0;
        int pri;
        if (dots) pri = 3;
        else if (IsArchPrefix(name, i) || _strnicmp(name + digitsEnd, "bit", 3) == 0) pri = 0;
        else if ((!IsAlpha(prev) || prev == 'v' || prev == 'V') && !IsAlpha(next)) pri = 2;
        else pri = 1;
        if (pri > bestPri || (pri == 1 && bestPri == 1 && j - i > bestEnd - bestStart)) {
            bestStart = i;
            bestEnd = j;
            bestPri = pri;
        }
        i = j;
    }
    return bestPri ? U8NDup(name + bestStart, bestEnd - bestStart) : NULL;
}

// 最初に現れる「数字(.数字)*」を取り出す。区切りは . と _
static int MainNumbers(const char *s, long long *nums, int max, const char **rest) {
    while (*s && !IsDigit((unsigned char)*s)) s++;
    int n = 0;
    while (IsDigit((unsigned char)*s) && n < max) {
        long long v = 0;
        while (IsDigit((unsigned char)*s)) { if (v < 100000000000LL) v = v * 10 + (*s - '0'); s++; }
        nums[n++] = v;
        if ((*s == '.' || *s == '_') && IsDigit((unsigned char)s[1])) s++;
        else break;
    }
    if (rest) *rest = s;
    return n;
}

// a が新しければ 1、古ければ -1、同じなら 0。どちらかに数字が無ければ 2
int VersionCompare(const char *a, const char *b) {
    long long na[16], nb[16];
    int ca = MainNumbers(a, na, 16, NULL), cb = MainNumbers(b, nb, 16, NULL);
    if (!ca || !cb) return 2;
    int m = ca > cb ? ca : cb;
    for (int i = 0; i < m; i++) {
        long long x = i < ca ? na[i] : 0, y = i < cb ? nb[i] : 0;
        if (x != y) return x > y ? 1 : -1;
    }
    return 0;
}

int VersionIsNewer(const char *latest, const char *installed) {
    if (!latest || !*latest) return 0;
    if (!installed || !*installed) return 1;
    if (strcmp(latest, installed) == 0) return 0;
    int c = VersionCompare(latest, installed);
    if (c == 2) return 1;          // 比べようがない → 違っていれば更新とみなす
    if (c != 0) return c > 0;
    // 数字が同じなら、後ろに付いた部分（-beta1 など）が違うときだけ更新とみなす（先頭の v の有無は無視）
    long long tmp[16];
    const char *ra, *rb;
    MainNumbers(latest, tmp, 16, &ra);
    MainNumbers(installed, tmp, 16, &rb);
    return _stricmp(ra, rb) != 0;
}

// ---------------------------------------------------------------- Windows 向けらしさ

static int HasToken(char toks[][32], int n, const char *t) {
    for (int i = 0; i < n; i++) if (strcmp(toks[i], t) == 0) return 1;
    return 0;
}

int WindowsScore(const char *name) {
    char low[512];
    snprintf(low, sizeof(low), "%s", name);
    for (char *p = low; *p; p++) *p = (char)tolower((unsigned char)*p);
    size_t len = strlen(low);

    static const char *badExt[] = {
        ".sig", ".asc", ".sha256", ".sha512", ".sha1", ".md5", ".sum", ".txt", ".json", ".yml", ".yaml", ".blockmap",
        ".deb", ".rpm", ".dmg", ".pkg", ".apk", ".aab", ".appimage", ".snap", ".flatpak", ".ipa", ".gz", ".xz",
        ".bz2", ".zst", ".tgz", ".sbom", ".spdx", ".jsonl", ".pem", ".minisig", ".pdb", ".zsync", ".sha256sum", NULL
    };
    for (int i = 0; badExt[i]; i++) {
        size_t e = strlen(badExt[i]);
        if (len > e && strcmp(low + len - e, badExt[i]) == 0) return -1000;
    }

    char toks[64][32];
    int nt = 0;
    for (const char *p = low; *p && nt < 64;) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        size_t n = 0;
        while (p[n] && isalnum((unsigned char)p[n])) n++;
        if (n) { snprintf(toks[nt++], 32, "%.*s", (int)(n < 31 ? n : 31), p); p += n; }
    }
    static const char *badTok[] = {
        "linux", "darwin", "macos", "mac", "osx", "apple", "android", "ios", "freebsd", "openbsd", "netbsd",
        "debian", "ubuntu", "fedora", "musl", "source", "src", "sources", "tarball", NULL
    };
    for (int i = 0; badTok[i]; i++) if (HasToken(toks, nt, badTok[i])) return -1000;

    int score = 0;
    if (HasToken(toks, nt, "win") || HasToken(toks, nt, "windows") || HasToken(toks, nt, "win64") ||
        HasToken(toks, nt, "win32") || HasToken(toks, nt, "msvc") || HasToken(toks, nt, "mingw")) score += 30;
    int x64 = HasToken(toks, nt, "x64") || HasToken(toks, nt, "amd64") || HasToken(toks, nt, "win64") ||
              strstr(low, "x86_64") || strstr(low, "x86-64");
    if (x64) score += 20;
    if (HasToken(toks, nt, "arm64") || HasToken(toks, nt, "aarch64") || HasToken(toks, nt, "arm")) score -= 50;
    if (!x64 && (HasToken(toks, nt, "x86") || HasToken(toks, nt, "i386") || HasToken(toks, nt, "i686") ||
                 HasToken(toks, nt, "ia32") || HasToken(toks, nt, "32bit"))) score -= 20;
    if (HasToken(toks, nt, "setup") || HasToken(toks, nt, "installer") || HasToken(toks, nt, "install")) score += 3;

    const char *dot = strrchr(low, '.');
    if (!dot || strchr(dot, ' ')) return -1000;   // 拡張子が無いものは Linux 向けの実行ファイルか、ページへのリンク
    if (strcmp(dot, ".exe") == 0) score += 15;
    else if (strcmp(dot, ".msi") == 0) score += 12;
    else if (strcmp(dot, ".msix") == 0 || strcmp(dot, ".msixbundle") == 0 || strcmp(dot, ".appx") == 0 ||
             strcmp(dot, ".appxbundle") == 0) score += 8;
    else if (strcmp(dot, ".zip") == 0) score += 6;
    else if (strcmp(dot, ".7z") == 0) score += 5;
    else if (strcmp(dot, ".xpi") == 0) score += 4;   // Firefox 拡張（GitHub のリリースに置かれることが多い）
    else return -1000;   // 自動で選ぶのは Windows で使う形式だけ。ほかは「ファイル名の条件」で指定してもらう
    return score;
}

// ---------------------------------------------------------------- 共通の小道具

static void SetErr(Latest *out, const char *fmt, const char *arg) {
    snprintf(out->err, sizeof(out->err), fmt, arg ? arg : "");
}

static void AddCand(Latest *out, const char *name, const char *url, const char *version, int score) {
    Cand *c = (Cand *)realloc(out->cands, sizeof(Cand) * (out->ncand + 1));
    if (!c) return;
    out->cands = c;
    c = &out->cands[out->ncand++];
    c->name = U8Dup(name);
    c->url = U8Dup(url);
    c->version = version ? U8Dup(version) : NULL;
    c->score = score;
}

static void ClearCands(Latest *out) {
    for (int i = 0; i < out->ncand; i++) {
        free(out->cands[i].name);
        free(out->cands[i].url);
        free(out->cands[i].version);
    }
    free(out->cands);
    out->cands = NULL;
    out->ncand = 0;
    out->chosen = -1;
}

void LatestFree(Latest *l) {
    ClearCands(l);
    free(l->version);
    free(l->fileUrl);
    free(l->fileName);
    free(l->title);
    free(l->page);
    memset(l, 0, sizeof(*l));
    l->chosen = -1;
}

// 正規表現で版を取り出す。括弧があれば 1 つ目の括弧、無ければ一致した全体
static char *ExtractVersion(const Rx *rx, const char *text) {
    int caps[2 * RX_MAXGROUP];
    if (!rx || !text || !RxSearch(rx, text, caps)) return NULL;
    int s = caps[0], e = caps[1];
    if (RxGroups(rx) >= 1 && caps[2] >= 0) { s = caps[2]; e = caps[3]; }
    if (e <= s) return NULL;
    return U8NDup(text + s, (size_t)(e - s));
}

typedef struct {
    Rx *filter;     // 利用者の条件（無ければ NULL）
    Rx *verRx;
} Rules;

static int CompileRules(const SourceCfg *cfg, Rules *r, Latest *out, const char *defaultFilter) {
    memset(r, 0, sizeof(*r));
    char err[200];
    const char *f = cfg->filter && *cfg->filter ? cfg->filter : defaultFilter;
    if (f) {
        r->filter = RxCompile(f, 1, err, sizeof(err));
        if (!r->filter) { snprintf(out->err, sizeof(out->err), "ファイル名の条件が正しくない: %s", err); return 0; }
    }
    if (cfg->verRx && *cfg->verRx) {
        r->verRx = RxCompile(cfg->verRx, 1, err, sizeof(err));
        if (!r->verRx) {
            RxFree(r->filter);
            snprintf(out->err, sizeof(out->err), "版の取り出しが正しくない: %s", err);
            return 0;
        }
    }
    return 1;
}

static void FreeRules(Rules *r) {
    RxFree(r->filter);
    RxFree(r->verRx);
}

// 条件に合うファイルの中から 1 つ選ぶ。利用者の条件が無いときは Windows 向けでないものを除く
static int ChooseByScore(const Latest *out, const Rules *r) {
    int best = -1;
    for (int i = 0; i < out->ncand; i++) {
        const Cand *c = &out->cands[i];
        if (r->filter) { if (!RxSearch(r->filter, c->name, NULL)) continue; }
        else if (c->score <= -1000) continue;
        if (best < 0 || c->score > out->cands[best].score) best = i;
    }
    return best;
}

// 回数制限の応答なら、問い合わせを再開してよい時刻を控える
static void NoteRateLimit(const HttpResp *r, Latest *out) {
    int limited = r->status == 429 ||
                  (r->status == 403 && (r->rateRemaining == 0 || (r->body.p && U8FindI(r->body.p, "rate limit"))));
    if (!limited) return;
    long long now = (long long)time(NULL);
    if (r->rateReset > now) out->retryAt = r->rateReset;
    else out->retryAt = now + (r->retryAfter > 0 ? r->retryAfter : 15 * 60);
}

static char *HttpErrorText(const HttpResp *r, const char *what) {
    if (r->status == 404) return U8Fmt("%sが見つからない（HTTP 404）", what);
    if ((r->status == 403 || r->status == 429) && r->body.p && U8FindI(r->body.p, "rate limit"))
        return U8Fmt("API の回数制限に達した（HTTP %d）。しばらく待つか、設定で GitHub トークンを指定する", r->status);
    return U8Fmt("%s: HTTP %d", what, r->status);
}

// ---------------------------------------------------------------- リリース API（GitHub / GitLab / Gitea）

// リリース 1 件分のファイルを候補に並べる
static void FillReleaseCands(Latest *out, const J *rel) {
    ClearCands(out);
    if (out->kind == SRC_GITLAB) {
        J *links = JsonGet(JsonGet(rel, "assets"), "links");
        for (J *a = links ? links->child : NULL; a; a = a->next) {
            const char *url = JsonStr(a, "direct_asset_url");
            if (!url) url = JsonStr(a, "url");
            const char *name = JsonStr(a, "name");
            if (!url) continue;
            // リンクの表示名ではなく、URL の末尾をファイル名として扱う
            const char *slash = strrchr(url, '/');
            char *fname = PercentDecode(slash && slash[1] ? slash + 1 : (name ? name : url));
            AddCand(out, fname, url, NULL, WindowsScore(fname));
            free(fname);
        }
    } else {
        J *assets = JsonGet(rel, "assets");
        for (J *a = assets ? assets->child : NULL; a; a = a->next) {
            const char *name = JsonStr(a, "name"), *url = JsonStr(a, "browser_download_url");
            if (name && url) AddCand(out, name, url, NULL, WindowsScore(name));
        }
    }
}


static int FromReleases(const SourceCfg *cfg, Latest *out) {
    UrlParts u;
    if (!SplitUrl(cfg->url, &u)) { SetErr(out, "URL の形式が正しくない", NULL); return 0; }
    char segs[16][256];
    int ns = PathSegments(u.path, segs, 16);
    if (ns < 2) { free(u.path); SetErr(out, "URL にリポジトリ（所有者/名前）が含まれていない", NULL); return 0; }

    char *api = NULL, *headers = NULL;
    if (out->kind == SRC_GITHUB) {
        StripGitSuffix(segs[1]);
        api = U8Fmt("https://api.github.com/repos/%s/%s/releases?per_page=30", segs[0], segs[1]);
        headers = U8Fmt("Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28%s%s",
                        cfg->token && *cfg->token ? "\r\nAuthorization: Bearer " : "",
                        cfg->token && *cfg->token ? cfg->token : "");
        out->title = U8Dup(segs[1]);
    } else if (out->kind == SRC_GITLAB) {
        // グループは入れ子にできる。/-/ より前がプロジェクトのパス
        Buf proj = {0};
        int last = 0;
        for (int i = 0; i < ns && strcmp(segs[i], "-") != 0; i++) {
            if (proj.n) BufAddStr(&proj, "%2F");
            StripGitSuffix(segs[i]);
            BufAddStr(&proj, segs[i]);
            last = i;
        }
        char *p = BufTake(&proj);
        api = U8Fmt("%s://%s/api/v4/projects/%s/releases?per_page=30", u.scheme, u.host, p);
        free(p);
        out->title = U8Dup(segs[last]);
    } else {
        StripGitSuffix(segs[1]);
        api = U8Fmt("%s://%s/api/v1/repos/%s/%s/releases?limit=30", u.scheme, u.host, segs[0], segs[1]);
        out->title = U8Dup(segs[1]);
    }
    free(u.path);

    HttpResp r;
    int ok = HttpGet(api, headers, 0, &r, out->err, sizeof(out->err));
    free(api);
    free(headers);
    if (!ok) return 0;
    if (r.status != 200) {
        NoteRateLimit(&r, out);
        char *e = HttpErrorText(&r, "リリースの一覧");
        SetErr(out, "%s", e);
        free(e);
        HttpRespFree(&r);
        return 0;
    }
    J *root = JsonParse(r.body.p);
    HttpRespFree(&r);
    if (!root || root->t != J_ARR) { JsonFree(root); SetErr(out, "リリースの一覧を読めない（JSON ではない）", NULL); return 0; }

    Rules rules;
    if (!CompileRules(cfg, &rules, out, NULL)) { JsonFree(root); return 0; }

    int found = 0, releases = 0;
    const J *firstRel = NULL;
    char firstTag[128] = "";
    for (J *rel = root->child; rel && !found; rel = rel->next) {
        if (out->kind == SRC_GITLAB) {
            if (JsonBool(rel, "upcoming_release")) continue;
        } else {
            if (JsonBool(rel, "draft")) continue;
            if (JsonBool(rel, "prerelease") && !cfg->prerelease) continue;
        }
        const char *tag = JsonStr(rel, "tag_name");
        if (!tag) continue;
        releases++;
        FillReleaseCands(out, rel);
        if (!firstRel) firstRel = rel;
        if (!firstTag[0]) snprintf(firstTag, sizeof(firstTag), "%s", tag);
        int pick = ChooseByScore(out, &rules);
        if (pick < 0) continue;

        out->chosen = pick;
        out->fileUrl = U8Dup(out->cands[pick].url);
        out->fileName = U8Dup(out->cands[pick].name);
        out->version = ExtractVersion(rules.verRx, tag);
        if (!out->version && rules.verRx) out->version = ExtractVersion(rules.verRx, out->fileName);
        if (!out->version) out->version = U8Dup(tag);
        const char *html = JsonStr(rel, "html_url");
        J *l = JsonGet(rel, "_links");
        if (!html && l) html = JsonStr(l, "self");
        out->page = U8Dup(html ? html : cfg->url);
        found = 1;
    }
    if (!found && firstRel) FillReleaseCands(out, firstRel);   // 見せるのは最新リリースの一覧
    JsonFree(root);
    FreeRules(&rules);
    if (found) return 1;
    if (!releases) SetErr(out, cfg->prerelease ? "リリースが 1 つも無い" : "リリースが 1 つも無い（プレリリースを除く）", NULL);
    else snprintf(out->err, sizeof(out->err), "条件に合うファイルが無い（最新 %s を含む %d 件のリリースを見た）", firstTag, releases);
    return 0;
}

// ---------------------------------------------------------------- Web ページ

static void DecodeEntitiesInto(Buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') { BufAddChar(b, s[i]); continue; }
        const char *e = s + i + 1;
        size_t rest = n - i - 1;
        size_t semi = 0;
        while (semi < rest && semi < 10 && e[semi] != ';') semi++;
        if (semi >= rest || e[semi] != ';') { BufAddChar(b, '&'); continue; }
        unsigned cp = 0;
        if (semi == 3 && strncmp(e, "amp", 3) == 0) cp = '&';
        else if (semi == 2 && strncmp(e, "lt", 2) == 0) cp = '<';
        else if (semi == 2 && strncmp(e, "gt", 2) == 0) cp = '>';
        else if (semi == 4 && strncmp(e, "quot", 4) == 0) cp = '"';
        else if (semi == 4 && strncmp(e, "apos", 4) == 0) cp = '\'';
        else if (semi == 4 && strncmp(e, "nbsp", 4) == 0) cp = ' ';
        else if (e[0] == '#') cp = (unsigned)strtoul(e[1] == 'x' || e[1] == 'X' ? e + 2 : e + 1, NULL, e[1] == 'x' || e[1] == 'X' ? 16 : 10);
        if (!cp || cp > 0x10FFFF) { BufAddChar(b, '&'); continue; }
        WCHAR w[3] = {0};
        if (cp >= 0x10000) { w[0] = (WCHAR)(0xD800 + ((cp - 0x10000) >> 10)); w[1] = (WCHAR)(0xDC00 + ((cp - 0x10000) & 0x3FF)); }
        else w[0] = (WCHAR)cp;
        char *u = W2U(w);
        if (u) { BufAddStr(b, u); free(u); }
        i += semi + 1;
    }
}

static char *DecodeEntities(const char *s, size_t n) {
    Buf b = {0};
    BufAdd(&b, "", 0);
    DecodeEntitiesInto(&b, s, n);
    return BufTake(&b);
}

// パスの . と .. を畳む（先頭の / を含むパスを受け取る）
static char *NormalizePath(const char *path) {
    char **segs = NULL;
    int n = 0, cap = 0;
    const char *p = path;
    int trailing = 0;
    while (*p) {
        while (*p == '/') p++;
        size_t len = strcspn(p, "/");
        trailing = p[len] == '/' || (len == 0);
        if (len == 1 && p[0] == '.') { trailing = 1; }
        else if (len == 2 && p[0] == '.' && p[1] == '.') { if (n) free(segs[--n]); trailing = 1; }
        else if (len) {
            if (n == cap) { cap = cap ? cap * 2 : 16; char **t = (char **)realloc(segs, sizeof(char *) * cap); if (!t) break; segs = t; }
            segs[n++] = U8NDup(p, len);
            trailing = p[len] == '/';
        }
        p += len;
    }
    Buf b = {0};
    for (int i = 0; i < n; i++) { BufAddChar(&b, '/'); BufAddStr(&b, segs[i]); free(segs[i]); }
    if (!n || trailing) BufAddChar(&b, '/');
    free(segs);
    return BufTake(&b);
}

// base を基準に ref を絶対 URL にする。http(s) 以外は NULL
static char *ResolveUrl(const char *base, const char *ref) {
    while (*ref == ' ' || *ref == '\t' || *ref == '\n' || *ref == '\r') ref++;
    if (!*ref || *ref == '#') return NULL;
    const char *colon = strchr(ref, ':');
    const char *slash = strpbrk(ref, "/?#");
    if (colon && (!slash || colon < slash)) {
        if (!U8StartsWithI(ref, "http://") && !U8StartsWithI(ref, "https://")) return NULL;   // mailto: javascript: など
        size_t n = strcspn(ref, "#");
        return U8NDup(ref, n);
    }
    UrlParts u;
    if (!SplitUrl(base, &u)) return NULL;
    char *frag = NULL;
    size_t rn = strcspn(ref, "#");
    char *r = U8NDup(ref, rn);
    char *res = NULL;
    if (r[0] == '/' && r[1] == '/') {
        res = U8Fmt("%s:%s", u.scheme, r);
    } else {
        // ref をパスとクエリに分けてから、パスを組み立てて畳む
        size_t qpos = strcspn(r, "?");
        char *rpath = U8NDup(r, qpos);
        const char *query = r + qpos;
        char *joined;
        if (rpath[0] == '/') joined = U8Dup(rpath);
        else if (!rpath[0]) joined = U8Dup(u.path);
        else {
            const char *last = strrchr(u.path, '/');
            joined = U8Fmt("%.*s%s", (int)(last - u.path + 1), u.path, rpath);
        }
        char *norm = NormalizePath(joined);
        if (!*query && !rpath[0]) {
            // 空の参照は base そのもの（クエリも含む）
            res = U8Dup(base);
            char *h = strchr(res, '#');
            if (h) *h = 0;
        } else {
            res = U8Fmt("%s://%s%s%s", u.scheme, u.host, norm, query);
        }
        free(norm);
        free(joined);
        free(rpath);
    }
    free(frag);
    free(r);
    free(u.path);
    return res;
}

typedef struct {
    char **v;
    int    n, cap;
} StrList;

static void ListAddUnique(StrList *l, char *s) {
    if (!s) return;
    for (int i = 0; i < l->n; i++) if (strcmp(l->v[i], s) == 0) { free(s); return; }
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        char **t = (char **)realloc(l->v, sizeof(char *) * cap);
        if (!t) { free(s); return; }
        l->v = t;
        l->cap = cap;
    }
    l->v[l->n++] = s;
}

static void ListFree(StrList *l) {
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof(*l));
}

static void AddLink(StrList *l, const char *base, const char *raw, size_t n) {
    char *dec = DecodeEntities(raw, n);
    // JSON の中に書かれた URL は / が \/ になっていることがある
    for (char *p = dec, *q = dec;; ) { if (p[0] == '\\' && p[1] == '/') p++; if (!(*q++ = *p++)) break; }
    ListAddUnique(l, ResolveUrl(base, dec));
    free(dec);
}

// href= / src= の値と、本文中に直接書かれた http(s):// の URL を集める
static void ExtractLinks(const char *html, const char *base, StrList *out) {
    for (const char *p = html; *p; p++) {
        const char *attr = NULL;
        if ((_strnicmp(p, "href", 4) == 0 && (attr = p + 4)) || (_strnicmp(p, "src", 3) == 0 && (attr = p + 3))) {
            if (p > html && (isalnum((unsigned char)p[-1]) || p[-1] == '-' || p[-1] == '_')) continue;
            const char *q = attr;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            if (*q != '=') continue;
            q++;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            size_t n;
            if (*q == '"' || *q == '\'') {
                char quote = *q++;
                const char *e = strchr(q, quote);
                if (!e) continue;
                n = (size_t)(e - q);
            } else {
                n = strcspn(q, " \t\r\n>");
            }
            if (n && n < 4096) AddLink(out, base, q, n);
        } else if (_strnicmp(p, "http://", 7) == 0 || _strnicmp(p, "https://", 8) == 0 ||
                   _strnicmp(p, "https:\\/\\/", 10) == 0 || _strnicmp(p, "http:\\/\\/", 9) == 0) {
            size_t n = strcspn(p, "\"'<> \t\r\n`)");
            if (n < 4096) AddLink(out, base, p, n);
            p += n ? n - 1 : 0;
        }
    }
}

static char *PageTitle(const char *html) {
    const char *t = U8FindI(html, "<title");
    if (!t) return NULL;
    t = strchr(t, '>');
    if (!t) return NULL;
    t++;
    const char *e = U8FindI(t, "</title");
    if (!e || e - t > 1000) return NULL;
    char *dec = DecodeEntities(t, (size_t)(e - t));
    // 空白をまとめ、「名前 - 説明」の形なら名前の部分だけにする
    Buf b = {0};
    BufAdd(&b, "", 0);
    int sp = 0;
    for (const char *p = dec; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') { sp = 1; continue; }
        if (sp && b.n) BufAddChar(&b, ' ');
        sp = 0;
        BufAddChar(&b, *p);
    }
    free(dec);
    char *s = BufTake(&b);
    static const char *seps[] = { " - ", " | ", " – ", " — ", " : ", NULL };
    for (int i = 0; seps[i]; i++) {
        char *x = strstr(s, seps[i]);
        if (x && x != s) { *x = 0; break; }
    }
    return s;
}

// URL の最後の区切り（クエリを除き、%XX を戻したもの）
static char *LastSegment(const char *url) {
    size_t n = strcspn(url, "?#");
    const char *start = url;
    for (size_t i = 0; i < n; i++) if (url[i] == '/') start = url + i + 1;
    char *raw = U8NDup(start, (size_t)(url + n - start));
    char *dec = PercentDecode(raw);
    free(raw);
    return dec;
}

// ファイルそのものの Last-Modified / ETag から版の代わりを作る
static char *VersionFromHeaders(const char *url, char *err, size_t errlen) {
    HttpResp r;
    int ok = HttpGet(url, NULL, 1, &r, err, errlen);
    if (!ok || r.status >= 400) {   // HEAD を受け付けないサーバーには先頭 1 バイトだけ取りに行く
        if (ok) HttpRespFree(&r);
        ok = HttpGet(url, "Range: bytes=0-0", 0, &r, err, errlen);
        if (ok && r.status >= 400) { snprintf(err, errlen, "ファイルの情報を取れない: HTTP %d", r.status); HttpRespFree(&r); return NULL; }
    }
    if (!ok) return NULL;
    char *v = NULL;
    SYSTEMTIME st;
    WCHAR *wlm = U2W(r.lastModified);
    if (r.lastModified[0] && wlm && WinHttpTimeToSystemTime(wlm, &st))
        v = U8Fmt("%04u.%02u.%02u.%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    else if (r.etag[0]) v = U8Fmt("etag:%s", strncmp(r.etag, "W/", 2) == 0 ? r.etag + 2 : r.etag);   // 弱い印は経路で変わる
    else snprintf(err, errlen, "版を判定できない（リンクにも、ファイルの更新日時にも手がかりが無い）");
    free(wlm);
    HttpRespFree(&r);
    return v;
}

static int FromWebPage(const SourceCfg *cfg, Latest *out) {
    HttpResp r;
    if (!HttpGet(cfg->url, "Accept: text/html,application/xhtml+xml,*/*;q=0.8", 0, &r, out->err, sizeof(out->err))) return 0;
    if (r.status != 200) {
        NoteRateLimit(&r, out);
        char *e = HttpErrorText(&r, "ページ");
        SetErr(out, "%s", e);
        free(e);
        HttpRespFree(&r);
        return 0;
    }
    out->page = U8Dup(cfg->url);
    Rules rules;
    if (!CompileRules(cfg, &rules, out, DEFAULT_WEB_FILTER)) { HttpRespFree(&r); return 0; }
    const char *base = r.finalUrl ? r.finalUrl : cfg->url;

    if (r.contentType[0] && !U8FindI(r.contentType, "html")) {
        // URL がファイルそのものを指している
        char *name = r.dispName[0] ? U8Dup(r.dispName) : LastSegment(base);
        char *ver = rules.verRx ? ExtractVersion(rules.verRx, name) : VersionFromName(name);
        AddCand(out, name, cfg->url, ver, WindowsScore(name));
        free(ver);
        free(name);
        out->allLinks = 1;
    } else {
        out->title = PageTitle(r.body.p);
        StrList links = {0};
        ExtractLinks(r.body.p, base, &links);
        out->allLinks = links.n;
        for (int i = 0; i < links.n; i++) {
            char *dec = PercentDecode(links.v[i]);
            if (RxSearch(rules.filter, dec, NULL)) {
                char *name = LastSegment(links.v[i]);
                char *ver = rules.verRx ? ExtractVersion(rules.verRx, dec) : VersionFromName(name);
                AddCand(out, name, links.v[i], ver, WindowsScore(name));
                free(ver);
                free(name);
            }
            free(dec);
        }
        ListFree(&links);
    }
    HttpRespFree(&r);
    FreeRules(&rules);

    if (!out->ncand) {
        snprintf(out->err, sizeof(out->err), "条件に合うリンクが無い（ページ内のリンク %d 件）", out->allLinks);
        return 0;
    }
    // 版が読めたもののうち最も新しいもの。並んだら Windows 向けらしいもの、それも並んだら先に出てきたもの
    int best = 0;
    for (int i = 1; i < out->ncand; i++) {
        const Cand *a = &out->cands[i], *b = &out->cands[best];
        if (a->version && !b->version) { best = i; continue; }
        if (!a->version && b->version) continue;
        int c = a->version ? VersionCompare(a->version, b->version) : 0;
        if (c == 1 || ((c == 0 || c == 2) && a->score > b->score)) best = i;
    }
    out->chosen = best;
    out->fileUrl = U8Dup(out->cands[best].url);
    out->fileName = U8Dup(out->cands[best].name);
    if (out->cands[best].version) out->version = U8Dup(out->cands[best].version);
    else {
        out->version = VersionFromHeaders(out->fileUrl, out->err, sizeof(out->err));
        if (!out->version) return 0;
    }
    return 1;
}

// ---------------------------------------------------------------- GitHub（API を使わない）

// API は トークン無しだと IP ごとに 1 時間 60 回までなので、ダウンロードの前の調べ直しは普通のページで行う。
//   /releases/latest は最新リリース（プレリリースと下書きを除く）の /releases/tag/<タグ> へ転送される
//   /releases/expanded_assets/<タグ> は、そのリリースの添付ファイルの一覧（HTML の断片）
// どちらも API の回数には数えられない（2026-10-11 に実測）。
static int FromGitHubWeb(const SourceCfg *cfg, Latest *out) {
    UrlParts u;
    if (!SplitUrl(cfg->url, &u)) { SetErr(out, "URL の形式が正しくない", NULL); return 0; }
    char segs[2][256];
    int ns = PathSegments(u.path, segs, 2);
    free(u.path);
    if (ns < 2) { SetErr(out, "URL にリポジトリ（所有者/名前）が含まれていない", NULL); return 0; }
    StripGitSuffix(segs[1]);
    out->title = U8Dup(segs[1]);

    char *latest = U8Fmt("https://github.com/%s/%s/releases/latest", segs[0], segs[1]);
    HttpResp r;
    int ok = HttpGet(latest, NULL, 1, &r, out->err, sizeof(out->err));
    free(latest);
    if (!ok) return 0;
    if (r.status != 200) {
        NoteRateLimit(&r, out);
        char *e = HttpErrorText(&r, "リポジトリ");
        SetErr(out, "%s", e);
        free(e);
        HttpRespFree(&r);
        return 0;
    }
    const char *t = r.finalUrl ? strstr(r.finalUrl, "/releases/tag/") : NULL;
    if (!t) { HttpRespFree(&r); SetErr(out, "リリースが 1 つも無い（プレリリースを除く）", NULL); return 0; }
    t += 14;
    char *tagEnc = U8NDup(t, strcspn(t, "?#"));
    char *tag = PercentDecode(tagEnc);
    out->page = U8Dup(r.finalUrl);
    HttpRespFree(&r);

    char *assets = U8Fmt("https://github.com/%s/%s/releases/expanded_assets/%s", segs[0], segs[1], tagEnc);
    ok = HttpGet(assets, "Accept: text/html", 0, &r, out->err, sizeof(out->err));
    free(assets);
    free(tagEnc);
    if (!ok) { free(tag); return 0; }
    if (r.status != 200) {
        NoteRateLimit(&r, out);
        char *e = HttpErrorText(&r, "添付ファイルの一覧");
        SetErr(out, "%s", e);
        free(e);
        HttpRespFree(&r);
        free(tag);
        return 0;
    }
    // 添付ファイルは /所有者/名前/releases/download/<タグ>/<ファイル名> へのリンクとして並んでいる
    StrList links = {0};
    for (const char *p = r.body.p; (p = strstr(p, "href=\"")) != NULL;) {
        p += 6;
        size_t n = strcspn(p, "\"");
        char *href = DecodeEntities(p, n);
        if (strstr(href, "/releases/download/")) ListAddUnique(&links, href[0] == '/' ? U8Fmt("https://github.com%s", href) : U8Dup(href));
        free(href);
        p += n;
    }
    HttpRespFree(&r);
    for (int i = 0; i < links.n; i++) {
        char *name = LastSegment(links.v[i]);
        AddCand(out, name, links.v[i], NULL, WindowsScore(name));
        free(name);
    }
    ListFree(&links);

    Rules rules;
    if (!CompileRules(cfg, &rules, out, NULL)) { free(tag); return 0; }
    int pick = ChooseByScore(out, &rules);
    if (pick >= 0) {
        out->chosen = pick;
        out->fileUrl = U8Dup(out->cands[pick].url);
        out->fileName = U8Dup(out->cands[pick].name);
        out->version = ExtractVersion(rules.verRx, tag);
        if (!out->version && rules.verRx) out->version = ExtractVersion(rules.verRx, out->fileName);
        if (!out->version) out->version = U8Dup(tag);
    } else {
        snprintf(out->err, sizeof(out->err), "条件に合うファイルが無い（最新 %s、添付 %d 件）", tag, out->ncand);
    }
    FreeRules(&rules);
    free(tag);
    return pick >= 0;
}

// ---------------------------------------------------------------- Firefox アドオン（AMO）

// https://addons.mozilla.org/ja/firefox/addon/<slug>/ の <slug>（数字の ID や GUID でもよい）で API を引く
static int FromAmo(const SourceCfg *cfg, Latest *out) {
    UrlParts u;
    if (!SplitUrl(cfg->url, &u)) { SetErr(out, "URL の形式が正しくない", NULL); return 0; }
    char segs[8][256];
    int ns = PathSegments(u.path, segs, 8);
    free(u.path);
    const char *slug = NULL;
    for (int i = 0; i + 1 < ns; i++) if (strcmp(segs[i], "addon") == 0) slug = segs[i + 1];
    if (!slug) { SetErr(out, "URL にアドオンの名前が無い（…/firefox/addon/名前/ の形にする）", NULL); return 0; }

    char *api = U8Fmt("https://addons.mozilla.org/api/v5/addons/addon/%s/?lang=ja", slug);
    HttpResp r;
    int ok = HttpGet(api, "Accept: application/json", 0, &r, out->err, sizeof(out->err));
    free(api);
    if (!ok) return 0;
    if (r.status != 200) {
        NoteRateLimit(&r, out);
        char *e = HttpErrorText(&r, "アドオン");
        SetErr(out, "%s", e);
        free(e);
        HttpRespFree(&r);
        return 0;
    }
    J *root = JsonParse(r.body.p);
    HttpRespFree(&r);
    J *cv = JsonGet(root, "current_version");
    const char *ver = JsonStr(cv, "version");
    const char *url = JsonStr(JsonGet(cv, "file"), "url");
    if (!ver || !url) {
        JsonFree(root);
        SetErr(out, "公開中の版が無い（AMO に載っていないか、取り下げられている）", NULL);
        return 0;
    }
    // 名前は言語ごとの辞書で返る（lang を付けても {"ja": "…"} の形）
    J *name = JsonGet(root, "name");
    if (name && name->t == J_OBJ) name = name->child;
    if (name && name->t == J_STR) out->title = U8Dup(name->s);
    const char *page = JsonStr(root, "url");
    out->page = U8Dup(page ? page : cfg->url);

    Rules rules;
    if (!CompileRules(cfg, &rules, out, NULL)) { JsonFree(root); return 0; }
    char *fname = LastSegment(url);
    AddCand(out, fname, url, ver, WindowsScore(fname));
    free(fname);
    int pick = ChooseByScore(out, &rules);
    if (pick < 0 && !rules.filter) pick = 0;   // 条件が無ければ、AMO が出す 1 つをそのまま使う
    if (pick >= 0) {
        out->chosen = pick;
        out->fileUrl = U8Dup(out->cands[pick].url);
        out->fileName = U8Dup(out->cands[pick].name);
        out->version = ExtractVersion(rules.verRx, ver);
        if (!out->version) out->version = U8Dup(ver);
    } else {
        SetErr(out, "ファイル名の条件に合わない", NULL);
    }
    FreeRules(&rules);
    JsonFree(root);
    return pick >= 0;
}

int SourceLatest(const SourceCfg *cfg, Latest *out) {
    memset(out, 0, sizeof(*out));
    out->chosen = -1;
    out->kind = cfg->kind == SRC_AUTO ? SourceDetect(cfg->url) : cfg->kind;
    if (out->kind == SRC_WEB) out->ok = FromWebPage(cfg, out);
    else if (UsesGitHubWeb(cfg, out->kind)) out->ok = FromGitHubWeb(cfg, out);
    else if (out->kind == SRC_AMO) out->ok = FromAmo(cfg, out);
    else out->ok = FromReleases(cfg, out);
    return out->ok;
}
