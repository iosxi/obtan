// WinHTTP での取得とダウンロード
//
// リダイレクトは WinHTTP に任せ（GitHub のアセットは別ホストへ飛ぶ）、最終 URL を控える。
// プロキシは Windows の設定（自動構成を含む）に従う。

#include "obtan.h"
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "winhttp.lib")

#define USER_AGENT   L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) obtan/" TEXT(APP_VERSION) L" (+https://github.com/iosxi/obtan)"
#define MAX_BODY     (32 * 1024 * 1024)

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

static HINTERNET g_session;

void NetInit(void) {
    g_session = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_session)   // Windows 8.1 より前は自動プロキシの指定が無い
        g_session = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_session) return;
    WinHttpSetTimeouts(g_session, 15000, 15000, 30000, 60000);
    DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(g_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos))) {
        protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;   // TLS 1.3 を知らない Windows 10
        WinHttpSetOption(g_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos));
    }
}

static void SetNetError(char *err, size_t errlen, const char *what) {
    DWORD e = GetLastError();
    const char *msg;
    switch (e) {
    case ERROR_WINHTTP_TIMEOUT:            msg = "時間切れ"; break;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:  msg = "ホスト名を解決できない"; break;
    case ERROR_WINHTTP_CANNOT_CONNECT:     msg = "接続できない"; break;
    case ERROR_WINHTTP_CONNECTION_ERROR:   msg = "接続が切れた"; break;
    case ERROR_WINHTTP_SECURE_FAILURE:     msg = "TLS（証明書）の確認に失敗"; break;
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
    case ERROR_WINHTTP_INVALID_URL:        msg = "URL の形式が正しくない"; break;
    case ERROR_WINHTTP_REDIRECT_FAILED:    msg = "リダイレクトに失敗"; break;
    case ERROR_OPERATION_ABORTED:          msg = "中止した"; break;
    default:                               msg = NULL;
    }
    if (msg) snprintf(err, errlen, "%s: %s", what, msg);
    else snprintf(err, errlen, "%s: 通信エラー %lu", what, e);
}

// 空白や ASCII 以外を %XX にする（WinHTTP は生の UTF-8 をそのまま送らない）
static char *EncodeUrl(const char *url) {
    Buf b = {0};
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        if (*p >= 0x80 || *p == ' ' || *p == '"' || *p == '<' || *p == '>') {
            char t[4];
            snprintf(t, sizeof(t), "%%%02X", *p);
            BufAddStr(&b, t);
        } else {
            BufAddChar(&b, (char)*p);
        }
    }
    return BufTake(&b);
}

typedef struct {
    HINTERNET conn, req;
} Req;

static void CloseReq(Req *r) {
    if (r->req) WinHttpCloseHandle(r->req);
    if (r->conn) WinHttpCloseHandle(r->conn);
    r->req = r->conn = NULL;
}

