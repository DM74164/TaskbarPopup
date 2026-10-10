// 小窗口留在全屏窗口上面：最大化（或全屏）的窗口上浮着几个小窗口时，点一下大窗口，
// 系统会把它提到最上面、小窗口全被盖住。在小窗口里双击一下，它就算“留在上面”的小窗口：
// 之后大窗口拿到前台（或者在它里面按了鼠标），就把大窗口挪回这些小窗口下面（不抢焦点），
// 大窗口照常能打字操作，小窗口一直看得见。没双击过的小窗口照系统原样被盖住；
// 双击过的小窗口最小化、最大化或关掉以后就不再管它。
//
// 光在事后挪会闪一下（大窗口先盖住小窗口，再被挪下去）。所以还装了个鼠标钩子：在大窗口上按下鼠标、
// 系统还没激活它之前，先把浮在上面的小窗口临时设成置顶，激活时大窗口怎么提也越不过它们，过一会儿再取消置顶。
// 钩子和这里的一切都跑在单独的线程上，主线程忙着画玻璃时鼠标也不会卡。
#include "common.h"

#include <algorithm>
#include <cstdlib>

namespace app {
namespace {

// 主线程启停
HANDLE s_thread = nullptr;
DWORD s_threadId = 0;

// 以下只在小窗口线程里访问
HWINEVENTHOOK s_hook = nullptr;
HHOOK s_mouseHook = nullptr;
std::vector<HWND> s_floats;  // 双击过的小窗口，最近双击的排前面

bool MonitorRect(HWND hwnd, RECT& out) {
    MONITORINFO mi = {sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    out = mi.rcMonitor;
    return true;
}

// 普通的应用窗口：看得见、没被藏起来、不是附属窗口和工具窗口、不是桌面和任务栏、不是本程序的
bool IsAppWindow(HWND hwnd) {
    if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd) || IsCloaked(hwnd) || IsOwnProcess(hwnd)) return false;
    if (GetWindow(hwnd, GW_OWNER) || GetAncestor(hwnd, GA_ROOT) != hwnd) return false;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false;
    std::wstring cls = GetClassNameStr(hwnd);
    return cls != L"Progman" && cls != L"WorkerW" && cls != L"Shell_TrayWnd" && cls != L"Shell_SecondaryTrayWnd";
}

// 铺满整块屏幕的大窗口：最大化的，或者盖住整块屏幕的（全屏）
bool IsBigWindow(HWND hwnd) {
    if (!IsAppWindow(hwnd)) return false;
    if (IsZoomed(hwnd)) return true;
    RECT r, screen;
    return GetWindowRect(hwnd, &r) && MonitorRect(hwnd, screen) && r.left <= screen.left && r.top <= screen.top &&
           r.right >= screen.right && r.bottom >= screen.bottom;
}

// 浮在 big 上面的小窗口：普通窗口、没置顶（置顶的本来就在上面）、和 big 在同一块屏上有重叠
bool IsFloatOver(HWND hwnd, HWND big) {
    if (hwnd == big || !IsAppWindow(hwnd) || IsBigWindow(hwnd)) return false;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) return false;
    RECT a, b, overlap;
    return GetWindowRect(hwnd, &a) && GetWindowRect(big, &b) && IntersectRect(&overlap, &a, &b);
}

void Forget(HWND hwnd) { s_floats.erase(std::remove(s_floats.begin(), s_floats.end(), hwnd), s_floats.end()); }

// 盖在小窗口上面的大窗口要挪下去。系统发出“前台换了”的通知时，大窗口自己的线程往往还没处理完激活，
// 马上挪会被激活时的“提到最上面”盖掉，所以过一会儿再看，分几次复查（有的程序激活后自己还会再提一次）
constexpr UINT kCheckDelays[] = {30, 120, 300, 700};
HWND s_big = nullptr;   // 要复查的大窗口
int s_stage = 0;        // 第几次复查
UINT_PTR s_timer = 0;

// big 下面（Z 序里）被它盖住的小窗口，从上到下
std::vector<HWND> CoveredFloats(HWND big) {
    s_floats.erase(std::remove_if(s_floats.begin(), s_floats.end(), [](HWND h) { return !IsWindow(h); }),
                   s_floats.end());
    std::vector<HWND> covered;
    for (HWND h = GetWindow(big, GW_HWNDNEXT); h; h = GetWindow(h, GW_HWNDNEXT))
        if (std::find(s_floats.begin(), s_floats.end(), h) != s_floats.end() && IsFloatOver(h, big)) covered.push_back(h);
    return covered;
}

