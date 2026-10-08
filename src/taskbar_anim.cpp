// 任务栏滑出 / 滑入的过渡动画。
//
// 滑出：截一张任务栏的图，用一个置顶、点击穿透的分层窗口盖在原位，再藏起真任务栏、
//       打开系统的自动隐藏任务栏（工作区变成整块屏幕，最大化的窗口跟着变大），
//       这些都发生在截图后面，看不出来；等窗口铺满了再让截图滑出屏幕。
// 滑入：截图从屏幕外滑回原位，盖住任务栏的位置以后关掉自动隐藏（窗口在截图后面缩回去），
//       等任务栏回到原位再显示它，截图停一下后淡出。
// 动画在单独的线程上按显示器刷新的节奏逐帧推进，不占用主线程。
#include "common.h"

#include <atomic>
#include <cmath>
#include <cstdlib>

namespace app {
namespace {

constexpr wchar_t kOverlayClass[] = L"TaskbarPopupSlide";
constexpr UINT kCmdHide = WM_APP + 1;
constexpr UINT kCmdShow = WM_APP + 2;
constexpr UINT kCmdAutoHideOff = WM_APP + 3;
constexpr double kHideMs = 230.0 * TP_ANIM_SCALE;
constexpr double kShowMs = 260.0 * TP_ANIM_SCALE;
constexpr double kHoldMs = 90.0 * TP_ANIM_SCALE;  // 真任务栏显示出来后，截图再盖一会儿，等它画好
constexpr double kFadeMs = 120.0 * TP_ANIM_SCALE;
constexpr double kFillWaitMs = 500.0 * TP_ANIM_SCALE;  // 截图滑走之前最多等窗口铺满这么久
constexpr double kDockWaitMs = 800.0;  // 关掉自动隐藏以后最多等任务栏回到原位这么久

struct HideCommand {
    HWND taskbar;
    HWND stretch;
    RECT stretchRect;
    bool animate;
    UINT autoHideSeq;  // 不为 0：藏好以后打开自动隐藏
};

struct Slide {
    HWND taskbar = nullptr;
    HWND overlay = nullptr;
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;  // 两倍大：一半是截图，一半透明，滑动时只改取图的起点
    HGDIOBJ oldBitmap = nullptr;
    RECT rect = {};            // 截图时任务栏在屏幕上的位置
    int w = 0, h = 0;
    int dx = 0, dy = 0;        // 滑出的方向
    double pos = 0;            // 0 = 在原位，1 = 完全滑出
    double from = 0, to = 0, start = 0, duration = 0;
    bool moving = false;
    HWND fillWindow = nullptr;  // 滑出前先等这个窗口铺满 fillRect（最多等到 fillDeadline）
    RECT fillRect = {};
    double fillDeadline = 0;
    bool docking = false;      // 已滑回原位，等任务栏回到原位（关掉自动隐藏后它要从屏幕外挪回来）再显示它
    double coveredAt = 0;      // 滑回原位的时间
    bool settling = false;     // 真任务栏正在显示出来，截图停留 / 淡出
    double shownAt = 0;        // 发出显示真任务栏请求的时间
    double settleStart = 0;    // 真任务栏真正显示出来的时间，0 = 还没显示
};

// 以下只在动画线程里访问
std::vector<std::unique_ptr<Slide>> s_slides;
UINT s_offSeq = 0;  // 还没执行的“关掉自动隐藏”请求的编号，0 = 没有

HANDLE s_thread = nullptr;
DWORD s_threadId = 0;
// TaskbarAnim_Stop 每次加一。动画线程记下自己启动时的值，对不上就是已经让它退出了：
// 主线程等它超时、已经把任务栏和自动隐藏还原以后，它不能再去藏任务栏、开自动隐藏
std::atomic<UINT> s_generation{0};
thread_local UINT t_generation = 0;
std::atomic<UINT> s_wantedSeq{0};  // 主线程现在要开的自动隐藏请求编号，0 = 要关

bool Stopped() { return t_generation != s_generation; }

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ReleaseImage(Slide& s) {
    if (s.dc) {
        SelectObject(s.dc, s.oldBitmap);
        DeleteDC(s.dc);
    }
    if (s.bitmap) DeleteObject(s.bitmap);
    s.dc = nullptr;
    s.bitmap = nullptr;
    s.oldBitmap = nullptr;
}

void Destroy(Slide& s) {
    ReleaseImage(s);
    if (s.overlay) DestroyWindow(s.overlay);
    s.overlay = nullptr;
}

Slide* Find(HWND taskbar) {
    for (auto& s : s_slides)
        if (s->taskbar == taskbar) return s.get();
    return nullptr;
}

// 任务栏确实整条露在屏幕上才值得做动画（截图也要截完整的一条）：自动隐藏时它缩在屏幕外、
// 或者正在滑进滑出，有全屏程序时它被压在下面（资源管理器会去掉它的置顶，或者全屏窗口自己是置顶的）
bool OnScreen(HWND taskbar, RECT& r) {
    if (!IsWindowVisible(taskbar) || !GetWindowRect(taskbar, &r)) return false;
    if (!(GetWindowLongPtrW(taskbar, GWL_EXSTYLE) & WS_EX_TOPMOST)) return false;
    MONITORINFO mi = {sizeof(mi)};
    RECT shown;
    if (!GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi) ||
        !IntersectRect(&shown, &r, &mi.rcMonitor))
        return false;
    int thickness = std::min(r.right - r.left, r.bottom - r.top);
    int visible = std::min(shown.right - shown.left, shown.bottom - shown.top);
    if (thickness <= 0 || visible < thickness - std::max(2, thickness / 16)) return false;
    // 露出来的那一段中间确实是任务栏本身，截图才不会截到盖在上面的别的窗口
    POINT mid = {(shown.left + shown.right) / 2, (shown.top + shown.bottom) / 2};
    HWND hit = WindowFromPoint(mid);
    return hit && GetAncestor(hit, GA_ROOT) == taskbar;
}

// 横着的任务栏上下滑，竖着的左右滑，朝离它最近的屏幕边缘滑出去
void SetDirection(Slide& s) {
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&s.rect, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& m = mi.rcMonitor;
    s.dx = s.dy = 0;
    if (s.w >= s.h) s.dy = (s.rect.top - m.top < m.bottom - s.rect.bottom) ? -1 : 1;
    else s.dx = (s.rect.left - m.left < m.right - s.rect.right) ? -1 : 1;
}

bool CaptureImage(Slide& s) {
    std::vector<DWORD> pixels;
    if (!CaptureScreen(s.rect, pixels)) return false;
    int bw = s.dx ? 2 * s.w : s.w, bh = s.dy ? 2 * s.h : s.h;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bw;
    bi.bmiHeader.biHeight = -bh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return false;
    DWORD* dst = static_cast<DWORD*>(bits);
    std::fill(dst, dst + static_cast<size_t>(bw) * bh, 0);
    int ox = s.dx > 0 ? s.w : 0, oy = s.dy > 0 ? s.h : 0;
    for (int y = 0; y < s.h; ++y)
        memcpy(dst + static_cast<size_t>(oy + y) * bw + ox, &pixels[static_cast<size_t>(y) * s.w], s.w * 4);

    ReleaseImage(s);
    s.dc = CreateCompatibleDC(nullptr);
    s.bitmap = bmp;
    s.oldBitmap = SelectObject(s.dc, bmp);
    return true;
}

// 截图在窗口里往外挪 pos（0~1）个任务栏的厚度
void Present(Slide& s, double pos, BYTE alpha) {
    int o = static_cast<int>(std::lround(pos * (s.dx ? s.w : s.h)));
    POINT src = {s.dx > 0 ? s.w - o : (s.dx < 0 ? o : 0), s.dy > 0 ? s.h - o : (s.dy < 0 ? o : 0)};
    POINT dst = {s.rect.left, s.rect.top};
    SIZE size = {s.w, s.h};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(s.overlay, nullptr, &dst, &size, s.dc, &src, 0, &blend, ULW_ALPHA);
}

// 系统给弹出窗口加的淡入淡出会让截图晚一点才完全盖上，关掉
void NoTransitions(HWND hwnd) {
    BOOL disable = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));
}

