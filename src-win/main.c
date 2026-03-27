/*
 * Native Windows tray + console window helper – JSON-lines stdin/stdout protocol.
 * Build (MSVC):
 * cl /O2 /DUNICODE /D_UNICODE main.c cJSON.c /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib gdi32.lib kernel32.lib comctl32.lib
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <richedit.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>

#include "cJSON.h"

/* -----------------------------------------------------------------------
 * Constants & Macros
 * ----------------------------------------------------------------------- */
#define WM_TRAYICON     (WM_USER + 1)
#define WM_STDIN_CMD    (WM_USER + 2)
#define WM_PARENT_DIED  (WM_USER + 3)
#define MAX_MENU_ITEMS  4096
#define MAX_TOOLTIP     128
#define LOG_WINDOW_ID   100

/* -----------------------------------------------------------------------
 * Globals
 * ----------------------------------------------------------------------- */
static HWND             gHwnd;           /* Hidden message window */
static HWND             gLogWnd;         /* Visible log console window */
static HWND             gLogEdit;        /* RichEdit control inside log window */
static NOTIFYICONDATAW  gNid;
static HMENU            gMenu;
static HICON            gIcon;
static UINT             gTaskbarCreatedMsg;
static CRITICAL_SECTION gOutputLock;
static HANDLE           gStdinHandle;
static HANDLE           gStdoutHandle;
static HFONT            gLogFont;
static BOOL             gLogWindowVisible = TRUE;
static BOOL             gParentDead = FALSE;   /* Set when stdin closes (parent crashed/exited) */

static char             gInitialIcon[MAX_PATH];
static WCHAR            gInitialTooltip[MAX_TOOLTIP];

typedef struct {
    UINT    cmdId;
    char    id[256];
} MenuIdEntry;

static MenuIdEntry gMenuIdMap[MAX_MENU_ITEMS];
static UINT        gMenuIdCount;
static UINT        gNextCmdId = 1;

/* -----------------------------------------------------------------------
 * JSON output
 * ----------------------------------------------------------------------- */
static void emit(const char *method, cJSON *params) {
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "method", method);
    if (params) cJSON_AddItemToObject(msg, "params", params);

    char *str = cJSON_PrintUnformatted(msg);
    if (str) {
        EnterCriticalSection(&gOutputLock);
        DWORD written;
        WriteFile(gStdoutHandle, str, (DWORD)strlen(str), &written, NULL);
        WriteFile(gStdoutHandle, "\n", 1, &written, NULL);
        FlushFileBuffers(gStdoutHandle);
        LeaveCriticalSection(&gOutputLock);
        free(str);
    }
    cJSON_Delete(msg);
}

/* -----------------------------------------------------------------------
 * Icon Logic
 * ----------------------------------------------------------------------- */
static HICON createDefaultIcon(void) {
    int sz = GetSystemMetrics(SM_CXSMICON);
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = sz;
    bmi.bmiHeader.biHeight = -sz;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HDC hdc = GetDC(NULL);
    HBITMAP hColor = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, hdc);

    if (bits) {
        BYTE *px = (BYTE *)bits;
        float half = sz / 2.0f;
        float r2 = (half - 1) * (half - 1);
        for (int y = 0; y < sz; y++) {
            for (int x = 0; x < sz; x++) {
                float dx = x - half + 0.5f;
                float dy = y - half + 0.5f;
                int off = (y * sz + x) * 4;
                if (dx*dx + dy*dy <= r2) {
                    px[off+0] = 0x33; px[off+1] = 0xad; px[off+2] = 0x2e; px[off+3] = 0xff;
                }
            }
        }
    }
    HBITMAP hMask = CreateBitmap(sz, sz, 1, 1, NULL);
    ICONINFO ii = {TRUE, 0, 0, hMask, hColor};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(hMask);
    DeleteObject(hColor);
    return icon;
}

