// obtan - アプリの新しい版を見張って、決めたフォルダへダウンロードする（Android の Obtainium の Windows 版）
//
// 取得元は GitHub / GitLab / Codeberg 系のリリースと、任意の Web ページ（ダウンロードのリンクを読む）。
// 確認とダウンロードは 1 本のワーカースレッドが順番に処理し、結果をメッセージで画面に返す。
// 設定と一覧は exe の隣の obtan.ini（UTF-16）に置く。展開やインストールはしない（保存まで）。

#include "obtan.h"
#define COBJMACROS
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <time.h>
#include "resource.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "advapi32.lib")

#define ID_ADD          100
#define ID_EDIT         101
#define ID_DELETE       102
#define ID_CHECKALL     103
#define ID_GETALL       104
#define ID_SETTINGS     105
#define ID_LIST         106
#define ID_STATUS       107
#define ID_CTX_DOWNLOAD 200
#define ID_CTX_CHECK    201
#define ID_CTX_PAGE     202
#define ID_CTX_FOLDER   203
#define ID_CTX_MARK     204
#define ID_CTX_UNMARK   205
#define ID_TRAY_OPEN    300
#define ID_TRAY_EXIT    301

#define TIMER_AUTO      1

#define WM_APP_JOBSTART (WM_APP + 1)   // wParam: id, lParam: 種類
#define WM_APP_JOBDONE  (WM_APP + 2)   // lParam: JobResult*（受け取り側で free）
#define WM_APP_PROGRESS (WM_APP + 3)   // wParam: id, lParam: 百分率（-1 は不明）
#define WM_APP_IDLE     (WM_APP + 4)
#define WM_APP_TRAY     (WM_APP + 5)
#define WM_APP_SHOWME   (WM_APP + 6)   // 二重起動されたときに前面へ
#define WM_APP_PROBED   (WM_APP + 7)   // 登録画面へ: lParam: WCHAR*（free）

enum { JOB_NONE, JOB_CHECK, JOB_DOWNLOAD };

typedef struct {
    int   id;
    WCHAR name[128];
    WCHAR url[1024];
    int   kind;
    WCHAR filter[512];
    WCHAR verRx[256];
    int   prerelease;
    WCHAR dest[MAX_PATH];
    int   autodl;
    int   enabled;           // 一覧のチェック。付いているものだけを「すべて確認」と自動確認の対象にする
    WCHAR installed[128];    // 取得済みの版
    WCHAR latest[128];       // 最後に確認した最新の版
    WCHAR latestName[260];
    WCHAR page[1024];
    WCHAR file[MAX_PATH];    // 最後に保存したファイル
    WCHAR error[512];
    WCHAR notified[128];     // 通知済みの版（同じ版を何度も知らせない）
    long long checked;       // 最後に確認した時刻（UNIX 秒）
    // 画面だけの状態
    int   queued;            // 待ち行列にある仕事の種類
    int   busy;              // 実行中の仕事の種類
    int   progress;
    long long retryAt;       // 回数制限で待っている仕事を再開する時刻（0 なら待っていない）
    int   retryType;
    int   retryAuto;
} App;

typedef struct Job {
    struct Job *next;
    int   type, id, automatic;
    int   kind, prerelease;
    char *url, *filter, *verRx, *token;
    WCHAR dest[MAX_PATH];
} Job;

typedef struct {
    int   type, id, automatic, ok;
    long long deferUntil;    // 回数制限で見送った（この時刻に再開）
    WCHAR err[512];
    WCHAR version[128], fileName[260], page[1024];
    WCHAR path[MAX_PATH];
} JobResult;

static HINSTANCE g_hinst;
static HWND  g_main, g_list, g_status, g_buttons[6];
static HFONT g_font;
static UINT  g_dpi = 96;
static WCHAR g_exeDir[MAX_PATH], g_ini[MAX_PATH];
static App **g_apps;
static int   g_napps;
static int   g_nextId = 1;
static UINT  g_taskbarCreated;
static NOTIFYICONDATAW g_nid;
static HICON g_iconSmall, g_iconBig;
static BOOL  g_exiting;

// 設定
static WCHAR g_defaultDir[MAX_PATH];
static int   g_intervalHours = 6;
static BOOL  g_trayOnClose = TRUE;
static BOOL  g_notify = TRUE;
static WCHAR g_token[256];
static long long g_lastAuto;

// 待ち行列とワーカー
static CRITICAL_SECTION g_qlock;
static Job   *g_qhead, *g_qtail;
static HANDLE g_worker;
static volatile LONG g_cancel;

// 自動確認の結果をまとめて通知するための控え
static WCHAR g_noteUpdates[512], g_noteDownloads[512];

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }
static long long NowUnix(void) { return (long long)time(NULL); }

// ---------------------------------------------------------------- ダークモード（OS の設定に追従。googit と同じ作り）

#define DARK_FACE   RGB(0x2b, 0x2b, 0x2b)
#define DARK_PANEL  RGB(0x1f, 0x1f, 0x1f)
#define DARK_TEXT   RGB(0xe8, 0xe8, 0xe8)
#define DARK_HINT   RGB(0xa0, 0xa0, 0xa0)
#define DARK_ACCENT RGB(0x6c, 0xb6, 0xff)
#define DARK_ERROR  RGB(0xff, 0x8a, 0x80)
#define LIGHT_ACCENT RGB(0x1a, 0x57, 0xb8)
#define LIGHT_ERROR  RGB(0xc4, 0x2b, 0x1c)

typedef enum { PAM_DEFAULT, PAM_ALLOWDARK, PAM_FORCEDARK, PAM_FORCELIGHT } PreferredAppMode;
typedef PreferredAppMode (WINAPI *FnSetPreferredAppMode)(PreferredAppMode);
typedef BOOL (WINAPI *FnAllowDarkModeForWindow)(HWND, BOOL);
typedef void (WINAPI *FnRefreshImmersiveColorPolicyState)(void);
typedef HRESULT (WINAPI *FnDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);

static BOOL   g_dark;
static HBRUSH g_brFace, g_brPanel;
static HWND   g_dialog;   // 開いている登録画面か設定画面
static FnSetPreferredAppMode              pSetPreferredAppMode;
static FnAllowDarkModeForWindow           pAllowDarkModeForWindow;
static FnRefreshImmersiveColorPolicyState pRefreshImmersiveColorPolicyState;
static FnDwmSetWindowAttribute            pDwmSetWindowAttribute;

static COLORREF ThemeFace(void)   { return g_dark ? DARK_FACE  : GetSysColor(COLOR_BTNFACE); }
static COLORREF ThemePanel(void)  { return g_dark ? DARK_PANEL : GetSysColor(COLOR_WINDOW); }
static COLORREF ThemeText(void)   { return g_dark ? DARK_TEXT  : GetSysColor(COLOR_WINDOWTEXT); }
static COLORREF ThemeHint(void)   { return g_dark ? DARK_HINT  : GetSysColor(COLOR_GRAYTEXT); }
static COLORREF ThemeAccent(void) { return g_dark ? DARK_ACCENT : LIGHT_ACCENT; }
static COLORREF ThemeError(void)  { return g_dark ? DARK_ERROR : LIGHT_ERROR; }

static BOOL SystemDarkMode(void) {
    DWORD light = 1, cb = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &cb) != ERROR_SUCCESS) return FALSE;
    return light == 0;
}

static void UpdateThemeBrushes(void) {
    HBRUSH oldFace = g_brFace, oldPanel = g_brPanel;
    g_brFace = CreateSolidBrush(ThemeFace());
    g_brPanel = CreateSolidBrush(ThemePanel());
    if (oldFace) DeleteObject(oldFace);
    if (oldPanel) DeleteObject(oldPanel);
}

// ダークモードの切り替えは uxtheme.dll の非公開 API（序数）でしか行えない。
// 序数 138 の有無で Windows 10 1903 以降かを判別し、無ければライトのまま動かす。
static void InitDarkMode(void) {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (ux && GetProcAddress(ux, MAKEINTRESOURCEA(138))) {
        pSetPreferredAppMode = (FnSetPreferredAppMode)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
        pAllowDarkModeForWindow = (FnAllowDarkModeForWindow)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(133));
        pRefreshImmersiveColorPolicyState = (FnRefreshImmersiveColorPolicyState)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(104));
    }
    HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dwm) pDwmSetWindowAttribute = (FnDwmSetWindowAttribute)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (pSetPreferredAppMode) {
        pSetPreferredAppMode(PAM_ALLOWDARK);
        if (pRefreshImmersiveColorPolicyState) pRefreshImmersiveColorPolicyState();
        g_dark = SystemDarkMode();
    }
    UpdateThemeBrushes();
}

// グループボックスの見出しはテーマが黒で描いてしまうので、ダーク時だけ自前で描く
static LRESULT CALLBACK GroupBoxSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    (void)id; (void)ref;
    if (msg == WM_PAINT && g_dark) {
        WCHAR text[256] = L"";
        GetWindowTextW(hwnd, text, _countof(text));
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HFONT oldFont = (HFONT)SelectObject(dc, (HFONT)SendMessageW(hwnd, WM_GETFONT, 0, 0));
        SIZE sz = {0};
        GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x50, 0x50, 0x50));
        HPEN oldPen = (HPEN)SelectObject(dc, pen);
        HBRUSH oldBrush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, rc.left, rc.top + sz.cy / 2, rc.right, rc.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        int pad = MulDiv(4, GetDpiForWindow(hwnd), 96);
        RECT tr = { rc.left + 2 * pad, rc.top, rc.left + 4 * pad + sz.cx, rc.top + sz.cy };
        FillRect(dc, &tr, g_brFace);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ThemeText());
        tr.left += pad;
        DrawTextW(dc, text, -1, &tr, DT_SINGLELINE | DT_LEFT | DT_TOP | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// 入力欄の縁（WS_EX_CLIENTEDGE の 2px）はダークでもテーマが明るい線で描くので、ダーク時は描き直す。
// 入力中の欄だけアクセントの色にする。
static LRESULT CALLBACK EditSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    (void)id; (void)ref;
    LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_ENABLE) {
        RedrawWindow(hwnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
    } else if (msg == WM_NCPAINT && g_dark) {
        int edge = (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) ? 2 : (GetWindowLongW(hwnd, GWL_STYLE) & WS_BORDER) ? 1 : 0;
        if (edge) {
            HDC dc = GetWindowDC(hwnd);
            RECT rc;
            GetWindowRect(hwnd, &rc);
            OffsetRect(&rc, -rc.left, -rc.top);
            HBRUSH line = CreateSolidBrush(GetFocus() == hwnd ? ThemeAccent() : RGB(0x5a, 0x5a, 0x5a));
            FrameRect(dc, &rc, line);
            if (edge == 2) {
                InflateRect(&rc, -1, -1);
                FrameRect(dc, &rc, g_brPanel);
            }
            DeleteObject(line);
            ReleaseDC(hwnd, dc);
        }
    }
    return r;
}