void ShowOverlay(Slide& s) {
    SetWindowPos(s.overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void StartMove(Slide& s, double to) {
    s.from = s.pos;
    s.to = to;
    s.start = NowMs();
    s.duration = (to > s.pos ? kHideMs : kShowMs) * std::max(0.3, std::fabs(to - s.pos));
    s.moving = true;
    s.fillWindow = nullptr;
    s.docking = false;
    s.settling = false;
}

// 关掉本程序打开的自动隐藏，再告诉主线程（它接着把没跟着缩回去的窗口、桌面图标收拾好）
// 资源管理器没在运行（崩溃、重启中）时关不掉，也照实告诉主线程：它会记着自动隐藏还开着，等资源管理器回来再关
void ApplyPendingOff() {
    if (!s_offSeq) return;
    UINT seq = s_offSeq;
    s_offSeq = 0;
    DesktopIcons_WaitUpdated(1000);  // 工作区一变桌面就重新排列，用户挪过的图标得先读完
    // 等的时候主线程改了主意（窗口又最大化了，已经要求重新打开），或者要退出了（主线程自己关）：不关了。
    // 主线程那边已经不等这次的回报
    if (Stopped() || s_wantedSeq != 0) return;
    bool done = Taskbar_SetAutoHide(false);
    PostMessageW(g_mainWnd, WM_APP_AUTOHIDE_OFF, seq, done);
}

bool SlidingIn() {
    for (auto& s : s_slides)
        if (IsWindow(s->taskbar) && ((s->moving && s->to == 0) || s->docking)) return true;
    return false;
}

// 拉伸前再确认一次：从决定拉伸到现在可能过了一两秒，窗口也许已经还原或者换了屏幕
void StretchIfStillMaximized(HWND hwnd, const RECT& rect) {
    if (!hwnd || !IsWindow(hwnd) || !IsZoomed(hwnd)) return;
    if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) != MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST)) return;
    SetWindowRectAsync(hwnd, rect);
}