static unsigned char b64_rev[256];
static void initB64(void) {
    memset(b64_rev, 0x40, 256);
    for (int i=0; i<64; i++) b64_rev["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]] = i;
}

static unsigned char *base64Decode(const char *src, size_t *outLen) {
    size_t len = strlen(src);
    if (len % 4 != 0) return NULL;
    size_t dLen = (len / 4) * 3;
    if (src[len-1] == '=') dLen--;
    if (src[len-2] == '=') dLen--;
    unsigned char *out = malloc(dLen);
    for (size_t i=0, j=0; i<len; ) {
        unsigned int a = b64_rev[(unsigned char)src[i++]];
        unsigned int b = b64_rev[(unsigned char)src[i++]];
        unsigned int c = b64_rev[(unsigned char)src[i++]];
        unsigned int d = b64_rev[(unsigned char)src[i++]];
        unsigned int triple = (a << 18) | (b << 12) | (c << 6) | d;
        if (j < dLen) out[j++] = (triple >> 16) & 0xFF;
        if (j < dLen) out[j++] = (triple >> 8) & 0xFF;
        if (j < dLen) out[j++] = triple & 0xFF;
    }
    *outLen = dLen;
    return out;
}

/* -----------------------------------------------------------------------
 * Log Window
 * ----------------------------------------------------------------------- */
static void appendLogText(const char *text) {
    if (!gLogEdit) return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    WCHAR *wtext = malloc(wlen * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wtext, wlen);

    /* Move caret to end and append */
    int len = GetWindowTextLengthW(gLogEdit);
    SendMessageW(gLogEdit, EM_SETSEL, len, len);
    SendMessageW(gLogEdit, EM_REPLACESEL, FALSE, (LPARAM)wtext);
    /* Auto-scroll to bottom */
    SendMessageW(gLogEdit, WM_VSCROLL, SB_BOTTOM, 0);
    free(wtext);
}

static void showLogWindow(void) {
    if (gLogWnd) {
        ShowWindow(gLogWnd, SW_RESTORE);
        SetForegroundWindow(gLogWnd);
        gLogWindowVisible = TRUE;
    }
}

static void hideLogWindow(void) {
    if (gLogWnd) {
        ShowWindow(gLogWnd, SW_HIDE);
        gLogWindowVisible = FALSE;
    }
}

static LRESULT CALLBACK LogWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            /* Resize the edit control to fill the window */
            if (gLogEdit) {
                MoveWindow(gLogEdit, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
            }
            if (wParam == SIZE_MINIMIZED) {
                /* Minimize to tray */
                hideLogWindow();
                return 0;
            }
            break;
        case WM_CLOSE:
            if (gParentDead) {
                /* Parent is gone — actually exit */
                DestroyWindow(gHwnd);
                return 0;
            }
            /* Close button hides to tray instead of destroying */
            hideLogWindow();
            return 0;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            /* Dark background for the log area */
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, RGB(204, 204, 204));  /* Light gray text */
            SetBkColor(hdc, RGB(30, 30, 30));        /* Dark background */
            static HBRUSH hBrush = NULL;
            if (!hBrush) hBrush = CreateSolidBrush(RGB(30, 30, 30));
            return (LRESULT)hBrush;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void createLogWindow(HINSTANCE hi) {
    /* Load RichEdit library */
    LoadLibraryW(L"Msftedit.dll");

    WNDCLASSEXW wc = {sizeof(wc), 0, LogWndProc, 0, 0, hi, 0,
        LoadCursor(NULL, IDC_ARROW), 0, 0, L"TrayConsoleLog", 0};
    wc.hIcon = gIcon;
    wc.hIconSm = gIcon;
    RegisterClassExW(&wc);

    /* Create the main visible window */
    gLogWnd = CreateWindowExW(
        0, L"TrayConsoleLog", L"Console",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 500,
        NULL, NULL, hi, NULL);

    /* Create a RichEdit control as the log area */
    gLogEdit = CreateWindowExW(
        0, MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 800, 500,
        gLogWnd, (HMENU)(UINT_PTR)LOG_WINDOW_ID, hi, NULL);

    /* Set monospace font */
    gLogFont = CreateFontW(
        -14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
    if (!gLogFont) {
        /* Fallback to Consolas */
        gLogFont = CreateFontW(
            -14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    }
    SendMessageW(gLogEdit, WM_SETFONT, (WPARAM)gLogFont, TRUE);

    /* Set dark background color for RichEdit */
    SendMessageW(gLogEdit, EM_SETBKGNDCOLOR, 0, RGB(30, 30, 30));

    /* Set default text color */
    CHARFORMAT2W cf = {0};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = RGB(204, 204, 204);
    SendMessageW(gLogEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);

    /* Remove text limit (default is 32K) */
    SendMessageW(gLogEdit, EM_EXLIMITTEXT, 0, 0x7FFFFFFF);

    gLogWindowVisible = TRUE;
}

/* -----------------------------------------------------------------------
 * Menu Builders
 * ----------------------------------------------------------------------- */
static void buildMenuItems(HMENU menu, cJSON *items) {
    int count = cJSON_GetArraySize(items);
    for (int i = 0; i < count; i++) {
        cJSON *cfg = cJSON_GetArrayItem(items, i);
        if (cJSON_IsTrue(cJSON_GetObjectItem(cfg, "separator"))) {
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            continue;
        }
        const char *title_val = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "title"));
        const char *title = title_val ? title_val : "";
        const char *id_val = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "id"));
        const char *itemId = id_val ? id_val : "";

        int wlen = MultiByteToWideChar(CP_UTF8, 0, title, -1, NULL, 0);
        WCHAR *wtitle = malloc(wlen * sizeof(WCHAR));
        MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, wlen);

        cJSON *jChildren = cJSON_GetObjectItem(cfg, "items");
        if (cJSON_IsArray(jChildren) && cJSON_GetArraySize(jChildren) > 0) {
            HMENU sub = CreatePopupMenu();
            buildMenuItems(sub, jChildren);
            AppendMenuW(menu, MF_POPUP | MF_STRING, (UINT_PTR)sub, wtitle);
        } else {
            UINT cmdId = gNextCmdId++;
            if (gMenuIdCount < MAX_MENU_ITEMS) {
                gMenuIdMap[gMenuIdCount].cmdId = cmdId;
                strncpy(gMenuIdMap[gMenuIdCount].id, itemId, 255);
                gMenuIdCount++;
            }
            UINT flags = MF_STRING;
            if (cJSON_IsFalse(cJSON_GetObjectItem(cfg, "enabled"))) flags |= MF_GRAYED;
            if (cJSON_IsTrue(cJSON_GetObjectItem(cfg, "checked"))) flags |= MF_CHECKED;
            AppendMenuW(menu, flags, cmdId, wtitle);
        }
        free(wtitle);
    }
}