// チェックボックスの文字はテーマが描くが、Windows 11 23H2 のダークテーマは黒で描いてしまう
// （WM_CTLCOLORBTN の文字色は効かない）。ダーク時は四角だけテーマで描き、文字はこちらで描く。
static BOOL DrawDarkCheckbox(NMCUSTOMDRAW *cd, LRESULT *result) {
    HWND h = cd->hdr.hwndFrom;
    WCHAR cls[16] = L"";
    GetClassNameW(h, cls, _countof(cls));
    LONG type = GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK;
    if (!g_dark || _wcsicmp(cls, L"Button") != 0 || (type != BS_AUTOCHECKBOX && type != BS_CHECKBOX)) return FALSE;
    if (cd->dwDrawStage != CDDS_PREPAINT) return FALSE;

    HDC dc = cd->hdc;
    RECT rc = cd->rc;
    UINT st = cd->uItemState;
    int dpi = (int)GetDpiForWindow(h);
    FillRect(dc, &rc, g_brFace);
    BOOL checked = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int state = (checked ? CBS_CHECKEDNORMAL : CBS_UNCHECKEDNORMAL) +
                ((st & CDIS_DISABLED) ? 3 : (st & CDIS_SELECTED) ? 2 : (st & CDIS_HOT) ? 1 : 0);
    SIZE box = { MulDiv(13, dpi, 96), MulDiv(13, dpi, 96) };
    HTHEME th = OpenThemeData(h, L"Button");
    if (th) GetThemePartSize(th, dc, BP_CHECKBOX, state, NULL, TS_DRAW, &box);
    RECT br = { rc.left, rc.top + (rc.bottom - rc.top - box.cy) / 2, 0, 0 };
    br.right = br.left + box.cx;
    br.bottom = br.top + box.cy;
    if (th) {
        DrawThemeBackground(th, dc, BP_CHECKBOX, state, &br, NULL);
        CloseThemeData(th);
    } else {
        DrawFrameControl(dc, &br, DFC_BUTTON, DFCS_BUTTONCHECK | (checked ? DFCS_CHECKED : 0));
    }

    WCHAR text[256] = L"";
    GetWindowTextW(h, text, _countof(text));
    HFONT old = (HFONT)SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
    RECT tr = rc;
    tr.left = br.right + MulDiv(4, dpi, 96);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, (st & CDIS_DISABLED) ? ThemeHint() : ThemeText());
    UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_LEFT | ((st & CDIS_SHOWKEYBOARDCUES) ? 0 : DT_HIDEPREFIX);
    DrawTextW(dc, text, -1, &tr, fmt);
    if ((st & CDIS_FOCUS) && (st & CDIS_SHOWKEYBOARDCUES)) {
        RECT fr = tr;
        DrawTextW(dc, text, -1, &fr, fmt | DT_CALCRECT);
        int hgt = fr.bottom - fr.top;
        fr.top = tr.top + (tr.bottom - tr.top - hgt) / 2;
        fr.bottom = fr.top + hgt;
        InflateRect(&fr, 1, 0);
        DrawFocusRect(dc, &fr);
    }
    SelectObject(dc, old);
    *result = CDRF_SKIPDEFAULT;
    return TRUE;
}

static void ApplyListColors(void);

static void ApplyThemeToWindow(HWND h) {
    WCHAR cls[32] = L"";
    GetClassNameW(h, cls, _countof(cls));
    if (pAllowDarkModeForWindow) pAllowDarkModeForWindow(h, g_dark);
    if (_wcsicmp(cls, WC_COMBOBOXW) == 0) SetWindowTheme(h, g_dark ? L"DarkMode_CFD" : NULL, NULL);
    else if (_wcsicmp(cls, WC_HEADERW) == 0) SetWindowTheme(h, g_dark ? L"DarkMode_ItemsView" : L"ItemsView", NULL);
    else if (_wcsicmp(cls, WC_LISTVIEWW) == 0) SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    else SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
    if (_wcsicmp(cls, L"Button") == 0 && (GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX)
        SetWindowSubclass(h, GroupBoxSubclass, 1, 0);
    if (_wcsicmp(cls, L"Edit") == 0) {
        SetWindowSubclass(h, EditSubclass, 1, 0);
        RedrawWindow(h, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
    }
}

static void ApplyThemeToTitleBar(HWND h) {
    BOOL v = g_dark;
    if (!pDwmSetWindowAttribute) return;
    if (FAILED(pDwmSetWindowAttribute(h, 20, &v, sizeof(v))))   // DWMWA_USE_IMMERSIVE_DARK_MODE
        pDwmSetWindowAttribute(h, 19, &v, sizeof(v));           // Windows 10 2004 より前の番号
}

static BOOL CALLBACK ApplyThemeChild(HWND h, LPARAM lp) { (void)lp; ApplyThemeToWindow(h); return TRUE; }

static void ApplyThemeTree(HWND root) {
    ApplyThemeToTitleBar(root);
    ApplyThemeToWindow(root);
    EnumChildWindows(root, ApplyThemeChild, 0);
    if (root == g_main) ApplyListColors();
    RedrawWindow(root, NULL, NULL, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
}

static void FollowSystemTheme(LPARAM lp) {
    if (!lp || CompareStringOrdinal((LPCWSTR)lp, -1, L"ImmersiveColorSet", -1, TRUE) != CSTR_EQUAL) return;
    BOOL dark = pSetPreferredAppMode ? SystemDarkMode() : FALSE;
    if (dark == g_dark) return;
    g_dark = dark;
    if (pRefreshImmersiveColorPolicyState) pRefreshImmersiveColorPolicyState();
    UpdateThemeBrushes();
    if (g_main) ApplyThemeTree(g_main);
    if (g_dialog) ApplyThemeTree(g_dialog);
}

// ダイアログ共通の配色。処理したら TRUE
static BOOL DialogColor(UINT msg, WPARAM wp, LPARAM lp, INT_PTR *ret) {
    HDC dc = (HDC)wp;
    switch (msg) {
    case WM_CTLCOLORDLG:
        *ret = (INT_PTR)g_brFace;
        return TRUE;
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        LRESULT r;
        if (nh->code == NM_CUSTOMDRAW && DrawDarkCheckbox((NMCUSTOMDRAW *)lp, &r)) {
            SetWindowLongPtrW(GetParent(nh->hwndFrom), DWLP_MSGRESULT, r);
            *ret = TRUE;
            return TRUE;
        }
        return FALSE;
    }
    case WM_CTLCOLORBTN:
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ThemeText());
        *ret = (INT_PTR)g_brFace;
        return TRUE;
    case WM_CTLCOLORSTATIC: {
        WCHAR cls[16] = L"";
        GetClassNameW((HWND)lp, cls, _countof(cls));
        if (_wcsicmp(cls, L"Edit") == 0) {   // 読み取り専用のエディットは入力欄の色で
            SetTextColor(dc, ThemeText());
            SetBkColor(dc, ThemePanel());
            *ret = (INT_PTR)g_brPanel;
            return TRUE;
        }
        int id = GetDlgCtrlID((HWND)lp);
        SetTextColor(dc, (id >= IDC_HINT1 && id <= IDC_HINT6) || id == IDC_DEST_HINT ? ThemeHint() : ThemeText());
        SetBkMode(dc, TRANSPARENT);
        *ret = (INT_PTR)g_brFace;
        return TRUE;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(dc, ThemeText());
        SetBkColor(dc, ThemePanel());
        *ret = (INT_PTR)g_brPanel;
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------- 小道具

static void WCopy(WCHAR *dst, size_t n, const WCHAR *src) { wcsncpy_s(dst, n, src ? src : L"", _TRUNCATE); }

static void WCopyU8(WCHAR *dst, size_t n, const char *src) {
    WCHAR *w = U2W(src ? src : "");
    WCopy(dst, n, w);
    free(w);
}

// ファイル名に使えない文字を _ にする
static void SanitizeFileName(WCHAR *s) {
    for (WCHAR *p = s; *p; p++)
        if (*p < 32 || wcschr(L"<>:\"/\\|?*", *p)) *p = L'_';
    size_t n = wcslen(s);
    while (n && (s[n - 1] == L'.' || s[n - 1] == L' ')) s[--n] = 0;
    if (!n) wcscpy_s(s, 9, L"download");
}

static void ExpandPath(const WCHAR *in, WCHAR *out, size_t n) {
    if (!ExpandEnvironmentStringsW(in, out, (DWORD)n)) WCopy(out, n, in);
}

static void AppDestDir(const App *a, WCHAR *out, size_t n) {
    if (a->dest[0]) { ExpandPath(a->dest, out, n); return; }
    WCHAR base[MAX_PATH], name[128];
    ExpandPath(g_defaultDir, base, _countof(base));
    WCopy(name, _countof(name), a->name[0] ? a->name : L"app");
    SanitizeFileName(name);
    swprintf_s(out, n, L"%s\\%s", base, name);
}

static App *FindApp(int id) {
    for (int i = 0; i < g_napps; i++) if (g_apps[i]->id == id) return g_apps[i];
    return NULL;
}

static int AppIndex(int id) {
    for (int i = 0; i < g_napps; i++) if (g_apps[i]->id == id) return i;
    return -1;
}

static BOOL AppHasUpdate(const App *a) {
    char *l = W2U(a->latest), *i = W2U(a->installed);
    BOOL r = VersionIsNewer(l, i);
    free(l);
    free(i);
    return r;
}

static void FormatTime(long long t, WCHAR *out, size_t n) {
    out[0] = 0;
    if (t <= 0) return;
    time_t tt = (time_t)t;
    struct tm lt;
    if (localtime_s(&lt, &tt)) return;
    swprintf_s(out, n, L"%d/%02d/%02d %02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
}

// ---------------------------------------------------------------- ini（UTF-16）

static void IniGet(const WCHAR *sec, const WCHAR *key, WCHAR *out, size_t n) {
    GetPrivateProfileStringW(sec, key, L"", out, (DWORD)n, g_ini);
}

// 値は常に "" で囲んで書く（GetPrivateProfileString は前後の空白と囲みの引用符を落とすため）
static void IniPut(const WCHAR *sec, const WCHAR *key, const WCHAR *v) {
    size_t n = wcslen(v) + 3;
    WCHAR *buf = (WCHAR *)malloc(n * sizeof(WCHAR));
    if (!buf) return;
    swprintf_s(buf, n, L"\"%s\"", v);
    WritePrivateProfileStringW(sec, key, buf, g_ini);
    free(buf);
}

static void IniPutInt(const WCHAR *sec, const WCHAR *key, long long v) {
    WCHAR buf[32];
    swprintf_s(buf, _countof(buf), L"%lld", v);
    WritePrivateProfileStringW(sec, key, buf, g_ini);
}

static long long IniGetInt(const WCHAR *sec, const WCHAR *key, long long def) {
    WCHAR buf[32];
    IniGet(sec, key, buf, _countof(buf));
    return buf[0] ? _wtoi64(buf) : def;
}

// 新しく作るときは BOM を書いて UTF-16 にしておく（Private Profile API が Unicode で読み書きする）
static BOOL EnsureIni(const WCHAR *path) {
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) return FALSE;
        CloseHandle(f);
        return TRUE;
    }
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    static const BYTE bom[] = { 0xFF, 0xFE };
    DWORD wr;
    WriteFile(f, bom, sizeof(bom), &wr, NULL);
    CloseHandle(f);
    return TRUE;
}

static void InitPaths(const WCHAR *iniOverride) {
    GetModuleFileNameW(NULL, g_exeDir, MAX_PATH);
    WCHAR *slash = wcsrchr(g_exeDir, L'\\');
    if (slash) *slash = 0;
    if (iniOverride) {
        GetFullPathNameW(iniOverride, MAX_PATH, g_ini, NULL);
        EnsureIni(g_ini);
        return;
    }
    swprintf_s(g_ini, MAX_PATH, L"%s\\obtan.ini", g_exeDir);
    if (EnsureIni(g_ini)) return;
    // exe の場所に書けない（Program Files など）ときは %APPDATA%\obtan に置く
    PWSTR appdata = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &appdata))) {
        swprintf_s(g_ini, MAX_PATH, L"%s\\obtan", appdata);
        CreateDirectoryW(g_ini, NULL);
        wcscat_s(g_ini, MAX_PATH, L"\\obtan.ini");
        CoTaskMemFree(appdata);
        EnsureIni(g_ini);
    }
}