// 真任务栏藏好以后：需要的话打开自动隐藏，再拉伸窗口（工作区变大以后 Chromium 系的程序才肯铺满）
void AfterHidden(const HideCommand& c) {
    if (c.autoHideSeq) {
        s_offSeq = 0;  // 还没执行的关闭请求作废
        DesktopIcons_WaitSaved(1500);  // 工作区一变桌面就重新排列，得先记完图标位置
        // 要退出了，或者主线程已经改主意要关了（窗口最大化后马上又还原）：不开，也不拉伸
        if (Stopped() || c.autoHideSeq != s_wantedSeq) return;
        bool done = Taskbar_SetAutoHide(true);
        PostMessageW(g_mainWnd, WM_APP_AUTOHIDE_ON, c.autoHideSeq, done);
        if (!done) return;  // 资源管理器没在运行：它回来以后主线程会重新藏、重新开
    }
    if (!Stopped()) StretchIfStillMaximized(c.stretch, c.stretchRect);
}

// 截图滑走之前等窗口在它后面铺满，免得先露出一条空白、窗口再变大
void WaitForFill(Slide& s, const HideCommand& c) {
    s.fillWindow = c.stretch;
    s.fillRect = c.stretchRect;
    s.fillDeadline = NowMs() + kFillWaitMs;
}

bool Filled(const Slide& s) {
    RECT r;
    if (!IsWindow(s.fillWindow) || IsIconic(s.fillWindow) || !GetWindowRect(s.fillWindow, &r)) return true;
    const RECT& want = s.fillRect;
    LONG tol = static_cast<LONG>(std::lround(4 * MonitorScale(MonitorFromRect(&want, MONITOR_DEFAULTTONEAREST))));
    return r.left <= want.left + tol && r.top <= want.top + tol && r.right >= want.right - tol &&
           r.bottom >= want.bottom - tol;
}

