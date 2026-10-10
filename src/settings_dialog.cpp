// 设置窗口：开关各项功能、自定义长按时长、迷你任务栏的大小和内容、开机自启动、最大化时不隐藏任务栏的程序。
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
    IDC_SCALE_LABEL,
    IDC_SCALE_EDIT,
    IDC_SCALE_SPIN,
    IDC_SCALE_HINT,
    IDC_PINNED,
    IDC_LEVELS,
    IDC_AUTOSTART,
    IDC_EXCLUDE_LABEL,
    IDC_EXCLUDE,
    IDC_EXCLUDE_HINT,
};

// 各个控件在 96 DPI 下的位置；换到缩放比例不同的显示器上时按新的 DPI 重新摆
struct Place {
    int id, x, y, w, h;
};
constexpr Place kLayout[] = {
    {IDC_AUTOHIDE, 16, 14, 320, 22},       {IDC_LONGPRESS, 16, 42, 300, 22},      {IDC_MS_LABEL, 36, 72, 128, 24},
    {IDC_MS_EDIT, 166, 72, 90, 24},        {IDC_MS_HINT, 36, 100, 280, 20},       {IDC_SCALE_LABEL, 16, 132, 148, 24},
    {IDC_SCALE_EDIT, 166, 132, 90, 24},    {IDC_SCALE_HINT, 36, 160, 280, 20},    {IDC_PINNED, 16, 184, 300, 22},
    {IDC_LEVELS, 16, 210, 300, 22},        {IDC_AUTOSTART, 16, 242, 300, 22},     {IDC_EXCLUDE_LABEL, 16, 276, 330, 20},
    {IDC_EXCLUDE, 16, 298, 320, 76},       {IDC_EXCLUDE_HINT, 16, 378, 330, 20},  {IDOK, 160, 408, 84, 28},
    {IDCANCEL, 252, 408, 84, 28},
};
constexpr int kClientW = 352, kClientH = 452;

HWND s_dlg = nullptr;
HFONT s_font = nullptr;
UINT s_dpi = 96;

// 打开窗口时各项的值。确定时只改用户在窗口里动过的项：窗口开着的时候在迷你任务栏或托盘菜单里改的设置不会被盖掉
struct Initial {
    bool autoHide = false, longPress = false, pinned = false, levels = false, autoStart = false;
    int ms = 0, scale = 0;
    std::vector<std::wstring> exclude;
} s_initial;

int S(int v) { return MulDiv(v, static_cast<int>(s_dpi), 96); }

const Place& PlaceOf(int id) {
    for (const Place& p : kLayout)
        if (p.id == id) return p;
    return kLayout[0];
}

HWND AddControl(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0) {
    const Place& p = PlaceOf(id);
    HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, S(p.x), S(p.y), S(p.w), S(p.h), s_dlg,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
    return c;
}

HFONT CreateDialogFont() {
    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, s_dpi);
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

SIZE WindowSize() {
    RECT rc = {0, 0, S(kClientW), S(kClientH)};
    AdjustWindowRectExForDpi(&rc, kStyle, FALSE, kExStyle, s_dpi);
    return {rc.right - rc.left, rc.bottom - rc.top};
}

