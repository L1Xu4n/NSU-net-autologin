#include "native.h"

#pragma comment(                                                                                             \
    linker,                                                                                                  \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

WCHAR errorText[512];
static HINSTANCE instance;
static HWND mainWindow, statusLabel, campusLabel, toastWindow, toastLabel;
static HWND aboutWindow;
#define REPOSITORY_URL L"https://github.com/L1Xu4n/NSU-net-autologin"
static HFONT font, titleFont, buttonFont;
static HBRUSH background, whiteBrush;
static BOOL busy, configOpen, startupMode, testMode, configured;
static int installed, lastExit, lastMode, statusTone = -1;
static HANDLE worker;
static HANDLE residentMutex;
static BOOL resident, choosing, choosePending, preferWifi, recheckPending, trayAdded;
static BOOL manualOffline, selectionPending;
static BOOL operationWifi;
static int uiChoice;
static NetworkLinks observedLinks, visibleLinks;
static NetworkAdapter selectedAdapter, previousWifi, rememberedWifi, rememberedWired;
static NetworkAdapter operationAdapter;
static HICON appIcon, smallIcon, headingIcon, wiredIcon, wifiIcon;
static int campusState[2] = {-1, -1}, campusTone = -1;
static BOOL campusVerification, statusQueryBlocked;
static ULONGLONG nextStatusCheck;
static NetworkSessionState operationStatus;
static NOTIFYICONDATAW tray;
static UINT taskbarCreated;
static BOOL choiceCaptured;
static int testChoice;
static int testOperation;
static BOOL choose_wifi(const NetworkLinks *links);
static void monitor_network(void);
static Config editing;
static COLORREF toastTextColor;
static double scale = 1.0;
#define DONE (WM_APP + 1)
#define NETWORK_PROGRESS (WM_APP + 2)
#define TRAY_MESSAGE (WM_APP + 3)
#define INSTANCE_MESSAGE (WM_APP + 4)
#define GREEN RGB(24, 122, 72)
#define RED RGB(180, 48, 48)
#define INK RGB(31, 45, 66)
#define MUTED RGB(93, 108, 129)
#define BLUE RGB(39, 92, 196)
#define BACK RGB(246, 248, 252)

/* Scale layout and font measurements together, keeping controls inside the visible desktop. */
static int px(int value) { return (int)(value * scale + 0.5); }

/* Create native controls with consistent fonts, keyboard focus and styled action buttons. */
static HWND control(HWND parent, const WCHAR *kind, const WCHAR *text, DWORD style, int x, int y, int w,
                    int h, int id) {
    if (!wcscmp(kind, L"BUTTON")) {
        if ((style & BS_TYPEMASK) != BS_AUTORADIOBUTTON)
            style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW;
        style |= WS_TABSTOP;
    }
    if (!wcscmp(kind, L"EDIT") || !wcscmp(kind, L"COMBOBOX"))
        style |= WS_TABSTOP;
    HWND c = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h), parent,
                             (HMENU)(INT_PTR)id, instance, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

