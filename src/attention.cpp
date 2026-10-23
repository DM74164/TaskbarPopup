// 消息提醒：程序在任务栏上闪烁（比如微信、QQ 来了消息）时，迷你任务栏里它的格子变成橙色；
// 任务栏被本程序藏着时，那块屏幕放任务栏的那条边上再亮起一道光，闪几下以后一直淡淡地亮着，去看过就灭。
// 谁在闪从外壳钩子消息（RegisterShellHookWindow）里知道：HSHELL_FLASH 记下，窗口到了前台、关掉、藏起来就去掉
#include "common.h"

#include <cmath>

#ifndef HSHELL_HIGHBIT
#define HSHELL_HIGHBIT 0x8000
#endif
#ifndef HSHELL_FLASH
#define HSHELL_FLASH (HSHELL_REDRAW | HSHELL_HIGHBIT)
#endif
#ifndef HSHELL_RUDEAPPACTIVATED
#define HSHELL_RUDEAPPACTIVATED (HSHELL_WINDOWACTIVATED | HSHELL_HIGHBIT)
#endif

namespace app {
namespace {

constexpr UINT kCheckMs = 250;                       // 有要提醒的窗口时多久看一遍
constexpr UINT kFrameMs = 16;                        // 光闪的时候多久画一帧
constexpr double kPulseMs = 2400.0 * TP_ANIM_SCALE;  // 光刚亮起时闪几下
constexpr int kPulses = 3;
constexpr double kRestLevel = 0.7;                   // 闪完以后一直亮着的亮度
constexpr wchar_t kGlowClass[] = L"TaskbarPopupGlow";

UINT s_msgShellHook = 0;
bool s_hooked = false;
std::vector<HWND> s_urgent;  // 闪过、还没去看的窗口
UINT_PTR s_checkTimer = 0;
UINT_PTR s_frameTimer = 0;

// 一块屏幕上的光：贴着任务栏那条边的一段，中间亮两头渐隐，屏幕边上最亮、往里慢慢变淡
struct Glow {
    HMONITOR monitor = nullptr;
    HWND hwnd = nullptr;
    RECT rect = {};
    double since = 0;  // 开始闪的时间
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old = nullptr;
};
std::vector<Glow> s_glows;

// 任务栏停在哪条边（ABE_*）。资源管理器卡住时去问它会一直等，按最常见的底边算
UINT TaskbarEdge() {
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (abd.hWnd && !IsHungAppWindow(abd.hWnd) && SHAppBarMessage(ABM_GETTASKBARPOS, &abd)) return abd.uEdge;
    return ABE_BOTTOM;
}

// 刚亮起时闪 kPulses 下，最后一下慢慢落到 kRestLevel 停住
double Level(double since) {
    double x = (NowMs() - since) / kPulseMs * kPulses;
    if (x >= kPulses) return kRestLevel;
    if (x < kPulses - 0.5) return 0.5 - 0.5 * std::cos(2 * 3.14159265358979 * x);
    double u = (x - (kPulses - 0.5)) / 0.5;
    return 1 - (1 - kRestLevel) * (0.5 - 0.5 * std::cos(3.14159265358979 * u));
}

void Present(Glow& g) {
    double level = Level(g.since);
    SIZE size = {g.rect.right - g.rect.left, g.rect.bottom - g.rect.top};
    POINT src = {0, 0}, pos = {g.rect.left, g.rect.top};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(255 * level)), AC_SRC_ALPHA};
    UpdateLayeredWindow(g.hwnd, nullptr, &pos, &size, g.dc, &src, 0, &blend, ULW_ALPHA);
}

void DestroyGlow(Glow& g) {
    if (g.hwnd) DestroyWindow(g.hwnd);
    if (g.dc) {
        SelectObject(g.dc, g.old);
        DeleteDC(g.dc);
    }
    if (g.bitmap) DeleteObject(g.bitmap);
    g = {};
}

void CALLBACK OnFrame(HWND, UINT, UINT_PTR, DWORD) {
    bool more = false;
    for (Glow& g : s_glows) {
        Present(g);
        more = more || NowMs() - g.since < kPulseMs;
    }
    if (!more && s_frameTimer) {
        KillTimer(nullptr, s_frameTimer);
        s_frameTimer = 0;
    }
}

void Pulse(Glow& g) {
    g.since = NowMs();
    if (!s_frameTimer) s_frameTimer = SetTimer(nullptr, 0, kFrameMs, OnFrame);
}