static void SaveApp(const App *a) {
    WCHAR sec[32];
    swprintf_s(sec, _countof(sec), L"app.%d", a->id);
    IniPut(sec, L"name", a->name);
    IniPut(sec, L"url", a->url);
    IniPutInt(sec, L"kind", a->kind);
    IniPut(sec, L"filter", a->filter);
    IniPut(sec, L"verrx", a->verRx);
    IniPutInt(sec, L"prerelease", a->prerelease);
    IniPut(sec, L"dest", a->dest);
    IniPutInt(sec, L"autodl", a->autodl);
    IniPutInt(sec, L"enabled", a->enabled);
    IniPut(sec, L"installed", a->installed);
    IniPut(sec, L"latest", a->latest);
    IniPut(sec, L"latestname", a->latestName);
    IniPut(sec, L"page", a->page);
    IniPut(sec, L"file", a->file);
    IniPut(sec, L"error", a->error);
    IniPut(sec, L"notified", a->notified);
    IniPutInt(sec, L"checked", a->checked);
}

static void LoadApp(App *a, const WCHAR *sec) {
    IniGet(sec, L"name", a->name, _countof(a->name));
    IniGet(sec, L"url", a->url, _countof(a->url));
    a->kind = (int)IniGetInt(sec, L"kind", 0);
    if (a->kind < 0 || a->kind >= SRC_COUNT) a->kind = SRC_AUTO;
    IniGet(sec, L"filter", a->filter, _countof(a->filter));
    IniGet(sec, L"verrx", a->verRx, _countof(a->verRx));
    a->prerelease = (int)IniGetInt(sec, L"prerelease", 0);
    IniGet(sec, L"dest", a->dest, _countof(a->dest));
    a->autodl = (int)IniGetInt(sec, L"autodl", 0);
    a->enabled = (int)IniGetInt(sec, L"enabled", 1);
    IniGet(sec, L"installed", a->installed, _countof(a->installed));
    IniGet(sec, L"latest", a->latest, _countof(a->latest));
    IniGet(sec, L"latestname", a->latestName, _countof(a->latestName));
    IniGet(sec, L"page", a->page, _countof(a->page));
    IniGet(sec, L"file", a->file, _countof(a->file));
    IniGet(sec, L"error", a->error, _countof(a->error));
    IniGet(sec, L"notified", a->notified, _countof(a->notified));
    a->checked = IniGetInt(sec, L"checked", 0);
}

static void SaveSettings(void) {
    IniPut(L"obtan", L"dir", g_defaultDir);
    IniPutInt(L"obtan", L"interval", g_intervalHours);
    IniPutInt(L"obtan", L"trayclose", g_trayOnClose);
    IniPutInt(L"obtan", L"notify", g_notify);
    IniPut(L"obtan", L"token", g_token);
    IniPutInt(L"obtan", L"lastauto", g_lastAuto);
    IniPutInt(L"obtan", L"nextid", g_nextId);
}

static void LoadAll(void) {
    IniGet(L"obtan", L"dir", g_defaultDir, _countof(g_defaultDir));
    if (!g_defaultDir[0]) {
        PWSTR dl = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Downloads, 0, NULL, &dl))) {
            swprintf_s(g_defaultDir, MAX_PATH, L"%s\\obtan", dl);
            CoTaskMemFree(dl);
        }
    }
    g_intervalHours = (int)IniGetInt(L"obtan", L"interval", 6);
    g_trayOnClose = (BOOL)IniGetInt(L"obtan", L"trayclose", 1);
    g_notify = (BOOL)IniGetInt(L"obtan", L"notify", 1);
    IniGet(L"obtan", L"token", g_token, _countof(g_token));
    g_lastAuto = IniGetInt(L"obtan", L"lastauto", 0);
    g_nextId = (int)IniGetInt(L"obtan", L"nextid", 1);

    // [app.N] を書かれた順に読む
    DWORD cap = 65536;
    WCHAR *names = (WCHAR *)malloc(cap * sizeof(WCHAR));
    if (!names) return;
    DWORD n = GetPrivateProfileSectionNamesW(names, cap, g_ini);
    for (WCHAR *p = names; p < names + n && *p; p += wcslen(p) + 1) {
        if (wcsncmp(p, L"app.", 4) != 0) continue;
        App *a = (App *)calloc(1, sizeof(App));
        if (!a) break;
        a->id = _wtoi(p + 4);
        a->progress = -1;
        LoadApp(a, p);
        if (a->id <= 0 || !a->url[0]) { free(a); continue; }
        App **t = (App **)realloc(g_apps, sizeof(App *) * (g_napps + 1));
        if (!t) { free(a); break; }
        g_apps = t;
        g_apps[g_napps++] = a;
        if (a->id >= g_nextId) g_nextId = a->id + 1;
    }
    free(names);
}

// ---------------------------------------------------------------- Windows の起動時に起動

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

static BOOL AutostartEnabled(void) {
    WCHAR v[MAX_PATH * 2];
    DWORD cb = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, L"obtan", RRF_RT_REG_SZ, NULL, v, &cb) == ERROR_SUCCESS;
}

