#include "common.h"

#include <cmath>
#include <cstdarg>
#include <cwchar>
#include <cwctype>
#include <mutex>

namespace app {

std::wstring ToLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring GetClassNameStr(HWND hwnd) {
    wchar_t buf[256] = {};
    GetClassNameW(hwnd, buf, ARRAYSIZE(buf));
    return buf;
}

std::wstring GetWindowTitle(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring s(len + 1, L'\0');
    int got = GetWindowTextW(hwnd, s.data(), len + 1);
    s.resize(got);
    return s;
}

std::wstring GetProcessPath(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return {};
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = ARRAYSIZE(buf);
    std::wstring path;
    if (QueryFullProcessImageNameW(proc, 0, buf, &size)) path.assign(buf, size);
    CloseHandle(proc);
    return path;
}

HWND AppWindowTarget(HWND hwnd) {
    // UWP 应用的顶层窗口属于 ApplicationFrameHost，真正的应用进程在子窗口 CoreWindow 里。
    // 最小化或者还在启动时 CoreWindow 不挂在框架下面，是一个同标题的顶层窗口
    if (GetClassNameStr(hwnd) == L"ApplicationFrameWindow") {
        if (HWND core = FindWindowExW(hwnd, nullptr, L"Windows.UI.Core.CoreWindow", nullptr)) return core;
        std::wstring title = GetWindowTitle(hwnd);
        if (!title.empty())
            if (HWND core = FindWindowExW(nullptr, nullptr, L"Windows.UI.Core.CoreWindow", title.c_str())) return core;
    }
    return hwnd;
}

std::wstring WindowExeName(HWND hwnd) { return NormalizeExeName(GetProcessPath(AppWindowTarget(hwnd))); }

std::wstring NormalizeExeName(const std::wstring& text) {
    size_t b = text.find_first_not_of(L" \t\"");
    size_t e = text.find_last_not_of(L" \t\"");
    if (b == std::wstring::npos) return {};
    std::wstring s = text.substr(b, e - b + 1);
    size_t slash = s.find_last_of(L"\\/");
    if (slash != std::wstring::npos) s = s.substr(slash + 1);
    if (s.empty()) return {};
    if (s.find(L'.') == std::wstring::npos) s += L".exe";
    return ToLower(s);
}

bool InAppList(const std::vector<std::wstring>& list, const std::wstring& exeName) {
    return !exeName.empty() && std::find(list.begin(), list.end(), exeName) != list.end();
}

ScreenPlace* FindScreenPlace(std::vector<ScreenPlace>& list, const std::wstring& key) {
    for (ScreenPlace& p : list)
        if (p.key == key) return &p;
    return nullptr;
}

void PopupPlaceFor(HMONITOR monitor, int& edge, int& align, int& offset) {
    edge = g_settings.popupEdge;
    align = g_settings.popupAlign;
    offset = g_settings.popupOffset;
    if (!g_settings.popupPerScreen) return;
    if (const ScreenPlace* p = FindScreenPlace(g_settings.screens, MonitorKey(monitor))) {
        edge = p->edge;
        align = p->align;
        offset = p->offset;
    }
}

float ColorLuma(COLORREF c) { return 0.299f * GetRValue(c) + 0.587f * GetGValue(c) + 0.114f * GetBValue(c); }

Gdiplus::Color SystemAccent(bool light) {
    // 系统的强调色：AccentPalette 里从浅到深 8 个颜色，深色时用第 2 个（浅色 2），浅色时用第 5 个（深色 1）。
    // 读注册表不算快，隔一会儿才重新读一次
    static BYTE palette[32] = {};
    static bool valid = false;
    static DWORD readAt = 0;
    DWORD now = GetTickCount();
    if (!readAt || now - readAt > 2000) {
        readAt = now ? now : 1;
        DWORD size = sizeof(palette);
        valid = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                             L"AccentPalette", RRF_RT_REG_BINARY, nullptr, palette, &size) == ERROR_SUCCESS &&
                size == sizeof(palette);
    }
    if (!valid) return light ? Gdiplus::Color(255, 0, 95, 184) : Gdiplus::Color(255, 96, 205, 255);
    const BYTE* p = palette + (light ? 4 * 4 : 1 * 4);
    return Gdiplus::Color(255, p[0], p[1], p[2]);
}

