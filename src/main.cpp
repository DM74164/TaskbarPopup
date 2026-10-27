// TaskbarPopup：Win11 任务栏增强
//   1. 窗口最大化或全屏时隐藏任务栏，窗口铺满整个屏幕
//   2. 长按 Win 键，从屏幕底部中央弹出迷你任务栏（含固定在任务栏的应用）
#include "common.h"

#include <cstdlib>
#include <cwchar>
#include <exception>

namespace app {

HINSTANCE g_instance = nullptr;
HWND g_mainWnd = nullptr;
Settings g_settings;

namespace {

constexpr wchar_t kMainClass[] = L"TaskbarPopupMain";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"TaskbarPopup";
constexpr DWORD kExitDeadlineMs = 15000;  // 退出最多花这么久，超过就直接结束进程（正常收尾各步都有自己的超时，加起来也到不了）

enum MenuId {
    ID_SHOW = 100, ID_SETTINGS, ID_CHECKUPDATE, ID_AUTOHIDE, ID_ALWAYSHIDE, ID_LONGPRESS, ID_AUTOSTART, ID_DEBUGLOG, ID_OPENLOG,
    ID_RUNASADMIN, ID_EXIT
};

NOTIFYICONDATAW s_nid = {};
HICON s_trayIcon = nullptr;
UINT s_msgTaskbarCreated = 0;
std::wstring s_balloonUrl;  // 最近一次弹出的气泡被点时要打开的网址，空 = 点了不做什么
const wchar_t* const kHotkeyKeys[kHotkeyCount] = {L"PopupHotkey", L"PinHotkey", L"MoveHotkey", L"NextScreenHotkey"};  // 设置文件里的名字，长按的再加 Hold

// 运行时画托盘图标：底部一条任务栏 + 向上的箭头
HICON CreateAppIcon() {
    using namespace Gdiplus;
    Bitmap bmp(32, 32, PixelFormat32bppARGB);
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.Clear(Color(0, 0, 0, 0));
        SolidBrush brush(Color(255, 96, 205, 255));
        GraphicsPath bar;
        AddRoundRect(bar, RectF(4, 22, 24, 6), 3);
        g.FillPath(&brush, &bar);
        PointF arrow[3] = {PointF(16, 2), PointF(26, 12), PointF(6, 12)};
        g.FillPolygon(&brush, arrow, 3);
        g.FillRectangle(&brush, 13, 11, 6, 8);
    }
    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

void AddTrayIcon() {
    s_nid = {};
    s_nid.cbSize = sizeof(s_nid);
    s_nid.hWnd = g_mainWnd;
    s_nid.uID = 1;
    s_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    s_nid.uCallbackMessage = WM_APP_TRAY;
    s_nid.hIcon = s_trayIcon;
    lstrcpynW(s_nid.szTip, Tr(L"TaskbarPopup：全屏隐藏任务栏 / 长按 Win 弹出"), ARRAYSIZE(s_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &s_nid);
}

void ShowPopupDeferred() {
    // 等托盘点击 / 菜单的焦点切换结束后再弹，否则弹窗刚拿到焦点就被抢走
    SetTimer(g_mainWnd, kTimerShowPopup, 150, nullptr);
}

// 切换“以管理员身份运行”：以新的权限重新启动本程序，旧实例退出（退出时照常还原任务栏）
void ToggleRunAsAdmin() {
    bool autoStart = IsAutoStartEnabled();  // 按切换前的方式查
    bool on = !g_settings.runAsAdmin;
    g_settings.runAsAdmin = on;
    SaveSettings();
    if (on == IsElevated()) {
        SetAutoStart(autoStart);  // 开机自启换成对应的方式
        return;
    }
    bool started = on ? RelaunchElevated() : RelaunchUnelevated();
    if (!started) {
        // 在 UAC 里点了“否”之类：保持原样
        g_settings.runAsAdmin = !on;
        SaveSettings();
        return;
    }
    if (!on) SetAutoStart(autoStart);  // 现在还是管理员，趁机删掉计划任务、换回普通的开机自启
    DestroyWindow(g_mainWnd);
}

void HandleCommand(UINT id) {
    switch (id) {
        case ID_SHOW:
            ShowPopupDeferred();
            break;
        case ID_SETTINGS:
            SettingsDialog_Show();
            break;
        case ID_CHECKUPDATE:
            Update_CheckNow(true);
            break;
        case ID_AUTOHIDE:
            g_settings.autoHideOnFullscreen = !g_settings.autoHideOnFullscreen;
            SaveSettings();
            ApplySettings();
            SettingsDialog_SettingsChanged();
            break;
        case ID_ALWAYSHIDE:
            g_settings.alwaysHideTaskbar = !g_settings.alwaysHideTaskbar;
            SaveSettings();
            ApplySettings();
            SettingsDialog_SettingsChanged();
            break;
        case ID_LONGPRESS:
            g_settings.longPressPopup = !g_settings.longPressPopup;
            SaveSettings();
            ApplySettings();
            SettingsDialog_SettingsChanged();
            break;
        case ID_AUTOSTART:
            SetAutoStart(!IsAutoStartEnabled());
            SettingsDialog_SettingsChanged();
            break;
        case ID_RUNASADMIN:
            ToggleRunAsAdmin();
            break;
        case ID_DEBUGLOG:
            g_settings.debugLog = !g_settings.debugLog;
            SaveSettings();
            if (g_settings.debugLog) {
                Log(L"---- 打开诊断日志 ----");
                Fullscreen_LogState();
            }
            break;
        case ID_OPENLOG: {
            // 打开日志所在的文件夹并选中它
            std::wstring args = L"/select,\"" + LogFile() + L"\"";
            ShellExecuteW(nullptr, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            break;
        }
        case ID_EXIT:
            DestroyWindow(g_mainWnd);
            break;
    }
}

void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    auto check = [](bool on) { return on ? MF_CHECKED : MF_UNCHECKED; };
    std::wstring longPressText = TrF(L"长按 Win 键弹出迷你任务栏（%d 毫秒）", g_settings.longPressMs);

    AppendMenuW(menu, MF_STRING, ID_SHOW, Tr(L"显示迷你任务栏"));
    AppendMenuW(menu, MF_STRING, ID_SETTINGS, Tr(L"设置..."));
    AppendMenuW(menu, MF_STRING, ID_CHECKUPDATE, Tr(L"检查更新"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | check(g_settings.autoHideOnFullscreen), ID_AUTOHIDE, Tr(L"窗口最大化或全屏时隐藏任务栏"));
    AppendMenuW(menu, MF_STRING | check(g_settings.alwaysHideTaskbar), ID_ALWAYSHIDE, Tr(L"一直隐藏任务栏"));
    AppendMenuW(menu, MF_STRING | check(g_settings.longPressPopup), ID_LONGPRESS, longPressText.c_str());
    AppendMenuW(menu, MF_STRING | check(IsAutoStartEnabled()), ID_AUTOSTART, Tr(L"开机自动启动"));
    AppendMenuW(menu, MF_STRING | check(g_settings.runAsAdmin), ID_RUNASADMIN, Tr(L"以管理员身份运行（游戏里也能长按 Win）"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | check(g_settings.debugLog), ID_DEBUGLOG, Tr(L"记录诊断日志"));
    if (g_settings.debugLog || GetFileAttributesW(LogFile().c_str()) != INVALID_FILE_ATTRIBUTES)
        AppendMenuW(menu, MF_STRING, ID_OPENLOG, Tr(L"打开诊断日志所在文件夹"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, Tr(L"退出"));
    SetMenuDefaultItem(menu, ID_SHOW, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_mainWnd);  // 否则点菜单外面时菜单不会消失
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_mainWnd, nullptr);
    PostMessageW(g_mainWnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (cmd) HandleCommand(cmd);
}

void Shutdown() {
    Log(L"退出");
    if (HWND dlg = SettingsDialog_Hwnd()) DestroyWindow(dlg);  // 先关：关的时候会存下没存的设置、重新应用
    Hook_Uninstall();
    Brightness_Stop();
    Radios_Stop();
    Media_Stop();
    Attention_Shutdown();
    Fullscreen_SetEnabled(false);
    Log(L"退出：还原任务栏");
    Taskbar_RestoreAll();
    Shell_NotifyIconW(NIM_DELETE, &s_nid);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_APP_LONGPRESS:
            Log(L"长按 Win");
            Popup_Toggle();
            return 0;

        case WM_HOTKEY:
            Hotkeys_OnHotkey(wParam);
            return 0;

        case WM_APP_HOTKEY:
            Hotkeys_Run(static_cast<int>(wParam), reinterpret_cast<HWND>(lParam));
            return 0;

        case WM_APP_AUTOHIDE_OFF:
            Fullscreen_OnAutoHideOff(static_cast<UINT>(wParam), lParam != 0);
            return 0;

        case WM_APP_AUTOHIDE_ON:
            Fullscreen_OnAutoHideOn(static_cast<UINT>(wParam), lParam != 0);
            return 0;

        case WM_APP_UPDATE:
            Update_OnResult();
            return 0;

        case WM_APP_WINTAP:
            Popup_StartMenu(static_cast<DWORD>(wParam));
            return 0;

        case WM_APP_TRAY_LAUNCH:
        case WM_APP_TRAY_MENU:
            Tray_OnMessage(msg, lParam);
            return 0;

        case WM_APP_TRAY_PEEK:
            return Fullscreen_PeekTaskbar(reinterpret_cast<HWND>(lParam), wParam != 0) ? 1 : 0;

        case WM_DISPLAYCHANGE:
            DesktopIcons_DisplayChanged();
            Fullscreen_DisplayChanged();
            Popup_DisplayChanged();
            Attention_DisplayChanged();
            SettingsDialog_DisplayChanged();
            break;

        case WM_TIMER:
            if (wParam == kTimerFullscreen) {
                Fullscreen_Check();
            } else if (wParam == kTimerShowPopup) {
                KillTimer(hwnd, kTimerShowPopup);
                Popup_Show();
            }
            return 0;

        case WM_APP_TRAY:
            switch (LOWORD(lParam)) {
                case WM_LBUTTONUP: ShowPopupDeferred(); break;
                case WM_RBUTTONUP: ShowTrayMenu(); break;
                case NIN_BALLOONUSERCLICK:
                    if (!s_balloonUrl.empty()) {
                        std::wstring url;
                        url.swap(s_balloonUrl);
                        LaunchAsUser(url);  // 本程序是管理员时浏览器也以普通权限打开
                    }
                    break;
            }
            return 0;

        case WM_ENDSESSION:
            if (!wParam) return 0;
            if (lParam & ENDSESSION_CLOSEAPP) {
                DestroyWindow(hwnd);  // 安装程序之类通过重启管理器要求退出：正常退出
                return 0;
            }
            // 关机 / 注销前把任务栏和拉伸过的窗口还原。先停掉最大化检测，免得还原以后又被重新打开自动隐藏
            Fullscreen_SetEnabled(false);
            Taskbar_RestoreAll();
            return 0;

        case WM_DESTROY:
            // 收尾时某一步卡住（资源管理器或别的程序没响应、后台线程卡在系统调用里）也要保证进程结束，
            // 不然托盘图标没了、任务管理器里还留着
            CreateThread(
                nullptr, 0,
                [](void*) -> DWORD {
                    Sleep(kExitDeadlineMs);
                    TerminateProcess(GetCurrentProcess(), 0);
                    return 0;
                },
                nullptr, 0, nullptr);
            Shutdown();
            PostQuitMessage(0);
            return 0;
    }
    if (Attention_OnMessage(msg, wParam, lParam)) return 0;
    if (msg == s_msgTaskbarCreated && s_msgTaskbarCreated) {
        AddTrayIcon();  // 资源管理器重启后托盘图标需要重新添加
        Fullscreen_OnTaskbarCreated();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS*) {
    Taskbar_EmergencyRestore();  // 崩溃也别把任务栏留在隐藏状态
    DesktopIcons_EmergencyShow();
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

bool ShowTrayBalloon(const wchar_t* title, const wchar_t* text, const wchar_t* clickUrl) {
    NOTIFYICONDATAW nid = s_nid;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    lstrcpynW(nid.szInfoTitle, title, ARRAYSIZE(nid.szInfoTitle));
    lstrcpynW(nid.szInfo, text, ARRAYSIZE(nid.szInfo));
    if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) return false;
    // 新气泡顶替旧的：点击只对应最近弹出的那个（别的提示被点时不打开网页）
    s_balloonUrl = clickUrl ? clickUrl : L"";
    return true;
}

std::wstring SettingsDir() {
    wchar_t buf[MAX_PATH] = {};
    GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    return std::wstring(buf) + L"\\TaskbarPopup";
}

std::wstring SettingsFile() { return SettingsDir() + L"\\settings.ini"; }

// WritePrivateProfileString 新建的文件是 ANSI 编码，系统代码页以外的字符（比如英文版 Windows 上中文名字的程序）
// 会存成“?”。先建好一个带 UTF-16 BOM 的文件，之后的读写就都按 Unicode；旧版本留下的 ANSI 文件转换一次
void EnsureUnicodeIni() {
    CreateDirectoryW(SettingsDir().c_str(), nullptr);
    HANDLE h = CreateFileW(SettingsFile().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size = {};
    std::string bytes;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (1 << 20);
    if (ok && size.QuadPart > 0) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
    }
    bool unicode = bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
                   static_cast<unsigned char>(bytes[1]) == 0xFE;
    if (ok && !unicode) {
        UINT codePage = CP_ACP;
        size_t skip = 0;
        if (bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) {  // 用户自己存成了 UTF-8
            codePage = CP_UTF8;
            skip = 3;
        }
        std::wstring text(1, L'\xFEFF');
        int length = static_cast<int>(bytes.size() - skip);
        int n = length > 0 ? MultiByteToWideChar(codePage, 0, bytes.data() + skip, length, nullptr, 0) : 0;
        if (n > 0) {
            text.resize(1 + static_cast<size_t>(n));
            MultiByteToWideChar(codePage, 0, bytes.data() + skip, length, &text[1], n);
        }
        DWORD written = 0;
        SetFilePointer(h, 0, nullptr, FILE_BEGIN);
        if (WriteFile(h, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &written, nullptr))
            SetEndOfFile(h);
    }
    CloseHandle(h);
}

namespace {

const wchar_t* const kModuleKeys[kModuleCount] = {L"windows", L"tray", L"media", L"ime", L"battery", L"clock"};

// 模块的顺序：名字用逗号隔开。认不出的跳过，没写到的按默认顺序补在后面
void ParseModuleOrder(const std::wstring& text, int order[kModuleCount]) {
    bool used[kModuleCount] = {};
    int n = 0;
    for (size_t start = 0; start < text.size() && n < kModuleCount;) {
        size_t end = text.find(L',', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring name = ToLower(text.substr(start, end - start));
        for (int m = 0; m < kModuleCount; ++m)
            if (name == kModuleKeys[m] && !used[m]) {
                used[m] = true;
                order[n++] = m;
            }
        start = end + 1;
    }
    for (int m = 0; m < kModuleCount; ++m)
        if (!used[m]) order[n++] = m;
}

// 底部那份用原来的键，侧边那份在键前面加 Side；侧边没写过的跟着底部
struct ModuleKey {
    const wchar_t* key;
    bool ModuleSet::*item;
};
const ModuleKey kModuleSetKeys[] = {{L"TrayIcons", &ModuleSet::tray},     {L"ShowLevels", &ModuleSet::levels},
                                    {L"ShowRadios", &ModuleSet::radios},  {L"ShowMedia", &ModuleSet::media},
                                    {L"ShowIme", &ModuleSet::ime},        {L"ShowBattery", &ModuleSet::battery},
                                    {L"ShowClock", &ModuleSet::clock}};
std::wstring SideKey(const wchar_t* key) { return std::wstring(L"Side") + key; }

std::wstring ModuleOrderText(const int order[kModuleCount]) {
    std::wstring text;
    for (int i = 0; i < kModuleCount; ++i) text += (i ? L"," : L"") + std::wstring(kModuleKeys[order[i]]);
    return text;
}

// 颜色存成 #RRGGBB
std::wstring ColorText(COLORREF c) {
    wchar_t text[16];
    swprintf(text, 16, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return text;
}

COLORREF ParseColor(const std::wstring& text, COLORREF fallback) {
    std::wstring hex = text;
    if (!hex.empty() && hex[0] == L'#') hex.erase(0, 1);
    if (hex.size() != 6 || hex.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring::npos) return fallback;
    unsigned long v = wcstoul(hex.c_str(), nullptr, 16);
    return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

int ClampOffset(int offset) { return std::clamp(offset, 0, kMaxPopupOffset); }

}  // namespace

void LoadSettings() {
    EnsureUnicodeIni();
    std::wstring f = SettingsFile();
    g_settings.autoHideOnFullscreen = GetPrivateProfileIntW(L"General", L"AutoHideOnFullscreen", 1, f.c_str()) != 0;
    g_settings.alwaysHideTaskbar = GetPrivateProfileIntW(L"General", L"AlwaysHideTaskbar", 0, f.c_str()) != 0;
    g_settings.longPressPopup = GetPrivateProfileIntW(L"General", L"LongPressPopup", 1, f.c_str()) != 0;
    int ms = static_cast<int>(GetPrivateProfileIntW(L"General", L"LongPressMs", kDefaultLongPressMs, f.c_str()));
    g_settings.longPressMs = std::max(kMinLongPressMs, std::min(ms, kMaxLongPressMs));
    g_settings.showPinnedApps = GetPrivateProfileIntW(L"General", L"ShowPinnedApps", 1, f.c_str()) != 0;
    for (const ModuleKey& k : kModuleSetKeys) {
        bool bottom = GetPrivateProfileIntW(L"General", k.key, 1, f.c_str()) != 0;
        g_settings.modules[kSideBottom].*k.item = bottom;
        g_settings.modules[kSideEdge].*k.item = GetPrivateProfileIntW(L"General", SideKey(k.key).c_str(), bottom, f.c_str()) != 0;
    }
    g_settings.clockSeconds = GetPrivateProfileIntW(L"General", L"ClockSeconds", 0, f.c_str()) != 0;
    g_settings.clockWeekday = GetPrivateProfileIntW(L"General", L"ClockWeekday", 0, f.c_str()) != 0;
    g_settings.clockDate = GetPrivateProfileIntW(L"General", L"ClockDate", 1, f.c_str()) != 0;
    g_settings.showTitle = GetPrivateProfileIntW(L"General", L"ShowTitle", 1, f.c_str()) != 0;
    g_settings.iconSize = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"General", L"IconSize", 100, f.c_str())),
                                     kMinIconSize, kMaxIconSize);
    g_settings.iconGap = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(L"General", L"IconGap", kDefaultIconGap, f.c_str())), 0, kMaxIconGap);
    g_settings.flashAlert = GetPrivateProfileIntW(L"General", L"FlashAlert", 1, f.c_str()) != 0;
    g_settings.edgePopup = GetPrivateProfileIntW(L"General", L"EdgePopup", 0, f.c_str()) != 0;
    g_settings.trayStyle = GetPrivateProfileIntW(L"General", L"TrayStyle", kTrayStacked, f.c_str()) == kTrayRow ? kTrayRow : kTrayStacked;
    int scale = static_cast<int>(GetPrivateProfileIntW(L"General", L"PopupScale", kDefaultPopupScale, f.c_str()));
    int edge = static_cast<int>(GetPrivateProfileIntW(L"General", L"PopupEdge", kEdgeBottom, f.c_str()));
    g_settings.popupEdge = edge == kEdgeLeft || edge == kEdgeRight ? edge : kEdgeBottom;
    int align = static_cast<int>(GetPrivateProfileIntW(L"General", L"PopupAlign", kAlignSystem, f.c_str()));
    g_settings.popupAlign = align >= kAlignSystem && align <= kAlignCustom ? align : kAlignSystem;
    g_settings.popupOffset = ClampOffset(static_cast<int>(GetPrivateProfileIntW(L"General", L"PopupOffset", 500, f.c_str())));
    g_settings.popupMonitor =
        GetPrivateProfileIntW(L"General", L"PopupMonitor", kPopupAtCursor, f.c_str()) == kPopupAtActive ? kPopupAtActive
                                                                                                       : kPopupAtCursor;
    g_settings.popupPerScreen = GetPrivateProfileIntW(L"General", L"PopupPerScreen", 0, f.c_str()) != 0;
    g_settings.keepOtherTaskbars = GetPrivateProfileIntW(L"General", L"KeepOtherTaskbars", 1, f.c_str()) != 0;
    int windows = static_cast<int>(GetPrivateProfileIntW(L"General", L"PopupWindows", kWindowsSystem, f.c_str()));
    g_settings.popupWindows = windows == kWindowsAll || windows == kWindowsOwnScreen ? windows : kWindowsSystem;
    g_settings.markScreens = GetPrivateProfileIntW(L"General", L"MarkScreens", 1, f.c_str()) != 0;
    g_settings.brightnessAllScreens = GetPrivateProfileIntW(L"General", L"BrightnessAllScreens", 0, f.c_str()) != 0;
    // 没设过语言：新装的跟随系统；以前用过的（设置文件里已经有东西，哪怕只有程序自己记的状态）保持原来的中文界面。
    // 马上记下来，不然新装的用过一次、设置文件里有了东西，下次启动就被当成以前用过的
    int language = static_cast<int>(GetPrivateProfileIntW(L"General", L"Language", -1, f.c_str()));
    if (language < kLangSystem || language > kLangEnglish) {
        wchar_t sections[64] = {};
        bool upgraded = GetPrivateProfileSectionNamesW(sections, ARRAYSIZE(sections), f.c_str()) > 0;
        language = upgraded ? kLangChinese : kLangSystem;
        WritePrivateProfileStringW(L"General", L"Language", std::to_wstring(language).c_str(), f.c_str());
    }
    g_settings.language = language;
    SetUiEnglish(LanguageIsEnglish(g_settings.language));
    g_settings.popupScale = std::max(kMinPopupScale, std::min(scale, kMaxPopupScale));
    g_settings.debugLog = GetPrivateProfileIntW(L"General", L"DebugLog", 0, f.c_str()) != 0;
    g_settings.runAsAdmin = GetPrivateProfileIntW(L"General", L"RunAsAdmin", 0, f.c_str()) != 0;
    g_settings.checkUpdates = GetPrivateProfileIntW(L"General", L"CheckUpdates", 0, f.c_str()) != 0;
    g_settings.keepFloatsOnTop = GetPrivateProfileIntW(L"General", L"KeepFloatsOnTop", 1, f.c_str()) != 0;
    g_settings.pinTopmost = GetPrivateProfileIntW(L"General", L"PinTopmost", 0, f.c_str()) != 0;
    int style = static_cast<int>(GetPrivateProfileIntW(L"General", L"GlassStyle", kGlassLiquid, f.c_str()));
    g_settings.glassStyle = style == 1 || style == 2 ? kGlassWin11 : kGlassLiquid;  // 1 是以前的毛玻璃，2 是上一版的 Win11 原版
    int radius = static_cast<int>(GetPrivateProfileIntW(L"General", L"PopupRadius", DefaultPopupRadius(g_settings.glassStyle), f.c_str()));
    g_settings.popupRadius = std::max(0, std::min(radius, kMaxPopupRadius));
    int length = static_cast<int>(GetPrivateProfileIntW(L"General", L"PopupLength", 0, f.c_str()));
    g_settings.popupLength = std::max(0, std::min(length, kMaxPopupLength));
    int theme = static_cast<int>(GetPrivateProfileIntW(L"General", L"Theme", kThemeSystem, f.c_str()));
    g_settings.theme = theme == kThemeLight || theme == kThemeDark ? theme : kThemeSystem;
    g_settings.glassTransparency = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(L"General", L"GlassTransparency", kDefaultGlassTransparency, f.c_str())), 0, 100);
    std::vector<wchar_t> buf(32768);
    auto text = [&](const wchar_t* key) {
        GetPrivateProfileStringW(L"General", key, L"", buf.data(), static_cast<DWORD>(buf.size()), f.c_str());
        return std::wstring(buf.data());
    };
    int* bottomOrder = g_settings.modules[kSideBottom].order;
    ParseModuleOrder(text(L"ModuleOrder"), bottomOrder);
    std::wstring sideOrder = text(SideKey(L"ModuleOrder").c_str());
    if (sideOrder.empty())
        std::copy(bottomOrder, bottomOrder + kModuleCount, g_settings.modules[kSideEdge].order);
    else
        ParseModuleOrder(sideOrder, g_settings.modules[kSideEdge].order);
    g_settings.customTint = GetPrivateProfileIntW(L"General", L"CustomTint", 0, f.c_str()) != 0;
    g_settings.tintColor = ParseColor(text(L"TintColor"), RGB(32, 32, 36));
    g_settings.customAccent = GetPrivateProfileIntW(L"General", L"CustomAccent", 0, f.c_str()) != 0;
    g_settings.accentColor = ParseColor(text(L"AccentColor"), RGB(0, 120, 212));
    for (int id = 0; id < kHotkeyCount; ++id) {
        GetPrivateProfileStringW(L"General", kHotkeyKeys[id], L"", buf.data(), static_cast<DWORD>(buf.size()), f.c_str());
        g_settings.hotkey[id] = ParseHotkey(buf.data());
        std::wstring hold = std::wstring(kHotkeyKeys[id]) + L"Hold";
        g_settings.hotkeyHold[id] = GetPrivateProfileIntW(L"General", hold.c_str(), 0, f.c_str()) != 0;
        // 没设过的话用长按 Win 的时长（以前长按的快捷键和它共用）
        int holdMs = static_cast<int>(GetPrivateProfileIntW(L"General", (hold + L"Ms").c_str(), g_settings.longPressMs, f.c_str()));
        g_settings.hotkeyHoldMs[id] = std::max(kMinLongPressMs, std::min(holdMs, kMaxLongPressMs));
    }
    auto loadList = [&](const wchar_t* key, std::vector<std::wstring>& apps) {
        GetPrivateProfileStringW(L"General", key, L"", buf.data(), static_cast<DWORD>(buf.size()), f.c_str());
        apps.clear();
        std::wstring list = buf.data();
        for (size_t start = 0; start <= list.size();) {
            size_t end = list.find(L';', start);
            if (end == std::wstring::npos) end = list.size();
            std::wstring name = NormalizeExeName(list.substr(start, end - start));
            if (!name.empty() && !InAppList(apps, name)) apps.push_back(name);
            start = end + 1;
        }
    };
    loadList(L"ExcludeApps", g_settings.excludeApps);
    loadList(L"IncludeApps", g_settings.includeApps);
    // 不隐藏任务栏的屏幕（MonitorKey），用 ; 隔开
    GetPrivateProfileStringW(L"General", L"NoHideScreens", L"", buf.data(), static_cast<DWORD>(buf.size()), f.c_str());
    g_settings.noHideScreens.clear();
    std::wstring screens = buf.data();
    for (size_t start = 0; start < screens.size();) {
        size_t end = screens.find(L';', start);
        if (end == std::wstring::npos) end = screens.size();
        std::wstring key = ToLower(screens.substr(start, end - start));
        if (!key.empty() && !InAppList(g_settings.noHideScreens, key)) g_settings.noHideScreens.push_back(key);
        start = end + 1;
    }
    // 每块屏幕的位置和对齐：屏幕|边|对齐|自定义位置（以前的版本没有最后一项），用 ; 隔开
    GetPrivateProfileStringW(L"General", L"ScreenPlaces", L"", buf.data(), static_cast<DWORD>(buf.size()), f.c_str());
    g_settings.screens.clear();
    std::wstring places = buf.data();
    for (size_t start = 0; start < places.size();) {
        size_t end = places.find(L';', start);
        if (end == std::wstring::npos) end = places.size();
        std::wstring item = places.substr(start, end - start);
        size_t a = item.find(L'|'), b = a == std::wstring::npos ? a : item.find(L'|', a + 1);
        if (b != std::wstring::npos && a > 0) {
            ScreenPlace p;
            p.key = ToLower(item.substr(0, a));
            size_t c = item.find(L'|', b + 1);
            int edge = _wtoi(item.substr(a + 1, b - a - 1).c_str()), align = _wtoi(item.substr(b + 1, c - b - 1).c_str());
            p.edge = edge == kEdgeLeft || edge == kEdgeRight ? edge : kEdgeBottom;
            p.align = align >= kAlignSystem && align <= kAlignCustom ? align : kAlignSystem;
            if (c != std::wstring::npos) p.offset = ClampOffset(_wtoi(item.substr(c + 1).c_str()));
            if (!FindScreenPlace(g_settings.screens, p.key)) g_settings.screens.push_back(p);
        }
        start = end + 1;
    }
}

void SaveSettings() {
    EnsureUnicodeIni();
    std::wstring f = SettingsFile();
    WritePrivateProfileStringW(L"General", L"AutoHideOnFullscreen", g_settings.autoHideOnFullscreen ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"AlwaysHideTaskbar", g_settings.alwaysHideTaskbar ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"LongPressPopup", g_settings.longPressPopup ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"LongPressMs", std::to_wstring(g_settings.longPressMs).c_str(), f.c_str());
    WritePrivateProfileStringW(L"General", L"ShowPinnedApps", g_settings.showPinnedApps ? L"1" : L"0", f.c_str());
    for (const ModuleKey& k : kModuleSetKeys) {
        WritePrivateProfileStringW(L"General", k.key, g_settings.modules[kSideBottom].*k.item ? L"1" : L"0", f.c_str());
        WritePrivateProfileStringW(L"General", SideKey(k.key).c_str(), g_settings.modules[kSideEdge].*k.item ? L"1" : L"0",
                                   f.c_str());
    }
    WritePrivateProfileStringW(L"General", L"ClockSeconds", g_settings.clockSeconds ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"ClockWeekday", g_settings.clockWeekday ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"ClockDate", g_settings.clockDate ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"ModuleOrder", ModuleOrderText(g_settings.modules[kSideBottom].order).c_str(),
                               f.c_str());
    WritePrivateProfileStringW(L"General", SideKey(L"ModuleOrder").c_str(), ModuleOrderText(g_settings.modules[kSideEdge].order).c_str(),
                               f.c_str());
    WritePrivateProfileStringW(L"General", L"ShowTitle", g_settings.showTitle ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"IconSize", std::to_wstring(g_settings.iconSize).c_str(), f.c_str());
    WritePrivateProfileStringW(L"General", L"IconGap", std::to_wstring(g_settings.iconGap).c_str(), f.c_str());
    WritePrivateProfileStringW(L"General", L"CustomTint", g_settings.customTint ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"TintColor", ColorText(g_settings.tintColor).c_str(), f.c_str());
    WritePrivateProfileStringW(L"General", L"CustomAccent", g_settings.customAccent ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"AccentColor", ColorText(g_settings.accentColor).c_str(), f.c_str());
    WritePrivateProfileStringW(L"General", L"FlashAlert", g_settings.flashAlert ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"EdgePopup", g_settings.edgePopup ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"TrayStyle", std::to_wstring(g_settings.trayStyle).c_str(), f.c_str());
    WritePrivateProfileStringW(L"General", L"PopupScale", std::to_wstring(g_settings.popupScale).c_str(), f.c_str());
    WritePrivateProfileStringW(L"General", L"PopupEdge", std::to_wstring(g_settings.popupEdge).c_str(), f.c_str());
    WritePrivateProfileStringW(L"General", L"PopupAlign", std::to_wstring(g_settings.popupAlign).c_str(), f.c_str());
    WritePrivateProfileStringW(L"General", L"PopupOffset", std::to_wstring(g_settings.popupOffset).c_str(), f.c_str());
    WritePrivateProfileStringW(L"General", L"PopupMonitor", std::to_wstring(g_settings.popupMonitor).c_str(), f.c_str());
    WritePrivateProfileStringW(L"General", L"PopupPerScreen", g_settings.popupPerScreen ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"KeepOtherTaskbars", g_settings.keepOtherTaskbars ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"PopupWindows", std::to_wstring(g_settings.popupWindows).c_str(), f.c_str());
    WritePrivateProfileStringW(L"General", L"MarkScreens", g_settings.markScreens ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"BrightnessAllScreens", g_settings.brightnessAllScreens ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"Language", std::to_wstring(g_settings.language).c_str(), f.c_str());
    WritePrivateProfileStringW(L"General", L"DebugLog", g_settings.debugLog ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"RunAsAdmin", g_settings.runAsAdmin ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"CheckUpdates", g_settings.checkUpdates ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"KeepFloatsOnTop", g_settings.keepFloatsOnTop ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"PinTopmost", g_settings.pinTopmost ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"GlassStyle", std::to_wstring(g_settings.glassStyle).c_str(), f.c_str());
    WritePrivateProfileStringW(L"General", L"PopupRadius", std::to_wstring(g_settings.popupRadius).c_str(), f.c_str());
    WritePrivateProfileStringW(L"General", L"PopupLength", std::to_wstring(g_settings.popupLength).c_str(), f.c_str());
    WritePrivateProfileStringW(L"General", L"Theme", std::to_wstring(g_settings.theme).c_str(), f.c_str());
    WritePrivateProfileStringW(L"General", L"GlassTransparency", std::to_wstring(g_settings.glassTransparency).c_str(), f.c_str());
    for (int id = 0; id < kHotkeyCount; ++id) {
        WritePrivateProfileStringW(L"General", kHotkeyKeys[id], HotkeyText(g_settings.hotkey[id]).c_str(), f.c_str());
        std::wstring hold = std::wstring(kHotkeyKeys[id]) + L"Hold";
        WritePrivateProfileStringW(L"General", hold.c_str(), g_settings.hotkeyHold[id] ? L"1" : L"0", f.c_str());
        WritePrivateProfileStringW(L"General", (hold + L"Ms").c_str(), std::to_wstring(g_settings.hotkeyHoldMs[id]).c_str(),
                                   f.c_str());
    }
    auto saveList = [&](const wchar_t* key, const std::vector<std::wstring>& apps) {
        std::wstring text;
        for (const std::wstring& name : apps) text += (text.empty() ? L"" : L";") + name;
        WritePrivateProfileStringW(L"General", key, text.c_str(), f.c_str());
    };
    saveList(L"ExcludeApps", g_settings.excludeApps);
    saveList(L"IncludeApps", g_settings.includeApps);
    saveList(L"NoHideScreens", g_settings.noHideScreens);
    std::wstring places;
    for (const ScreenPlace& p : g_settings.screens)
        places += (places.empty() ? L"" : L";") + p.key + L"|" + std::to_wstring(p.edge) + L"|" + std::to_wstring(p.align) +
                  L"|" + std::to_wstring(p.offset);
    WritePrivateProfileStringW(L"General", L"ScreenPlaces", places.c_str(), f.c_str());
}

namespace {

// 外观方案里存的键（和 [General] 里的同名）：材质、颜色、大小、模块、时钟、位置
const wchar_t* const kLookKeys[] = {
    L"GlassStyle",  L"Theme",       L"GlassTransparency", L"PopupRadius", L"PopupScale",   L"PopupLength", L"IconSize",
    L"IconGap",     L"ShowTitle",   L"CustomTint",        L"TintColor",   L"CustomAccent", L"AccentColor", L"ModuleOrder",
    L"TrayIcons",   L"TrayStyle",   L"ShowMedia",         L"ShowIme",     L"ShowBattery",  L"ShowClock",   L"ClockSeconds",
    L"ClockWeekday", L"ClockDate",  L"ShowLevels",        L"ShowRadios",  L"PopupEdge",    L"PopupAlign",  L"PopupOffset",
    L"SideModuleOrder", L"SideTrayIcons", L"SideShowMedia", L"SideShowIme", L"SideShowBattery", L"SideShowClock",
    L"SideShowLevels",  L"SideShowRadios", L"PopupPerScreen", L"ScreenPlaces"};

std::wstring LookSection(int slot) { return L"Look" + std::to_wstring(slot); }

// 把外观方案的键从一节抄到另一节。没有的键（旧版本存的方案）在目标那节也删掉，读的时候用默认值（侧边的跟着底部）
void CopyLookKeys(const wchar_t* fromSection, const wchar_t* toSection) {
    std::wstring f = SettingsFile();
    wchar_t value[512];
    for (const wchar_t* key : kLookKeys) {
        DWORD n = GetPrivateProfileStringW(fromSection, key, L"\x01", value, ARRAYSIZE(value), f.c_str());
        bool missing = n == 1 && value[0] == L'\x01';
        // 每块屏幕的位置是后来加进方案的：旧方案里没有就留着现在的
        if (missing && (!wcscmp(key, L"PopupPerScreen") || !wcscmp(key, L"ScreenPlaces"))) continue;
        WritePrivateProfileStringW(toSection, key, missing ? nullptr : value, f.c_str());
    }
}

bool SectionExists(const wchar_t* section, const std::wstring& file) {
    wchar_t buf[4] = {};
    return GetPrivateProfileSectionW(section, buf, ARRAYSIZE(buf), file.c_str()) > 0;
}

}  // namespace

std::vector<int> LookPresets() {
    std::vector<int> slots;
    std::wstring f = SettingsFile();
    for (int slot = 1; slot <= kMaxLookPresets; ++slot)
        if (SectionExists(LookSection(slot).c_str(), f)) slots.push_back(slot);
    return slots;
}

int SaveLookPreset(int slot) {
    if (slot <= 0) {
        std::vector<int> used = LookPresets();
        for (slot = 1; slot <= kMaxLookPresets && std::find(used.begin(), used.end(), slot) != used.end(); ++slot) {
        }
        if (slot > kMaxLookPresets) return 0;
    }
    SaveSettings();
    CopyLookKeys(L"General", LookSection(slot).c_str());
    Log(L"存了外观方案 %d", slot);
    return slot;
}

void ApplyLookPreset(int slot) {
    if (!SectionExists(LookSection(slot).c_str(), SettingsFile())) return;
    SaveSettings();
    CopyLookKeys(LookSection(slot).c_str(), L"General");
    LoadSettings();
    ApplySettings();
    Log(L"换上了外观方案 %d", slot);
}

void DeleteLookPreset(int slot) {
    WritePrivateProfileStringW(LookSection(slot).c_str(), nullptr, nullptr, SettingsFile().c_str());
}

bool ExportSettings(const std::wstring& path) {
    SaveSettings();
    // 新建一个 UTF-16 的文件，再把 [General] 和外观方案各节抄过去（[State] 是程序自己记的状态，不导出）
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    bool ok = WriteFile(h, &bom, sizeof(bom), &written, nullptr) && written == sizeof(bom);
    CloseHandle(h);
    if (!ok) return false;
    std::wstring f = SettingsFile();
    std::vector<wchar_t> buf(65536);
    std::vector<std::wstring> sections = {L"General"};
    for (int slot : LookPresets()) sections.push_back(LookSection(slot));
    for (const std::wstring& section : sections) {
        DWORD n = GetPrivateProfileSectionW(section.c_str(), buf.data(), static_cast<DWORD>(buf.size()), f.c_str());
        if (n > 0 && !WritePrivateProfileSectionW(section.c_str(), buf.data(), path.c_str())) return false;
    }
    Log(L"导出了设置：%ls", path.c_str());
    return true;
}

bool ImportSettings(const std::wstring& path) {
    std::vector<wchar_t> buf(65536);
    DWORD n = GetPrivateProfileSectionW(L"General", buf.data(), static_cast<DWORD>(buf.size()), path.c_str());
    if (n == 0) return false;  // 不是本程序的设置文件
    EnsureUnicodeIni();
    std::wstring f = SettingsFile();
    // 旧版本导出的没有侧边那份：先删掉现在的，读的时候跟着导入的底部
    for (const ModuleKey& k : kModuleSetKeys) WritePrivateProfileStringW(L"General", SideKey(k.key).c_str(), nullptr, f.c_str());
    WritePrivateProfileStringW(L"General", SideKey(L"ModuleOrder").c_str(), nullptr, f.c_str());
    // 快捷键也先删掉：旧版本导出的没有后加的几个，留着现在的可能和导入的撞上
    for (const wchar_t* key : kHotkeyKeys)
        for (const wchar_t* suffix : {L"", L"Hold", L"HoldMs"})
            WritePrivateProfileStringW(L"General", (std::wstring(key) + suffix).c_str(), nullptr, f.c_str());
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        std::wstring line = p;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || eq == 0) continue;
        std::wstring key = line.substr(0, eq);
        // 以管理员身份运行要配合计划任务、UAC，诊断日志是排查问题时临时开的：都跟着这台电脑，不导入
        if (_wcsicmp(key.c_str(), L"RunAsAdmin") == 0 || _wcsicmp(key.c_str(), L"DebugLog") == 0) continue;
        WritePrivateProfileStringW(L"General", key.c_str(), line.substr(eq + 1).c_str(), f.c_str());
    }
    for (int slot = 1; slot <= kMaxLookPresets; ++slot) {  // 外观方案整个换成文件里的
        std::wstring section = LookSection(slot);
        WritePrivateProfileStringW(section.c_str(), nullptr, nullptr, f.c_str());
        DWORD len = GetPrivateProfileSectionW(section.c_str(), buf.data(), static_cast<DWORD>(buf.size()), path.c_str());
        if (len > 0) WritePrivateProfileSectionW(section.c_str(), buf.data(), f.c_str());
    }
    LoadSettings();
    ApplySettings();
    Log(L"导入了设置：%ls", path.c_str());
    return true;
}

