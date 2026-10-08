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

enum MenuId { ID_SHOW = 100, ID_SETTINGS, ID_AUTOHIDE, ID_LONGPRESS, ID_AUTOSTART, ID_DEBUGLOG, ID_OPENLOG, ID_RUNASADMIN, ID_EXIT };

NOTIFYICONDATAW s_nid = {};
HICON s_trayIcon = nullptr;
UINT s_msgTaskbarCreated = 0;

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
    lstrcpynW(s_nid.szTip, L"TaskbarPopup：全屏隐藏任务栏 / 长按 Win 弹出", ARRAYSIZE(s_nid.szTip));
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
        case ID_AUTOHIDE:
            g_settings.autoHideOnFullscreen = !g_settings.autoHideOnFullscreen;
            SaveSettings();
            ApplySettings();
            break;
        case ID_LONGPRESS:
            g_settings.longPressPopup = !g_settings.longPressPopup;
            SaveSettings();
            ApplySettings();
            break;
        case ID_AUTOSTART:
            SetAutoStart(!IsAutoStartEnabled());
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
    wchar_t longPressText[64];
    swprintf(longPressText, 64, L"长按 Win 键弹出迷你任务栏（%d 毫秒）", g_settings.longPressMs);

    AppendMenuW(menu, MF_STRING, ID_SHOW, L"显示迷你任务栏");
    AppendMenuW(menu, MF_STRING, ID_SETTINGS, L"设置...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | check(g_settings.autoHideOnFullscreen), ID_AUTOHIDE, L"窗口最大化或全屏时隐藏任务栏");
    AppendMenuW(menu, MF_STRING | check(g_settings.longPressPopup), ID_LONGPRESS, longPressText);
    AppendMenuW(menu, MF_STRING | check(IsAutoStartEnabled()), ID_AUTOSTART, L"开机自动启动");
    AppendMenuW(menu, MF_STRING | check(g_settings.runAsAdmin), ID_RUNASADMIN, L"以管理员身份运行（游戏里也能长按 Win）");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | check(g_settings.debugLog), ID_DEBUGLOG, L"记录诊断日志");
    if (g_settings.debugLog || GetFileAttributesW(LogFile().c_str()) != INVALID_FILE_ATTRIBUTES)
        AppendMenuW(menu, MF_STRING, ID_OPENLOG, L"打开诊断日志所在文件夹");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"退出");
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
    Hook_Uninstall();
    Brightness_Stop();
    Fullscreen_SetEnabled(false);
    Taskbar_RestoreAll();
    Shell_NotifyIconW(NIM_DELETE, &s_nid);
    if (HWND dlg = SettingsDialog_Hwnd()) DestroyWindow(dlg);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_APP_LONGPRESS:
            Log(L"长按 Win");
            Popup_Toggle();
            return 0;

        case WM_APP_AUTOHIDE_OFF:
            Fullscreen_OnAutoHideOff(static_cast<UINT>(wParam), lParam != 0);
            return 0;

        case WM_APP_AUTOHIDE_ON:
            Fullscreen_OnAutoHideOn(static_cast<UINT>(wParam), lParam != 0);
            return 0;

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
            Shutdown();
            PostQuitMessage(0);
            return 0;
    }
    if (msg == s_msgTaskbarCreated && s_msgTaskbarCreated) {
        AddTrayIcon();  // 资源管理器重启后托盘图标需要重新添加
        Fullscreen_OnTaskbarCreated();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS*) {
    Taskbar_EmergencyRestore();  // 崩溃也别把任务栏留在隐藏状态
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

bool ShowTrayBalloon(const wchar_t* title, const wchar_t* text) {
    NOTIFYICONDATAW nid = s_nid;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    lstrcpynW(nid.szInfoTitle, title, ARRAYSIZE(nid.szInfoTitle));
    lstrcpynW(nid.szInfo, text, ARRAYSIZE(nid.szInfo));
    return Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
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

void LoadSettings() {
    EnsureUnicodeIni();
    std::wstring f = SettingsFile();
    g_settings.autoHideOnFullscreen = GetPrivateProfileIntW(L"General", L"AutoHideOnFullscreen", 1, f.c_str()) != 0;
    g_settings.longPressPopup = GetPrivateProfileIntW(L"General", L"LongPressPopup", 1, f.c_str()) != 0;
    int ms = static_cast<int>(GetPrivateProfileIntW(L"General", L"LongPressMs", kDefaultLongPressMs, f.c_str()));
    g_settings.longPressMs = std::max(kMinLongPressMs, std::min(ms, kMaxLongPressMs));
    g_settings.showPinnedApps = GetPrivateProfileIntW(L"General", L"ShowPinnedApps", 1, f.c_str()) != 0;
    g_settings.debugLog = GetPrivateProfileIntW(L"General", L"DebugLog", 0, f.c_str()) != 0;
    g_settings.runAsAdmin = GetPrivateProfileIntW(L"General", L"RunAsAdmin", 0, f.c_str()) != 0;
    std::vector<wchar_t> buf(32768);
    GetPrivateProfileStringW(L"General", L"ExcludeApps", L"", buf.data(), static_cast<DWORD>(buf.size()), f.c_str());
    g_settings.excludeApps.clear();
    std::wstring list = buf.data();
    for (size_t start = 0; start <= list.size();) {
        size_t end = list.find(L';', start);
        if (end == std::wstring::npos) end = list.size();
        std::wstring name = NormalizeExeName(list.substr(start, end - start));
        if (!name.empty() && !IsExcludedExe(name)) g_settings.excludeApps.push_back(name);
        start = end + 1;
    }
}

void SaveSettings() {
    EnsureUnicodeIni();
    std::wstring f = SettingsFile();
    WritePrivateProfileStringW(L"General", L"AutoHideOnFullscreen", g_settings.autoHideOnFullscreen ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"LongPressPopup", g_settings.longPressPopup ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"LongPressMs", std::to_wstring(g_settings.longPressMs).c_str(), f.c_str());
    WritePrivateProfileStringW(L"General", L"ShowPinnedApps", g_settings.showPinnedApps ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"DebugLog", g_settings.debugLog ? L"1" : L"0", f.c_str());
    WritePrivateProfileStringW(L"General", L"RunAsAdmin", g_settings.runAsAdmin ? L"1" : L"0", f.c_str());
    std::wstring exclude;
    for (const std::wstring& name : g_settings.excludeApps) exclude += (exclude.empty() ? L"" : L";") + name;
    WritePrivateProfileStringW(L"General", L"ExcludeApps", exclude.c_str(), f.c_str());
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
    Fullscreen_SetEnabled(g_settings.autoHideOnFullscreen);
    Hook_Configure(g_settings.longPressPopup, g_settings.longPressMs);
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

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  // 清单里也声明了，这里兜底
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
        MessageBoxW(nullptr, L"无法安装键盘钩子，长按 Win 功能不可用。", L"TaskbarPopup", MB_ICONWARNING);
    AddTrayIcon();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        HWND dlg = SettingsDialog_Hwnd();
        if (dlg && IsDialogMessageW(dlg, &msg)) continue;  // 设置窗口里的 Tab / 回车 / Esc
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Elevation_Shutdown();
    Popup_Destroy();
    Apps_Stop();
    Volume_Release();
    Windows_ClearCache();
    if (s_trayIcon) DestroyIcon(s_trayIcon);
    Gdiplus::GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    // 后台线程（桌面图标、任务栏动画）没在限定时间里结束时还可能在用全局对象，
    // 正常 return 会跑静态析构、把它们释放掉。收尾已经做完，直接结束进程
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