/* -----------------------------------------------------------------------
 * Message Window Proc (hidden, handles tray + stdin commands)
 * ----------------------------------------------------------------------- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == gTaskbarCreatedMsg) { Shell_NotifyIconW(NIM_ADD, &gNid); return 0; }
    switch (msg) {
        case WM_TRAYICON:
            if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
                /* Double-click tray icon: show/restore log window */
                showLogWindow();
            } else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP) {
                emit("menuRequested", NULL);
                POINT pt; GetCursorPos(&pt);
                SetForegroundWindow(hwnd);
                TrackPopupMenu(gMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            }
            break;
        case WM_COMMAND: {
            for (UINT i=0; i<gMenuIdCount; i++) {
                if (gMenuIdMap[i].cmdId == LOWORD(wParam)) {
                    cJSON *p = cJSON_CreateObject();
                    cJSON_AddStringToObject(p, "id", gMenuIdMap[i].id);
                    emit("clicked", p);
                    break;
                }
            }
            break;
        }
        case WM_STDIN_CMD: {
            cJSON *m = (cJSON *)lParam;
            const char *meth = cJSON_GetStringValue(cJSON_GetObjectItem(m, "method"));
            cJSON *p = cJSON_GetObjectItem(m, "params");
            if (!strcmp(meth, "setMenu")) {
                if (gMenu) DestroyMenu(gMenu);
                gMenu = CreatePopupMenu(); gMenuIdCount = 0; gNextCmdId = 1;
                buildMenuItems(gMenu, cJSON_GetObjectItem(p, "items"));
            } else if (!strcmp(meth, "setIcon")) {
                size_t len; unsigned char *d = base64Decode(cJSON_GetStringValue(cJSON_GetObjectItem(p, "base64")), &len);
                if (d) {
                    WCHAR tmpPath[MAX_PATH], tmpFile[MAX_PATH];
                    GetTempPathW(MAX_PATH, tmpPath);
                    GetTempFileNameW(tmpPath, L"ico", 0, tmpFile);
                    HANDLE hf = CreateFileW(tmpFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hf != INVALID_HANDLE_VALUE) {
                        DWORD w; WriteFile(hf, d, (DWORD)len, &w, NULL); CloseHandle(hf);
                        HICON n = (HICON)LoadImageW(NULL, tmpFile, IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
                        if (n) {
                            if (gIcon) DestroyIcon(gIcon);
                            gIcon = n;
                            gNid.hIcon = gIcon;
                            Shell_NotifyIconW(NIM_MODIFY, &gNid);
                            /* Also update log window icon */
                            if (gLogWnd) {
                                SendMessageW(gLogWnd, WM_SETICON, ICON_BIG, (LPARAM)gIcon);
                                SendMessageW(gLogWnd, WM_SETICON, ICON_SMALL, (LPARAM)gIcon);
                            }
                        }
                        DeleteFileW(tmpFile);
                    }
                    free(d);
                }
            } else if (!strcmp(meth, "setTooltip")) {
                MultiByteToWideChar(CP_UTF8, 0, cJSON_GetStringValue(cJSON_GetObjectItem(p, "text")), -1, gNid.szTip, MAX_TOOLTIP);
                Shell_NotifyIconW(NIM_MODIFY, &gNid);
            } else if (!strcmp(meth, "appendLog")) {
                const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(p, "text"));
                if (text) appendLogText(text);
            } else if (!strcmp(meth, "showWindow")) {
                showLogWindow();
            } else if (!strcmp(meth, "hideWindow")) {
                hideLogWindow();
            } else if (!strcmp(meth, "setTitle")) {
                const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(p, "text"));
                if (title && gLogWnd) {
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, title, -1, NULL, 0);
                    WCHAR *wtitle = malloc(wlen * sizeof(WCHAR));
                    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, wlen);
                    SetWindowTextW(gLogWnd, wtitle);
                    free(wtitle);
                }
            }
            cJSON_Delete(m); break;
        }
        case WM_PARENT_DIED:
            gParentDead = TRUE;
            appendLogText("\r\n--- Process exited ---\r\n");
            showLogWindow();
            /* Don't exit — keep window and tray alive so user can read the log */
            break;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &gNid);
            if (gIcon) DestroyIcon(gIcon);
            if (gLogFont) DeleteObject(gLogFont);
            PostQuitMessage(0);
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* -----------------------------------------------------------------------
 * Stdin Reader Thread
 * ----------------------------------------------------------------------- */