void OnHide(const HideCommand& c) {
    if (Stopped()) return;  // 主线程已经在收拾了
    Slide* s = Find(c.taskbar);
    if (s && s->moving && s->to == 1) {
        // 已经在往外滑了；资源管理器要是趁这时把任务栏又显示了出来，再藏一次（它在截图后面，看不出来）
        if (IsWindowVisible(c.taskbar)) ShowWindowAsync(c.taskbar, SW_HIDE);
        AfterHidden(c);
        return;
    }
    if (s && s->bitmap && (s->moving || s->docking || s->settling)) {
        // 正在滑回来，或者刚滑回来：从当前位置掉头
        Present(*s, s->pos, 255);
        if (s->docking || s->settling) {
            WaitForVBlank();  // 截图先恢复不透明，再藏真任务栏
            ShowWindowAsync(c.taskbar, SW_HIDE);
        }
        AfterHidden(c);
        StartMove(*s, 1);
        if (c.stretch) WaitForFill(*s, c);
        return;
    }

    RECT r;
    if (!c.animate || !OnScreen(c.taskbar, r)) {
        bool visible = IsWindowVisible(c.taskbar) != FALSE;
        if (visible) ShowWindowAsync(c.taskbar, SW_HIDE);
        AfterHidden(c);
        // 任务栏显示着却不在屏幕上（用户自己开着自动隐藏、被全屏程序压着）：直接藏，以后放出来也不做动画。
        // 已经藏好的就保留上次的截图；被资源管理器重新显示出来的也保留，放出来时照样滑回来。
        // 本程序开着自动隐藏时，任务栏缩在屏幕外是正常的（比如开始菜单关掉以后），截图还是原位的，也保留
        if (s && visible) {
            if (s->overlay) ShowWindow(s->overlay, SW_HIDE);
            if (c.animate && !c.autoHideSeq) ReleaseImage(*s);
            s->pos = 1;
        }
        return;
    }

    if (!s) {
        s_slides.push_back(std::make_unique<Slide>());
        s = s_slides.back().get();
        s->taskbar = c.taskbar;
    }
    s->rect = r;
    s->w = r.right - r.left;
    s->h = r.bottom - r.top;
    SetDirection(*s);
    if (!s->overlay) {
        s->overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                     kOverlayClass, L"", WS_POPUP, r.left, r.top, s->w, s->h, nullptr, nullptr,
                                     g_instance, nullptr);
        if (s->overlay) NoTransitions(s->overlay);
    }
    if (!s->overlay || !CaptureImage(*s)) {
        ShowWindowAsync(c.taskbar, SW_HIDE);
        AfterHidden(c);
        ReleaseImage(*s);
        s->pos = 1;
        return;
    }

    s->pos = 0;
    Present(*s, 0, 255);
    ShowOverlay(*s);
    WaitForVBlank();  // 截图上屏以后再藏真任务栏、打开自动隐藏、拉伸窗口
    ShowWindowAsync(c.taskbar, SW_HIDE);
    AfterHidden(c);
    StartMove(*s, 1);
    if (c.stretch) WaitForFill(*s, c);
}

void OnShow(HWND taskbar, UINT offSeq) {
    if (Stopped()) return;  // 主线程已经把任务栏显示出来、关掉了自动隐藏
    if (offSeq) s_offSeq = offSeq;
    Slide* s = Find(taskbar);
    RECT r;
    bool sameRect = s && GetWindowRect(taskbar, &r) && EqualRect(&r, &s->rect);
    // 没有截图（上次没做动画），或者藏起来以后任务栏换了位置、大小：直接显示。
    // 本程序打开的自动隐藏还开着时任务栏缩在屏幕外，位置对不上是正常的，关掉以后它会回原位
    if (!s || !s->bitmap || (!sameRect && !s_offSeq)) {
        if (s && s->overlay) ShowWindow(s->overlay, SW_HIDE);
        if (s) ReleaseImage(*s);
        ApplyPendingOff();
        // 不管现在看上去是否可见都发：藏的请求可能还排在资源管理器的队列里，显示的请求排在它后面
        if (IsWindow(taskbar)) ShowWindowAsync(taskbar, SW_SHOWNA);
        return;
    }
    if (s->docking || s->settling) {  // 已经滑回原位了
        ApplyPendingOff();
        return;
    }
    if (s->moving && s->to == 0) return;  // 正在滑回来，滑到原位时关自动隐藏
    if (!s->moving && s->pos == 0) {  // 本来就在原位
        ApplyPendingOff();
        ShowWindowAsync(taskbar, SW_SHOWNA);
        return;
    }
    if (!s->moving && sameRect && OnScreen(taskbar, r)) {
        // 资源管理器已经自己把任务栏显示在原位了，不用再滑一张截图上去
        ShowWindow(s->overlay, SW_HIDE);
        ReleaseImage(*s);
        s->pos = 0;
        ApplyPendingOff();
        ShowWindowAsync(taskbar, SW_SHOWNA);
        return;
    }
    bool realShown = IsWindowVisible(taskbar) != FALSE;
    if (!IsWindowVisible(s->overlay)) {
        // 真任务栏显示着却不在原位（开始菜单关掉后资源管理器正把它滑走）：截图从它现在的位置接着滑
        RECT cur;
        if (realShown && GetWindowRect(taskbar, &cur)) {
            double off = s->dx ? double(cur.left - s->rect.left) * s->dx / s->w : double(cur.top - s->rect.top) * s->dy / s->h;
            s->pos = std::min(1.0, std::max(0.0, off));
        }
        Present(*s, s->pos, 255);
        ShowOverlay(*s);
    }
    if (realShown) {
        // 截图盖上以后把真任务栏藏起来，免得两条任务栏一起动；截图滑到原位、任务栏回到原位以后再显示它
        WaitForVBlank();
        ShowWindowAsync(taskbar, SW_HIDE);
    }
    StartMove(*s, 0);
}