void Schedule();

void CALLBACK OnCheck(HWND, UINT, UINT_PTR, DWORD) {
    KillTimer(nullptr, s_timer);
    s_timer = 0;
    HWND big = s_big;
    if (!big || GetForegroundWindow() != big || !IsBigWindow(big)) return;
    std::vector<HWND> covered = CoveredFloats(big);
    if (covered.empty() && s_stage == 0)
        Log(L"小窗口留在上面：%ls 在前台，双击过的 %d 个小窗口都没被盖住", GetClassNameStr(big).c_str(),
            static_cast<int>(s_floats.size()));
    if (!covered.empty()) {
        const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS;
        // 大窗口插到最靠下的那个小窗口后面；异步，对方没响应也不会卡住本程序
        SetWindowPos(big, covered.back(), 0, 0, 0, 0, flags);
        // 前两次挪了还被盖住：换个办法，把小窗口从下到上一个个提到最上面（不激活）
        if (s_stage >= 2)
            for (auto it = covered.rbegin(); it != covered.rend(); ++it) SetWindowPos(*it, HWND_TOP, 0, 0, 0, 0, flags);
        Log(L"小窗口留在上面：%ls 盖住了 %d 个小窗口，挪下去（第 %d 次）", GetClassNameStr(big).c_str(),
            static_cast<int>(covered.size()), s_stage + 1);
    }
    ++s_stage;
    Schedule();
}

void Schedule() {
    if (s_timer) KillTimer(nullptr, s_timer);
    s_timer = 0;
    if (s_stage < static_cast<int>(ARRAYSIZE(kCheckDelays))) s_timer = SetTimer(nullptr, 0, kCheckDelays[s_stage], OnCheck);
}

// 大窗口到前台了，或者在已经是前台的大窗口里按了鼠标：从头开始复查
void Watch(HWND big) {
    s_big = big;
    s_stage = 0;
    Schedule();
}

// 按下鼠标时临时置顶的小窗口（从下到上），多久以后取消置顶
constexpr UINT kLiftMs = 400;
std::vector<HWND> s_lifted;
UINT_PTR s_liftTimer = 0;

// 取消临时置顶。大窗口（或别的全屏窗口、或其中一个小窗口）在前台时放回普通窗口的最上面，正好还在大窗口上面；
// 这期间换到了别的普通窗口，就放到它下面。异步，对方没响应也不会卡住鼠标
void Restore() {
    if (s_liftTimer) KillTimer(nullptr, s_liftTimer);
    s_liftTimer = 0;
    if (s_lifted.empty()) return;
    HWND fg = GetForegroundWindow();
    HWND after = HWND_NOTOPMOST;
    if (fg && std::find(s_lifted.begin(), s_lifted.end(), fg) == s_lifted.end() && !IsBigWindow(fg) &&
        !(GetWindowLongPtrW(fg, GWL_EXSTYLE) & WS_EX_TOPMOST))
        after = fg;
    for (HWND h : s_lifted)  // 从下到上一个个放，彼此的上下顺序不变
        if (IsWindow(h) && (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST))
            SetWindowPos(h, after, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
    s_lifted.clear();
}

void CALLBACK OnLiftTimer(HWND, UINT, UINT_PTR, DWORD) { Restore(); }

// 鼠标在大窗口上按下（还没交给它）：浮在它上面的小窗口临时置顶
void BeforeClick(HWND big) {
    static bool busy = false;  // 等对方响应时可能又收到钩子调用
    if (busy || !IsBigWindow(big)) return;
    std::vector<HWND> above;  // 从下到上
    for (HWND h = GetWindow(big, GW_HWNDPREV); h; h = GetWindow(h, GW_HWNDPREV))
        if (std::find(s_floats.begin(), s_floats.end(), h) != s_floats.end() && IsFloatOver(h, big))
            above.push_back(h);
    if (above.empty()) return;
    busy = true;
    for (HWND h : above) {
        // 同步设置，保证在这次点击交给大窗口之前生效；对方卡住的话改成异步，最多耽误鼠标几十毫秒
        UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
        if (!SendMessageTimeoutW(h, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 50, nullptr)) flags |= SWP_ASYNCWINDOWPOS;
        if (SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, flags)) s_lifted.push_back(h);
    }
    busy = false;
    if (s_liftTimer) KillTimer(nullptr, s_liftTimer);
    s_liftTimer = s_lifted.empty() ? 0 : SetTimer(nullptr, 0, kLiftMs, OnLiftTimer);
}