static unsigned __stdcall stdinReaderThread(void *arg) {
    char chunk[4096], *buf = NULL; size_t bufLen = 0, bufCap = 0; DWORD read;
    while (ReadFile(gStdinHandle, chunk, sizeof(chunk), &read, NULL) && read > 0) {
        if (bufLen + read >= bufCap) { bufCap = (bufLen+read)*2+1; buf = realloc(buf, bufCap); }
        memcpy(buf + bufLen, chunk, read); bufLen += read;
        char *s = buf, *nl;
        while ((nl = memchr(s, '\n', bufLen - (s - buf)))) {
            *nl = '\0';
            cJSON *m = cJSON_Parse(s);
            if (m) PostMessage(gHwnd, WM_STDIN_CMD, 0, (LPARAM)m);
            s = nl + 1;
        }
        size_t rem = bufLen - (s - buf); if (rem > 0) memmove(buf, s, rem); bufLen = rem;
    }
    /* stdin closed — parent process exited/crashed. Post to main thread
     * so UI updates happen safely. */
    PostMessage(gHwnd, WM_PARENT_DIED, 0, 0);
    return 0;
}

/* -----------------------------------------------------------------------
 * Entry Point
 * ----------------------------------------------------------------------- */
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR lp, int n) {
    SetProcessDPIAware();
    initB64(); InitializeCriticalSection(&gOutputLock);
    gStdinHandle = GetStdHandle(STD_INPUT_HANDLE); gStdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);

    /* Register hidden message window class */
    WNDCLASSEXW wc = {sizeof(wc), 0, WndProc, 0, 0, hi, 0, 0, 0, 0, L"TrayConsole", 0};
    RegisterClassExW(&wc);
    gHwnd = CreateWindowExW(0, L"TrayConsole", L"TrayConsole", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, hi, 0);

    /* Enable double-click notification on tray icon */
    gIcon = createDefaultIcon();
    gNid.cbSize = sizeof(gNid); gNid.hWnd = gHwnd; gNid.uID = 1;
    gNid.uFlags = NIF_ICON|NIF_MESSAGE|NIF_TIP;
    gNid.uCallbackMessage = WM_TRAYICON; gNid.hIcon = gIcon; wcscpy(gNid.szTip, L"Tray");
    gNid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_ADD, &gNid);
    Shell_NotifyIconW(NIM_SETVERSION, &gNid);
    gMenu = CreatePopupMenu();

    /* Create the visible log console window */
    createLogWindow(hi);

    gTaskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    _beginthreadex(NULL, 0, stdinReaderThread, NULL, 0, NULL);
    emit("ready", NULL);

    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}