/* Paint rounded action buttons while retaining Win32 keyboard and click behavior. */
static BOOL draw_button(LPARAM lp) {
    DRAWITEMSTRUCT *item = (DRAWITEMSTRUCT *)lp;
    WCHAR text[128];
    BOOL primary = item->CtlID == 101 || item->CtlID == 305 || item->CtlID == 401;
    BOOL disabled = (item->itemState & ODS_DISABLED) != 0;
    COLORREF fill = disabled ? RGB(230, 235, 242) : (primary ? BLUE : RGB(255, 255, 255));
    COLORREF border = disabled ? RGB(218, 226, 236) : (primary ? BLUE : RGB(202, 213, 229));
    if (item->itemState & ODS_SELECTED)
        fill = primary ? RGB(28, 72, 159) : RGB(232, 239, 249);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(item->hDC, brush), oldPen = SelectObject(item->hDC, pen);
    HFONT oldFont = SelectObject(item->hDC, buttonFont);
    RECT r = item->rcItem;
    FillRect(item->hDC, &r, GetParent(item->hwndItem) == toastWindow ? whiteBrush : background);
    RoundRect(item->hDC, r.left, r.top, r.right, r.bottom, px(12), px(12));
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, disabled ? MUTED : (primary ? RGB(255, 255, 255) : INK));
    GetWindowTextW(item->hwndItem, text, 128);
    DrawTextW(item->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (item->itemState & ODS_FOCUS) {
        InflateRect(&r, -px(5), -px(5));
        DrawFocusRect(item->hDC, &r);
    }
    SelectObject(item->hDC, oldFont);
    SelectObject(item->hDC, oldPen);
    SelectObject(item->hDC, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
    return TRUE;
}

/* Color neutral explanatory text and operation results without default gray static-control blocks. */
static LRESULT paint_text(WPARAM wp, LPARAM lp) {
    HDC dc = (HDC)wp;
    int id = GetDlgCtrlID((HWND)lp);
    COLORREF color = MUTED;
    if (id == 510)
        color = INK;
    if (id == 520 || id == 521)
        color = (id == 520 ? visibleLinks.wired.index : visibleLinks.wifi.index) ? GREEN : MUTED;
    if ((HWND)lp == statusLabel)
        color = statusTone < 0 ? INK : (statusTone == 0 ? GREEN : RED);
    if ((HWND)lp == campusLabel)
        color = campusTone == 0 ? GREEN : (campusTone == 1 ? RED : INK);
    SetTextColor(dc, color);
    SetBkColor(dc, BACK);
    return (LRESULT)background;
}

/* Set the visible operation message and its neutral/success/error color together. */
static void set_status(const WCHAR *text, int tone) {
    statusTone = tone;
    SetWindowTextW(statusLabel, text);
    InvalidateRect(statusLabel, NULL, TRUE);
}

/* Render confirmed campus authentication separately from physical links and mirror it in the tray tooltip. */
static void update_campus_view(void) {
    WCHAR text[128], tip[128];
    BOOL wired = visibleLinks.wired.index != 0, wifi = visibleLinks.wifi.index != 0;
    BOOL wiredOnline = wired && campusState[0] == 1, wifiOnline = wifi && campusState[1] == 1;
    campusTone = -1;
    if (!configured) StringCchCopyW(text, 128, L"校园网 · 尚未配置账号");
    else if (!wired && !wifi) StringCchCopyW(text, 128, L"校园网 · 未连接网络");
    else if (busy && (lastMode == 101 || lastMode == 106 || lastMode == 105))
        swprintf_s(text, 128, L"校园网 · 正在%s（%s）", lastMode == 105 ? L"下线" : L"上线", operationWifi ? L"Wi-Fi" : L"网线");
    else if (campusVerification) {
        StringCchCopyW(text, 128, L"校园网 · 需要电话验证");
        campusTone = 1;
    } else if (wiredOnline || wifiOnline) {
        swprintf_s(text, 128, L"校园网 · 已上线（%s）", wiredOnline && wifiOnline ? L"网线 / Wi-Fi" : (wiredOnline ? L"网线" : L"Wi-Fi"));
        campusTone = 0;
    } else if ((wired && campusState[0] < 0) || (wifi && campusState[1] < 0))
        StringCchCopyW(text, 128, busy && lastMode == 107 ? L"校园网 · 正在查询" : L"校园网 · 状态待确认");
    else StringCchCopyW(text, 128, L"校园网 · 未上线");
    if (campusLabel) {
        SetWindowTextW(campusLabel, text);
        InvalidateRect(campusLabel, NULL, TRUE);
    }
    swprintf_s(tip, 128, L"NSU %s%s", text, manualOffline ? L"\n自动重连已暂停" : L"");
    if (wcscmp(tray.szTip, tip)) {
        StringCchCopyW(tray.szTip, 128, tip);
        if (trayAdded) {
            tray.uFlags = NIF_TIP;
            Shell_NotifyIconW(NIM_MODIFY, &tray);
        }
    }
}

/* Show physical link availability separately from campus authentication, and gate actions by selection. */
static void update_network_view(const NetworkLinks *links) {
    const NetworkAdapter *adapters[2] = {&links->wired, &links->wifi};
    BOOL connected[2] = {links->wiredConnected, links->wifiConnected};
    if (!same_adapter(&visibleLinks.wired, &links->wired)) campusState[0] = -1;
    if (!same_adapter(&visibleLinks.wifi, &links->wifi)) campusState[1] = -1;
    if (!same_adapter(&visibleLinks.wired, &links->wired) || !same_adapter(&visibleLinks.wifi, &links->wifi)) {
        nextStatusCheck = 0;
        statusQueryBlocked = campusVerification = FALSE;
    }
    visibleLinks = *links;
    if ((uiChoice == 1 && !links->wired.index) || (uiChoice == 2 && !links->wifi.index) || !uiChoice) {
        uiChoice = links->wired.index ? 1 : (links->wifi.index ? 2 : 0);
        selectionPending = FALSE;
    }
    for (int i = 0; i < 2; i++) {
        WCHAR detail[128];
        const WCHAR *state = adapters[i]->index ? L"已连接" : (connected[i] ? L"等待 IP 地址" : L"未连接");
        SetDlgItemTextW(mainWindow, 520 + i, state);
        if (adapters[i]->index)
            swprintf_s(detail, 128, L"%s   ·   MAC %s", adapters[i]->ip, adapters[i]->mac);
        else StringCchCopyW(detail, 128, i ? L"连接校园网 Wi-Fi 后可用" : L"接入校园网网线后可用");
        SetDlgItemTextW(mainWindow, 522 + i, detail);
        EnableWindow(GetDlgItem(mainWindow, 110 + i), !busy && adapters[i]->index);
        SendDlgItemMessageW(mainWindow, 110 + i, BM_SETCHECK, uiChoice == i + 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    BOOL ready = uiChoice == 1 ? links->wired.index != 0 : (uiChoice == 2 && links->wifi.index != 0);
    EnableWindow(GetDlgItem(mainWindow, 101), !busy && configured && ready);
    EnableWindow(GetDlgItem(mainWindow, 105), !busy && configured && ready);
    EnableWindow(GetDlgItem(mainWindow, 108), !busy && configured && ready);
    update_campus_view();
}

/* Read current startup/config state and show the next useful step; tests substitute only fictional state. */
static void refresh_state(BOOL explain) {
    if (!testMode) {
        Config c = {0};
        configured = config_io(&c, FALSE);
        SecureZeroMemory(&c, sizeof(c));
        installed = task_status();
    }
    SetDlgItemTextW(mainWindow, 103,
                    installed < 0 ? L"重新检查自启动" : (installed ? L"卸载自启动" : L"注册开机自启"));
    update_network_view(&visibleLinks);
    EnableWindow(GetDlgItem(mainWindow, 102), !busy);
    EnableWindow(GetDlgItem(mainWindow, 103), !busy && (configured || installed != 0));
    if (explain) {
        if (!configured)
            set_status(L"请先配置账号，再注册开机自启。", -1);
        else if (installed == 0)
            set_status(L"选择网卡后点击“上线”。", -1);
        else if (installed == 1)
            set_status(L"自动连接已开启，也可选择网卡手动上线。", -1);
        else
            set_status(L"账号已就绪，自启动状态读取失败；仍可手动连接。", 1);
    }
}

/* Keep the corner notice non-activating and open only the fixed portal on an explicit button click. */
static LRESULT CALLBACK toast_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (msg == WM_CTLCOLORSTATIC) {
        SetTextColor((HDC)wp, toastTextColor);
        SetBkColor((HDC)wp, RGB(255, 255, 255));
        return (LRESULT)whiteBrush;
    }
    if (msg == WM_DRAWITEM)
        return draw_button(lp);
    if (msg == WM_COMMAND && LOWORD(wp) == 401) {
        ShellExecuteW(w, L"open", L"http://2.2.2.2/", NULL, NULL, SW_SHOWNORMAL);
        return 0;
    }
    if (msg == WM_TIMER || msg == WM_CLOSE) {
        DestroyWindow(w);
        return 0;
    }
    if (msg == WM_DESTROY) {
        toastWindow = NULL;
        toastLabel = NULL;
        if (startupMode && !busy && !resident)
            DestroyWindow(mainWindow);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

/* Measure the full message before sizing the notice, keeping the verification button in the client area. */
static void notify_message(int result, const WCHAR *message) {
    RECT area, outer, textRect = {0, 0, px(412), 0};
    int textHeight, clientHeight;
    HDC dc = GetDC(mainWindow);
    HFONT oldFont = SelectObject(dc, font);
    DrawTextW(dc, message, -1, &textRect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, oldFont);
    ReleaseDC(mainWindow, dc);
    textHeight = textRect.bottom + px(8);
    if (textHeight < px(48))
        textHeight = px(48);
    clientHeight = px(40) + textHeight + (result == 3 ? px(64) : 0);
    outer.left = outer.top = 0;
    outer.right = px(460);
    outer.bottom = clientHeight;
    AdjustWindowRectEx(&outer, WS_CAPTION | WS_SYSMENU, FALSE,
                       WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE);
    if (toastWindow) {
        BOOL saved = startupMode;
        startupMode = FALSE;
        DestroyWindow(toastWindow);
        startupMode = saved;
    }
    MONITORINFO monitor = {sizeof(MONITORINFO)};
    if (GetMonitorInfoW(MonitorFromWindow(mainWindow, MONITOR_DEFAULTTONEAREST), &monitor))
        area = monitor.rcWork;
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    toastTextColor = result == 4 ? BLUE : (result == 0 ? GREEN : RED);
    toastWindow =
        CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"NSUToast",
                        result == 4 ? L"校园网连接进度" : (result == 3 ? L"需要电话验证" : L"校园网连接结果"),
                        WS_CAPTION | WS_SYSMENU, area.right - (outer.right - outer.left) - px(16),
                        max(area.top + px(12), area.bottom - (outer.bottom - outer.top) - px(64)), outer.right - outer.left,
                        outer.bottom - outer.top, NULL, NULL, instance, NULL);
    if (!toastWindow) {
        if (startupMode && !busy && !resident)
            DestroyWindow(mainWindow);
        return;
    }
    toastLabel = CreateWindowExW(0, L"STATIC", message, WS_CHILD | WS_VISIBLE | SS_NOPREFIX, px(24), px(20),
                                 px(412), textHeight, toastWindow, (HMENU)501, instance, NULL);
    SendMessageW(toastLabel, WM_SETFONT, (WPARAM)font, TRUE);
    if (result == 3) {
        HWND button = control(toastWindow, L"BUTTON", L"打开验证网页", 0, 24, 0, 412, 44, 401);
        SetWindowPos(button, NULL, px(24), px(36) + textHeight, px(412), px(44), SWP_NOZORDER);
    }
    if (result != 4)
        SetTimer(toastWindow, 1, result == 3 ? 120000 : 8000, NULL);
    ShowWindow(toastWindow, SW_SHOWNOACTIVATE);
}
/* Display the final result only after the worker has finished writing its result buffer. */
static void notify_result(int result) { notify_message(result, errorText); }

/* Send only a stage number from the worker; all window access remains on the UI thread. */
static void report_network_progress(int stage) {
    if (mainWindow)
        PostMessageW(mainWindow, NETWORK_PROGRESS, (WPARAM)stage, 0);
}

/* Show one persistent progress notice per state change, independent of the worker's result text. */
static void show_network_progress(int stage) {
    const WCHAR *message =
        stage == NETWORK_WAITING
            ? WAIT_GUIDANCE
            : (stage == NETWORK_READY
                   ? L"校园网已就绪，正在自动登录，请稍候。"
                   : L"正在检查校园网连接。尚未连接时，将等待 Wi-Fi 或网线就绪后自动登录。");
    set_status(stage == NETWORK_WAITING ? L"正在等待校园网连接，连接 NSU-SDN 后将自动登录（最多约 3 分钟）。"
                                        : message,
               -1);
    notify_message(4, message);
}

/* Render only an app-owned test window, never the desktop or private configuration. */
static BOOL preview(HWND w, const WCHAR *path) {
    RECT r;
    HDC dc = GetDC(w), memory = CreateCompatibleDC(dc);
    HBITMAP bitmap, old;
    BITMAPINFO info = {0};
    BITMAPFILEHEADER header = {0};
    BYTE *pixels;
    DWORD bytes, written;
    HANDLE file;
    BOOL ok = FALSE;
    GetWindowRect(w, &r);
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = r.right - r.left;
    info.bmiHeader.biHeight = r.bottom - r.top;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    bytes = info.bmiHeader.biWidth * info.bmiHeader.biHeight * 4;
    pixels = calloc(bytes, 1);
    bitmap = CreateCompatibleBitmap(dc, info.bmiHeader.biWidth, info.bmiHeader.biHeight);
    old = SelectObject(memory, bitmap);
    /* Let native radio-button theme transitions settle before capturing their visual state. */
    Sleep(250);
    RedrawWindow(w, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    PrintWindow(w, memory, 0);
    SelectObject(memory, old);
    if (pixels && GetDIBits(memory, bitmap, 0, info.bmiHeader.biHeight, pixels, &info, DIB_RGB_COLORS)) {
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
        header.bfSize = header.bfOffBits + bytes;
        file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            ok = WriteFile(file, &header, sizeof(header), &written, NULL) &&
                 WriteFile(file, &info.bmiHeader, sizeof(info.bmiHeader), &written, NULL) &&
                 WriteFile(file, pixels, bytes, &written, NULL);
            CloseHandle(file);
        }
    }
    free(pixels);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(w, dc);
    return ok;
}

/* Verify the entire child control is inside its parent's client rectangle. */
static BOOL inside(HWND parent, HWND child) {
    RECT p, c;
    if (!child)
        return FALSE;
    GetClientRect(parent, &p);
    GetWindowRect(child, &c);
    MapWindowPoints(NULL, parent, (POINT *)&c, 2);
    return c.left >= p.left && c.top >= p.top && c.right <= p.right && c.bottom <= p.bottom;
}

/* Test vertical ordering, real button-state transitions and fully visible colored phone notices. */
static BOOL ui_test(HWND w) {
    RECT a, b, c, area, r;
    WCHAR label[128];
    BOOL ok;
    NetworkLinks fixtures = {0};
    StringCchCopyW(fixtures.wired.mac, 13, L"001122334455");
    StringCchCopyW(fixtures.wifi.mac, 13, L"aabbccddeeff");
    ULONGLONG started = GetTickCount64();
    ok = !choose_wifi(&fixtures) && GetTickCount64() - started >= 10000;
    testChoice = 702;
    ok = choose_wifi(&fixtures) && ok;
    testChoice = 0;
    fixtures.wired.index = 1; fixtures.wifi.index = 2;
    fixtures.wiredConnected = fixtures.wifiConnected = TRUE;
    StringCchCopyW(fixtures.wired.ip, 16, L"10.0.0.2");
    StringCchCopyW(fixtures.wifi.ip, 16, L"10.0.0.1");
    update_network_view(&fixtures);
    configured = FALSE;
    installed = 0;
    refresh_state(TRUE);
    ok = ok && !IsWindowEnabled(GetDlgItem(w, 101)) && !IsWindowEnabled(GetDlgItem(w, 103));
    GetWindowRect(GetDlgItem(w, 110), &a);
    GetWindowRect(GetDlgItem(w, 111), &b);
    GetWindowRect(GetDlgItem(w, 101), &c);
    ok = ok && a.bottom < b.top && b.bottom < c.top;
    ShowWindow(w, SW_SHOWNOACTIVATE);
    UpdateWindow(w);
    ok = preview(w, L"native-menu.bmp") && ok;
    SendMessageW(w, WM_COMMAND, 601, 0);
    if (!aboutWindow)
        ok = FALSE;
    else {
        WCHAR url[128];
        GetDlgItemTextW(aboutWindow, 602, url, 128);
        ok = ok && !wcscmp(url, REPOSITORY_URL) && inside(aboutWindow, GetDlgItem(aboutWindow, 603));
        UpdateWindow(aboutWindow);
        ok = preview(aboutWindow, L"native-about.bmp") && ok;
        SendMessageW(aboutWindow, WM_CLOSE, 0, 0);
        ok = ok && IsWindow(w) && !aboutWindow;
    }
    configured = TRUE;
    installed = 1;
    refresh_state(TRUE);
    SendMessageW(w, WM_COMMAND, 111, 0);
    ok = ok && uiChoice == 2 && IsWindowEnabled(GetDlgItem(w, 105)) && appIcon && smallIcon && headingIcon;
    ok = ok && SendDlgItemMessageW(w, 111, BM_GETCHECK, 0, 0) == BST_CHECKED &&
         SendDlgItemMessageW(w, 110, BM_GETCHECK, 0, 0) == BST_UNCHECKED;
    SendMessageW(w, WM_COMMAND, 105, 0);
    ok = ok && testOperation == 105 && manualOffline && same_adapter(&selectedAdapter, &fixtures.wifi);
    campusState[0] = campusState[1] = 0;
    update_campus_view();
    ok = ok && wcsstr(tray.szTip, L"未上线") && wcsstr(tray.szTip, L"暂停");
    testOperation = 0;
    recheckPending = TRUE;
    resident = TRUE;
    monitor_network();
    resident = FALSE;
    ok = ok && testOperation == 0 && manualOffline;
    SendMessageW(w, WM_COMMAND, 101, 0);
    ok = ok && testOperation == 106 && !manualOffline && preferWifi &&
         same_adapter(&previousWifi, &fixtures.wired);
    campusState[0] = 0; campusState[1] = 1;
    update_campus_view();
    set_status(L"", -1);
    ok = ok && wcsstr(tray.szTip, L"已上线（Wi-Fi）") && !wcsstr(tray.szTip, L"暂停") &&
         wiredIcon && wifiIcon && inside(w, GetDlgItem(w, 531)) && inside(w, GetDlgItem(w, 532));
    SendMessageW(w, WM_COMMAND, 108, 0);
    ok = ok && testOperation == 107 && !manualOffline && preferWifi;
    ok = ok && SendDlgItemMessageW(w, 111, BM_GETCHECK, 0, 0) == BST_CHECKED &&
         SendDlgItemMessageW(w, 110, BM_GETCHECK, 0, 0) == BST_UNCHECKED;
    GetDlgItemTextW(w, 103, label, 128);
    ok = ok && !wcscmp(label, L"卸载自启动");
    UpdateWindow(w);
    ok = preview(w, L"native-menu-installed.bmp") && ok;
    installed = 0;
    refresh_state(TRUE);
    GetDlgItemTextW(w, 103, label, 128);
    ok = ok && !wcscmp(label, L"注册开机自启") && IsWindowEnabled(GetDlgItem(w, 103));
    busy = TRUE;
    startupMode = TRUE;
    show_network_progress(NETWORK_WAITING);
    UpdateWindow(toastWindow);
    ok = ok && toastWindow && inside(toastWindow, toastLabel);
    ok = preview(toastWindow, L"native-wait.bmp") && ok;
    /* Closing a waiting notice must not terminate a startup worker's hidden main window. */
    SendMessageW(toastWindow, WM_CLOSE, 0, 0);
    ok = ok && IsWindow(w) && busy && !toastWindow;
    show_network_progress(NETWORK_READY);
    ok = ok && IsWindow(w) && toastWindow;
    startupMode = FALSE;
    busy = FALSE;
    StringCchCopyW(errorText, 512, L"校园网已连接，可以上网了。");
    notify_result(0);
    UpdateWindow(toastWindow);
    if (!toastWindow || !toastLabel)
        ok = FALSE;
    else {
        HDC dc = GetDC(toastLabel);
        SendMessageW(toastWindow, WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)toastLabel);
        ok = ok && GetTextColor(dc) == GREEN;
        ReleaseDC(toastLabel, dc);
        GetWindowRect(toastWindow, &r);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
        ok = ok && r.right == area.right - px(16) && r.bottom == area.bottom - px(64) &&
             (GetWindowLongPtrW(toastWindow, GWL_EXSTYLE) & WS_EX_NOACTIVATE);
        ok = preview(toastWindow, L"native-toast.bmp") && ok;
        DestroyWindow(toastWindow);
    }
    StringCchCopyW(errorText, 512, L"需要电话验证，请拨打：028-12345678\r\n\r\n按校园网网页提示完成验证后，再点击“上线”。");
    notify_result(3);
    UpdateWindow(toastWindow);
    if (!toastWindow || !toastLabel)
        ok = FALSE;
    else {
        HDC dc = GetDC(toastLabel);
        SendMessageW(toastWindow, WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)toastLabel);
        ok = ok && GetTextColor(dc) == RED;
        ReleaseDC(toastLabel, dc);
        ok = ok && inside(toastWindow, toastLabel) && inside(toastWindow, GetDlgItem(toastWindow, 401));
        GetWindowRect(toastLabel, &a);
        GetWindowRect(GetDlgItem(toastWindow, 401), &b);
        ok = ok && a.bottom < b.top;
        ok = preview(toastWindow, L"native-phone.bmp") && ok;
        DestroyWindow(toastWindow);
    }
    ShowWindow(GetDlgItem(w, 401), SW_SHOW);
    campusVerification = TRUE;
    update_campus_view();
    ok = ok && wcsstr(tray.szTip, L"需要电话验证");
    set_status(L"需要电话验证。完成验证后，可在网页中将本机设为常用设备。", 1);
    ok = ok && inside(w, GetDlgItem(w, 401));
    UpdateWindow(w);
    ok = preview(w, L"native-menu-phone.bmp") && ok;
    if (!ok) {
        GetClientRect(w, &r);
        fprintf(stderr, "UI layout failed: scale=%.2f, client=%ldx%ld, expected=%dx%d.\n", scale, r.right,
                r.bottom, px(544), px(584));
    }
    DestroyWindow(w);
    return ok;
}

/* Serialize account saves, startup changes and network operations with the shared application mutex. */
static DWORD WINAPI operate(void *parameter) {
    int mode = (int)(INT_PTR)parameter, r = 1;
    Config c = {0};
    HANDLE mutex = CreateMutexW(NULL, FALSE, L"Local\\CampusNetworkLogin_2222");
    DWORD lock = mutex ? WaitForSingleObject(mutex, 0) : WAIT_FAILED;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED) {
        r = 6;
        goto done;
    }
    if (mode == 103) {
        if (!config_io(&c, FALSE)) {
            r = 2;
            StringCchCopyW(errorText, 512, L"请先配置账号，再注册开机自启。");
        } else {
            r = task_change(TRUE) && task_status() == 1 ? 0 : 1;
            if (r)
                task_failure(errorText, 512);
            else
                StringCchCopyW(errorText, 512, L"自启动已注册。现在可点击“上线”。");
        }
    } else if (mode == 201 || mode == 202) {
        r = task_change(FALSE) && task_status() == 0 ? 0 : 1;
        if (!r && mode == 202) {
            r = remove_config() ? 0 : 1;
            if (!r) reset_network_session();
            StringCchCopyW(errorText, 512,
                           r ? L"自启动已卸载，但配置未完全删除，请检查文件权限。"
                             : L"已检查：自启动已卸载，账号配置已删除。");
        } else
            StringCchCopyW(errorText, 512,
                           r ? L"卸载未完成：仍有自启项或无法确认状态，请稍后重试。"
                             : L"已检查：自启动已卸载，账号配置已保留。");
    } else if (mode == 102) {
        r = config_io(&editing, TRUE) ? 0 : 1;
        if (!r) reset_network_session();
        StringCchCopyW(errorText, 512,
                       r ? L"配置保存失败。" : L"账号已保存。建议注册开机自启，也可直接连接。");
        SecureZeroMemory(&editing, sizeof(editing));
    } else if (mode == 104 || config_io(&c, FALSE)) {
        int action = mode == 104 ? NETWORK_CHECK_ONLY : (mode == 105 ? NETWORK_DISCONNECT : (mode == 107 ? NETWORK_INSPECT : NETWORK_CONNECT));
        r = connect_network(&c, action, mode == 105 || mode == 107 ? NULL : report_network_progress,
                            &operationAdapter, &previousWifi, &operationStatus);
        if (!r) {
            if (mode == 104) StringCchCopyW(errorText, 512, L"校园网接口检查通过。");
            else if (mode != 107) StringCchCopyW(errorText, 512, mode == 105 ? L"自动重连已暂停，点击“上线”恢复。" : L"可以正常使用网络了。");
        }
    } else {
        r = 2;
        StringCchCopyW(errorText, 512, L"请先打开主程序配置账号。");
    }
    if (mode != 107) write_log(r == 3 ? L"校园网要求电话验证，自动操作已暂停。" : errorText);
    ReleaseMutex(mutex);
done:
    if (mutex)
        CloseHandle(mutex);
    SecureZeroMemory(&c, sizeof(c));
    CoUninitialize();
    PostMessageW(mainWindow, DONE, r, 0);
    return r;
}