static int OpenReq(const char *url, const WCHAR *method, const char *headers, Req *r, char *err, size_t errlen) {
    memset(r, 0, sizeof(*r));
    if (!g_session) { snprintf(err, errlen, "WinHTTP を初期化できない"); return 0; }
    char *enc = EncodeUrl(url);
    WCHAR *w = U2W(enc);
    free(enc);
    URL_COMPONENTS uc = { sizeof(uc) };
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!w || !WinHttpCrackUrl(w, 0, 0, &uc) ||
        (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS)) {
        snprintf(err, errlen, "URL の形式が正しくない（http:// か https:// で始める）");
        free(w);
        return 0;
    }
    WCHAR host[256];
    wcsncpy_s(host, _countof(host), uc.lpszHostName, uc.dwHostNameLength < 255 ? uc.dwHostNameLength : 255);
    // パスとクエリは文字列上で続いているので、まとめて渡す
    WCHAR *path = (WCHAR *)malloc(sizeof(WCHAR) * (uc.dwUrlPathLength + uc.dwExtraInfoLength + 2));
    if (!path) { free(w); return 0; }
    if (uc.dwUrlPathLength == 0) { wcscpy_s(path, 2, L"/"); }
    else { wmemcpy(path, uc.lpszUrlPath, uc.dwUrlPathLength); path[uc.dwUrlPathLength] = 0; }
    if (uc.dwExtraInfoLength) {
        size_t n = wcslen(path);
        wmemcpy(path + n, uc.lpszExtraInfo, uc.dwExtraInfoLength);
        path[n + uc.dwExtraInfoLength] = 0;
    }
    int ok = 0;
    r->conn = WinHttpConnect(g_session, host, uc.nPort, 0);
    if (r->conn)
        r->req = WinHttpOpenRequest(r->conn, method, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (r->req) {
        DWORD dec = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(r->req, WINHTTP_OPTION_DECOMPRESSION, &dec, sizeof(dec));
        if (headers && *headers) {
            WCHAR *wh = U2W(headers);
            if (wh) WinHttpAddRequestHeaders(r->req, wh, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            free(wh);
        }
        if (WinHttpSendRequest(r->req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(r->req, NULL))
            ok = 1;
    }
    if (!ok) {
        WCHAR *hostOnly = host;
        char *h = W2U(hostOnly);
        SetNetError(err, errlen, h ? h : "接続");
        free(h);
        CloseReq(r);
    }
    free(path);
    free(w);
    return ok;
}

static void QueryHeaderU8(HINTERNET req, DWORD what, char *out, size_t outlen) {
    WCHAR buf[1024];
    DWORD cb = sizeof(buf);
    out[0] = 0;
    if (!WinHttpQueryHeaders(req, what, WINHTTP_HEADER_NAME_BY_INDEX, buf, &cb, WINHTTP_NO_HEADER_INDEX)) return;
    char *u = W2U(buf);
    if (u) { snprintf(out, outlen, "%s", u); free(u); }
}

// Content-Disposition から保存名を取り出す（filename* を優先）
static void ParseDisposition(const char *cd, char *out, size_t outlen) {
    out[0] = 0;
    const char *p = U8FindI(cd, "filename*=");
    if (p) {
        p += 10;
        const char *q = strstr(p, "''");
        if (q) p = q + 2;
        size_t n = strcspn(p, "; ");
        char *raw = U8NDup(p, n);
        char *dec = raw ? PercentDecode(raw) : NULL;
        if (dec) snprintf(out, outlen, "%s", dec);
        free(raw);
        free(dec);
        if (out[0]) return;
    }
    p = U8FindI(cd, "filename=");
    if (!p) return;
    p += 9;
    if (*p == '"') {
        p++;
        size_t n = strcspn(p, "\"");
        snprintf(out, outlen, "%.*s", (int)n, p);
    } else {
        size_t n = strcspn(p, "; ");
        snprintf(out, outlen, "%.*s", (int)n, p);
    }
}

static void FillRespInfo(HINTERNET req, HttpResp *r) {
    DWORD status = 0, cb = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &cb, WINHTTP_NO_HEADER_INDEX);
    r->status = (int)status;
    QueryHeaderU8(req, WINHTTP_QUERY_CONTENT_TYPE, r->contentType, sizeof(r->contentType));
    QueryHeaderU8(req, WINHTTP_QUERY_ETAG, r->etag, sizeof(r->etag));
    QueryHeaderU8(req, WINHTTP_QUERY_LAST_MODIFIED, r->lastModified, sizeof(r->lastModified));
    char len[64], cd[1024];
    QueryHeaderU8(req, WINHTTP_QUERY_CONTENT_LENGTH, len, sizeof(len));
    r->length = len[0] ? _atoi64(len) : -1;
    QueryHeaderU8(req, WINHTTP_QUERY_CONTENT_DISPOSITION, cd, sizeof(cd));
    ParseDisposition(cd, r->dispName, sizeof(r->dispName));

    DWORD ulen = 0;
    WinHttpQueryOption(req, WINHTTP_OPTION_URL, NULL, &ulen);
    if (ulen) {
        WCHAR *u = (WCHAR *)malloc(ulen + sizeof(WCHAR));
        if (u && WinHttpQueryOption(req, WINHTTP_OPTION_URL, u, &ulen)) r->finalUrl = W2U(u);
        free(u);
    }
}

int HttpGet(const char *url, const char *headers, int headOnly, HttpResp *r, char *err, size_t errlen) {
    memset(r, 0, sizeof(*r));
    Req q;
    if (!OpenReq(url, headOnly ? L"HEAD" : L"GET", headers, &q, err, errlen)) return 0;
    FillRespInfo(q.req, r);
    int ok = 1;
    if (!headOnly) {
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(q.req, &avail)) { SetNetError(err, errlen, "受信"); ok = 0; break; }
            if (!avail) break;
            if (r->body.n + avail > MAX_BODY) { snprintf(err, errlen, "応答が大きすぎる"); ok = 0; break; }
            char *tmp = (char *)malloc(avail);
            if (!tmp) { ok = 0; break; }
            DWORD got = 0;
            if (!WinHttpReadData(q.req, tmp, avail, &got)) { free(tmp); SetNetError(err, errlen, "受信"); ok = 0; break; }
            BufAdd(&r->body, tmp, got);
            free(tmp);
        }
        if (!r->body.p) BufAdd(&r->body, "", 0);
    }
    CloseReq(&q);
    if (!ok) HttpRespFree(r);
    return ok;
}

