#include "native.h"

#pragma comment(                                                                                             \
    linker,                                                                                                  \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

WCHAR errorText[512];
static HINSTANCE instance;
static HWND mainWindow, statusLabel, toastWindow, toastLabel;
static HWND aboutWindow;
#define REPOSITORY_URL L"https://github.com/L1Xu4n/NSU-net-autologin"
static HFONT font, titleFont, buttonFont;
static HBRUSH background, whiteBrush;
static BOOL busy, configOpen, startupMode, testMode, configured;
static int installed, lastExit, lastMode, statusTone = -1;
static HANDLE worker;
static Config editing;
static COLORREF toastTextColor;
static double scale = 1.0;
#define DONE (WM_APP + 1)
#define NETWORK_PROGRESS (WM_APP + 2)
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
    if (!wcscmp(kind, L"BUTTON"))
        style = (style & ~BS_TYPEMASK) | BS_OWNERDRAW | WS_TABSTOP;
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
    if ((HWND)lp == statusLabel)
        color = statusTone < 0 ? INK : (statusTone == 0 ? GREEN : RED);
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
    SetDlgItemTextW(mainWindow, 512,
                    configured ? L"账号已配置，可随时修改。" : L"第一步：填写校园网账号、密码和运营商。");
    SetDlgItemTextW(mainWindow, 513,
                    installed < 0 ? L"自启动状态暂时无法读取，点击重试。"
                                  : (installed ? L"已注册，可在这里取消并选择是否保留账号。"
                                               : L"第二步：登录或解锁 Windows 后自动连接。"));
    EnableWindow(GetDlgItem(mainWindow, 101), !busy && configured);
    EnableWindow(GetDlgItem(mainWindow, 102), !busy);
    EnableWindow(GetDlgItem(mainWindow, 103), !busy && (configured || installed != 0));
    if (explain) {
        if (!configured)
            set_status(L"请先配置账号，再注册开机自启。", -1);
        else if (installed == 0)
            set_status(L"账号已就绪。建议注册开机自启，也可直接连接。", -1);
        else if (installed == 1)
            set_status(L"账号与自启动已就绪，点击“立即连接”即可。", -1);
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
        if (startupMode && !busy)
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
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
    toastTextColor = result == 4 ? BLUE : (result == 0 ? GREEN : RED);
    toastWindow =
        CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"NSUToast",
                        result == 4 ? L"校园网连接进度" : (result == 3 ? L"需要电话验证" : L"校园网连接结果"),
                        WS_CAPTION | WS_SYSMENU, area.right - (outer.right - outer.left) - 16,
                        area.bottom - (outer.bottom - outer.top) - 16, outer.right - outer.left,
                        outer.bottom - outer.top, NULL, NULL, instance, NULL);
    if (!toastWindow) {
        if (startupMode && !busy)
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
    configured = FALSE;
    installed = 0;
    refresh_state(TRUE);
    ok = !IsWindowEnabled(GetDlgItem(w, 101)) && !IsWindowEnabled(GetDlgItem(w, 103));
    GetWindowRect(GetDlgItem(w, 102), &a);
    GetWindowRect(GetDlgItem(w, 103), &b);
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
        ok = ok && r.right == area.right - 16 && r.bottom == area.bottom - 16 &&
             (GetWindowLongPtrW(toastWindow, GWL_EXSTYLE) & WS_EX_NOACTIVATE);
        ok = preview(toastWindow, L"native-toast.bmp") && ok;
        DestroyWindow(toastWindow);
    }
    StringCchCopyW(errorText, 512, PHONE_GUIDANCE);
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
    set_status(L"需要电话验证。完成验证后，可在网页中将本机设为常用设备。", 1);
    ok = ok && inside(w, GetDlgItem(w, 401));
    UpdateWindow(w);
    ok = preview(w, L"native-menu-phone.bmp") && ok;
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
                StringCchCopyW(errorText, 512, L"自启动已注册。现在可点击“立即连接”。");
        }
    } else if (mode == 201 || mode == 202) {
        r = task_change(FALSE) && task_status() == 0 ? 0 : 1;
        if (!r && mode == 202) {
            r = remove_config() ? 0 : 1;
            StringCchCopyW(errorText, 512,
                           r ? L"自启动已卸载，但配置未完全删除，请检查文件权限。"
                             : L"已检查：自启动已卸载，账号配置已删除。");
        } else
            StringCchCopyW(errorText, 512,
                           r ? L"卸载未完成：仍有自启项或无法确认状态，请稍后重试。"
                             : L"已检查：自启动已卸载，账号配置已保留。");
    } else if (mode == 102) {
        r = config_io(&editing, TRUE) ? 0 : 1;
        StringCchCopyW(errorText, 512,
                       r ? L"配置保存失败。" : L"账号已保存。建议注册开机自启，也可直接连接。");
        SecureZeroMemory(&editing, sizeof(editing));
    } else if (mode == 104 || config_io(&c, FALSE)) {
        r = connect_network(&c, mode == 104, report_network_progress);
        if (!r)
            StringCchCopyW(errorText, 512,
                           mode == 104 ? L"校园网接口检查通过。" : L"校园网已连接，可以上网了。");
    } else {
        r = 2;
        StringCchCopyW(errorText, 512, L"请先打开主程序配置账号。");
    }
    write_log(errorText);
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
    lastMode = mode;
    busy = TRUE;
    EnableWindow(GetDlgItem(mainWindow, 101), FALSE);
    EnableWindow(GetDlgItem(mainWindow, 102), FALSE);
    EnableWindow(GetDlgItem(mainWindow, 103), FALSE);
    ShowWindow(GetDlgItem(mainWindow, 401), SW_HIDE);
    set_status(L"正在处理，请稍候……", -1);
    worker = CreateThread(NULL, 0, operate, (void *)(INT_PTR)mode, 0, NULL);
    if (!worker) {
        busy = FALSE;
        refresh_state(FALSE);
        set_status(L"无法启动操作，请稍后重试。", 1);
        StringCchCopyW(errorText, 512, L"无法启动操作，请稍后重试。");
        if (startupMode)
            notify_result(1);
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
        control(w, L"STATIC", L"v1.0   ·   作者：L1Xu4n", 0, 24, 78, 472, 26, 0);
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
    if (msg == WM_CTLCOLORSTATIC)
        return paint_text(wp, lp);
    if (msg == WM_DRAWITEM)
        return draw_button(lp);
    if (msg == WM_CREATE) {
        HWND heading;
        mainWindow = w;
        heading = control(w, L"STATIC", L"NSU 校园网", 0, 24, 24, 370, 36, 510);
        SendMessageW(heading, WM_SETFONT, (WPARAM)titleFont, TRUE);
        control(w, L"BUTTON", L"关于", 0, 414, 26, 72, 32, 601);
        control(w, L"STATIC", L"使用网线或 WIFI 连接“NSU-SDN”后连接", 0, 24, 74, 462, 28, 511);
        control(w, L"STATIC", L"首次使用：先配置账号，再注册开机自启。", 0, 24, 116, 462, 28, 0);
        control(w, L"BUTTON", L"配置账号", 0, 24, 160, 462, 48, 102);
        control(w, L"STATIC", L"", 0, 30, 218, 450, 26, 512);
        control(w, L"BUTTON", L"注册开机自启", 0, 24, 264, 462, 48, 103);
        control(w, L"STATIC", L"", 0, 30, 322, 450, 26, 513);
        control(w, L"BUTTON", L"立即连接", 0, 24, 368, 462, 50, 101);
        control(w, L"STATIC", L"第三步：现在连接校园网。", 0, 30, 428, 450, 24, 0);
        statusLabel = control(w, L"STATIC", L"", 0, 24, 472, 462, 60, 500);
        control(w, L"BUTTON", L"打开验证网页", 0, 24, 544, 462, 40, 401);
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
        if (busy)
            return 0;
        startupMode = FALSE;
        if (id == 401) {
            ShellExecuteW(w, L"open", L"http://2.2.2.2/", NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        if (id == 102)
            configure();
        else if (id == 101)
            begin(101);
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
        if (busy && (lastMode == 101 || lastMode == 104))
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
        refresh_state(FALSE);
        if (wp == 6)
            StringCchCopyW(errorText, 512, L"已有操作正在运行，请稍后重试。");
        set_status(wp == 3 ? L"需要电话验证。完成后建议在网页中将本机设为常用设备。" : errorText,
                   wp == 0 ? 0 : 1);
        ShowWindow(GetDlgItem(w, 401), wp == 3 ? SW_SHOW : SW_HIDE);
        if (lastMode == 101 || lastMode == 104)
            notify_result((int)wp);
        return 0;
    }
    if (msg == WM_CLOSE) {
        if (busy || configOpen)
            return 0;
        DestroyWindow(w);
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

/* Release all UI-owned fonts and brushes after windows have been destroyed. */
static void dispose_ui(void) {
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
    (void)previous;
    (void)show;
    instance = h;
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
    cls.hInstance = h;
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
    w = window(L"NSUNative", L"NSU 校园网自动登录", 510, 608, NULL);
    if (!w) {
        dispose_ui();
        CoUninitialize();
        return 1;
    }
    if (testMode) {
        BOOL ok = ui_test(w);
        dispose_ui();
        CoUninitialize();
        return ok ? 0 : 1;
    }
    startupMode = *cmd != 0;
    if (startupMode)
        begin(!wcscmp(cmd, L"--check-only") ? 104 : 101);
    else
        ShowWindow(w, SW_SHOW);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(GetActiveWindow(), &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    dispose_ui();
    CoUninitialize();
    return lastExit;
}