/* Start one background operation, disable duplicate clicks, and recover if thread creation fails. */
static void begin(int mode) {
    if (busy)
        return;
    if (mode == 101 || mode == 104 || mode == 105 || mode == 106 || mode == 107) {
        NetworkLinks links;
        if (testMode) links = visibleLinks;
        else if (!network_links(&links)) {
            set_status(L"无法读取网卡状态，请稍后重试。", 1);
            return;
        }
        if (mode == 107) {
            operationAdapter = uiChoice == 2 ? links.wifi : links.wired;
            if (!operationAdapter.index) return;
        } else if (mode == 105 || mode == 106) {
            selectedAdapter = uiChoice == 2 ? links.wifi : links.wired;
            if (!selectedAdapter.index) {
                update_network_view(&links);
                set_status(L"所选网卡已断开，请重新选择。", 1);
                return;
            }
            preferWifi = uiChoice == 2;
            manualOffline = mode == 105;
            selectionPending = choosePending = FALSE;
        } else {
            if (!links.wifi.index || !same_adapter(&observedLinks.wired, &links.wired)) preferWifi = FALSE;
            selectedAdapter = preferWifi && links.wifi.index ? links.wifi :
                              (links.wired.index ? links.wired : links.wifi);
            uiChoice = same_adapter(&selectedAdapter, &links.wifi) && links.wifi.index ? 2 : 1;
        }
        previousWifi = uiChoice == 2 ? (links.wired.index ? links.wired : rememberedWired) :
                                      (links.wifi.index ? links.wifi : rememberedWifi);
        if (links.wifi.index) rememberedWifi = links.wifi;
        if (links.wired.index) rememberedWired = links.wired;
        if (mode != 107) {
            observedLinks = links;
            operationAdapter = selectedAdapter;
            recheckPending = FALSE;
        }
        update_network_view(&links);
        operationWifi = uiChoice == 2;
        operationStatus.selected = operationStatus.other = -1;
        if (mode != 104) campusVerification = FALSE;
    }
    lastMode = mode;
    if (testMode) { testOperation = mode; return; }
    busy = TRUE;
    update_network_view(&visibleLinks);
    EnableWindow(GetDlgItem(mainWindow, 101), FALSE);
    EnableWindow(GetDlgItem(mainWindow, 102), FALSE);
    EnableWindow(GetDlgItem(mainWindow, 103), FALSE);
    if (mode != 107) {
        ShowWindow(GetDlgItem(mainWindow, 401), SW_HIDE);
        set_status(mode == 105 ? L"正在下线所选网卡的校园网会话……" : L"正在处理，请稍候……", -1);
    }
    worker = CreateThread(NULL, 0, operate, (void *)(INT_PTR)mode, 0, NULL);
    if (!worker) {
        busy = FALSE;
        if (mode == 107) statusQueryBlocked = TRUE;
        refresh_state(FALSE);
        set_status(L"无法启动操作，请稍后重试。", 1);
        StringCchCopyW(errorText, 512, L"无法启动操作，请稍后重试。");
        if (startupMode)
            notify_result(1);
    }
}

