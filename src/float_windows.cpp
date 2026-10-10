// 小窗口留在全屏窗口上面：最大化（或全屏）的窗口上浮着几个小窗口时，点一下大窗口，
// 系统会把它提到最上面、小窗口全被盖住。这里在大窗口拿到前台（或者在它里面按了鼠标）后把它挪回这些小窗口下面（不抢焦点），
// 大窗口照常能打字操作，小窗口一直看得见。小窗口最小化、最大化或关掉以后就不再管它。
#include "common.h"

#include <algorithm>

namespace app {
namespace {

HWINEVENTHOOK s_hook = nullptr;
std::vector<HWND> s_floats;  // 当过前台的小窗口，按最近一次到前台的先后排

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
        Log(L"小窗口留在上面：%ls 在前台，记下的 %d 个小窗口都没被盖住", GetClassNameStr(big).c_str(),
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
    } else if (IsAppWindow(hwnd) && !(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
        Forget(hwnd);
        s_floats.insert(s_floats.begin(), hwnd);
        if (s_floats.size() > 32) s_floats.resize(32);
        s_big = nullptr;
    }
}

}  // namespace

void FloatWindows_Configure(bool enabled) {
    if (enabled == (s_hook != nullptr)) return;
    if (enabled) {
        s_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZESTART, nullptr, OnEvent, 0, 0,
                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        HWND fg = GetForegroundWindow();
        if (IsAppWindow(fg) && !IsBigWindow(fg)) s_floats.push_back(fg);
    } else {
        UnhookWinEvent(s_hook);
        s_hook = nullptr;
        s_floats.clear();
        if (s_timer) KillTimer(nullptr, s_timer);
        s_timer = 0;
        s_big = nullptr;
    }
}

}  // namespace app
