// 小窗口留在全屏窗口上面：最大化（或全屏）的窗口上浮着几个小窗口时，点一下大窗口，
// 系统会把它提到最上面、小窗口全被盖住。这里在大窗口拿到前台后把它挪回这些小窗口下面（不抢焦点），
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

// 大窗口到前台了：在它下面（Z 序里）找最靠下的那个小窗口，把大窗口插到它后面
void SinkBelowFloats(HWND big) {
    s_floats.erase(std::remove_if(s_floats.begin(), s_floats.end(), [](HWND h) { return !IsWindow(h); }),
                   s_floats.end());
    HWND lowest = nullptr;
    int count = 0;
    for (HWND h = GetWindow(big, GW_HWNDNEXT); h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (std::find(s_floats.begin(), s_floats.end(), h) == s_floats.end() || !IsFloatOver(h, big)) continue;
        lowest = h;
        ++count;
    }
    if (!lowest) return;
    // 异步：大窗口的线程处理完激活以后再挪，不会被它自己的“激活时提到最上面”盖掉，也不会卡住本程序
    SetWindowPos(big, lowest, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
    Log(L"小窗口留在上面：%ls 挪到 %d 个小窗口下面", GetClassNameStr(big).c_str(), count);
}

void CALLBACK OnEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || !hwnd) return;
    if (event == EVENT_SYSTEM_MINIMIZESTART) {
        Forget(hwnd);
        return;
    }
    if (event != EVENT_SYSTEM_FOREGROUND) return;  // 钩的是一段范围，中间别的事件不管
    if (IsBigWindow(hwnd)) {
        Forget(hwnd);  // 小窗口最大化了，就不再算小窗口
        SinkBelowFloats(hwnd);
    } else if (IsAppWindow(hwnd) && !(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
        Forget(hwnd);
        s_floats.insert(s_floats.begin(), hwnd);
        if (s_floats.size() > 32) s_floats.resize(32);
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
    }
}

}  // namespace app