/* Update the native choice countdown; closing the prompt also keeps the Ethernet default. */
static HRESULT CALLBACK choice_callback(HWND w, UINT notification, WPARAM elapsed, LPARAM lp, LONG_PTR data) {
    (void)lp;
    (void)data;
    if (notification == TDN_TIMER) {
        if (testMode && !choiceCaptured) {
            UpdateWindow(w);
            preview(w, L"native-choice.bmp");
            choiceCaptured = TRUE;
        }
        if (testMode && testChoice) {
            SendMessageW(w, TDM_CLICK_BUTTON, testChoice, 0);
            return S_OK;
        }
        WCHAR text[80];
        unsigned seconds = elapsed >= 10000 ? 0 : 10 - (unsigned)(elapsed / 1000);
        swprintf_s(text, 80, L"%u 秒后自动使用网线 MAC", seconds);
        SendMessageW(w, TDM_SET_ELEMENT_TEXT, TDE_FOOTER, (LPARAM)text);
        if (elapsed >= 10000) SendMessageW(w, TDM_CLICK_BUTTON, 701, 0);
    }
    return S_OK;
}

/* Ask once at automatic startup when both links are ready; default to Ethernet after ten seconds. */
static BOOL choose_wifi(const NetworkLinks *links) {
    WCHAR wired[96], wifi[96];
    TASKDIALOG_BUTTON buttons[2];
    TASKDIALOGCONFIG dialog = {0};
    int answer = 701;
    swprintf_s(wired, 96, L"使用网线 MAC\n%s", links->wired.mac);
    swprintf_s(wifi, 96, L"使用 Wi-Fi MAC\n%s", links->wifi.mac);
    buttons[0].nButtonID = 701; buttons[0].pszButtonText = wired;
    buttons[1].nButtonID = 702; buttons[1].pszButtonText = wifi;
    dialog.cbSize = sizeof(dialog);
    dialog.hwndParent = mainWindow;
    dialog.hInstance = instance;
    dialog.dwFlags = TDF_USE_COMMAND_LINKS | TDF_CALLBACK_TIMER | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    dialog.pszWindowTitle = L"校园网连接方式";
    dialog.pszMainInstruction = L"Wi-Fi 和网线均已连接";
    dialog.pszContent = L"选择用于校园网认证的网卡。";
    dialog.pszFooter = L"10 秒后自动使用网线 MAC";
    dialog.cButtons = 2;
    dialog.pButtons = buttons;
    dialog.nDefaultButton = 701;
    dialog.pfCallback = choice_callback;
    choosing = TRUE;
    choiceCaptured = FALSE;
    HRESULT result = TaskDialogIndirect(&dialog, &answer, NULL, NULL);
    choosing = FALSE;
    return SUCCEEDED(result) && answer == 702;
}