std::vector<std::wstring>& ExceptionApps() {
    return g_settings.autoHideOnFullscreen ? g_settings.excludeApps : g_settings.includeApps;
}

bool IsExceptionExe(const std::wstring& exeName) { return InAppList(ExceptionApps(), exeName); }

void SetException(const std::wstring& exeName, bool listed) {
    if (exeName.empty() || IsExceptionExe(exeName) == listed) return;
    auto& list = ExceptionApps();
    if (listed) list.push_back(exeName);
    else list.erase(std::remove(list.begin(), list.end(), exeName), list.end());
    SaveSettings();
    Log(L"%ls %ls%ls名单", exeName.c_str(), listed ? L"加入" : L"移出",
        g_settings.autoHideOnFullscreen ? L"最大化时不隐藏任务栏的" : L"最大化时隐藏任务栏的");
    ApplySettings();
}

std::wstring GetWindowAumid(HWND hwnd, bool keepCase) {
    std::wstring id;
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store)))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(kPkeyAppUserModelId, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) id = pv.pwszVal;
        PropVariantClear(&pv);
        store->Release();
    }
    if (!id.empty()) return keepCase ? id : ToLower(id);

    // 应用商店应用、打包的桌面应用：从进程本身取
    using GetAumidFn = LONG(WINAPI*)(HANDLE, UINT32*, PWSTR);
    static auto getAumid =
        reinterpret_cast<GetAumidFn>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetApplicationUserModelId")));
    if (!getAumid) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(AppWindowTarget(hwnd), &pid);
    HANDLE proc = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!proc) return {};
    wchar_t buf[256] = {};
    UINT32 len = ARRAYSIZE(buf);
    if (getAumid(proc, &len, buf) == ERROR_SUCCESS) id = buf;
    CloseHandle(proc);
    return keepCase ? id : ToLower(id);
}

bool IsCloaked(HWND hwnd) {
    int cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

bool IsOwnProcess(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

bool IsFullscreenApp(HWND hwnd) {
    if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd) || IsZoomed(hwnd) || IsOwnProcess(hwnd)) return false;
    std::wstring cls = GetClassNameStr(hwnd);
    if (cls == L"Progman" || cls == L"WorkerW") return false;
    RECT r;
    MONITORINFO mi = {sizeof(mi)};
    if (!GetWindowRect(hwnd, &r) || !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    const RECT& m = mi.rcMonitor;
    return r.left <= m.left && r.top <= m.top && r.right >= m.right && r.bottom >= m.bottom;
}

// 后台进程直接 SetForegroundWindow 常被系统拒绝，先把输入队列挂到当前前台线程上再切换
void ForceForeground(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    if (fg == hwnd) return;
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD me = GetCurrentThreadId();
    // 前台程序卡住时不挂：挂上以后对它的窗口操作会变成同步的，跟着卡住
    bool attached = fgThread && fgThread != me && !IsHungAppWindow(fg) && AttachThreadInput(me, fgThread, TRUE);
    // 别的程序的窗口用异步的方式提到最前（BringWindowToTop 要等它处理完，它卡住时本程序也跟着卡住）
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_ASYNCWINDOWPOS);
    SetForegroundWindow(hwnd);
    if (attached) AttachThreadInput(me, fgThread, FALSE);
}

float MonitorScale(HMONITOR monitor) {
    UINT dpiX = 96, dpiY = 96;
    if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX > 0)
        return dpiX / 96.0f;
    return 1.0f;
}

std::wstring MonitorKey(HMONITOR monitor) {
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) return L"";
    DISPLAY_DEVICEW dd = {sizeof(dd)};
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME) && dd.DeviceID[0]) return ToLower(dd.DeviceID);
    return ToLower(mi.szDevice);
}

int MonitorNumber(HMONITOR monitor) {
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) return 0;
    // 设备名形如 \\.\DISPLAY2，取最后的编号
    const wchar_t* digits = mi.szDevice + wcslen(mi.szDevice);
    while (digits > mi.szDevice && iswdigit(digits[-1])) --digits;
    return *digits ? std::max(0, _wtoi(digits)) : 0;
}