// 双击了一个小窗口：记下它，以后留在全屏窗口上面
void Pin(HWND hwnd) {
    if (!IsAppWindow(hwnd) || IsBigWindow(hwnd) || (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) return;
    bool known = std::find(s_floats.begin(), s_floats.end(), hwnd) != s_floats.end();
    Forget(hwnd);
    s_floats.insert(s_floats.begin(), hwnd);
    if (s_floats.size() > 32) s_floats.resize(32);
    if (!known) Log(L"小窗口留在上面：双击了 %ls，记下它（共 %d 个）", GetClassNameStr(hwnd).c_str(),
                    static_cast<int>(s_floats.size()));
}

// 和系统判断双击的规则一样：同一个窗口上、双击时间内、位置挪得不多
bool IsDoubleClick(HWND root, const MSLLHOOKSTRUCT& m) {
    static HWND lastRoot = nullptr;
    static DWORD lastTime = 0;
    static POINT lastPt = {};
    bool twice = root == lastRoot && m.time - lastTime <= GetDoubleClickTime() &&
                 std::abs(m.pt.x - lastPt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                 std::abs(m.pt.y - lastPt.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
    lastRoot = twice ? nullptr : root;  // 三击不算两次双击
    lastTime = m.time;
    lastPt = m.pt;
    return twice;
}

LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION &&
        (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN || wParam == WM_MBUTTONDOWN || wParam == WM_XBUTTONDOWN)) {
        const auto& m = *reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        if (HWND root = GetAncestor(WindowFromPoint(m.pt), GA_ROOT)) {
            if (wParam == WM_LBUTTONDOWN && IsDoubleClick(root, m)) Pin(root);
            BeforeClick(root);
        }
    }
    return CallNextHookEx(s_mouseHook, code, wParam, lParam);
}

void CALLBACK OnEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (!hwnd) return;
    if (event == EVENT_SYSTEM_CAPTURESTART || event == EVENT_SYSTEM_CAPTUREEND) {
        // 鼠标按下、松开：点的是前台的大窗口（它里面的子窗口也算）
        HWND root = GetAncestor(hwnd, GA_ROOT);
        if (root && root == GetForegroundWindow() && IsBigWindow(root)) Watch(root);
        return;
    }
    if (idObject != OBJID_WINDOW) return;
    if (event == EVENT_SYSTEM_MINIMIZESTART) {
        Forget(hwnd);
        return;
    }
    if (event != EVENT_SYSTEM_FOREGROUND) return;  // 钩的是一段范围，中间别的事件不管
    if (IsBigWindow(hwnd)) {
        Forget(hwnd);  // 小窗口最大化了，就不再算小窗口
        Watch(hwnd);
        return;
    }
    Restore();  // 换到了别的窗口，临时置顶的小窗口不用再护着
    s_big = nullptr;
}

DWORD WINAPI FloatThread(LPVOID ready) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // 先建好消息队列
    s_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZESTART, nullptr, OnEvent, 0, 0,
                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    s_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    if (!s_mouseHook) Log(L"小窗口留在上面：鼠标钩子装不上（错误 %lu），点全屏窗口时会闪一下", GetLastError());
    SetEvent(static_cast<HANDLE>(ready));

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);

    Restore();
    if (s_timer) KillTimer(nullptr, s_timer);
    s_timer = 0;
    if (s_mouseHook) UnhookWindowsHookEx(s_mouseHook);
    s_mouseHook = nullptr;
    if (s_hook) UnhookWinEvent(s_hook);
    s_hook = nullptr;
    s_floats.clear();
    s_big = nullptr;
    return 0;
}

}  // namespace

void FloatWindows_Configure(bool enabled) {
    if (enabled == (s_thread != nullptr)) return;
    if (enabled) {
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        s_thread = CreateThread(nullptr, 0, FloatThread, ready, 0, &s_threadId);
        if (s_thread) WaitForSingleObject(ready, INFINITE);
        CloseHandle(ready);
    } else {
        PostThreadMessageW(s_threadId, WM_QUIT, 0, 0);
        WaitForSingleObject(s_thread, 2000);
        CloseHandle(s_thread);
        s_thread = nullptr;
    }
}

}  // namespace app