bool GetRestoreAutoHideFlag() {
    return GetPrivateProfileIntW(L"State", L"RestoreAutoHide", 0, SettingsFile().c_str()) != 0;
}

void SetRestoreAutoHideFlag(bool value) {
    EnsureUnicodeIni();
    WritePrivateProfileStringW(L"State", L"RestoreAutoHide", value ? L"1" : L"0", SettingsFile().c_str());
}

namespace {
bool RunKeyExists() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    bool exists = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return exists;
}
}  // namespace

// 开机自启有两种：普通权限写注册表的 Run；以管理员身份运行时用最高权限的计划任务（开机不弹 UAC）
bool IsAutoStartEnabled() { return RunKeyExists() || (g_settings.runAsAdmin && AdminTask_Exists()); }

void SetAutoStart(bool enabled) {
    // 管理员才能建、删最高权限的计划任务
    bool useTask = enabled && g_settings.runAsAdmin && IsElevated() && AdminTask_Set(true);
    if (IsElevated() && !useTask) AdminTask_Set(false);
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    if (enabled && !useTask) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring value = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

void ApplySettings() {
    // 设置窗口开着时它在预览选中的语言（关掉时会换回设置里的），别在托盘菜单里点了开关就把它打乱
    if (!SettingsDialog_Hwnd()) SetUiEnglish(LanguageIsEnglish(g_settings.language));
    if (s_nid.cbSize) {  // 托盘图标已经加上了：提示文字换成现在的语言
        lstrcpynW(s_nid.szTip, Tr(L"TaskbarPopup：全屏隐藏任务栏 / 长按 Win 弹出"), ARRAYSIZE(s_nid.szTip));
        UINT flags = s_nid.uFlags;
        s_nid.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &s_nid);
        s_nid.uFlags = flags;
    }
    // 关着“最大化或全屏时隐藏任务栏”时，名单里有程序也要看着前台（只给名单里的程序藏任务栏）；一直隐藏任务栏时总要开着
    Fullscreen_SetEnabled(g_settings.autoHideOnFullscreen || !g_settings.includeApps.empty() || g_settings.alwaysHideTaskbar);
    Hook_Configure(g_settings.longPressPopup, g_settings.longPressMs);
    Update_Configure();
    FloatWindows_Configure(g_settings.keepFloatsOnTop, g_settings.pinTopmost);
    Hotkeys_Apply();
    Attention_Configure();
    Popup_Configure();
}

}  // namespace app

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    using namespace app;

    // 切换权限时新实例带 kRestartArg 启动，等旧实例收拾完、退出后再运行。
    // 管理员实例建的互斥量普通权限打不开（ERROR_ACCESS_DENIED），也算已经在运行
    bool restart = wcsstr(GetCommandLineW(), kRestartArg) != nullptr;
    HANDLE mutex = nullptr;
    for (int i = 0;; ++i) {
        mutex = CreateMutexW(nullptr, TRUE, L"Local\\TaskbarPopup.SingleInstance");
        DWORD err = GetLastError();
        if (mutex && err != ERROR_ALREADY_EXISTS) break;
        if (mutex) CloseHandle(mutex);
        mutex = nullptr;
        if (!restart || i >= 100) return 0;
        Sleep(100);
    }

    // 清单里也声明了，这里兜底。Win10 1703 以前没有这个函数，直接调用的话 exe 在那些系统上根本启动不了
    using SetDpiContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (auto setDpiContext = reinterpret_cast<SetDpiContextFn>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))))
        setDpiContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_instance = instance;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS};
    InitCommonControlsEx(&icc);

    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);

    SetUnhandledExceptionFilter(OnCrash);
    std::set_terminate([] {  // 未捕获的 C++ 异常不经过上面的过滤器
        Taskbar_EmergencyRestore();
        std::abort();
    });
    LoadSettings();
    if (g_settings.runAsAdmin && !IsElevated()) {
        // 设置了以管理员身份运行却是普通权限启动的（比如双击 exe）：交给计划任务（不弹 UAC）或者弹 UAC 重新启动
        CloseHandle(mutex);
        mutex = nullptr;
        // 计划任务里记的是建任务时的程序路径：程序换了位置（解压了新版本）就别用它，弹 UAC，启动后会更新任务
        if ((AdminTask_MatchesExe() && AdminTask_Run()) || RelaunchElevated()) return 0;
        mutex = CreateMutexW(nullptr, TRUE, L"Local\\TaskbarPopup.SingleInstance");
        if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    }
    Log(L"---- 启动 ----");
    if (IsElevated()) Log(L"以管理员身份运行");
    Taskbar_RestoreAll();  // 上次异常退出可能把任务栏留在隐藏状态、自动隐藏还开着

    s_trayIcon = CreateAppIcon();
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = instance;
    wc.lpszClassName = kMainClass;
    wc.hIcon = s_trayIcon;
    RegisterClassExW(&wc);
    g_mainWnd = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, L"TaskbarPopup", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                instance, nullptr);
    SendMessageW(g_mainWnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(s_trayIcon));
    s_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // 以管理员身份运行时，资源管理器（普通权限）发来的消息默认会被拦下
    ChangeWindowMessageFilterEx(g_mainWnd, s_msgTaskbarCreated, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_mainWnd, WM_APP_TRAY, MSGFLT_ALLOW, nullptr);
    // 刚切换成以管理员身份运行：原来写在 Run 里的开机自启换成计划任务；程序换了位置时计划任务也改成现在的路径
    if (g_settings.runAsAdmin && IsElevated() && (RunKeyExists() || (!AdminTask_MatchesExe() && AdminTask_Exists())))
        SetAutoStart(true);
    Elevation_Init();

    Popup_Init();
    ApplySettings();
    if (g_settings.debugLog) Fullscreen_LogState();
    if (!Hook_Install())
        MessageBoxW(nullptr, Tr(L"无法安装键盘钩子，长按 Win 功能不可用。"), L"TaskbarPopup", MB_ICONWARNING);
    AddTrayIcon();
    Hotkeys_Apply();  // 快捷键注册不上时，上面那次托盘图标还没加上、气泡弹不出来，现在再试一次

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Log(L"退出：结束后台线程");
    FloatWindows_Configure(false);
    Elevation_Shutdown();
    Update_Stop();
    Popup_Destroy();
    Apps_Stop();
    Volume_Release();
    Windows_ClearCache();
    if (s_trayIcon) DestroyIcon(s_trayIcon);
    Gdiplus::GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    Log(L"退出完成");
    // 后台线程（桌面图标、任务栏动画）没在限定时间里结束时还可能在用全局对象，
    // 正常 return 会跑静态析构、把它们释放掉。收尾已经做完，直接结束进程
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