std::wstring MonitorLabel(HMONITOR monitor) {
    MONITORINFO mi = {sizeof(mi)};
    bool primary = GetMonitorInfoW(monitor, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY);
    int number = MonitorNumber(monitor);
    if (number <= 0) return primary ? Tr(L"主屏幕") : Tr(L"屏幕");
    return primary ? TrF(L"屏幕 %d（主屏幕）", number) : TrF(L"屏幕 %d", number);
}

std::vector<HMONITOR> MonitorsInOrder() {
    std::vector<std::pair<RECT, HMONITOR>> found;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT r, LPARAM param) -> BOOL {
            reinterpret_cast<std::vector<std::pair<RECT, HMONITOR>>*>(param)->push_back({*r, m});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.first.left != b.first.left ? a.first.left < b.first.left : a.first.top < b.first.top;
    });
    std::vector<HMONITOR> list;
    for (const auto& f : found) list.push_back(f.second);
    return list;
}

HMONITOR NextMonitor(HMONITOR monitor) {
    std::vector<HMONITOR> list = MonitorsInOrder();
    if (list.empty()) return nullptr;
    auto it = std::find(list.begin(), list.end(), monitor);
    return it == list.end() || ++it == list.end() ? list.front() : *it;
}

namespace {

// r 在 from 里的位置换到 to 里：中心在差不多的相对位置，大小不变（放不下就缩小），整个放进 to
RECT MapRect(const RECT& r, const RECT& from, const RECT& to) {
    LONG tw = to.right - to.left, th = to.bottom - to.top;
    LONG w = std::min(r.right - r.left, tw), h = std::min(r.bottom - r.top, th);
    double rx = ((r.left + r.right) / 2.0 - from.left) / std::max(1L, from.right - from.left);
    double ry = ((r.top + r.bottom) / 2.0 - from.top) / std::max(1L, from.bottom - from.top);
    LONG left = std::clamp(to.left + static_cast<LONG>(std::lround(rx * tw - w / 2.0)), to.left, to.right - w);
    LONG top = std::clamp(to.top + static_cast<LONG>(std::lround(ry * th - h / 2.0)), to.top, to.bottom - h);
    return {left, top, left + w, top + h};
}

// 挪过去的最大化窗口：过一会儿看看系统有没有照还原后的位置把它最大化到那块屏上
struct PendingMove {
    HWND hwnd;
    HMONITOR to;
};
std::vector<PendingMove> s_moves;

void CALLBACK OnMoveCheck(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    std::vector<PendingMove> moves;
    moves.swap(s_moves);
    for (const PendingMove& m : moves) {
        if (!IsWindow(m.hwnd) || !IsZoomed(m.hwnd) || MonitorFromWindow(m.hwnd, MONITOR_DEFAULTTONEAREST) == m.to) continue;
        // 还最大化在原来的屏上：先还原（还原后的位置已经在那块屏上了），再最大化
        Log(L"最大化的窗口没跟着换屏幕，还原到那块屏上再最大化");
        WINDOWPLACEMENT wp = {sizeof(wp)};
        if (!GetWindowPlacement(m.hwnd, &wp)) continue;
        wp.showCmd = SW_SHOWNOACTIVATE;
        wp.flags = (wp.flags & ~WPF_RESTORETOMAXIMIZED) | WPF_ASYNCWINDOWPLACEMENT;
        SetWindowPlacement(m.hwnd, &wp);
        ShowWindowAsync(m.hwnd, SW_SHOWMAXIMIZED);
    }
}

}  // namespace