bool CreateGlow(HMONITOR monitor, Glow& g) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = g_instance;
        wc.lpszClassName = kGlowClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    MONITORINFO mi = {sizeof(mi)};
    if (!GetMonitorInfoW(monitor, &mi)) return false;
    const RECT& m = mi.rcMonitor;
    float scale = MonitorScale(monitor);
    UINT edge = TaskbarEdge();
    bool horizontal = edge == ABE_BOTTOM || edge == ABE_TOP;
    int span = horizontal ? m.right - m.left : m.bottom - m.top;
    int length = std::min(span, std::max(static_cast<int>(240 * scale), span * 2 / 5));
    int thick = std::max(4, static_cast<int>(std::lround(12 * scale)));
    int start = (horizontal ? m.left : m.top) + (span - length) / 2;
    switch (edge) {
        case ABE_TOP: g.rect = {start, m.top, start + length, m.top + thick}; break;
        case ABE_LEFT: g.rect = {m.left, start, m.left + thick, start + length}; break;
        case ABE_RIGHT: g.rect = {m.right - thick, start, m.right, start + length}; break;
        default: g.rect = {start, m.bottom - thick, start + length, m.bottom}; break;
    }
    int w = g.rect.right - g.rect.left, h = g.rect.bottom - g.rect.top;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    g.bitmap = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g.bitmap) return false;
    g.dc = CreateCompatibleDC(nullptr);
    g.old = SelectObject(g.dc, g.bitmap);
    // 橙色（和任务栏上闪烁的按钮一个颜色），预乘过的 BGRA
    auto* px = static_cast<DWORD*>(bits);
    float core = 2.5f * scale, fade = length * 0.25f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int along = horizontal ? x : y;
            float across = 0;  // 离屏幕边多远
            switch (edge) {
                case ABE_TOP: across = static_cast<float>(y); break;
                case ABE_LEFT: across = static_cast<float>(x); break;
                case ABE_RIGHT: across = static_cast<float>(w - 1 - x); break;
                default: across = static_cast<float>(h - 1 - y); break;
            }
            float e = std::min(1.0f, std::min(along, length - 1 - along) / fade);
            e = e * e * (3 - 2 * e);
            float f = 1;
            if (across >= core) {
                f = 1 - (across - core) / std::max(1.0f, thick - core);
                f = 0.6f * f * f;
            }
            float a = std::clamp(e * f, 0.0f, 1.0f);
            BYTE alpha = static_cast<BYTE>(std::lround(255 * a));
            BYTE r = static_cast<BYTE>(std::lround(255 * a)), gr = static_cast<BYTE>(std::lround(140 * a));
            px[y * w + x] = (static_cast<DWORD>(alpha) << 24) | (static_cast<DWORD>(r) << 16) | (static_cast<DWORD>(gr) << 8);
        }
    }

    // 点不到、不抢前台，盖在最大化的窗口上面
    g.hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             kGlowClass, L"", WS_POPUP, g.rect.left, g.rect.top, w, h, nullptr, nullptr, g_instance,
                             nullptr);
    if (!g.hwnd) return false;
    BOOL disable = TRUE;
    DwmSetWindowAttribute(g.hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));
    g.monitor = monitor;
    Pulse(g);
    Present(g);
    ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
    return true;
}

// 哪些屏幕上要亮：有要提醒的窗口，任务栏被本程序藏着，迷你任务栏没在那块屏上开着（开着的话格子已经变色了），
// 前台也不是在那块屏上全屏的游戏、视频
std::vector<HMONITOR> WantedGlows() {
    std::vector<HMONITOR> want;
    if (!g_settings.flashAlert || !Fullscreen_TaskbarHidden()) return want;
    HMONITOR popup = Popup_Monitor();
    HWND fg = GetForegroundWindow();
    HMONITOR fullscreen = IsFullscreenApp(fg) ? MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) : nullptr;
    for (HWND h : s_urgent) {
        HMONITOR m = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        if (m == popup || m == fullscreen || std::find(want.begin(), want.end(), m) != want.end() || Taskbar_IsShownOn(m))
            continue;
        want.push_back(m);
    }
    return want;
}