static void SetAutostart(BOOL on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        WCHAR exe[MAX_PATH], cmd[MAX_PATH * 2 + 32];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        swprintf_s(cmd, _countof(cmd), L"\"%s\" -tray", exe);
        RegSetValueExW(k, L"obtan", 0, REG_SZ, (const BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(k, L"obtan");
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------- 調べた結果の文章（登録画面とコマンドラインで共用）

static char *FormatLatest(const Latest *l, const char *nl) {
    Buf b = {0};
    char *s = U8Fmt("取得元: %s%s", SourceName(l->kind), nl);
    BufAddStr(&b, s);
    free(s);
    if (l->title) { s = U8Fmt("名前の候補: %s%s", l->title, nl); BufAddStr(&b, s); free(s); }
    if (l->ok) {
        s = U8Fmt("最新の版: %s%sダウンロードするファイル: %s%s", l->version, nl, l->fileName, nl);
        BufAddStr(&b, s);
        free(s);
        s = U8Fmt("URL: %s%s", l->fileUrl, nl);
        BufAddStr(&b, s);
        free(s);
    } else {
        s = U8Fmt("エラー: %s%s", l->err, nl);
        BufAddStr(&b, s);
        free(s);
        if (l->retryAt) {
            WCHAR t[32];
            FormatTime(l->retryAt, t, _countof(t));
            char *u = W2U(t);
            s = U8Fmt("回数制限: %s に解除される%s", u, nl);
            BufAddStr(&b, s);
            free(s);
            free(u);
        }
    }
    if (l->kind == SRC_WEB && l->allLinks) {
        s = U8Fmt("ページ内のリンク %d 件のうち、条件に合うもの %d 件%s", l->allLinks, l->ncand, nl);
        BufAddStr(&b, s);
        free(s);
    }
    if (l->ncand) {
        s = U8Fmt("%sファイルの一覧（* が選んだもの）:%s", nl, nl);
        BufAddStr(&b, s);
        free(s);
        for (int i = 0; i < l->ncand && i < 80; i++) {
            const Cand *c = &l->cands[i];
            if (l->kind == SRC_WEB)
                s = U8Fmt("%s %s  [版 %s]%s", i == l->chosen ? "*" : " ", c->name, c->version ? c->version : "?", nl);
            else
                s = U8Fmt("%s %s%s", i == l->chosen ? "*" : " ", c->name, nl);
            BufAddStr(&b, s);
            free(s);
        }
        if (l->ncand > 80) { s = U8Fmt("  …ほか %d 件%s", l->ncand - 80, nl); BufAddStr(&b, s); free(s); }
    }
    return BufTake(&b);
}

// ---------------------------------------------------------------- ワーカー（確認とダウンロード）

typedef struct {
    int id;
    int lastPct;
    ULONGLONG lastTick;
} ProgressCtx;

static void OnProgress(void *ctx, long long got, long long total) {
    ProgressCtx *p = (ProgressCtx *)ctx;
    int pct = total > 0 ? (int)(got * 100 / total) : -1;
    ULONGLONG now = GetTickCount64();
    if (pct == p->lastPct || now - p->lastTick < 200) return;
    p->lastPct = pct;
    p->lastTick = now;
    PostMessageW(g_main, WM_APP_PROGRESS, (WPARAM)p->id, (LPARAM)pct);
}

// 回数制限に当たったホストと、問い合わせを再開してよい時刻（ワーカースレッドだけが触る）
static struct { char host[128]; long long until; } g_blocks[16];

static long long BlockedUntil(const char *host) {
    for (int i = 0; i < 16; i++) if (strcmp(g_blocks[i].host, host) == 0) return g_blocks[i].until;
    return 0;
}

static void SetBlocked(const char *host, long long until) {
    int slot = 0;
    for (int i = 0; i < 16; i++) {
        if (strcmp(g_blocks[i].host, host) == 0) { slot = i; break; }
        if (g_blocks[i].until < g_blocks[slot].until) slot = i;   // 空き（0）か一番古いものを使う
    }
    snprintf(g_blocks[slot].host, sizeof(g_blocks[slot].host), "%s", host);
    g_blocks[slot].until = until;
}

static JobResult *RunJob(const Job *j) {
    JobResult *r = (JobResult *)calloc(1, sizeof(JobResult));
    if (!r) return NULL;
    r->type = j->type;
    r->id = j->id;
    r->automatic = j->automatic;
    // 制限に当たっているホストには、解除の時刻まで問い合わせない（残りの登録をまとめてエラーにしないため）
    char host[128];
    SourceApiHost(j->url, (SourceKind)j->kind, host, sizeof(host));
    long long until = BlockedUntil(host);
    if (until > NowUnix()) {
        r->deferUntil = until;
        return r;
    }
    SourceCfg cfg = { j->url, (SourceKind)j->kind, j->filter, j->verRx, j->prerelease, j->token };
    Latest l;
    SourceLatest(&cfg, &l);
    if (l.retryAt) {
        SetBlocked(host, l.retryAt + 5);   // 解除の時刻ちょうどは避けて少し後に
        r->deferUntil = l.retryAt + 5;
        LatestFree(&l);
        return r;
    }
    r->ok = l.ok;
    if (!l.ok) {
        WCopyU8(r->err, _countof(r->err), l.err);
        LatestFree(&l);
        return r;
    }
    WCopyU8(r->version, _countof(r->version), l.version);
    WCopyU8(r->fileName, _countof(r->fileName), l.fileName);
    WCopyU8(r->page, _countof(r->page), l.page);

    if (j->type == JOB_DOWNLOAD) {
        int rc = SHCreateDirectoryExW(NULL, j->dest, NULL);
        if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS) {
            r->ok = 0;
            swprintf_s(r->err, _countof(r->err), L"保存先フォルダを作れない（エラー %d）: %s", rc, j->dest);
            LatestFree(&l);
            return r;
        }
        WCHAR name[260];
        WCopy(name, _countof(name), r->fileName);
        SanitizeFileName(name);
        swprintf_s(r->path, MAX_PATH, L"%s\\%s", j->dest, name);
        ProgressCtx pc = { j->id, -2, 0 };
        char disp[260], err[512];
        if (!HttpDownload(l.fileUrl, r->path, OnProgress, &pc, &g_cancel, disp, sizeof(disp), err, sizeof(err))) {
            r->ok = 0;
            WCopyU8(r->err, _countof(r->err), err);
        } else if (disp[0]) {
            // サーバーが名前を指定してきたら、そちらに付け替える（download.php?id=3 のようなリンク向け）
            WCHAR dname[260], dpath[MAX_PATH];
            WCopyU8(dname, _countof(dname), disp);
            SanitizeFileName(dname);
            if (_wcsicmp(dname, name) != 0) {
                swprintf_s(dpath, MAX_PATH, L"%s\\%s", j->dest, dname);
                if (MoveFileExW(r->path, dpath, MOVEFILE_REPLACE_EXISTING)) WCopy(r->path, MAX_PATH, dpath);
            }
        }
    }
    LatestFree(&l);
    return r;
}

static void FreeJob(Job *j) {
    free(j->url);
    free(j->filter);
    free(j->verRx);
    free(j->token);
    free(j);
}

static DWORD WINAPI WorkerProc(void *arg) {
    (void)arg;
    for (;;) {
        EnterCriticalSection(&g_qlock);
        Job *j = g_qhead;
        if (j) {
            g_qhead = j->next;
            if (!g_qhead) g_qtail = NULL;
        } else {
            CloseHandle(g_worker);
            g_worker = NULL;
        }
        LeaveCriticalSection(&g_qlock);
        if (!j) {
            PostMessageW(g_main, WM_APP_IDLE, 0, 0);
            return 0;
        }
        PostMessageW(g_main, WM_APP_JOBSTART, (WPARAM)j->id, (LPARAM)j->type);
        JobResult *r = g_cancel ? NULL : RunJob(j);
        if (r && !PostMessageW(g_main, WM_APP_JOBDONE, 0, (LPARAM)r)) free(r);
        FreeJob(j);
    }
}

static void UpdateRow(const App *a);
static void UpdateStatusBar(void);

static void Enqueue(App *a, int type, int automatic) {
    // 同じアプリの仕事が待っているなら、ダウンロードの方を優先して 1 つにまとめる
    if (a->queued >= type) return;
    Job *j = (Job *)calloc(1, sizeof(Job));
    if (!j) return;
    j->type = type;
    j->id = a->id;
    j->automatic = automatic;
    j->kind = a->kind;
    j->prerelease = a->prerelease;
    j->url = W2U(a->url);
    j->filter = W2U(a->filter);
    j->verRx = W2U(a->verRx);
    j->token = W2U(g_token);
    AppDestDir(a, j->dest, _countof(j->dest));

    EnterCriticalSection(&g_qlock);
    if (a->queued) {   // 待っている確認をダウンロードに置き換える
        for (Job **pp = &g_qhead; *pp; pp = &(*pp)->next) {
            if ((*pp)->id == a->id) {
                Job *old = *pp;
                j->next = old->next;
                *pp = j;
                if (g_qtail == old) g_qtail = j;
                FreeJob(old);
                j = NULL;
                break;
            }
        }
    }
    if (j) {
        if (g_qtail) g_qtail->next = j;
        else g_qhead = j;
        g_qtail = j;
    }
    if (!g_worker)
        g_worker = CreateThread(NULL, 4 * 1024 * 1024, WorkerProc, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    LeaveCriticalSection(&g_qlock);
    a->queued = type;
    a->retryAt = 0;
    a->retryType = a->retryAuto = 0;
    UpdateRow(a);
    UpdateStatusBar();
}

static void DropQueued(int id) {
    EnterCriticalSection(&g_qlock);
    for (Job **pp = &g_qhead; *pp;) {
        if ((*pp)->id == id) {
            Job *old = *pp;
            *pp = old->next;
            FreeJob(old);
        } else {
            pp = &(*pp)->next;
        }
    }
    g_qtail = NULL;
    for (Job *j = g_qhead; j; j = j->next) g_qtail = j;
    LeaveCriticalSection(&g_qlock);
}

static BOOL WorkerBusy(void) {
    EnterCriticalSection(&g_qlock);
    BOOL b = g_worker != NULL;
    LeaveCriticalSection(&g_qlock);
    return b;
}

static void CheckAll(int automatic) {
    g_lastAuto = NowUnix();
    IniPutInt(L"obtan", L"lastauto", g_lastAuto);
    if (automatic) g_noteUpdates[0] = g_noteDownloads[0] = 0;
    for (int i = 0; i < g_napps; i++)
        if (g_apps[i]->enabled) Enqueue(g_apps[i], JOB_CHECK, automatic);
}

// ---------------------------------------------------------------- 一覧（ListView）

enum { COL_NAME, COL_INSTALLED, COL_LATEST, COL_STATE, COL_CHECKED, COL_COUNT };
static const int g_colWidth[COL_COUNT] = { 170, 110, 110, 230, 120 };
static const WCHAR *g_colName[COL_COUNT] = { L"名前", L"取得済みの版", L"最新の版", L"状態", L"確認した日時" };

static void StateText(const App *a, WCHAR *out, size_t n) {
    if (a->busy == JOB_DOWNLOAD) {
        if (a->progress >= 0) swprintf_s(out, n, L"ダウンロード中 %d%%", a->progress);
        else WCopy(out, n, L"ダウンロード中…");
    } else if (a->busy == JOB_CHECK) WCopy(out, n, L"確認中…");
    else if (a->queued) WCopy(out, n, a->queued == JOB_DOWNLOAD ? L"ダウンロード待ち" : L"確認待ち");
    else if (a->retryAt) {
        WCHAR t[32];
        FormatTime(a->retryAt, t, _countof(t));
        swprintf_s(out, n, L"回数制限のため待機中（%s に再開）", wcschr(t, L' ') ? wcschr(t, L' ') + 1 : t);
    }
    else if (a->error[0]) swprintf_s(out, n, L"エラー: %s", a->error);
    else if (!a->latest[0]) WCopy(out, n, L"未確認");
    else if (AppHasUpdate(a)) WCopy(out, n, a->installed[0] ? L"新しい版があります" : L"未取得");
    else WCopy(out, n, L"最新");
}

static void ApplyListColors(void) {
    if (!g_list) return;
    ListView_SetBkColor(g_list, ThemePanel());
    ListView_SetTextBkColor(g_list, ThemePanel());
    ListView_SetTextColor(g_list, ThemeText());
}

static void UpdateRow(const App *a) {
    int i = AppIndex(a->id);
    if (i < 0 || !g_list) return;
    WCHAR state[600], checked[32];
    StateText(a, state, _countof(state));
    FormatTime(a->checked, checked, _countof(checked));
    ListView_SetItemText(g_list, i, COL_NAME, (LPWSTR)a->name);
    ListView_SetItemText(g_list, i, COL_INSTALLED, (LPWSTR)a->installed);
    ListView_SetItemText(g_list, i, COL_LATEST, (LPWSTR)a->latest);
    ListView_SetItemText(g_list, i, COL_STATE, state);
    ListView_SetItemText(g_list, i, COL_CHECKED, checked);
}

static BOOL g_rebuilding;   // 一覧を作り直している間は、チェックの変化を利用者の操作とみなさない

static void RebuildList(void) {
    g_rebuilding = TRUE;
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    for (int i = 0; i < g_napps; i++) {
        LVITEMW it = { LVIF_TEXT | LVIF_PARAM };
        it.iItem = i;
        it.pszText = g_apps[i]->name;
        it.lParam = g_apps[i]->id;
        ListView_InsertItem(g_list, &it);
        ListView_SetCheckState(g_list, i, g_apps[i]->enabled);
        UpdateRow(g_apps[i]);
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    g_rebuilding = FALSE;
    InvalidateRect(g_list, NULL, TRUE);
}

static void UpdateStatusBar(void) {
    if (!g_status) return;
    WCHAR s[512];
    int updates = 0, errors = 0, waiting = 0, enabled = 0;
    for (int i = 0; i < g_napps; i++) {
        if (AppHasUpdate(g_apps[i])) updates++;
        if (g_apps[i]->retryAt) waiting++;
        if (g_apps[i]->enabled) enabled++;
        if (g_apps[i]->error[0]) errors++;
    }
    if (!g_napps) {
        WCopy(s, _countof(s), L"「追加」で、見張りたいアプリの URL を登録してください。");
    } else {
        WCHAR next[64] = L"自動確認はしない";
        if (g_intervalHours > 0) {
            WCHAR t[32];
            long long at = g_lastAuto + (long long)g_intervalHours * 3600;
            if (at < NowUnix()) at = NowUnix() + 60;
            FormatTime(at, t, _countof(t));
            swprintf_s(next, _countof(next), L"次の自動確認 %s", t);
        }
        WCHAR parts[128] = L"";
        if (updates) swprintf_s(parts, _countof(parts), L"　新しい版あり %d 件", updates);
        if (waiting) {
            WCHAR w[64];
            swprintf_s(w, _countof(w), L"　回数制限で待機 %d 件", waiting);
            wcscat_s(parts, _countof(parts), w);
        }
        if (errors) {
            WCHAR e[64];
            swprintf_s(e, _countof(e), L"　エラー %d 件", errors);
            wcscat_s(parts, _countof(parts), e);
        }
        WCHAR count[64];
        if (enabled == g_napps) swprintf_s(count, _countof(count), L"%d 件", g_napps);
        else swprintf_s(count, _countof(count), L"%d 件（確認するもの %d 件）", g_napps, enabled);
        swprintf_s(s, _countof(s), L"%s%s%s　｜　%s", count, parts, WorkerBusy() ? L"　処理中…" : L"", next);
    }
    SetWindowTextW(g_status, s);
}

// 選ばれている行の id を集める（無ければフォーカス行）
static int SelectedIds(int *ids, int max) {
    int n = 0;
    for (int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED); i >= 0 && n < max;
         i = ListView_GetNextItem(g_list, i, LVNI_SELECTED))
        if (i < g_napps) ids[n++] = g_apps[i]->id;
    return n;
}

static App *FocusedApp(void) {
    int i = ListView_GetNextItem(g_list, -1, LVNI_FOCUSED | LVNI_SELECTED);
    if (i < 0) i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    return i >= 0 && i < g_napps ? g_apps[i] : NULL;
}

// ---------------------------------------------------------------- 通知とトレイ

static void TrayAdd(void) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_iconSmall;
    WCopy(g_nid.szTip, _countof(g_nid.szTip), L"obtan");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static void Notify(const WCHAR *title, const WCHAR *text) {
    if (!g_notify) return;
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    WCopy(n.szInfoTitle, _countof(n.szInfoTitle), title);
    WCopy(n.szInfo, _countof(n.szInfo), text);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void AppendNote(WCHAR *buf, size_t n, const WCHAR *item) {
    if (buf[0]) wcscat_s(buf, n, L"、");
    if (wcslen(buf) + wcslen(item) + 2 < n) wcscat_s(buf, n, item);
}

static void ShowMain(void) {
    ShowWindow(g_main, IsIconic(g_main) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_main);
}

static void ExitApp(void) {
    g_exiting = TRUE;
    DestroyWindow(g_main);
}

// ---------------------------------------------------------------- フォルダ選択

static BOOL PickFolder(HWND owner, WCHAR *path, size_t n) {
    IFileOpenDialog *d = NULL;
    BOOL ok = FALSE;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return FALSE;
    DWORD opt = 0;
    IFileOpenDialog_GetOptions(d, &opt);
    IFileOpenDialog_SetOptions(d, opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    WCHAR cur[MAX_PATH];
    ExpandPath(path, cur, _countof(cur));
    IShellItem *si = NULL;
    if (cur[0] && SUCCEEDED(SHCreateItemFromParsingName(cur, NULL, &IID_IShellItem, (void **)&si))) {
        IFileOpenDialog_SetFolder(d, si);
        IShellItem_Release(si);
    }
    if (SUCCEEDED(IFileOpenDialog_Show(d, owner)) && SUCCEEDED(IFileOpenDialog_GetResult(d, &si))) {
        PWSTR p = NULL;
        if (SUCCEEDED(IShellItem_GetDisplayName(si, SIGDN_FILESYSPATH, &p))) {
            WCopy(path, n, p);
            CoTaskMemFree(p);
            ok = TRUE;
        }
        IShellItem_Release(si);
    }
    IFileOpenDialog_Release(d);
    return ok;
}

// ---------------------------------------------------------------- 登録画面

typedef struct {
    HWND  dlg;
    SourceCfg cfg;
    char *url, *filter, *verRx, *token;
} ProbeJob;

static DWORD WINAPI ProbeProc(void *arg) {
    ProbeJob *p = (ProbeJob *)arg;
    Latest l;
    SourceLatest(&p->cfg, &l);
    char *text = FormatLatest(&l, "\r\n");
    // 1 行目に名前の候補を入れて渡す（登録画面で名前が空なら使う）
    char *msg = U8Fmt("%s\n%s", l.title ? l.title : "", text);
    WCHAR *w = U2W(msg);
    if (w && !PostMessageW(p->dlg, WM_APP_PROBED, 0, (LPARAM)w)) free(w);
    free(msg);
    free(text);
    LatestFree(&l);
    free(p->url);
    free(p->filter);
    free(p->verRx);
    free(p->token);
    free(p);
    return 0;
}

static void GetDlgText(HWND dlg, int id, WCHAR *out, size_t n) {
    GetDlgItemTextW(dlg, id, out, (int)n);
    // 前後の空白を落とす
    WCHAR *s = out;
    while (*s == L' ' || *s == L'\t') s++;
    if (s != out) memmove(out, s, (wcslen(s) + 1) * sizeof(WCHAR));
    size_t len = wcslen(out);
    while (len && (out[len - 1] == L' ' || out[len - 1] == L'\t')) out[--len] = 0;
}

static void UpdateDestHint(HWND dlg) {
    App tmp = {0};
    GetDlgText(dlg, IDC_NAME, tmp.name, _countof(tmp.name));
    GetDlgText(dlg, IDC_DEST, tmp.dest, _countof(tmp.dest));
    WCHAR dir[MAX_PATH], text[MAX_PATH + 32];
    AppDestDir(&tmp, dir, _countof(dir));
    swprintf_s(text, _countof(text), L"→ %s", dir);
    SetDlgItemTextW(dlg, IDC_DEST_HINT, text);
}

static BOOL CheckRegex(HWND dlg, int id, const WCHAR *label) {
    WCHAR w[512];
    GetDlgText(dlg, id, w, _countof(w));
    if (!w[0]) return TRUE;
    char *u = W2U(w), err[200];
    Rx *rx = RxCompile(u, 1, err, sizeof(err));
    free(u);
    if (rx) { RxFree(rx); return TRUE; }
    WCHAR *we = U2W(err), msg[400];
    swprintf_s(msg, _countof(msg), L"%s の正規表現が正しくありません。\n%s", label, we ? we : L"");
    free(we);
    MessageBoxW(dlg, msg, APP_NAME, MB_ICONWARNING);
    SetFocus(GetDlgItem(dlg, id));
    return FALSE;
}

static INT_PTR CALLBACK AppDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    App *a = (App *)GetWindowLongPtrW(dlg, DWLP_USER);
    INT_PTR ret;
    if (DialogColor(msg, wp, lp, &ret)) return ret;
    switch (msg) {
    case WM_INITDIALOG: {
        a = (App *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)a);
        g_dialog = dlg;
        SetWindowTextW(dlg, a->id ? L"アプリの編集" : L"アプリの追加");
        SetDlgItemTextW(dlg, IDC_URL, a->url);
        SetDlgItemTextW(dlg, IDC_NAME, a->name);
        HWND cb = GetDlgItem(dlg, IDC_KIND);
        for (int k = 0; k < SRC_COUNT; k++) {
            WCHAR *w = U2W(SourceName((SourceKind)k));
            SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)w);
            free(w);
        }
        SendMessageW(cb, CB_SETCURSEL, a->kind, 0);
        SetDlgItemTextW(dlg, IDC_FILTER, a->filter);
        SetDlgItemTextW(dlg, IDC_VERRX, a->verRx);
        CheckDlgButton(dlg, IDC_PRE, a->prerelease ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(dlg, IDC_DEST, a->dest);
        CheckDlgButton(dlg, IDC_AUTODL, a->autodl ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(dlg, IDC_INSTALLED, a->installed);
        SetDlgItemTextW(dlg, IDC_PROBE_OUT, L"「調べる」を押すと、今の設定で最新の版とダウンロードするファイルを確かめます。");
        UpdateDestHint(dlg);
        ApplyThemeTree(dlg);
        SetFocus(GetDlgItem(dlg, a->url[0] ? IDC_NAME : IDC_URL));
        return FALSE;
    }
    case WM_SETTINGCHANGE:
        FollowSystemTheme(lp);
        return TRUE;
    case WM_DESTROY:
        g_dialog = NULL;
        return FALSE;
    case WM_APP_PROBED: {
        WCHAR *w = (WCHAR *)lp;
        WCHAR *nl = wcschr(w, L'\n');
        if (nl) {
            *nl = 0;
            WCHAR name[128];
            GetDlgText(dlg, IDC_NAME, name, _countof(name));
            if (!name[0] && w[0]) { SetDlgItemTextW(dlg, IDC_NAME, w); UpdateDestHint(dlg); }
            SetDlgItemTextW(dlg, IDC_PROBE_OUT, nl + 1);
        }
        free(w);
        EnableWindow(GetDlgItem(dlg, IDC_PROBE_BTN), TRUE);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_NAME:
        case IDC_DEST:
            if (HIWORD(wp) == EN_CHANGE) UpdateDestHint(dlg);
            return TRUE;
        case IDC_DEST_BROWSE: {
            WCHAR path[MAX_PATH];
            GetDlgText(dlg, IDC_DEST, path, _countof(path));
            if (!path[0]) WCopy(path, _countof(path), g_defaultDir);
            if (PickFolder(dlg, path, _countof(path))) SetDlgItemTextW(dlg, IDC_DEST, path);
            return TRUE;
        }
        case IDC_PROBE_BTN: {
            WCHAR url[1024], filter[512], verRx[256];
            GetDlgText(dlg, IDC_URL, url, _countof(url));
            if (!url[0]) { SetFocus(GetDlgItem(dlg, IDC_URL)); return TRUE; }
            if (!CheckRegex(dlg, IDC_FILTER, L"ファイル名の条件") || !CheckRegex(dlg, IDC_VERRX, L"版の取り出し")) return TRUE;
            GetDlgText(dlg, IDC_FILTER, filter, _countof(filter));
            GetDlgText(dlg, IDC_VERRX, verRx, _countof(verRx));
            ProbeJob *p = (ProbeJob *)calloc(1, sizeof(ProbeJob));
            if (!p) return TRUE;
            p->dlg = dlg;
            p->url = W2U(url);
            p->filter = W2U(filter);
            p->verRx = W2U(verRx);
            p->token = W2U(g_token);
            p->cfg.url = p->url;
            p->cfg.filter = p->filter;
            p->cfg.verRx = p->verRx;
            p->cfg.token = p->token;
            p->cfg.kind = (SourceKind)SendDlgItemMessageW(dlg, IDC_KIND, CB_GETCURSEL, 0, 0);
            p->cfg.prerelease = IsDlgButtonChecked(dlg, IDC_PRE) == BST_CHECKED;
            SetDlgItemTextW(dlg, IDC_PROBE_OUT, L"調べています…");
            EnableWindow(GetDlgItem(dlg, IDC_PROBE_BTN), FALSE);
            HANDLE th = CreateThread(NULL, 4 * 1024 * 1024, ProbeProc, p, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
            if (th) CloseHandle(th);
            else { free(p->url); free(p->filter); free(p->verRx); free(p->token); free(p); }
            return TRUE;
        }
        case IDOK: {
            App n = *a;
            GetDlgText(dlg, IDC_URL, n.url, _countof(n.url));
            if (_wcsnicmp(n.url, L"http://", 7) != 0 && _wcsnicmp(n.url, L"https://", 8) != 0) {
                MessageBoxW(dlg, L"URL は http:// か https:// で始めてください。", APP_NAME, MB_ICONWARNING);
                SetFocus(GetDlgItem(dlg, IDC_URL));
                return TRUE;
            }
            if (!CheckRegex(dlg, IDC_FILTER, L"ファイル名の条件") || !CheckRegex(dlg, IDC_VERRX, L"版の取り出し")) return TRUE;
            GetDlgText(dlg, IDC_NAME, n.name, _countof(n.name));
            if (!n.name[0]) {
                // 名前が空なら URL のホスト名とパスの最後から作る
                char *u = W2U(n.url);
                const char *p = strstr(u, "://");
                p = p ? p + 3 : u;
                size_t len = strlen(p);
                while (len && p[len - 1] == '/') len--;
                const char *last = p;
                for (size_t i = 0; i < len; i++) if (p[i] == '/') last = p + i + 1;
                char *nm = U8NDup(last, (size_t)(p + len - last));
                WCopyU8(n.name, _countof(n.name), nm);
                free(nm);
                free(u);
            }
            n.kind = (int)SendDlgItemMessageW(dlg, IDC_KIND, CB_GETCURSEL, 0, 0);
            if (n.kind < 0) n.kind = 0;
            GetDlgText(dlg, IDC_FILTER, n.filter, _countof(n.filter));
            GetDlgText(dlg, IDC_VERRX, n.verRx, _countof(n.verRx));
            n.prerelease = IsDlgButtonChecked(dlg, IDC_PRE) == BST_CHECKED;
            GetDlgText(dlg, IDC_DEST, n.dest, _countof(n.dest));
            n.autodl = IsDlgButtonChecked(dlg, IDC_AUTODL) == BST_CHECKED;
            GetDlgText(dlg, IDC_INSTALLED, n.installed, _countof(n.installed));
            // 判定に関わる設定が変わったら、前の確認結果は捨てる
            if (wcscmp(n.url, a->url) || n.kind != a->kind || wcscmp(n.filter, a->filter) ||
                wcscmp(n.verRx, a->verRx) || n.prerelease != a->prerelease) {
                n.latest[0] = n.latestName[0] = n.error[0] = n.page[0] = 0;
                n.checked = 0;
            }
            *a = n;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void EditApp(App *existing) {
    App work = {0};
    if (existing) work = *existing;
    else { work.progress = -1; work.enabled = 1; }
    if (DialogBoxParamW(g_hinst, MAKEINTRESOURCEW(IDD_APP), g_main, AppDlgProc, (LPARAM)&work) != IDOK) return;
    App *a = existing;
    if (!a) {
        a = (App *)calloc(1, sizeof(App));
        App **t = a ? (App **)realloc(g_apps, sizeof(App *) * (g_napps + 1)) : NULL;
        if (!t) { free(a); return; }
        g_apps = t;
        work.id = g_nextId++;
        IniPutInt(L"obtan", L"nextid", g_nextId);
        g_apps[g_napps++] = a;
    }
    // 画面だけの状態（実行中かどうか）は今のものを残す
    work.queued = a->queued;
    work.busy = a->busy;
    work.progress = a->progress;
    *a = work;
    SaveApp(a);
    RebuildList();
    int idx = AppIndex(a->id);
    ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED);
    ListView_SetItemState(g_list, idx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g_list, idx, FALSE);
    if (!a->latest[0]) Enqueue(a, JOB_CHECK, 0);
    UpdateStatusBar();
}

static void DeleteSelected(void) {
    int ids[512];
    int n = SelectedIds(ids, 512);
    if (!n) return;
    WCHAR msg[400];
    if (n == 1) swprintf_s(msg, _countof(msg), L"「%s」を一覧から消しますか？\n（ダウンロードしたファイルは消しません）", FindApp(ids[0])->name);
    else swprintf_s(msg, _countof(msg), L"%d 件を一覧から消しますか？\n（ダウンロードしたファイルは消しません）", n);
    if (MessageBoxW(g_main, msg, APP_NAME, MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK) return;
    for (int k = 0; k < n; k++) {
        int i = AppIndex(ids[k]);
        if (i < 0) continue;
        DropQueued(ids[k]);
        WCHAR sec[32];
        swprintf_s(sec, _countof(sec), L"app.%d", ids[k]);
        WritePrivateProfileStringW(sec, NULL, NULL, g_ini);
        free(g_apps[i]);
        memmove(g_apps + i, g_apps + i + 1, sizeof(App *) * (g_napps - i - 1));
        g_napps--;
    }
    RebuildList();
    UpdateStatusBar();
}

// ---------------------------------------------------------------- 設定画面

static INT_PTR CALLBACK SettingsDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    INT_PTR ret;
    if (DialogColor(msg, wp, lp, &ret)) return ret;
    switch (msg) {
    case WM_INITDIALOG:
        g_dialog = dlg;
        SetDlgItemTextW(dlg, IDC_DEFDIR, g_defaultDir);
        SetDlgItemInt(dlg, IDC_INTERVAL, (UINT)g_intervalHours, FALSE);
        CheckDlgButton(dlg, IDC_AUTOSTART, AutostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_TRAYCLOSE, g_trayOnClose ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_NOTIFY, g_notify ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(dlg, IDC_TOKEN, g_token);
        ApplyThemeTree(dlg);
        return TRUE;
    case WM_SETTINGCHANGE:
        FollowSystemTheme(lp);
        return TRUE;
    case WM_DESTROY:
        g_dialog = NULL;
        return FALSE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_DEFDIR_BROWSE: {
            WCHAR path[MAX_PATH];
            GetDlgText(dlg, IDC_DEFDIR, path, _countof(path));
            if (PickFolder(dlg, path, _countof(path))) SetDlgItemTextW(dlg, IDC_DEFDIR, path);
            return TRUE;
        }
        case IDOK: {
            WCHAR dir[MAX_PATH];
            GetDlgText(dlg, IDC_DEFDIR, dir, _countof(dir));
            if (!dir[0]) {
                MessageBoxW(dlg, L"既定の保存先を指定してください。", APP_NAME, MB_ICONWARNING);
                return TRUE;
            }
            WCopy(g_defaultDir, _countof(g_defaultDir), dir);
            g_intervalHours = (int)GetDlgItemInt(dlg, IDC_INTERVAL, NULL, FALSE);
            g_trayOnClose = IsDlgButtonChecked(dlg, IDC_TRAYCLOSE) == BST_CHECKED;
            g_notify = IsDlgButtonChecked(dlg, IDC_NOTIFY) == BST_CHECKED;
            GetDlgText(dlg, IDC_TOKEN, g_token, _countof(g_token));
            BOOL autostart = IsDlgButtonChecked(dlg, IDC_AUTOSTART) == BST_CHECKED;
            if (autostart != AutostartEnabled()) SetAutostart(autostart);
            SaveSettings();
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------- 主画面

static void RecreateFont(void) {
    if (g_font) DeleteObject(g_font);
    g_font = CreateFontW(-MulDiv(9, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
}

static void LayoutMain(void) {
    RECT rc;
    GetClientRect(g_main, &rc);
    int m = S(8), bh = S(28), x = m;
    HDC dc = GetDC(g_main);
    HFONT old = (HFONT)SelectObject(dc, g_font);
    for (int i = 0; i < 6; i++) {
        WCHAR t[64];
        GetWindowTextW(g_buttons[i], t, _countof(t));
        SIZE sz;
        GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
        int w = sz.cx + S(24);
        if (w < S(72)) w = S(72);
        if (i == 3 || i == 5) x += S(10);   // 「すべて確認」と「設定」の前は少し空ける
        MoveWindow(g_buttons[i], x, m, w, bh, TRUE);
        x += w + S(4);
    }
    SelectObject(dc, old);
    ReleaseDC(g_main, dc);
    int sh = S(24);
    int top = m + bh + m;
    MoveWindow(g_list, m, top, rc.right - 2 * m, rc.bottom - top - sh - S(4), TRUE);
    MoveWindow(g_status, m, rc.bottom - sh, rc.right - 2 * m, sh - S(4), TRUE);
}

static void SetColumnWidths(UINT oldDpi) {
    for (int c = 0; c < COL_COUNT; c++) {
        int w = oldDpi ? MulDiv(ListView_GetColumnWidth(g_list, c), (int)g_dpi, (int)oldDpi) : S(g_colWidth[c]);
        ListView_SetColumnWidth(g_list, c, w);
    }
}

static void ApplyFonts(void) {
    for (int i = 0; i < 6; i++) SendMessageW(g_buttons[i], WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, TRUE);
}

// 見出しの文字色はテーマ任せだとダークで黒くなるので、一覧のサブクラスで指定する
static LRESULT CALLBACK ListSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    (void)id; (void)ref;
    if (msg == WM_NOTIFY && ((NMHDR *)lp)->code == NM_CUSTOMDRAW && ((NMHDR *)lp)->hwndFrom == ListView_GetHeader(hwnd)) {
        NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)lp;
        if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
            SetTextColor(cd->hdc, ThemeText());
            return CDRF_DODEFAULT;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void ShowContextMenu(int x, int y) {
    App *a = FocusedApp();
    if (!a) return;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_CTX_DOWNLOAD, L"ダウンロード");
    AppendMenuW(m, MF_STRING, ID_CTX_CHECK, L"確認");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EDIT, L"編集...");
    AppendMenuW(m, MF_STRING, ID_CTX_PAGE, L"ページを開く");
    AppendMenuW(m, MF_STRING, ID_CTX_FOLDER, L"保存先を開く");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (a->latest[0] ? 0 : MF_GRAYED), ID_CTX_MARK, L"取得済みにする（今の最新の版で）");
    AppendMenuW(m, MF_STRING | (a->installed[0] ? 0 : MF_GRAYED), ID_CTX_UNMARK, L"未取得に戻す");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_DELETE, L"削除");
    SetMenuDefaultItem(m, ID_EDIT, FALSE);
    if (x == -1 && y == -1) {   // キーボードから開いたときは選択行の位置に出す
        RECT rc;
        int i = AppIndex(a->id);
        ListView_GetItemRect(g_list, i, &rc, LVIR_LABEL);
        POINT pt = { rc.left, rc.bottom };
        ClientToScreen(g_list, &pt);
        x = pt.x;
        y = pt.y;
    }
    TrackPopupMenu(m, TPM_RIGHTBUTTON, x, y, 0, g_main, NULL);
    DestroyMenu(m);
}

static void OpenFolderOf(const App *a) {
    if (a->file[0] && GetFileAttributesW(a->file) != INVALID_FILE_ATTRIBUTES) {
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(a->file);
        if (pidl) {
            SHOpenFolderAndSelectItems(pidl, 0, NULL, 0);
            ILFree(pidl);
            return;
        }
    }
    WCHAR dir[MAX_PATH];
    AppDestDir(a, dir, _countof(dir));
    SHCreateDirectoryExW(NULL, dir, NULL);
    ShellExecuteW(g_main, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
}

static void OnCommand(int id) {
    int ids[512], n;
    switch (id) {
    case ID_ADD: EditApp(NULL); break;
    case ID_EDIT: { App *a = FocusedApp(); if (a) EditApp(a); break; }
    case ID_DELETE: DeleteSelected(); break;
    case ID_CHECKALL: CheckAll(0); break;
    case ID_GETALL: {
        int count = 0;
        for (int i = 0; i < g_napps; i++)
            if (g_apps[i]->enabled && AppHasUpdate(g_apps[i]) && !g_apps[i]->error[0]) { Enqueue(g_apps[i], JOB_DOWNLOAD, 0); count++; }
        if (!count)
            MessageBoxW(g_main, L"ダウンロードを待っている新しい版はありません。\n（先に「すべて確認」で確かめてください）", APP_NAME, MB_ICONINFORMATION);
        break;
    }
    case ID_SETTINGS:
        DialogBoxParamW(g_hinst, MAKEINTRESOURCEW(IDD_SETTINGS), g_main, SettingsDlgProc, 0);
        UpdateStatusBar();
        break;
    case ID_CTX_DOWNLOAD:
        n = SelectedIds(ids, 512);
        for (int k = 0; k < n; k++) Enqueue(FindApp(ids[k]), JOB_DOWNLOAD, 0);
        break;
    case ID_CTX_CHECK:
        n = SelectedIds(ids, 512);
        for (int k = 0; k < n; k++) Enqueue(FindApp(ids[k]), JOB_CHECK, 0);
        break;
    case ID_CTX_PAGE: {
        App *a = FocusedApp();
        if (a) ShellExecuteW(g_main, L"open", a->page[0] ? a->page : a->url, NULL, NULL, SW_SHOWNORMAL);
        break;
    }
    case ID_CTX_FOLDER: { App *a = FocusedApp(); if (a) OpenFolderOf(a); break; }
    case ID_CTX_MARK:
    case ID_CTX_UNMARK:
        n = SelectedIds(ids, 512);
        for (int k = 0; k < n; k++) {
            App *a = FindApp(ids[k]);
            if (id == ID_CTX_MARK && !a->latest[0]) continue;
            WCopy(a->installed, _countof(a->installed), id == ID_CTX_MARK ? a->latest : L"");
            SaveApp(a);
            UpdateRow(a);
        }
        UpdateStatusBar();
        break;
    case ID_TRAY_OPEN: ShowMain(); break;
    case ID_TRAY_EXIT: ExitApp(); break;
    }
}

static void OnJobDone(JobResult *r) {
    App *a = FindApp(r->id);
    if (!a) { free(r); return; }
    a->busy = JOB_NONE;
    a->progress = -1;
    if (r->deferUntil) {
        // 見送り。前の結果はそのまま残し、時刻が来たらタイマーで同じ仕事をやり直す
        a->retryAt = r->deferUntil;
        if (r->type > a->retryType) a->retryType = r->type;
        a->retryAuto |= r->automatic;
        UpdateRow(a);
        UpdateStatusBar();
        free(r);
        return;
    }
    a->checked = NowUnix();
    if (r->ok) {
        WCopy(a->latest, _countof(a->latest), r->version);
        WCopy(a->latestName, _countof(a->latestName), r->fileName);
        WCopy(a->page, _countof(a->page), r->page);
        a->error[0] = 0;
    } else {
        WCopy(a->error, _countof(a->error), r->err);
    }
    if (r->type == JOB_DOWNLOAD && r->ok) {
        WCopy(a->installed, _countof(a->installed), r->version);
        WCopy(a->file, _countof(a->file), r->path);
        if (r->automatic) {
            WCHAR item[200];
            swprintf_s(item, _countof(item), L"%s %s", a->name, a->installed);
            AppendNote(g_noteDownloads, _countof(g_noteDownloads), item);
        }
    }
    if (r->type == JOB_CHECK && r->ok && AppHasUpdate(a)) {
        if (a->autodl && r->automatic) {
            Enqueue(a, JOB_DOWNLOAD, 1);
        } else if (r->automatic && wcscmp(a->notified, a->latest) != 0) {
            WCHAR item[200];
            swprintf_s(item, _countof(item), L"%s %s", a->name, a->latest);
            AppendNote(g_noteUpdates, _countof(g_noteUpdates), item);
            WCopy(a->notified, _countof(a->notified), a->latest);
        }
    }
    SaveApp(a);
    UpdateRow(a);
    UpdateStatusBar();
    free(r);
}

static void OnIdle(void) {
    if (g_noteDownloads[0]) {
        WCHAR t[600];
        swprintf_s(t, _countof(t), L"%s", g_noteDownloads);
        Notify(L"ダウンロードしました", t);
    } else if (g_noteUpdates[0]) {
        Notify(L"新しい版があります", g_noteUpdates);
    }
    g_noteDownloads[0] = g_noteUpdates[0] = 0;
    UpdateStatusBar();
}

static void SetEnabled(App *a, int on) {
    if (a->enabled == on) return;
    a->enabled = on;
    if (!on) a->retryAt = 0;   // 回数制限で待っていた分も取りやめる
    WCHAR sec[32];
    swprintf_s(sec, _countof(sec), L"app.%d", a->id);
    IniPutInt(sec, L"enabled", on);
    UpdateRow(a);
    int i = AppIndex(a->id);
    ListView_RedrawItems(g_list, i, i);
}

static LRESULT OnListNotify(NMHDR *h) {
    switch (h->code) {
    case LVN_ITEMCHANGED: {
        NMLISTVIEW *nm = (NMLISTVIEW *)h;
        if (g_rebuilding || !(nm->uChanged & LVIF_STATE) || nm->iItem < 0 || nm->iItem >= g_napps) return 0;
        if (!((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK) || !(nm->uOldState & LVIS_STATEIMAGEMASK)) return 0;
        int on = ((nm->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
        SetEnabled(g_apps[nm->iItem], on);
        // 選んでいる行の中で切り替えたなら、選んでいる行をまとめて揃える
        if (ListView_GetItemState(g_list, nm->iItem, LVIS_SELECTED) & LVIS_SELECTED) {
            g_rebuilding = TRUE;
            for (int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(g_list, i, LVNI_SELECTED)) {
                if (i == nm->iItem || i >= g_napps) continue;
                ListView_SetCheckState(g_list, i, on);
                SetEnabled(g_apps[i], on);
            }
            g_rebuilding = FALSE;
        }
        UpdateStatusBar();
        return 0;
    }
    case NM_DBLCLK:
        if (((NMITEMACTIVATE *)h)->iItem >= 0) OnCommand(ID_EDIT);
        return 0;
    case NM_RCLICK: {
        POINT pt;
        GetCursorPos(&pt);
        if (((NMITEMACTIVATE *)h)->iItem >= 0) ShowContextMenu(pt.x, pt.y);
        return 0;
    }
    case LVN_KEYDOWN: {
        WORD vk = ((NMLVKEYDOWN *)h)->wVKey;
        if (vk == VK_DELETE) OnCommand(ID_DELETE);
        else if (vk == VK_RETURN) OnCommand(ID_EDIT);
        else if (vk == VK_F5) OnCommand(ID_CHECKALL);
        else if (vk == 'A' && GetKeyState(VK_CONTROL) < 0) ListView_SetItemState(g_list, -1, LVIS_SELECTED, LVIS_SELECTED);
        return 0;
    }
    case NM_CUSTOMDRAW: {
        NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)h;
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            int i = (int)cd->nmcd.dwItemSpec;
            if (i >= 0 && i < g_napps) {
                const App *a = g_apps[i];
                if (!a->enabled && !a->busy && !a->queued) cd->clrText = ThemeHint();
                else if (a->error[0] && !a->busy && !a->queued) cd->clrText = ThemeError();
                else if (AppHasUpdate(a) || a->busy == JOB_DOWNLOAD) cd->clrText = ThemeAccent();
                else cd->clrText = ThemeText();
            }
            return CDRF_DODEFAULT;
        }
        return CDRF_DODEFAULT;
    }
    }
    return 0;
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && msg) { TrayAdd(); return 0; }
    switch (msg) {
    case WM_CREATE: {
        g_main = hwnd;
        g_dpi = GetDpiForWindow(hwnd);
        RecreateFont();
        static const WCHAR *labels[6] = { L"追加...", L"編集...", L"削除", L"すべて確認", L"新しい版をすべて取得", L"設定..." };
        static const int ids[6] = { ID_ADD, ID_EDIT, ID_DELETE, ID_CHECKALL, ID_GETALL, ID_SETTINGS };
        for (int i = 0; i < 6; i++)
            g_buttons[i] = CreateWindowExW(0, L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                           0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ids[i], g_hinst, NULL);
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                                 0, 0, 10, 10, hwnd, (HMENU)ID_LIST, g_hinst, NULL);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES);
        for (int c = 0; c < COL_COUNT; c++) {
            LVCOLUMNW col = { LVCF_TEXT | LVCF_WIDTH };
            col.pszText = (LPWSTR)g_colName[c];
            col.cx = S(g_colWidth[c]);
            ListView_InsertColumn(g_list, c, &col);
        }
        SetWindowSubclass(g_list, ListSubclass, 1, 0);
        g_status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS | SS_CENTERIMAGE,
                                   0, 0, 10, 10, hwnd, (HMENU)ID_STATUS, g_hinst, NULL);
        ApplyFonts();
        ApplyThemeTree(hwnd);
        RebuildList();
        LayoutMain();
        UpdateStatusBar();
        TrayAdd();
        SetTimer(hwnd, TIMER_AUTO, 60 * 1000, NULL);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_brFace);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, ThemeText());
        return (LRESULT)g_brFace;
    case WM_SETTINGCHANGE:
        FollowSystemTheme(lp);
        break;
    case WM_SIZE:
        if (g_list) LayoutMain();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(560);
        mm->ptMinTrackSize.y = S(240);
        return 0;
    }
    case WM_DPICHANGED: {
        UINT old = g_dpi;
        g_dpi = HIWORD(wp);
        RecreateFont();
        ApplyFonts();
        SetColumnWidths(old);
        RECT *r = (RECT *)lp;
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutMain();
        return 0;
    }
    case WM_COMMAND:
        OnCommand(LOWORD(wp));
        return 0;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->idFrom == ID_LIST) return OnListNotify((NMHDR *)lp);
        break;
    case WM_CONTEXTMENU:
        if ((HWND)wp == g_list) ShowContextMenu(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_TIMER:
        if (wp == TIMER_AUTO) {
            long long now = NowUnix();
            for (int i = 0; i < g_napps; i++) {
                App *a = g_apps[i];
                if (!a->retryAt || a->retryAt > now || a->queued || a->busy || !a->enabled) continue;
                int type = a->retryType ? a->retryType : JOB_CHECK, automatic = a->retryAuto;
                Enqueue(a, type, automatic);
            }
        }
        if (wp == TIMER_AUTO && g_intervalHours > 0 && g_napps &&
            NowUnix() - g_lastAuto >= (long long)g_intervalHours * 3600)
            CheckAll(1);
        return 0;
    case WM_APP_JOBSTART: {
        App *a = FindApp((int)wp);
        if (a) {
            a->queued = JOB_NONE;
            a->busy = (int)lp;
            a->progress = -1;
            UpdateRow(a);
        }
        UpdateStatusBar();
        return 0;
    }
    case WM_APP_PROGRESS: {
        App *a = FindApp((int)wp);
        if (a && a->busy == JOB_DOWNLOAD) {
            a->progress = (int)lp;
            UpdateRow(a);
        }
        return 0;
    }
    case WM_APP_JOBDONE:
        OnJobDone((JobResult *)lp);
        return 0;
    case WM_APP_IDLE:
        OnIdle();
        return 0;
    case WM_APP_SHOWME:
        ShowMain();
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
        case NIN_BALLOONUSERCLICK:
            ShowMain();
            break;
        case WM_CONTEXTMENU: {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, L"開く");
            AppendMenuW(m, MF_STRING, ID_CHECKALL, L"すべて確認");
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"終了");
            SetMenuDefaultItem(m, ID_TRAY_OPEN, FALSE);
            SetForegroundWindow(hwnd);
            TrackPopupMenu(m, TPM_RIGHTBUTTON, GET_X_LPARAM(wp), GET_Y_LPARAM(wp), 0, hwnd, NULL);
            DestroyMenu(m);
            break;
        }
        }
        return 0;
    case WM_CLOSE:
        if (g_trayOnClose && !g_exiting) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        ExitApp();
        return 0;
    case WM_ENDSESSION:
        if (wp) { g_exiting = TRUE; DestroyWindow(hwnd); }
        return 0;
    case WM_DESTROY: {
        WINDOWPLACEMENT wpl = { sizeof(wpl) };
        if (GetWindowPlacement(hwnd, &wpl)) {
            RECT *r = &wpl.rcNormalPosition;
            IniPutInt(L"obtan", L"x", r->left);
            IniPutInt(L"obtan", L"y", r->top);
            IniPutInt(L"obtan", L"w", r->right - r->left);
            IniPutInt(L"obtan", L"h", r->bottom - r->top);
        }
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        KillTimer(hwnd, TIMER_AUTO);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- コマンドライン（動作確認用）

static HANDLE g_out;

static void Out(const char *s) {
    DWORD mode, wr;
    if (!g_out || g_out == INVALID_HANDLE_VALUE) return;
    if (GetConsoleMode(g_out, &mode)) {
        WCHAR *w = U2W(s);
        if (w) WriteConsoleW(g_out, w, (DWORD)wcslen(w), &wr, NULL);
        free(w);
    } else {
        WriteFile(g_out, s, (DWORD)strlen(s), &wr, NULL);
    }
}

static void OutInit(void) {
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!g_out || g_out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS))
            g_out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    }
}

static int SelfTest(void) {
    int fail = 0;
    struct { const char *re, *s; int icase; int match; const char *g1; } rt[] = {
        { "abc", "xxabcxx", 0, 1, NULL },
        { "^abc$", "abc", 0, 1, NULL },
        { "^abc$", "abcd", 0, 0, NULL },
        { "a.c", "a\xE3\x81\x82" "c", 0, 1, NULL },
        { "zoom-pon-v(\\d+)\\.zip$", "https://x/dist/zoom-pon-v12.zip", 0, 1, "12" },
        { "zoom-pon-v(\\d+)\\.zip$", "https://x/dist/zoom-pon-v12.zip.sha256", 0, 0, NULL },
        { "x64.*\\.(exe|msi)$", "npp.8.9.Installer.X64.EXE", 1, 1, "EXE" },
        { "x64.*\\.(exe|msi)$", "npp.8.9.Installer.X64.EXE", 0, 0, NULL },
        { "(?i)SETUP", "my-setup.exe", 0, 1, NULL },
        { "v(\\d+(?:\\.\\d+){1,3})", "release v1.20.3-beta", 0, 1, "1.20.3" },
        { "a{2,3}", "caab", 0, 1, NULL },
        { "a{2,3}", "cab", 0, 0, NULL },
        { "[^/]+$", "https://a/b/c.zip", 0, 1, NULL },
        { "(a|bc)+d", "xbcabcd", 0, 1, "bc" },
        { "<(.+?)>", "<a><b>", 0, 1, "a" },
        { "\\bwin\\b", "app-win-x64", 0, 1, NULL },
        { "\\bwin\\b", "darwin", 0, 0, NULL },
        { "[a-c-]+", "--abc", 0, 1, NULL },
        { "(a*)*b", "aaaaaaaaaaaaaaaaaaaaaaaaac", 0, 0, NULL },
    };
    for (size_t i = 0; i < sizeof(rt) / sizeof(rt[0]); i++) {
        char err[200] = "";
        Rx *rx = RxCompile(rt[i].re, rt[i].icase, err, sizeof(err));
        int caps[2 * RX_MAXGROUP];
        int m = rx ? RxSearch(rx, rt[i].s, caps) : -1;
        char g1[64] = "";
        if (m == 1 && caps[2] >= 0) snprintf(g1, sizeof(g1), "%.*s", caps[3] - caps[2], rt[i].s + caps[2]);
        int ok = m == rt[i].match && (!rt[i].g1 || strcmp(g1, rt[i].g1) == 0);
        char *line = U8Fmt("%s rx  /%s/ on \"%s\" -> %d [%s] %s\n", ok ? "ok  " : "FAIL", rt[i].re, rt[i].s, m, g1, err);
        Out(line);
        free(line);
        if (!ok) fail++;
        RxFree(rx);
    }
    const char *bad[] = { "(abc", "abc)", "[abc", "*a", "a{3,1}" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char err[200] = "";
        Rx *rx = RxCompile(bad[i], 0, err, sizeof(err));
        char *line = U8Fmt("%s rx  bad /%s/ -> %s\n", rx ? "FAIL" : "ok  ", bad[i], err);
        Out(line);
        free(line);
        if (rx) { fail++; RxFree(rx); }
    }
    struct { const char *latest, *installed; int newer; } vt[] = {
        { "v12", "v11", 1 }, { "v11", "v12", 0 }, { "v12", "v12", 0 }, { "v8.9.8.1", "v8.9.8", 1 },
        { "1.10.0", "1.9.9", 1 }, { "v1.2", "1.2", 0 }, { "1.2.0", "1.2.0-beta1", 1 }, { "2026.10.10.1200", "2026.10.01.0900", 1 },
        { "etag:\"abc\"", "etag:\"abd\"", 1 }, { "INKSCAPE_1_4", "INKSCAPE_1_3_2", 1 }, { "v13", "", 1 },
    };
    for (size_t i = 0; i < sizeof(vt) / sizeof(vt[0]); i++) {
        int r = VersionIsNewer(vt[i].latest, vt[i].installed);
        char *line = U8Fmt("%s ver %s vs %s -> %d\n", r == vt[i].newer ? "ok  " : "FAIL", vt[i].latest, vt[i].installed, r);
        Out(line);
        free(line);
        if (r != vt[i].newer) fail++;
    }
    struct { const char *name, *ver; } nt[] = {
        { "zoom-pon-v12.zip", "12" }, { "npp.8.9.8.1.Installer.x64.exe", "8.9.8.1" }, { "setup-win64-2.3.exe", "2.3" },
        { "app_x64_64bit.7z", NULL }, { "tool-1.4.2.7z", "1.4.2" }, { "foo.exe", NULL },
        { "7z2501-x64.exe", "2501" }, { "app-x86_64-3.exe", "3" }, { "tool-win32-i386.zip", NULL },
        { "Everything-1.4.1.1026.x64-Setup.exe", "1.4.1.1026" }, { "rel_20261001.zip", "20261001" },
    };
    for (size_t i = 0; i < sizeof(nt) / sizeof(nt[0]); i++) {
        char *v = VersionFromName(nt[i].name);
        int ok = (!v && !nt[i].ver) || (v && nt[i].ver && strcmp(v, nt[i].ver) == 0);
        char *line = U8Fmt("%s name %s -> %s\n", ok ? "ok  " : "FAIL", nt[i].name, v ? v : "(none)");
        Out(line);
        free(line);
        free(v);
        if (!ok) fail++;
    }
    struct { const char *name; int better; const char *than; } st[] = {
        { "npp.8.9.8.1.Installer.x64.exe", 1, "npp.8.9.8.1.Installer.arm64.exe" },
        { "npp.8.9.8.1.Installer.x64.exe", 1, "npp.8.9.8.1.Installer.x64.msi" },
        { "app-windows-amd64.zip", 1, "app-linux-amd64.tar.gz" },
        { "follient-29.0.0-signed.xpi", 1, "follient-29.0.0-signed.xpi.sig" },
    };
    for (size_t i = 0; i < sizeof(st) / sizeof(st[0]); i++) {
        int a = WindowsScore(st[i].name), b = WindowsScore(st[i].than);
        int ok = (a > b) == st[i].better;
        char *line = U8Fmt("%s score %s(%d) > %s(%d)\n", ok ? "ok  " : "FAIL", st[i].name, a, st[i].than, b);
        Out(line);
        free(line);
        if (!ok) fail++;
    }
    char *line = U8Fmt("%d failed\n", fail);
    Out(line);
    free(line);
    return fail ? 1 : 0;
}

// -probe <url> [-kind github|gitlab|gitea|web|amo] [-filter re] [-ver re] [-pre] [-token t] [-dl dir]
static int Probe(int argc, WCHAR **argv) {
    char *url = NULL, *filter = NULL, *ver = NULL, *token = NULL;
    WCHAR *dl = NULL;
    SourceKind kind = SRC_AUTO;
    int pre = 0;
    for (int i = 1; i < argc; i++) {
        if (!_wcsicmp(argv[i], L"-probe") && i + 1 < argc) url = W2U(argv[++i]);
        else if (!_wcsicmp(argv[i], L"-filter") && i + 1 < argc) filter = W2U(argv[++i]);
        else if (!_wcsicmp(argv[i], L"-ver") && i + 1 < argc) ver = W2U(argv[++i]);
        else if (!_wcsicmp(argv[i], L"-token") && i + 1 < argc) token = W2U(argv[++i]);
        else if (!_wcsicmp(argv[i], L"-dl") && i + 1 < argc) dl = argv[++i];
        else if (!_wcsicmp(argv[i], L"-pre")) pre = 1;
        else if (!_wcsicmp(argv[i], L"-kind") && i + 1 < argc) {
            i++;
            if (!_wcsicmp(argv[i], L"github")) kind = SRC_GITHUB;
            else if (!_wcsicmp(argv[i], L"gitlab")) kind = SRC_GITLAB;
            else if (!_wcsicmp(argv[i], L"gitea")) kind = SRC_GITEA;
            else if (!_wcsicmp(argv[i], L"web")) kind = SRC_WEB;
            else if (!_wcsicmp(argv[i], L"amo")) kind = SRC_AMO;
        }
    }
    SourceCfg cfg = { url, kind, filter, ver, pre, token };
    Latest l;
    SourceLatest(&cfg, &l);
    char *text = FormatLatest(&l, "\n");
    Out(text);
    free(text);
    int rc = l.ok ? 0 : 1;
    if (l.ok && dl) {
        SHCreateDirectoryExW(NULL, dl, NULL);
        WCHAR *name = U2W(l.fileName), path[MAX_PATH];
        SanitizeFileName(name);
        swprintf_s(path, MAX_PATH, L"%s\\%s", dl, name);
        char disp[260], err[512];
        ULONGLONG t0 = GetTickCount64();
        if (HttpDownload(l.fileUrl, path, NULL, NULL, NULL, disp, sizeof(disp), err, sizeof(err))) {
            WIN32_FILE_ATTRIBUTE_DATA fa;
            GetFileAttributesExW(path, GetFileExInfoStandard, &fa);
            char *p = W2U(path);
            char *line = U8Fmt("\n保存した: %s（%llu バイト、%llu ms）%s%s\n", p,
                               ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow, GetTickCount64() - t0,
                               disp[0] ? " Content-Disposition: " : "", disp);
            Out(line);
            free(line);
            free(p);
        } else {
            char *line = U8Fmt("\nダウンロード失敗: %s\n", err);
            Out(line);
            free(line);
            rc = 1;
        }
        free(name);
    }
    LatestFree(&l);
    free(url);
    free(filter);
    free(ver);
    free(token);
    return rc;
}

// ---------------------------------------------------------------- 入口

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev; (void)cmd;
    g_hinst = hinst;
    int argc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    BOOL tray = FALSE;
    const WCHAR *iniOverride = NULL;
    for (int i = 1; i < argc; i++) {
        if (!_wcsicmp(argv[i], L"-tray")) tray = TRUE;
        else if (!_wcsicmp(argv[i], L"-ini") && i + 1 < argc) iniOverride = argv[++i];
        else if (!_wcsicmp(argv[i], L"-selftest")) { OutInit(); return SelfTest(); }
        else if (!_wcsicmp(argv[i], L"-probe")) { OutInit(); NetInit(); return Probe(argc, argv); }
    }

    // 二重起動しない（既存のウィンドウを前面へ）。-ini を指定したときは別の一覧として並べて動かせる
    HANDLE mutex = NULL;
    if (!iniOverride) {
        mutex = CreateMutexW(NULL, TRUE, L"obtan-single-instance");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND w = FindWindowW(L"ObtanMain", NULL);
            if (w) {
                DWORD pid = 0;
                GetWindowThreadProcessId(w, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(w, WM_APP_SHOWME, 0, 0);
            }
            return 0;
        }
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);
    InitializeCriticalSection(&g_qlock);
    InitDarkMode();
    NetInit();
    InitPaths(iniOverride);
    LoadAll();
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    LoadIconMetric(hinst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &g_iconSmall);
    LoadIconMetric(hinst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &g_iconBig);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hinst;
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_iconSmall;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"ObtanMain";
    RegisterClassExW(&wc);

    int x = (int)IniGetInt(L"obtan", L"x", CW_USEDEFAULT);
    int y = (int)IniGetInt(L"obtan", L"y", CW_USEDEFAULT);
    int w = (int)IniGetInt(L"obtan", L"w", 0);
    int h = (int)IniGetInt(L"obtan", L"h", 0);
    RECT saved = { x, y, x + w, y + h };
    if (x == CW_USEDEFAULT || w <= 0 || h <= 0 || !MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) {
        x = y = CW_USEDEFAULT;
        HDC dc = GetDC(NULL);
        int sdpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
        w = MulDiv(820, sdpi, 96);
        h = MulDiv(440, sdpi, 96);
    }
    WCHAR title[MAX_PATH + 64] = L"obtan - アプリの新しい版を見張る";
    if (iniOverride) swprintf_s(title, _countof(title), L"obtan - %s", g_ini);
    HWND hwnd = CreateWindowExW(0, L"ObtanMain", title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                x, y, w, h, NULL, NULL, hinst, NULL);
    if (!hwnd) return 1;
    if (!tray) {
        ShowWindow(hwnd, show);
        UpdateWindow(hwnd);
    }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    // 走っているダウンロードを止めて、少しだけ終わりを待つ
    InterlockedExchange(&g_cancel, 1);
    for (int i = 0; i < 30 && WorkerBusy(); i++) Sleep(100);
    if (g_font) DeleteObject(g_font);
    if (mutex) CloseHandle(mutex);
    LocalFree(argv);
    return 0;
}