bool MoveWindowToMonitor(HWND hwnd, HMONITOR to) {
    HMONITOR from = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO a = {sizeof(a)}, b = {sizeof(b)};
    if (!IsWindow(hwnd) || !to || to == from || !GetMonitorInfoW(from, &a) || !GetMonitorInfoW(to, &b)) return false;
    if (!IsIconic(hwnd) && !IsZoomed(hwnd)) {
        RECT cur;
        if (!GetWindowRect(hwnd, &cur)) return false;
        RECT r = MapRect(cur, a.rcWork, b.rcWork);
        Log(L"把窗口挪到%ls (%ld,%ld)", MonitorLabel(to).c_str(), r.left, r.top);
        SetWindowRectAsync(hwnd, r);
        return true;
    }
    // 最大化、最小化的窗口改还原后的位置。它是工作区坐标：原点是所在那块屏工作区的左上角（工具窗口除外）
    WINDOWPLACEMENT wp = {sizeof(wp)};
    if (!GetWindowPlacement(hwnd, &wp)) return false;
    bool tool = (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;
    auto shift = [tool](const MONITORINFO& mi) {
        return tool ? POINT{0, 0} : POINT{mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top};
    };
    POINT sa = shift(a), sb = shift(b);
    RECT normal = wp.rcNormalPosition;
    OffsetRect(&normal, sa.x, sa.y);
    RECT r = MapRect(normal, a.rcWork, b.rcWork);
    OffsetRect(&r, -sb.x, -sb.y);
    wp.rcNormalPosition = r;
    wp.flags |= WPF_ASYNCWINDOWPLACEMENT;
    bool zoomed = IsZoomed(hwnd);
    // 最小化的在那块屏上还原出来（本来还原成最大化的照样最大化）
    wp.showCmd = zoomed || (wp.flags & WPF_RESTORETOMAXIMIZED) ? SW_SHOWMAXIMIZED : SW_RESTORE;
    Log(L"把%ls窗口挪到%ls", zoomed ? L"最大化的" : L"最小化的", MonitorLabel(to).c_str());
    if (!SetWindowPlacement(hwnd, &wp)) {
        // 改不了还原后的位置（Wine 里别的程序的窗口就不行）：先还原、挪过去，本来要最大化的再最大化
        Log(L"改不了还原后的位置（错误 %lu），还原了再挪", GetLastError());
        OffsetRect(&r, sb.x, sb.y);
        ShowWindowAsync(hwnd, SW_SHOWNOACTIVATE);
        SetWindowRectAsync(hwnd, r);
        if (wp.showCmd == SW_SHOWMAXIMIZED) ShowWindowAsync(hwnd, SW_SHOWMAXIMIZED);
        return true;
    }
    if (zoomed) {
        if (s_moves.empty()) SetTimer(nullptr, 0, 300, OnMoveCheck);
        s_moves.push_back({hwnd, to});
    }
    return true;
}

// HICON → 带 alpha 通道的 GDI+ 位图（Bitmap::FromHICON 会丢掉透明度）
std::shared_ptr<Gdiplus::Bitmap> BitmapFromIcon(HICON hIcon) {
    ICONINFO ii = {};
    if (!GetIconInfo(hIcon, &ii)) return nullptr;

    std::shared_ptr<Gdiplus::Bitmap> result;
    BITMAP bm = {};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm)) {
        int w = bm.bmWidth, h = bm.bmHeight;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        std::vector<DWORD> px(static_cast<size_t>(w) * h);
        HDC dc = GetDC(nullptr);
        GetDIBits(dc, ii.hbmColor, 0, h, px.data(), &bi, DIB_RGB_COLORS);

        bool hasAlpha = std::any_of(px.begin(), px.end(), [](DWORD p) { return (p & 0xFF000000) != 0; });
        if (!hasAlpha) {
            // 老式图标没有 alpha，用掩码决定透明区域
            std::vector<DWORD> mask(px.size(), 0);
            if (ii.hbmMask) GetDIBits(dc, ii.hbmMask, 0, h, mask.data(), &bi, DIB_RGB_COLORS);
            for (size_t i = 0; i < px.size(); ++i) px[i] = (mask[i] & 0x00FFFFFF) ? 0 : (px[i] | 0xFF000000);
        }
        ReleaseDC(nullptr, dc);

        auto bmp = std::make_shared<Gdiplus::Bitmap>(w, h, PixelFormat32bppARGB);
        Gdiplus::Rect rect(0, 0, w, h);
        Gdiplus::BitmapData data;
        if (bmp->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data) == Gdiplus::Ok) {
            for (int y = 0; y < h; ++y)
                memcpy(static_cast<BYTE*>(data.Scan0) + y * data.Stride, &px[static_cast<size_t>(y) * w], w * 4);
            bmp->UnlockBits(&data);
            result = bmp;
        }
    } else {
        // 单色图标
        result.reset(Gdiplus::Bitmap::FromHICON(hIcon));
    }

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return result;
}

double NowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart * 1000.0 / freq;
}

DWORD WindowsBuild() {
    // GetVersionEx 会按清单里声明支持的系统谎报版本，RtlGetVersion 不会
    static const DWORD build = [] {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
        OSVERSIONINFOW ver = {sizeof(ver)};
        return rtlGetVersion && rtlGetVersion(&ver) == 0 ? ver.dwBuildNumber : 0;
    }();
    return build;
}

std::wstring LogFile() { return SettingsDir() + L"\\debug.log"; }

void Log(const wchar_t* format, ...) {
    if (!g_settings.debugLog) return;
    wchar_t text[1024];
    va_list args;
    va_start(args, format);
    vswprintf(text, ARRAYSIZE(text), format, args);
    va_end(args);
    text[ARRAYSIZE(text) - 1] = L'\0';

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[1200];
    int n = swprintf(line, ARRAYSIZE(line), L"%02u:%02u:%02u.%03u %ls\r\n", st.wHour, st.wMinute, st.wSecond,
                     st.wMilliseconds, text);
    if (n <= 0) return;
    std::string utf8(static_cast<size_t>(n) * 3, '\0');
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, n, utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
    if (bytes <= 0) return;

    static std::mutex lock;  // 几个线程都会写
    std::lock_guard<std::mutex> guard(lock);
    CreateDirectoryW(SettingsDir().c_str(), nullptr);
    std::wstring path = LogFile();
    auto open = [&] {
        return CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    };
    HANDLE file = open();
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size = {};
    if (GetFileSizeEx(file, &size) && size.QuadPart > 4 * 1024 * 1024) {
        // 一直开着也不会越记越大：满了就换成 debug.old.log，从头再记
        CloseHandle(file);
        MoveFileExW(path.c_str(), (SettingsDir() + L"\\debug.old.log").c_str(), MOVEFILE_REPLACE_EXISTING);
        file = open();
        if (file == INVALID_HANDLE_VALUE) return;
    }
    if (GetFileSizeEx(file, &size) && size.QuadPart == 0) {
        static const char bom[] = "\xEF\xBB\xBF";  // 记事本按 UTF-8 打开
        DWORD written = 0;
        WriteFile(file, bom, 3, &written, nullptr);
    }
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(bytes), &written, nullptr);
    CloseHandle(file);
}

// DwmFlush 会一直等到 DWM 合成下一帧；不可用（或者直接返回）时退回到睡 8 毫秒，免得空转
void WaitForVBlank() {
    double start = NowMs();
    if (FAILED(DwmFlush()) || NowMs() - start < 0.3) Sleep(8);
}

bool ThemeIsLight(int theme, bool taskbar) {
    if (theme == kThemeLight) return true;
    if (theme == kThemeDark) return false;
    DWORD value = taskbar ? 0 : 1, size = sizeof(value);  // 读不到时和 Windows 的默认一样：任务栏深色、应用浅色
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 taskbar ? L"SystemUsesLightTheme" : L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value != 0;
}

bool CaptureScreen(const RECT& area, std::vector<DWORD>& pixels) {
    int w = area.right - area.left, h = area.bottom - area.top;
    if (w <= 0 || h <= 0) return false;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib) {
        HGDIOBJ old = SelectObject(mem, dib);
        // CAPTUREBLT：分层窗口也要截进来
        ok = BitBlt(mem, 0, 0, w, h, screen, area.left, area.top, SRCCOPY | CAPTUREBLT) != FALSE;
        GdiFlush();
        if (ok) {
            const DWORD* src = static_cast<const DWORD*>(bits);
            pixels.assign(src, src + static_cast<size_t>(w) * h);
            for (DWORD& p : pixels) p |= 0xFF000000;  // BitBlt 不填 alpha
        }
        SelectObject(mem, old);
        DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

void SetWindowRectAsync(HWND hwnd, const RECT& rect) {
    SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

void AddRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
    float d = std::min({radius * 2, r.Width, r.Height});
    if (d <= 0) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

}  // namespace app