// 推进一帧，返回是否还有动画在进行
bool Step() {
    double now = NowMs();
    bool active = false;
    for (auto it = s_slides.begin(); it != s_slides.end();) {
        Slide& s = **it;
        if (!IsWindow(s.taskbar)) {  // 资源管理器重启过，旧任务栏没了
            Destroy(s);
            it = s_slides.erase(it);
            continue;
        }
        if (s.moving && s.fillWindow) {
            if (now < s.fillDeadline && !Filled(s)) {
                active = true;  // 截图停在原位，等窗口在后面铺满
                ++it;
                continue;
            }
            s.fillWindow = nullptr;
            s.start = now;
        }
        if (s.moving) {
            double t = std::min(1.0, (now - s.start) / s.duration);
            // 滑出先慢后快，滑入先快后慢
            double e = s.to > s.from ? t * t : 1 - (1 - t) * (1 - t) * (1 - t);
            s.pos = s.from + (s.to - s.from) * e;
            Present(s, s.pos, 255);
            if (t >= 1) {
                s.moving = false;
                if (s.to > 0) {
                    ShowWindow(s.overlay, SW_HIDE);
                } else {
                    // 截图盖住了任务栏的位置：这时关掉自动隐藏，窗口在截图后面缩回去
                    ApplyPendingOff();
                    s.docking = true;
                    s.coveredAt = NowMs();
                }
            }
        } else if (s.docking) {
            // 关掉自动隐藏以后，资源管理器要把任务栏从屏幕外挪回原位；挪好了再显示，免得它在截图前面再滑一遍
            RECT r;
            LONG tol = std::max(2, std::min(s.w, s.h) / 16);  // 截图时任务栏可能还差一两个像素才停稳
            bool docked = GetWindowRect(s.taskbar, &r) && std::abs(r.left - s.rect.left) <= tol &&
                          std::abs(r.top - s.rect.top) <= tol && std::abs(r.right - s.rect.right) <= tol &&
                          std::abs(r.bottom - s.rect.bottom) <= tol;
            if (docked || now - s.coveredAt > kDockWaitMs) {
                s.docking = false;
                ShowWindowAsync(s.taskbar, SW_SHOWNA);
                s.settling = true;
                s.shownAt = now;
                s.settleStart = 0;
            }
        } else if (s.settling) {
            // 停留时间从真任务栏确实显示出来算起（资源管理器忙的时候会晚一些），最多等 1 秒
            if (!s.settleStart && (IsWindowVisible(s.taskbar) || now - s.shownAt > 1000)) s.settleStart = now;
            double t = s.settleStart ? (now - s.settleStart - kHoldMs) / kFadeMs : 0;
            if (t >= 1) {
                s.settling = false;
                ShowWindow(s.overlay, SW_HIDE);
            } else if (t > 0) {
                Present(s, 0, static_cast<BYTE>(std::lround(255 * (1 - t))));
            }
        }
        active = active || s.moving || s.docking || s.settling;
        ++it;
    }
    // 等着滑回来的那条任务栏没了（资源管理器重启）或者已经归位：排着的关闭请求现在执行
    if (s_offSeq && !SlidingIn()) ApplyPendingOff();
    return active;
}

// 处理一条消息，返回 false 表示该退出了
bool Handle(MSG& msg) {
    if (msg.message == WM_QUIT) return false;
    if (!msg.hwnd && msg.message == kCmdHide) {
        std::unique_ptr<HideCommand> c(reinterpret_cast<HideCommand*>(msg.lParam));
        OnHide(*c);
        return true;
    }
    if (!msg.hwnd && msg.message == kCmdShow) {
        OnShow(reinterpret_cast<HWND>(msg.lParam), static_cast<UINT>(msg.wParam));
        return true;
    }
    if (!msg.hwnd && msg.message == kCmdAutoHideOff) {
        if (Stopped()) return true;
        s_offSeq = static_cast<UINT>(msg.wParam);
        if (!SlidingIn()) ApplyPendingOff();  // 正在滑回来的话等它滑到原位再关
        return true;
    }
    DispatchMessageW(&msg);
    return true;
}