/* Poll physical link/address changes without retrying rejected credentials or verification prompts. */
static void monitor_network(void) {
    NetworkLinks links;
    NetworkAdapter desired;
    if (!resident || choosing) return;
    if (testMode) links = visibleLinks;
    else if (!network_links(&links)) return;
    update_network_view(&links);
    if (busy || configOpen || !configured) return;
    if (!links.wifi.index || !same_adapter(&observedLinks.wired, &links.wired)) preferWifi = FALSE;
    if (!manualOffline && !selectionPending && choosePending && (links.wired.index || links.wifi.index)) {
        choosePending = FALSE;
        if (links.wired.index && links.wifi.index) preferWifi = choose_wifi(&links);
        recheckPending = TRUE;
    }
    desired = preferWifi && links.wifi.index ? links.wifi : (links.wired.index ? links.wired : links.wifi);
    if (links.wifi.index) rememberedWifi = links.wifi;
    if (links.wired.index) rememberedWired = links.wired;
    observedLinks = links;
    if (!manualOffline && !selectionPending && desired.index && (recheckPending || !same_adapter(&selectedAdapter, &desired))) {
        begin(101);
        return;
    } else if (!desired.index) {
        if (selectedAdapter.index) set_status(L"尚未连接校园网 Wi-Fi 或网线。", -1);
        ZeroMemory(&selectedAdapter, sizeof(selectedAdapter));
    }
    if (!testMode && desired.index && !statusQueryBlocked && GetTickCount64() >= nextStatusCheck)
        begin(107);
}

/* Maintain a tray entry so closing the main window keeps cable detection available. */
static void add_tray(void) {
    trayAdded = FALSE;
    ZeroMemory(&tray, sizeof(tray));
    tray.cbSize = sizeof(tray);
    tray.hWnd = mainWindow;
    tray.uID = 1;
    tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray.uCallbackMessage = TRAY_MESSAGE;
    tray.hIcon = smallIcon;
    update_campus_view();
    trayAdded = Shell_NotifyIconW(NIM_ADD, &tray);
}

