// 设置窗口：开关两个功能、自定义长按时长、开机自启动、最大化时不隐藏任务栏的程序。
#include "common.h"

namespace app {
namespace {

constexpr wchar_t kClassName[] = L"TaskbarPopupSettings";
constexpr DWORD kStyle = WS_CAPTION | WS_SYSMENU | WS_POPUP;
constexpr DWORD kExStyle = WS_EX_DLGMODALFRAME;

enum {
    IDC_AUTOHIDE = 1001,
    IDC_LONGPRESS,
    IDC_MS_LABEL,
    IDC_MS_EDIT,
    IDC_MS_SPIN,
    IDC_MS_HINT,
    IDC_PINNED,
    IDC_AUTOSTART,
    IDC_EXCLUDE_LABEL,
    IDC_EXCLUDE,
    IDC_EXCLUDE_HINT,
};

HWND s_dlg = nullptr;
HFONT s_font = nullptr;
UINT s_dpi = 96;

int S(int v) { return MulDiv(v, static_cast<int>(s_dpi), 96); }

HWND AddControl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id,
                DWORD exStyle = 0) {
    HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), s_dlg,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
    return c;
}

void UpdateEnabled() {
    BOOL on = IsDlgButtonChecked(s_dlg, IDC_LONGPRESS) == BST_CHECKED;
    for (int id : {IDC_MS_LABEL, IDC_MS_EDIT, IDC_MS_SPIN, IDC_MS_HINT, IDC_PINNED})
        EnableWindow(GetDlgItem(s_dlg, id), on);
}

void CreateControls() {
    s_dpi = GetDpiForWindow(s_dlg);
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, s_dpi);
    s_font = CreateFontIndirectW(&ncm.lfMessageFont);

    AddControl(L"BUTTON", L"窗口最大化或全屏时隐藏任务栏，窗口铺满屏幕", BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, 16,
               14, 320, 22, IDC_AUTOHIDE);
    AddControl(L"BUTTON", L"长按 Win 键弹出迷你任务栏", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 42, 300, 22, IDC_LONGPRESS);
    AddControl(L"STATIC", L"长按时长（毫秒）：", SS_LEFT | SS_CENTERIMAGE, 36, 72, 128, 24, IDC_MS_LABEL);
    HWND edit = AddControl(L"EDIT", L"", ES_NUMBER | ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP, 166, 72, 90, 24,
                           IDC_MS_EDIT, WS_EX_CLIENTEDGE);
    HWND spin = CreateWindowExW(0, UPDOWN_CLASSW, nullptr,
                                WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                                0, 0, 0, 0, s_dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MS_SPIN)),
                                g_instance, nullptr);
    SendMessageW(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(edit), 0);
    SendMessageW(spin, UDM_SETRANGE32, kMinLongPressMs, kMaxLongPressMs);
    UDACCEL accel = {0, 100};
    SendMessageW(spin, UDM_SETACCEL, 1, reinterpret_cast<LPARAM>(&accel));
    SendMessageW(spin, UDM_SETPOS32, 0, g_settings.longPressMs);

    wchar_t hint[96];
    swprintf(hint, 96, L"可设置 %d – %d 毫秒，默认 %d", kMinLongPressMs, kMaxLongPressMs, kDefaultLongPressMs);
    AddControl(L"STATIC", hint, SS_LEFT, 36, 100, 280, 20, IDC_MS_HINT);
    AddControl(L"BUTTON", L"显示固定在任务栏的应用", BS_AUTOCHECKBOX | WS_TABSTOP, 34, 124, 300, 22, IDC_PINNED);
    AddControl(L"BUTTON", L"开机自动启动", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 156, 300, 22, IDC_AUTOSTART);
    AddControl(L"STATIC", L"最大化时不隐藏任务栏的程序（每行一个，如 notepad.exe）：", SS_LEFT, 16, 190, 330, 20,
               IDC_EXCLUDE_LABEL);
    std::wstring exclude;
    for (const std::wstring& name : g_settings.excludeApps) exclude += (exclude.empty() ? L"" : L"\r\n") + name;
    AddControl(L"EDIT", exclude.c_str(),
               ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP, 16, 212, 320, 76, IDC_EXCLUDE,
               WS_EX_CLIENTEDGE);
    AddControl(L"STATIC", L"也可以在迷你任务栏里右键运行中的程序来添加或去掉", SS_LEFT, 16, 292, 330, 20,
               IDC_EXCLUDE_HINT);
    AddControl(L"BUTTON", L"确定", BS_DEFPUSHBUTTON | WS_TABSTOP, 160, 322, 84, 28, IDOK);
    AddControl(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, 252, 322, 84, 28, IDCANCEL);

    CheckDlgButton(s_dlg, IDC_AUTOHIDE, g_settings.autoHideOnFullscreen ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_dlg, IDC_LONGPRESS, g_settings.longPressPopup ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_dlg, IDC_PINNED, g_settings.showPinnedApps ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_dlg, IDC_AUTOSTART, IsAutoStartEnabled() ? BST_CHECKED : BST_UNCHECKED);
    UpdateEnabled();

    // 按 DPI 调整窗口大小，并居中到鼠标所在的显示器
    RECT rc = {0, 0, S(352), S(366)};
    AdjustWindowRectExForDpi(&rc, kStyle, FALSE, kExStyle, s_dpi);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    SetWindowPos(s_dlg, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2,
                 w, h, SWP_NOZORDER);
}

