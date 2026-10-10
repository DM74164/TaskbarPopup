#include "common.h"

#include <cstdarg>
#include <cwchar>
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

bool IsExcludedExe(const std::wstring& exeName) {
    return !exeName.empty() &&
           std::find(g_settings.excludeApps.begin(), g_settings.excludeApps.end(), exeName) != g_settings.excludeApps.end();
}

void SetExcluded(const std::wstring& exeName, bool excluded) {
    if (exeName.empty() || IsExcludedExe(exeName) == excluded) return;
    auto& list = g_settings.excludeApps;
    if (excluded) list.push_back(exeName);
    else list.erase(std::remove(list.begin(), list.end(), exeName), list.end());
    SaveSettings();
    Log(L"%ls %ls排除名单", exeName.c_str(), excluded ? L"加入" : L"移出");
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