void HttpRespFree(HttpResp *r) {
    BufFree(&r->body);
    free(r->finalUrl);
    r->finalUrl = NULL;
}

int HttpDownload(const char *url, const WCHAR *path, ProgressFn fn, void *ctx, volatile LONG *cancel,
                 char *dispName, size_t dispLen, char *err, size_t errlen) {
    if (dispName && dispLen) dispName[0] = 0;
    Req q;
    if (!OpenReq(url, L"GET", NULL, &q, err, errlen)) return 0;
    HttpResp info = {0};
    FillRespInfo(q.req, &info);
    if (info.status != 200) {
        snprintf(err, errlen, "ダウンロード: HTTP %d", info.status);
        HttpRespFree(&info);
        CloseReq(&q);
        return 0;
    }
    if (dispName && dispLen) snprintf(dispName, dispLen, "%s", info.dispName);

    WCHAR part[MAX_PATH + 8];
    swprintf_s(part, _countof(part), L"%s.part", path);
    HANDLE f = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        snprintf(err, errlen, "保存先に書き込めない（エラー %lu）", GetLastError());
        HttpRespFree(&info);
        CloseReq(&q);
        return 0;
    }
    long long got = 0;
    int ok = 1;
    static char buf[64 * 1024];   // ダウンロードは 1 本ずつなので共有でよい
    for (;;) {
        if (cancel && *cancel) { snprintf(err, errlen, "中止した"); ok = 0; break; }
        DWORD n = 0;
        if (!WinHttpReadData(q.req, buf, sizeof(buf), &n)) { SetNetError(err, errlen, "ダウンロード"); ok = 0; break; }
        if (!n) break;
        DWORD wr = 0;
        if (!WriteFile(f, buf, n, &wr, NULL) || wr != n) {
            snprintf(err, errlen, "書き込みに失敗（エラー %lu）", GetLastError());
            ok = 0;
            break;
        }
        got += n;
        if (fn) fn(ctx, got, info.length);
    }
    CloseHandle(f);
    if (ok && info.length >= 0 && got != info.length) {
        snprintf(err, errlen, "途中で切れた（%lld / %lld バイト）", got, info.length);
        ok = 0;
    }
    if (ok && !MoveFileExW(part, path, MOVEFILE_REPLACE_EXISTING)) {
        snprintf(err, errlen, "保存名に置き換えられない（使用中かもしれない。エラー %lu）", GetLastError());
        ok = 0;
    }
    if (!ok) DeleteFileW(part);
    HttpRespFree(&info);
    CloseReq(&q);
    return ok;
}