bool Apply() {
    BOOL ok = FALSE;
    UINT ms = GetDlgItemInt(s_dlg, IDC_MS_EDIT, &ok, FALSE);
    if (!ok || ms < static_cast<UINT>(kMinLongPressMs) || ms > static_cast<UINT>(kMaxLongPressMs)) {
        wchar_t msg[128];
        swprintf(msg, 128, L"长按时长需要在 %d 到 %d 毫秒之间。", kMinLongPressMs, kMaxLongPressMs);
        MessageBoxW(s_dlg, msg, L"TaskbarPopup", MB_ICONWARNING);
        SetFocus(GetDlgItem(s_dlg, IDC_MS_EDIT));
        return false;
    }

    g_settings.autoHideOnFullscreen = IsDlgButtonChecked(s_dlg, IDC_AUTOHIDE) == BST_CHECKED;
    g_settings.longPressPopup = IsDlgButtonChecked(s_dlg, IDC_LONGPRESS) == BST_CHECKED;
    g_settings.longPressMs = static_cast<int>(ms);
    g_settings.showPinnedApps = IsDlgButtonChecked(s_dlg, IDC_PINNED) == BST_CHECKED;
    std::wstring text(GetWindowTextLengthW(GetDlgItem(s_dlg, IDC_EXCLUDE)) + 1, L'\0');
    text.resize(GetDlgItemTextW(s_dlg, IDC_EXCLUDE, text.data(), static_cast<int>(text.size())));
    std::vector<std::wstring> exclude;
    for (size_t start = 0; start <= text.size();) {
        size_t end = text.find_first_of(L"\r\n;", start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring name = NormalizeExeName(text.substr(start, end - start));
        if (!name.empty() && std::find(exclude.begin(), exclude.end(), name) == exclude.end()) exclude.push_back(name);
        start = end + 1;
    }
    g_settings.excludeApps = exclude;
    SaveSettings();
    SetAutoStart(IsDlgButtonChecked(s_dlg, IDC_AUTOSTART) == BST_CHECKED);
    ApplySettings();
    return true;
}

LRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK:
                    if (Apply()) DestroyWindow(hwnd);
                    return 0;
                case IDCANCEL:
                    DestroyWindow(hwnd);
                    return 0;
                case IDC_LONGPRESS:
                    if (HIWORD(wParam) == BN_CLICKED) UpdateEnabled();
                    return 0;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            s_dlg = nullptr;
            if (s_font) DeleteObject(s_font);
            s_font = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

HWND SettingsDialog_Hwnd() { return s_dlg; }

void SettingsDialog_Show() {
    if (s_dlg) {
        ShowWindow(s_dlg, SW_RESTORE);
        ForceForeground(s_dlg);
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = DialogProc;
        wc.hInstance = g_instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kClassName;
        wc.hIcon = reinterpret_cast<HICON>(SendMessageW(g_mainWnd, WM_GETICON, ICON_BIG, 0));
        RegisterClassExW(&wc);
        registered = true;
    }

    s_dlg = CreateWindowExW(kExStyle, kClassName, L"TaskbarPopup 设置", kStyle, CW_USEDEFAULT, CW_USEDEFAULT, 360, 260,
                            nullptr, nullptr, g_instance, nullptr);
    if (!s_dlg) return;
    CreateControls();
    ShowWindow(s_dlg, SW_SHOW);
    ForceForeground(s_dlg);
    SetFocus(GetDlgItem(s_dlg, IDC_MS_EDIT));
}

}  // namespace app