// 每行一个（也接受分号），去掉重复的
std::vector<std::wstring> ParseExcludeList(const std::wstring& text) {
    std::vector<std::wstring> list;
    for (size_t start = 0; start <= text.size();) {
        size_t end = text.find_first_of(L"\r\n;", start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring name = NormalizeExeName(text.substr(start, end - start));
        if (!name.empty() && std::find(list.begin(), list.end(), name) == list.end()) list.push_back(name);
        start = end + 1;
    }
    return list;
}

bool Checked(int id) { return IsDlgButtonChecked(s_dlg, id) == BST_CHECKED; }

// 换到 DPI 不同的显示器：字体、控件位置和窗口大小都按新的 DPI 重新算
void OnDpiChanged(UINT dpi, const RECT& suggested) {
    s_dpi = dpi;
    HFONT old = s_font;
    s_font = CreateDialogFont();
    for (const Place& p : kLayout) {
        HWND c = GetDlgItem(s_dlg, p.id);
        SetWindowPos(c, nullptr, S(p.x), S(p.y), S(p.w), S(p.h), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
    }
    // 微调按钮重新贴到输入框右边（会把输入框缩窄一点）
    SendMessageW(GetDlgItem(s_dlg, IDC_MS_SPIN), UDM_SETBUDDY, reinterpret_cast<WPARAM>(GetDlgItem(s_dlg, IDC_MS_EDIT)), 0);
    SendMessageW(GetDlgItem(s_dlg, IDC_SCALE_SPIN), UDM_SETBUDDY,
                 reinterpret_cast<WPARAM>(GetDlgItem(s_dlg, IDC_SCALE_EDIT)), 0);
    if (old) DeleteObject(old);
    SIZE size = WindowSize();
    SetWindowPos(s_dlg, nullptr, suggested.left, suggested.top, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(s_dlg, nullptr, TRUE);
}

// 迷你任务栏的大小和内容不跟着长按开关变灰：左键单击托盘图标也能弹出迷你任务栏
void UpdateEnabled() {
    BOOL on = IsDlgButtonChecked(s_dlg, IDC_LONGPRESS) == BST_CHECKED;
    for (int id : {IDC_MS_LABEL, IDC_MS_EDIT, IDC_MS_SPIN, IDC_MS_HINT})
        EnableWindow(GetDlgItem(s_dlg, id), on);
}

// 数字输入框加右边的微调按钮
HWND AddNumberBox(int editId, int spinId, int minValue, int maxValue, int step, int value) {
    HWND edit = AddControl(L"EDIT", L"", ES_NUMBER | ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP, editId, WS_EX_CLIENTEDGE);
    HWND spin = CreateWindowExW(0, UPDOWN_CLASSW, nullptr,
                                WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                                0, 0, 0, 0, s_dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(spinId)), g_instance,
                                nullptr);
    SendMessageW(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(edit), 0);
    SendMessageW(spin, UDM_SETRANGE32, minValue, maxValue);
    UDACCEL accel = {0, static_cast<UINT>(step)};
    SendMessageW(spin, UDM_SETACCEL, 1, reinterpret_cast<LPARAM>(&accel));
    SendMessageW(spin, UDM_SETPOS32, 0, value);
    return edit;
}

// 读一个数字输入框，不在范围内时提示并把焦点放回去
bool ReadNumber(int editId, int minValue, int maxValue, const wchar_t* what, const wchar_t* unit, int& value) {
    BOOL ok = FALSE;
    UINT v = GetDlgItemInt(s_dlg, editId, &ok, FALSE);
    if (!ok || v < static_cast<UINT>(minValue) || v > static_cast<UINT>(maxValue)) {
        wchar_t msg[128];
        swprintf(msg, 128, L"%ls需要在 %d 到 %d%ls之间。", what, minValue, maxValue, unit);
        MessageBoxW(s_dlg, msg, L"TaskbarPopup", MB_ICONWARNING);
        SetFocus(GetDlgItem(s_dlg, editId));
        return false;
    }
    value = static_cast<int>(v);
    return true;
}

void CreateControls() {
    s_dpi = GetDpiForWindow(s_dlg);
    s_font = CreateDialogFont();

    AddControl(L"BUTTON", L"窗口最大化或全屏时隐藏任务栏，窗口铺满屏幕", BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP,
               IDC_AUTOHIDE);
    AddControl(L"BUTTON", L"长按 Win 键弹出迷你任务栏", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_LONGPRESS);
    AddControl(L"STATIC", L"长按时长（毫秒）：", SS_LEFT | SS_CENTERIMAGE, IDC_MS_LABEL);
    AddNumberBox(IDC_MS_EDIT, IDC_MS_SPIN, kMinLongPressMs, kMaxLongPressMs, 100, g_settings.longPressMs);
    wchar_t hint[96];
    swprintf(hint, 96, L"可设置 %d – %d 毫秒，默认 %d", kMinLongPressMs, kMaxLongPressMs, kDefaultLongPressMs);
    AddControl(L"STATIC", hint, SS_LEFT, IDC_MS_HINT);

    AddControl(L"STATIC", L"迷你任务栏大小（%）：", SS_LEFT | SS_CENTERIMAGE, IDC_SCALE_LABEL);
    AddNumberBox(IDC_SCALE_EDIT, IDC_SCALE_SPIN, kMinPopupScale, kMaxPopupScale, 10, g_settings.popupScale);
    swprintf(hint, 96, L"可设置 %d – %d%%，默认 %d%%", kMinPopupScale, kMaxPopupScale, kDefaultPopupScale);
    AddControl(L"STATIC", hint, SS_LEFT, IDC_SCALE_HINT);
    AddControl(L"BUTTON", L"迷你任务栏里显示固定在任务栏的应用", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_PINNED);
    AddControl(L"BUTTON", L"迷你任务栏里显示音量和亮度调节", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_LEVELS);
    AddControl(L"BUTTON", L"开机自动启动", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_AUTOSTART);
    AddControl(L"STATIC", L"最大化时不隐藏任务栏的程序（每行一个，如 notepad.exe）：", SS_LEFT, IDC_EXCLUDE_LABEL);
    std::wstring exclude;
    for (const std::wstring& name : g_settings.excludeApps) exclude += (exclude.empty() ? L"" : L"\r\n") + name;
    AddControl(L"EDIT", exclude.c_str(), ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP,
               IDC_EXCLUDE, WS_EX_CLIENTEDGE);
    AddControl(L"STATIC", L"也可以在迷你任务栏里右键运行中的程序来添加或去掉", SS_LEFT, IDC_EXCLUDE_HINT);
    AddControl(L"BUTTON", L"确定", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
    AddControl(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);

    s_initial.autoHide = g_settings.autoHideOnFullscreen;
    s_initial.longPress = g_settings.longPressPopup;
    s_initial.pinned = g_settings.showPinnedApps;
    s_initial.levels = g_settings.showLevels;
    s_initial.autoStart = IsAutoStartEnabled();
    s_initial.ms = g_settings.longPressMs;
    s_initial.scale = g_settings.popupScale;
    s_initial.exclude = g_settings.excludeApps;
    CheckDlgButton(s_dlg, IDC_AUTOHIDE, s_initial.autoHide ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_dlg, IDC_LONGPRESS, s_initial.longPress ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_dlg, IDC_PINNED, s_initial.pinned ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_dlg, IDC_LEVELS, s_initial.levels ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_dlg, IDC_AUTOSTART, s_initial.autoStart ? BST_CHECKED : BST_UNCHECKED);
    UpdateEnabled();

    // 按 DPI 调整窗口大小，并居中到鼠标所在的显示器
    SIZE size = WindowSize();
    int w = size.cx, h = size.cy;
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    SetWindowPos(s_dlg, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2,
                 w, h, SWP_NOZORDER);
}

bool Apply() {
    int ms = 0, scale = 0;
    if (!ReadNumber(IDC_MS_EDIT, kMinLongPressMs, kMaxLongPressMs, L"长按时长", L" 毫秒", ms) ||
        !ReadNumber(IDC_SCALE_EDIT, kMinPopupScale, kMaxPopupScale, L"迷你任务栏大小", L"%", scale))
        return false;

    // 只改窗口里动过的项，其余的保持现在的值（窗口开着的时候可能在别处改过）
    if (Checked(IDC_AUTOHIDE) != s_initial.autoHide) g_settings.autoHideOnFullscreen = Checked(IDC_AUTOHIDE);
    if (Checked(IDC_LONGPRESS) != s_initial.longPress) g_settings.longPressPopup = Checked(IDC_LONGPRESS);
    if (Checked(IDC_PINNED) != s_initial.pinned) g_settings.showPinnedApps = Checked(IDC_PINNED);
    if (Checked(IDC_LEVELS) != s_initial.levels) g_settings.showLevels = Checked(IDC_LEVELS);
    if (ms != s_initial.ms) g_settings.longPressMs = ms;
    if (scale != s_initial.scale) g_settings.popupScale = scale;

    // 名单按增删合并到现在的名单上，不整个替换
    std::wstring text(GetWindowTextLengthW(GetDlgItem(s_dlg, IDC_EXCLUDE)) + 1, L'\0');
    text.resize(GetDlgItemTextW(s_dlg, IDC_EXCLUDE, text.data(), static_cast<int>(text.size())));
    std::vector<std::wstring> edited = ParseExcludeList(text);
    auto contains = [](const std::vector<std::wstring>& list, const std::wstring& name) {
        return std::find(list.begin(), list.end(), name) != list.end();
    };
    std::vector<std::wstring>& current = g_settings.excludeApps;
    for (const std::wstring& name : s_initial.exclude)
        if (!contains(edited, name)) current.erase(std::remove(current.begin(), current.end(), name), current.end());
    for (const std::wstring& name : edited)
        if (!contains(s_initial.exclude, name) && !contains(current, name)) current.push_back(name);

    SaveSettings();
    if (Checked(IDC_AUTOSTART) != s_initial.autoStart) SetAutoStart(Checked(IDC_AUTOSTART));
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

        // 多行输入框里按 Tab 时会让父窗口切换焦点（普通对话框由 DefDlgProc 处理，这里不是对话框类）
        case WM_NEXTDLGCTL: {
            HWND next = LOWORD(lParam) ? reinterpret_cast<HWND>(wParam)
                                       : GetNextDlgTabItem(hwnd, GetFocus(), wParam != 0);
            if (next) {
                SetFocus(next);
                if (SendMessageW(next, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL) SendMessageW(next, EM_SETSEL, 0, -1);
            }
            return 0;
        }

        case WM_DPICHANGED:
            OnDpiChanged(HIWORD(wParam), *reinterpret_cast<const RECT*>(lParam));
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