/* Open the main window or explicitly stop the resident monitor from the tray menu. */
static void tray_action(LPARAM event) {
    if (event == WM_LBUTTONDBLCLK) {
        ShowWindow(mainWindow, SW_SHOW);
        SetForegroundWindow(mainWindow);
    } else if (event == WM_RBUTTONUP) {
        POINT point;
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 801, L"打开主窗口");
        AppendMenuW(menu, MF_STRING | (busy || configOpen || choosing ? MF_GRAYED : 0), 802, L"退出");
        GetCursorPos(&point);
        SetForegroundWindow(mainWindow);
        int action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, mainWindow, NULL);
        DestroyMenu(menu);
        if (action == 801) tray_action(WM_LBUTTONDBLCLK);
        else if (action == 802) DestroyWindow(mainWindow);
        PostMessageW(mainWindow, WM_NULL, 0, 0);
    }
}

/* Validate and save the local account form; a blank password retains the original account's password. */
static LRESULT CALLBACK config_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CTLCOLORSTATIC)
        return paint_text(wp, lp);
    if (msg == WM_DRAWITEM)
        return draw_button(lp);
    if (msg == WM_CREATE) {
        HWND combo;
        ZeroMemory(&editing, sizeof(editing));
        config_io(&editing, FALSE);
        control(w, L"STATIC", L"校园网账号", 0, 24, 20, 412, 24, 0);
        control(w, L"EDIT", editing.account, WS_BORDER | ES_AUTOHSCROLL, 24, 48, 412, 30, 301);
        control(w, L"STATIC", L"密码（原账号不修改密码时留空）", 0, 24, 92, 412, 24, 0);
        control(w, L"EDIT", L"", WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL, 24, 120, 412, 30, 302);
        control(w, L"STATIC", L"运营商", 0, 24, 164, 412, 24, 0);
        combo = control(w, L"COMBOBOX", L"", CBS_DROPDOWNLIST, 24, 192, 412, 150, 303);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"移动");
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"联通");
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"电信");
        SendMessageW(combo, CB_SELECTSTRING, (WPARAM)-1, (LPARAM)editing.provider);
        control(w, L"STATIC", L"完整套餐名（可选，多个套餐时填写）", 0, 24, 240, 412, 24, 0);
        control(w, L"EDIT", editing.package, WS_BORDER | ES_AUTOHSCROLL, 24, 268, 412, 30, 304);
        control(w, L"STATIC", L"账号信息将加密保存在这台电脑上。", 0, 24, 324, 412, 24, 0);
        control(w, L"BUTTON", L"保存账号", 0, 24, 374, 254, 44, 305);
        control(w, L"BUTTON", L"取消", 0, 294, 374, 142, 44, 306);
        SendDlgItemMessageW(w, 301, EM_SETLIMITTEXT, 255, 0);
        SendDlgItemMessageW(w, 302, EM_SETLIMITTEXT, 255, 0);
        SendDlgItemMessageW(w, 304, EM_SETLIMITTEXT, 511, 0);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == 305) {
        WCHAR account[256], password[256];
        int sel = (int)SendDlgItemMessageW(w, 303, CB_GETCURSEL, 0, 0);
        GetDlgItemTextW(w, 301, account, 256);
        GetDlgItemTextW(w, 302, password, 256);
        if (!*account || sel < 0 ||
            (!*password && (!editing.password[0] || wcscmp(account, editing.account)))) {
            MessageBoxW(w, L"请填写账号、密码并选择运营商。", L"配置提示", MB_OK);
            SecureZeroMemory(password, sizeof(password));
            return 0;
        }
        StringCchCopyW(editing.account, 256, account);
        if (*password)
            StringCchCopyW(editing.password, 256, password);
        SecureZeroMemory(password, sizeof(password));
        SendDlgItemMessageW(w, 303, CB_GETLBTEXT, sel, (LPARAM)editing.provider);
        GetDlgItemTextW(w, 304, editing.package, 512);
        SetDlgItemTextW(w, 302, L"");
        DestroyWindow(w);
        begin(102);
        return 0;
    }
    if ((msg == WM_COMMAND && LOWORD(wp) == 306) || msg == WM_CLOSE) {
        SecureZeroMemory(&editing, sizeof(editing));
        DestroyWindow(w);
        return 0;
    }
    if (msg == WM_DESTROY) {
        configOpen = FALSE;
        EnableWindow(mainWindow, TRUE);
        SetForegroundWindow(mainWindow);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

/* Create windows from a desired client size, so the title bar never consumes control space. */
static HWND window(const WCHAR *kind, const WCHAR *title, int width, int height, HWND owner) {
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | (owner ? 0 : WS_MINIMIZEBOX);
    RECT r = {0, 0, px(width), px(height)}, area;
    AdjustWindowRectEx(&r, style, FALSE, 0);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    return CreateWindowExW(0, kind, title, style, (area.right + area.left - (r.right - r.left)) / 2,
                           (area.bottom + area.top - (r.bottom - r.top)) / 2, r.right - r.left,
                           r.bottom - r.top, owner, NULL, instance, NULL);
}

/* Open the account editor and restore the main window if creating the form fails. */
static void configure(void) {
    HWND w;
    if (configOpen || busy)
        return;
    configOpen = TRUE;
    EnableWindow(mainWindow, FALSE);
    w = window(L"NSUConfig", L"配置账号", 460, 444, mainWindow);
    if (!w) {
        configOpen = FALSE;
        EnableWindow(mainWindow, TRUE);
        set_status(L"配置窗口创建失败，请稍后重试。", 1);
        SecureZeroMemory(&editing, sizeof(editing));
        return;
    }
    ShowWindow(w, SW_SHOW);
    SetFocus(GetDlgItem(w, 301));
}

/* Show selectable project information; only an explicit click opens the fixed public repository. */
static LRESULT CALLBACK about_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CTLCOLORSTATIC)
        return paint_text(wp, lp);
    if (msg == WM_DRAWITEM)
        return draw_button(lp);
    if (msg == WM_CREATE) {
        HWND title = control(w, L"STATIC", L"NSU 校园网自动登录", 0, 24, 24, 472, 36, 510);
        SendMessageW(title, WM_SETFONT, (WPARAM)titleFont, TRUE);
        control(w, L"STATIC", L"v1.3   ·   作者：L1Xu4n", 0, 24, 78, 472, 26, 0);
        control(w, L"EDIT", REPOSITORY_URL, ES_READONLY | ES_AUTOHSCROLL, 24, 116, 472, 30, 602);
        control(w, L"BUTTON", L"打开 GitHub 仓库", 0, 24, 172, 472, 44, 603);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == 603) {
        if ((INT_PTR)ShellExecuteW(w, L"open", REPOSITORY_URL, NULL, NULL, SW_SHOWNORMAL) <= 32)
            MessageBoxW(w, L"浏览器未能打开，请复制仓库地址后手动访问。", L"打开仓库",
                        MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (msg == WM_CLOSE) {
        DestroyWindow(w);
        return 0;
    }
    if (msg == WM_DESTROY) {
        aboutWindow = NULL;
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

/* Handle guided setup, the startup toggle, verification links and background-operation results. */
static LRESULT CALLBACK main_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == 9) { monitor_network(); return 0; }
    if (msg == TRAY_MESSAGE) { tray_action(lp); return 0; }
    if (taskbarCreated && msg == taskbarCreated) { add_tray(); return 0; }
    if (msg == INSTANCE_MESSAGE) {
        if (wp) recheckPending = TRUE;
        else { ShowWindow(w, SW_SHOW); SetForegroundWindow(w); }
        return 0;
    }
    if ((msg == WM_WTSSESSION_CHANGE && wp == WTS_SESSION_UNLOCK) ||
        (msg == WM_POWERBROADCAST && wp == PBT_APMRESUMEAUTOMATIC)) {
        recheckPending = TRUE;
        return TRUE;
    }
    /* UI fixtures deliberately render 150% windows even on a smaller CI desktop.
       Production still fits its scale to the available work area. */
    if (msg == WM_GETMINMAXINFO && testMode) {
        MINMAXINFO *limits = (MINMAXINFO *)lp;
        limits->ptMaxTrackSize.x = px(1200);
        limits->ptMaxTrackSize.y = px(1200);
        return 0;
    }
    if (msg == WM_CTLCOLORSTATIC)
        return paint_text(wp, lp);
    if (msg == WM_DRAWITEM)
        return draw_button(lp);
    if (msg == WM_CREATE) {
        HWND heading;
        mainWindow = w;
        HWND logo = control(w, L"STATIC", L"", SS_ICON | SS_REALSIZECONTROL, 24, 24, 48, 48, 530);
        SendMessageW(logo, STM_SETICON, (WPARAM)headingIcon, 0);
        heading = control(w, L"STATIC", L"NSU 校园网", 0, 88, 30, 320, 40, 510);
        SendMessageW(heading, WM_SETFONT, (WPARAM)titleFont, TRUE);
        control(w, L"BUTTON", L"关于", 0, 448, 32, 72, 32, 601);
        HWND wiredMark = control(w, L"STATIC", L"", SS_ICON | SS_REALSIZECONTROL, 28, 120, 24, 24, 531);
        SendMessageW(wiredMark, STM_SETICON, (WPARAM)wiredIcon, 0);
        control(w, L"BUTTON", L"网线", BS_AUTORADIOBUTTON | WS_GROUP, 68, 116, 160, 32, 110);
        control(w, L"STATIC", L"", SS_RIGHT, 250, 120, 266, 28, 520);
        control(w, L"STATIC", L"", 0, 68, 154, 448, 28, 522);
        HWND wifiMark = control(w, L"STATIC", L"", SS_ICON | SS_REALSIZECONTROL, 28, 212, 24, 24, 532);
        SendMessageW(wifiMark, STM_SETICON, (WPARAM)wifiIcon, 0);
        control(w, L"BUTTON", L"Wi-Fi", BS_AUTORADIOBUTTON, 68, 208, 160, 32, 111);
        control(w, L"STATIC", L"", SS_RIGHT, 250, 212, 266, 28, 521);
        control(w, L"STATIC", L"", 0, 68, 246, 448, 28, 523);
        control(w, L"BUTTON", L"上线", WS_GROUP, 24, 308, 310, 52, 101);
        control(w, L"BUTTON", L"下线", 0, 350, 308, 170, 52, 105);
        campusLabel = control(w, L"STATIC", L"校园网 · 状态待确认", 0, 24, 386, 404, 32, 524);
        SendMessageW(campusLabel, WM_SETFONT, (WPARAM)buttonFont, TRUE);
        control(w, L"BUTTON", L"刷新", 0, 448, 384, 72, 32, 108);
        statusLabel = control(w, L"STATIC", L"", 0, 24, 426, 496, 48, 500);
        control(w, L"BUTTON", L"打开验证网页", 0, 24, 478, 496, 32, 401);
        control(w, L"BUTTON", L"配置账号", 0, 24, 516, 230, 44, 102);
        control(w, L"BUTTON", L"注册开机自启", 0, 270, 516, 250, 44, 103);
        ShowWindow(GetDlgItem(w, 401), SW_HIDE);
        refresh_state(TRUE);
        return 0;
    }
    if (msg == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE && statusLabel && !busy && !configOpen) {
        refresh_state(FALSE);
        return 0;
    }
    if (msg == WM_COMMAND) {
        int id = LOWORD(wp);
        if (id == 601) {
            if (!aboutWindow)
                aboutWindow = window(L"NSUAbout", L"关于", 520, 244, w);
            if (aboutWindow) {
                ShowWindow(aboutWindow, SW_SHOW);
                SetForegroundWindow(aboutWindow);
            }
            return 0;
        }
        if (busy || choosing)
            return 0;
        startupMode = FALSE;
        if (id == 110 || id == 111) {
            uiChoice = id == 110 ? 1 : 2;
            selectionPending = TRUE;
            update_network_view(&visibleLinks);
            set_status(id == 110 ? L"已选择网线，点击“上线”或“下线”。" : L"已选择 Wi-Fi，点击“上线”或“下线”。", -1);
            return 0;
        }
        if (id == 401) {
            ShellExecuteW(w, L"open", L"http://2.2.2.2/", NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        if (id == 102)
            configure();
        else if (id == 101)
            begin(106);
        else if (id == 105)
            begin(105);
        else if (id == 108) {
            statusQueryBlocked = FALSE;
            begin(107);
        }
        else if (id == 103) {
            refresh_state(FALSE);
            if (installed < 0) {
                set_status(L"无法确认自启状态，请检查 Windows 任务计划程序后重试。", 1);
                return 0;
            }
            if (installed) {
                int answer =
                    MessageBoxW(w,
                                L"卸载自启动时，是否一并删除账号配置？\r\n\r\n是：卸载自启动并删除账号配置（"
                                L"包括旧版）。\r\n否：仅卸载自启动，保留账号配置。\r\n取消：不做修改。",
                                L"卸载自启动", MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
                if (answer == IDYES)
                    begin(202);
                else if (answer == IDNO)
                    begin(201);
            } else if (configured)
                begin(103);
            else
                set_status(L"请先配置账号。", 1);
        }
        return 0;
    }
    if (msg == NETWORK_PROGRESS) {
        if (busy && (lastMode == 101 || lastMode == 104 || lastMode == 106))
            show_network_progress((int)wp);
        return 0;
    }
    if (msg == DONE) {
        busy = FALSE;
        if (worker) {
            CloseHandle(worker);
            worker = NULL;
        }
        lastExit = (int)wp;
        if (lastMode == 101 || lastMode == 105 || lastMode == 106 || lastMode == 107) {
            int index = operationWifi ? 1 : 0;
            const NetworkAdapter *current = index ? &visibleLinks.wifi : &visibleLinks.wired;
            const NetworkAdapter *other = index ? &visibleLinks.wired : &visibleLinks.wifi;
            campusState[index] = same_adapter(current, &operationAdapter) ? operationStatus.selected : -1;
            campusState[1 - index] = same_adapter(other, &previousWifi) ? operationStatus.other : -1;
            campusVerification = wp == 3;
            statusQueryBlocked = wp != 0 && same_adapter(current, &operationAdapter);
            nextStatusCheck = GetTickCount64() + 20000;
        } else if ((lastMode == 102 || lastMode == 202) && wp == 0) {
            campusState[0] = campusState[1] = -1;
            campusVerification = statusQueryBlocked = FALSE;
            nextStatusCheck = 0;
        }
        refresh_state(FALSE);
        if (lastMode == 107) {
            if (wp == 3) {
                set_status(L"请按提示完成电话验证。", 1);
                ShowWindow(GetDlgItem(w, 401), SW_SHOW);
                notify_result(3);
            } else if (wp) set_status(L"状态查询失败，点击“刷新”重试。", 1);
            else {
                ShowWindow(GetDlgItem(w, 401), SW_HIDE);
                set_status(manualOffline ? L"自动重连已暂停，点击“上线”恢复。" : L"", -1);
            }
            return 0;
        }
        if (wp == 6)
            StringCchCopyW(errorText, 512, L"已有操作正在运行，请稍后重试。");
        WCHAR summary[512];
        StringCchCopyW(summary, 512, errorText);
        WCHAR *newline = wcschr(summary, L'\r');
        if (newline) *newline = 0;
        set_status(summary, wp == 0 ? 0 : 1);
        ShowWindow(GetDlgItem(w, 401), wp == 3 ? SW_SHOW : SW_HIDE);
        if (lastMode == 101 || lastMode == 104 || lastMode == 105 || lastMode == 106)
            notify_result((int)wp);
        return 0;
    }
    if (msg == WM_CLOSE) {
        if (busy || configOpen || choosing)
            return 0;
        if (resident && trayAdded) ShowWindow(w, SW_HIDE);
        else DestroyWindow(w);
        return 0;
    }
    if (msg == WM_DESTROY) {
        if (resident) {
            KillTimer(w, 9);
            WTSUnRegisterSessionNotification(w);
            if (trayAdded) Shell_NotifyIconW(NIM_DELETE, &tray);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

/* Release all UI-owned fonts and brushes after windows have been destroyed. */
static void dispose_ui(void) {
    DestroyIcon(appIcon);
    DestroyIcon(smallIcon);
    DestroyIcon(headingIcon);
    DestroyIcon(wiredIcon);
    DestroyIcon(wifiIcon);
    reset_network_session();
    DeleteObject(titleFont);
    DeleteObject(buttonFont);
    DeleteObject(font);
    DeleteObject(background);
    DeleteObject(whiteBrush);
}

/* Initialize the native GUI or isolated offline tests, without any script runtime. */
int WINAPI wWinMain(HINSTANCE h, HINSTANCE previous, PWSTR cmd, int show) {
    WNDCLASSW cls = {0};
    MSG msg;
    HWND w;
    HDC dc;
    RECT area;
    WSADATA sockets;
    (void)previous;
    (void)show;
    instance = h;
    if (WSAStartup(MAKEWORD(2, 2), &sockets)) return 1;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    /* Explicit repair/install entry uses the same guarded operation as the GUI, without login requests. */
    if (!wcscmp(cmd, L"--install")) {
        int result = (int)operate((void *)(INT_PTR)103);
        if (result)
            fwprintf(stderr, L"%s\n", errorText);
        CoUninitialize();
        return result;
    }
    if (!wcscmp(cmd, L"--self-test")) {
        int r = native_tests();
        if (!transport_tests()) { fputs("Transport tests failed.\n", stderr); r = 1; }
        if (!task_change(2)) {
            fputs("Task XML validation failed.\n", stderr);
            r = 1;
        }
        if (!system_tests()) {
            fputs("Isolated config tests failed.\n", stderr);
            r = 1;
        }
        if (!task_tests()) {
            fputs("Isolated task lifecycle tests failed.\n", stderr);
            r = 1;
        }
        CoUninitialize();
        return r;
    }
    testMode = !wcscmp(cmd, L"--ui-test") || !wcscmp(cmd, L"--ui-test-large");
    if (*cmd && wcscmp(cmd, L"--startup") && wcscmp(cmd, L"--check-only") && !testMode) {
        CoUninitialize();
        return 2;
    }
    resident = !testMode && wcscmp(cmd, L"--check-only") != 0;
    if (resident) {
        residentMutex = CreateMutexW(NULL, TRUE, L"Local\\NSU_Net_Resident_1_1");
        if (!residentMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND existing = FindWindowW(L"NSUNative", NULL);
            if (existing) PostMessageW(existing, INSTANCE_MESSAGE, *cmd != 0, 0);
            if (residentMutex) CloseHandle(residentMutex);
            CoUninitialize();
            WSACleanup();
            return 0;
        }
    }
    SetProcessDPIAware();
    dc = GetDC(NULL);
    scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.0;
    ReleaseDC(NULL, dc);
    if (testMode)
        scale = 1.0;
    if (!wcscmp(cmd, L"--ui-test-large"))
        scale = 1.5;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    if (!testMode && scale > (area.bottom - area.top - 40) / 640.0)
        scale = (area.bottom - area.top - 40) / 640.0;
    if (scale < 0.75)
        scale = 0.75;
    font = CreateFontW(-px(17), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    titleFont = CreateFontW(-px(28), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                            CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    buttonFont = CreateFontW(-px(18), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    background = CreateSolidBrush(BACK);
    whiteBrush = CreateSolidBrush(RGB(255, 255, 255));
    appIcon = (HICON)LoadImageW(h, MAKEINTRESOURCEW(200), IMAGE_ICON, px(32), px(32), 0);
    smallIcon = (HICON)LoadImageW(h, MAKEINTRESOURCEW(200), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    headingIcon = (HICON)LoadImageW(h, MAKEINTRESOURCEW(200), IMAGE_ICON, px(48), px(48), 0);
    wiredIcon = (HICON)LoadImageW(h, MAKEINTRESOURCEW(201), IMAGE_ICON, px(24), px(24), 0);
    wifiIcon = (HICON)LoadImageW(h, MAKEINTRESOURCEW(202), IMAGE_ICON, px(24), px(24), 0);
    cls.hInstance = h;
    cls.hIcon = appIcon;
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hbrBackground = background;
    cls.lpfnWndProc = main_proc;
    cls.lpszClassName = L"NSUNative";
    RegisterClassW(&cls);
    cls.lpfnWndProc = config_proc;
    cls.lpszClassName = L"NSUConfig";
    RegisterClassW(&cls);
    cls.lpfnWndProc = about_proc;
    cls.lpszClassName = L"NSUAbout";
    RegisterClassW(&cls);
    cls.hbrBackground = whiteBrush;
    cls.lpfnWndProc = toast_proc;
    cls.lpszClassName = L"NSUToast";
    RegisterClassW(&cls);
    w = window(L"NSUNative", L"NSU 校园网自动登录", 544, 584, NULL);
    if (!w) {
        dispose_ui();
        CoUninitialize();
        return 1;
    }
    SendMessageW(w, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
    if (testMode) {
        BOOL ok = ui_test(w);
        dispose_ui();
        CoUninitialize();
        return ok ? 0 : 1;
    }
    startupMode = *cmd != 0;
    if (resident) {
        taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        add_tray();
        WTSRegisterSessionNotification(w, NOTIFY_FOR_THIS_SESSION);
        network_links(&observedLinks);
        update_network_view(&observedLinks);
        rememberedWifi = observedLinks.wifi;
        rememberedWired = observedLinks.wired;
        choosePending = startupMode;
        recheckPending = startupMode;
        if (!startupMode)
            selectedAdapter = observedLinks.wired.index ? observedLinks.wired : observedLinks.wifi;
        SetTimer(w, 9, 2000, NULL);
        if (startupMode && configured)
            notify_message(4, L"正在等待校园网连接。\r\n连接 Wi-Fi 或插入网线后将自动登录；关闭提示不影响后台检测。");
        else ShowWindow(w, SW_SHOW);
    } else if (startupMode)
        begin(104);
    else
        ShowWindow(w, SW_SHOW);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(GetActiveWindow(), &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    dispose_ui();
    if (residentMutex) CloseHandle(residentMutex);
    CoUninitialize();
    WSACleanup();
    return lastExit;
}