void UpdateGlows() {
    std::vector<HMONITOR> want = WantedGlows();
    for (auto it = s_glows.begin(); it != s_glows.end();) {
        if (std::find(want.begin(), want.end(), it->monitor) == want.end()) {
            DestroyGlow(*it);
            it = s_glows.erase(it);
        } else {
            ++it;
        }
    }
    for (HMONITOR m : want) {
        if (std::any_of(s_glows.begin(), s_glows.end(), [&](const Glow& g) { return g.monitor == m; })) continue;
        Glow g;
        if (CreateGlow(m, g)) s_glows.push_back(g);
        else DestroyGlow(g);
    }
}

void StopChecking() {
    if (s_checkTimer) KillTimer(nullptr, s_checkTimer);
    s_checkTimer = 0;
}

// 去看过（到了前台，或者前台是它的同一组窗口）、关掉、藏起来（关到托盘里）的不再提醒
void Check() {
    HWND fg = GetForegroundWindow();
    HWND fgRoot = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    size_t before = s_urgent.size();
    s_urgent.erase(std::remove_if(s_urgent.begin(), s_urgent.end(),
                                  [&](HWND h) {
                                      return !IsWindow(h) || !IsWindowVisible(h) || h == fg ||
                                             (fgRoot && GetAncestor(h, GA_ROOTOWNER) == fgRoot);
                                  }),
                   s_urgent.end());
    if (s_urgent.size() != before) Popup_Refresh();
    UpdateGlows();
    if (s_urgent.empty()) StopChecking();
}

void CALLBACK OnCheck(HWND, UINT, UINT_PTR, DWORD) { Check(); }

void Flash(HWND h) {
    if (!h || h == GetForegroundWindow() || !IsTaskbarWindow(h)) return;
    if (std::find(s_urgent.begin(), s_urgent.end(), h) != s_urgent.end()) return;  // 一直在闪的，每闪一下都会来一次
    s_urgent.push_back(h);
    Log(L"消息提醒：%ls 在任务栏上闪", WindowExeName(h).c_str());
    Popup_Refresh();
    // 已经亮着的光再闪几下：又来了新消息
    HMONITOR m = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    for (Glow& g : s_glows)
        if (g.monitor == m) Pulse(g);
    UpdateGlows();
    if (!s_checkTimer) s_checkTimer = SetTimer(nullptr, 0, kCheckMs, OnCheck);
}

void Forget(HWND h) {
    auto it = std::find(s_urgent.begin(), s_urgent.end(), h);
    if (it == s_urgent.end()) return;
    s_urgent.erase(it);
    Popup_Refresh();
    UpdateGlows();
    if (s_urgent.empty()) StopChecking();
}

void Clear() {
    s_urgent.clear();
    StopChecking();
    for (Glow& g : s_glows) DestroyGlow(g);
    s_glows.clear();
    if (s_frameTimer) KillTimer(nullptr, s_frameTimer);
    s_frameTimer = 0;
}

}  // namespace

void Attention_Configure() {
    if (!g_mainWnd) return;
    if (g_settings.flashAlert && !s_hooked) {
        s_msgShellHook = RegisterWindowMessageW(L"SHELLHOOK");
        s_hooked = RegisterShellHookWindow(g_mainWnd) != FALSE;
        if (s_hooked) ChangeWindowMessageFilterEx(g_mainWnd, s_msgShellHook, MSGFLT_ALLOW, nullptr);
        else Log(L"消息提醒：注册外壳钩子失败（%lu）", GetLastError());
    } else if (!g_settings.flashAlert && s_hooked) {
        DeregisterShellHookWindow(g_mainWnd);
        s_hooked = false;
        Clear();
        Popup_Refresh();
    }
}

bool Attention_OnMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!s_hooked || msg != s_msgShellHook) return false;
    HWND h = reinterpret_cast<HWND>(lParam);
    switch (wParam) {
        case HSHELL_FLASH:
            Flash(h);
            break;
        case HSHELL_WINDOWACTIVATED:
        case HSHELL_RUDEAPPACTIVATED:
        case HSHELL_WINDOWDESTROYED:
            if (!s_urgent.empty()) {
                if (wParam == HSHELL_WINDOWDESTROYED) Forget(h);
                else Check();
            }
            break;
    }
    return true;
}

bool Attention_IsUrgent(HWND hwnd) {
    return hwnd && std::find(s_urgent.begin(), s_urgent.end(), hwnd) != s_urgent.end();
}

void Attention_Update() {
    if (!s_urgent.empty() || !s_glows.empty()) UpdateGlows();
}

void Attention_Shutdown() {
    if (s_hooked && g_mainWnd) DeregisterShellHookWindow(g_mainWnd);
    s_hooked = false;
    Clear();
}

}  // namespace app