DWORD WINAPI AnimThread(LPVOID ready) {
    t_generation = s_generation;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = g_instance;
    wc.lpszClassName = kOverlayClass;
    RegisterClassExW(&wc);
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // 先建好消息队列
    SetEvent(static_cast<HANDLE>(ready));

    bool active = false;
    bool running = true;
    while (running) {
        // 没有动画时睡着等命令；有动画时每帧顺手处理新来的消息
        if (!active) {
            if (GetMessageW(&msg, nullptr, 0, 0) <= 0 || !Handle(msg)) break;
        }
        while (running && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) running = Handle(msg);
        if (!running) break;
        active = Step();
        if (active) WaitForVBlank();
    }

    for (auto& s : s_slides) Destroy(*s);
    s_slides.clear();
    while (PeekMessageW(&msg, nullptr, kCmdHide, kCmdHide, PM_REMOVE))  // 没来得及处理的命令
        delete reinterpret_cast<HideCommand*>(msg.lParam);
    return 0;
}

bool EnsureThread() {
    if (s_thread) return true;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_thread = CreateThread(nullptr, 0, AnimThread, ready, 0, &s_threadId);
    // 一直等到线程建好消息队列：等不到就关掉事件的话，线程以后会去 SetEvent 一个已经关掉（可能被重用）的句柄，
    // 之前发给它的命令和退出消息也会丢
    if (s_thread && ready) WaitForSingleObject(ready, INFINITE);
    if (ready) CloseHandle(ready);
    return s_thread != nullptr;
}

}  // namespace

void TaskbarAnim_Hide(HWND taskbar, HWND stretch, const RECT& stretchRect, bool animate, UINT autoHideSeq) {
    if (EnsureThread()) {
        auto* c = new HideCommand{taskbar, stretch, stretchRect, animate, autoHideSeq};
        if (PostThreadMessageW(s_threadId, kCmdHide, 0, reinterpret_cast<LPARAM>(c))) return;
        delete c;
    }
    // 动画线程不可用：直接藏
    ShowWindowAsync(taskbar, SW_HIDE);
    if (autoHideSeq) {
        DesktopIcons_WaitSaved(1500);
        bool done = Taskbar_SetAutoHide(true);
        PostMessageW(g_mainWnd, WM_APP_AUTOHIDE_ON, autoHideSeq, done);
        if (!done) return;
    }
    StretchIfStillMaximized(stretch, stretchRect);
}

void TaskbarAnim_WantAutoHide(UINT seq) { s_wantedSeq = seq; }

void TaskbarAnim_Abort() {
    ++s_generation;
    s_wantedSeq = 0;
}

void TaskbarAnim_Show(HWND taskbar, UINT offSeq) {
    if (EnsureThread() &&
        PostThreadMessageW(s_threadId, kCmdShow, offSeq, reinterpret_cast<LPARAM>(taskbar)))
        return;
    if (offSeq) TaskbarAnim_AutoHideOff(offSeq);
    ShowWindowAsync(taskbar, SW_SHOWNA);
}

void TaskbarAnim_AutoHideOff(UINT seq) {
    if (EnsureThread() && PostThreadMessageW(s_threadId, kCmdAutoHideOff, seq, 0)) return;
    DesktopIcons_WaitUpdated(1000);
    bool done = Taskbar_SetAutoHide(false);
    PostMessageW(g_mainWnd, WM_APP_AUTOHIDE_OFF, seq, done);
}

void TaskbarAnim_Stop() {
    if (!s_thread) return;
    ++s_generation;
    PostThreadMessageW(s_threadId, WM_QUIT, 0, 0);
    // 等的时候照样处理别的线程发来的消息：动画线程开关自动隐藏时，资源管理器要等每个窗口处理完广播才返回
    DWORD start = GetTickCount();
    for (DWORD elapsed = 0; elapsed < 2000; elapsed = GetTickCount() - start) {
        if (MsgWaitForMultipleObjectsEx(1, &s_thread, 2000 - elapsed, QS_SENDMESSAGE, 0) != WAIT_OBJECT_0 + 1) break;
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
    CloseHandle(s_thread);
    s_thread = nullptr;
    s_threadId = 0;
}

}  // namespace app
