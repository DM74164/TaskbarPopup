// 窗口最大化或全屏时隐藏任务栏，并让窗口铺满整个屏幕。
//
// 前台窗口最大化时：任务栏滑出屏幕藏起来，同时打开系统的“自动隐藏任务栏”。
// 工作区由资源管理器按任务栏算出来，别的程序改了（SPI_SETWORKAREA）它也会马上改回去（微软说这是设计如此），
// 而 Edge、Chrome 这类程序会把最大化的窗口强行改回工作区的大小，所以只能让资源管理器自己把工作区算成整块屏幕，
// 也就是打开自动隐藏；最大化的窗口随后铺满（仍保持最大化状态）。
// 自动隐藏的任务栏碰到屏幕边缘会弹出来，所以真任务栏同时用 ShowWindow 藏着，要用任务栏时长按 Win。
// 前台换成普通窗口或桌面、窗口还原或最小化时：任务栏滑回来，盖住原来的位置以后关掉自动隐藏，窗口在它后面缩回去。
// 工作区一变，桌面会重新排列图标，所以打开前记下图标的位置，关掉以后摆回去。
// 打开了“桌面上用迷你任务栏代替系统任务栏”时，桌面在前台也藏起任务栏（不为它打开自动隐藏），
// 迷你任务栏停靠在屏幕底部；换成别的窗口时它收起来，照上面的规则处理。
#include "common.h"

#include <cmath>
#include <cstdlib>
#include <cwchar>

namespace app {
namespace {

// 被拉伸过的窗口
struct Stretched {
    HWND hwnd;
    HMONITOR monitor;
    RECT border;  // 最大化时边框伸出工作区的量，缩回去时照原样恢复
    RECT rect;    // 拉伸后的位置
};

// 本程序打开自动隐藏之前各块屏幕的工作区。自动隐藏对所有屏幕上的任务栏都生效，所以每块屏都记
struct Before {
    HMONITOR monitor;
    RECT screen;  // 整块屏幕的范围：分辨率、屏幕排列变了就不再照它缩窗口
    RECT work;    // 打开之前的工作区
    RECT during;  // 打开期间量到的工作区（跟着变大的窗口照它最大化），还没量到时为空
};

// 本程序藏着的任务栏
struct Hidden {
    HWND taskbar;
    DWORD at;  // 最近一次发出隐藏请求的时间
};

// 有的程序会把窗口大小改回去。短时间里要反复拉伸的窗口就算它不肯铺满，
// 在它还原、最小化或者换到别的屏幕之前不再为它藏任务栏（不然任务栏会一直上下伸缩）
struct Attempt {
    HWND hwnd;
    DWORD last;   // 上次发拉伸请求的时间，0 = 没有在等结果的请求
    RECT before;  // 发请求时窗口的位置
    DWORD windowStart;
    int requests;  // windowStart 以来发过几次请求
    bool refused;
    HMONITOR monitor;  // 拒绝时所在的屏幕
    UINT serial;       // 第几次请求，用来认出探测消息的回音
    bool probing;      // 已经发了探测消息
    bool pumped;       // 探测消息有回音了：窗口的线程在处理消息，拉伸请求应该已经处理过了
};

constexpr DWORD kRetryMs = 400;
constexpr DWORD kRequestWindowMs = 10000;
constexpr int kMaxRequests = 3;       // kRequestWindowMs 里最多发这么多次，还要再发就算它拒绝
constexpr DWORD kReshowMs = 3000;     // 刚藏起来又被资源管理器显示出来时，不再播动画，直接藏
constexpr DWORD kTrayHoldMs = 2000;   // 点了任务栏以后至少这么久都当用户还在用它（鼠标还在它上面时一直算）
constexpr DWORD kHiddenFrontMs = 500; // 前台停在看不见的窗口上这么久才算（程序启动时它藏着的窗口有时会短暂拿到前台）

std::vector<Hidden> s_hidden;
std::vector<HWND> s_everHidden;  // 本次运行藏过的任务栏，退出时无论如何都发一次显示
std::vector<Stretched> s_stretched;
std::vector<Before> s_before;  // 不为空 = 打开过自动隐藏、跟着变大的窗口还没收拾完
std::vector<Attempt> s_attempts;
// 自动隐藏是本程序打开的（或者已经让动画线程去打开）。用户自己开着的不算，本程序也不会去关
bool s_ownAutoHide = false;
bool s_desktopMarked = false;  // 自动隐藏开着期间桌面到了前台，已经让图标线程读过一遍位置（DesktopIcons_Mark）
DWORD s_autoHideOnAt = 0;      // 确认自动隐藏打开的时刻
DWORD s_markedAt = 0;          // s_desktopMarked 是什么时候记的
bool s_offRequested = false;  // 已经让动画线程去关
bool s_autoHideConfirmed = false;  // 动画线程回报已经打开（工作区已经变大），之前窗口没铺满不算它不肯
DWORD s_offFailedAt = 0;           // 上次没能关掉的时间（资源管理器没在运行），过一会儿再试
int s_onFailures = 0;              // 资源管理器在运行却连续几次没能打开自动隐藏（比如被组策略锁住）
HWND s_lastTray = nullptr;         // 上次看到的主任务栏：变了说明资源管理器刚重新启动
DWORD s_explorerStartedAt = 0;     // 资源管理器（重新）启动的时间：副屏任务栏要过一会儿才出来
UINT s_autoHideSeq = 0;       // 给动画线程的开 / 关请求编号：动画线程回报“关掉了”时用来认出是不是最新的请求
DWORD s_autoHideAskedAt = 0;  // 上次让动画线程打开自动隐藏的时间
HMONITOR s_targetMonitor = nullptr;
HWND s_targetWindow = nullptr;
HMONITOR s_dockMonitor = nullptr;  // 桌面在前台：这块屏上藏起任务栏，迷你任务栏停靠在底部
bool s_dockAside = false;     // 开始菜单等系统界面在前台，停靠的迷你任务栏让开了、任务栏显示着：这期间别再把任务栏藏起
HWND s_lastFront = nullptr;  // 最近一个普通的前台窗口（不算开始菜单、任务栏这类临时界面和本程序）：
                             // 任务栏到了前台时，分辨是用户点的还是窗口没了落到它上面
HWND s_seenFront = nullptr;   // 上一次看到的前台窗口
DWORD s_seenSince = 0;        // 它到前台的时间
bool s_trayClicked = false;   // 前台换到任务栏上的那一刻鼠标就在它上面：是用户点的
bool s_goneAtClick = false;   // 点任务栏的那一刻，之前的前台窗口就已经不在了
bool s_trayLeft = false;      // 点过任务栏以后鼠标离开了：当用户不用它了
bool s_handedOff = false;     // 这次任务栏在前台时已经把前台交给过桌面
HWND s_minimizing = nullptr;  // 刚开始最小化的目标窗口（事件比窗口状态变化早一点到）
DWORD s_minimizingTick = 0;
bool s_enabled = false;
HWINEVENTHOOK s_foregroundHook = nullptr;
HWINEVENTHOOK s_minimizeHook = nullptr;
HWINEVENTHOOK s_locationHook = nullptr;  // 只盯前台进程的窗口位置变化
DWORD s_locationPid = 0;

// 覆盖在其它窗口上的临时系统界面（开始菜单、搜索、Alt+Tab 等）以及本程序自己的窗口：
// 它们在前台时保持现状，不去动任务栏
const wchar_t* const kTransientClasses[] = {
    L"XamlExplorerHostIslandWindow",
    L"MultitaskingViewFrame",
    L"ForegroundStaging",
    L"TaskSwitcherWnd",
    L"Windows.UI.Core.CoreWindow",
    L"Shell_TrayWnd",
    L"Shell_SecondaryTrayWnd",
    L"NotifyIconOverflowWindow",
    L"TopLevelWindowForOverflowXamlIsland",
    L"Shell_InputSwitchTopLevelWindow",
};

const wchar_t* const kDesktopClasses[] = {L"Progman", L"WorkerW"};

// 其中开始菜单、搜索、通知中心、托盘溢出区和任务栏本身在前台时，把藏着的任务栏显示出来：
// 自动隐藏开着，资源管理器会照常把它弹出来，开始菜单的位置才对；关掉后它自己滑走，本程序再藏好
const wchar_t* const kShellUiClasses[] = {
    L"Windows.UI.Core.CoreWindow",
    L"Shell_TrayWnd",
    L"Shell_SecondaryTrayWnd",
    L"NotifyIconOverflowWindow",
    L"TopLevelWindowForOverflowXamlIsland",
};

template <size_t N>
bool InList(const std::wstring& cls, const wchar_t* const (&list)[N]) {
    for (const wchar_t* item : list)
        if (cls == item) return true;
    return false;
}

// 主屏任务栏是 Shell_TrayWnd，其他屏幕是 Shell_SecondaryTrayWnd
std::vector<HWND> FindTaskbars() {
    std::vector<HWND> list;
    if (HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr)) list.push_back(primary);
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) list.push_back(h);
    return list;
}

// 资源管理器在运行而且没卡住（卡住时去问它会一直等）
bool ExplorerRunning() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    return tray && !IsHungAppWindow(tray);
}

// 系统的自动隐藏设置：1 = 开着，0 = 关着，-1 = 不知道（资源管理器没在运行、卡住了，或者问的时候退出了）
int AutoHideState() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray || IsHungAppWindow(tray)) return -1;
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = tray;
    UINT state = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
    if (!IsWindow(tray) || FindWindowW(L"Shell_TrayWnd", nullptr) != tray) return -1;
    return (state & ABS_AUTOHIDE) ? 1 : 0;
}

// 上次运行打开了自动隐藏、没来得及关就退出了（被强行结束、崩溃、退出时资源管理器没在运行）。
// 关不掉、或者不知道现在是什么状态的话记号留着，资源管理器回来以后、或者下次启动时再关
void UndoSavedAutoHide() {
    if (!GetRestoreAutoHideFlag()) return;
    int state = AutoHideState();
    if (state < 0) return;
    if (state > 0) {
        Log(L"上次没关掉自动隐藏任务栏，现在关掉");
        if (!Taskbar_SetAutoHide(false)) return;
    }
    SetRestoreAutoHideFlag(false);
}

// 任务栏停靠着时在哪块屏幕上
struct TaskbarHome {
    HWND taskbar;
    HMONITOR monitor;
};
std::vector<TaskbarHome> s_homes;

// 任务栏属于哪块屏幕。本程序开着自动隐藏时任务栏缩到屏幕外、只留一两个像素，
// 屏幕上下排列时它大半截落在相邻的那块屏上，按窗口位置找最近的屏幕会找错。
// 整条在一块屏上（停靠着）时就是那块，记下来；不完整时用记下的。
// 没记过的看它两条长边：都在同一块屏上就是那块；跨在两块屏之间时，
// 留在自己屏上的是靠里的那条边（停靠在底边的任务栏缩下去以后，留下的是上沿）
HMONITOR TaskbarMonitor(HWND tb) {
    RECT r;
    if (!GetWindowRect(tb, &r)) return nullptr;
    bool horizontal = r.right - r.left >= r.bottom - r.top;
    POINT a, b;  // a 在上沿（竖着的在左沿），b 在下沿（右沿）
    if (horizontal) {
        LONG x = (r.left + r.right) / 2;
        a = {x, r.top};
        b = {x, r.bottom - 1};
    } else {
        LONG y = (r.top + r.bottom) / 2;
        a = {r.left, y};
        b = {r.right - 1, y};
    }
    HMONITOR ma = MonitorFromPoint(a, MONITOR_DEFAULTTONULL);
    HMONITOR mb = MonitorFromPoint(b, MONITOR_DEFAULTTONULL);
    MONITORINFO mi = {sizeof(mi)};
    HMONITOR whole = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
    // 两条长边落在另一块屏上时不算整条在这块屏上：上下排列时缩下去的任务栏只差一两个像素就整条在下面那块屏上了
    bool sameScreen = (!ma || ma == whole) && (!mb || mb == whole);
    if (whole && sameScreen && GetMonitorInfoW(whole, &mi)) {
        const RECT& m = mi.rcMonitor;
        LONG tol = 4;  // 有的任务栏会伸出屏幕边缘一两个像素
        if (r.left >= m.left - tol && r.top >= m.top - tol && r.right <= m.right + tol && r.bottom <= m.bottom + tol) {
            auto it = std::find_if(s_homes.begin(), s_homes.end(), [&](const TaskbarHome& h) { return h.taskbar == tb; });
            if (it != s_homes.end()) it->monitor = whole;
            else s_homes.push_back({tb, whole});
            return whole;
        }
    }
    for (const TaskbarHome& h : s_homes)
        if (h.taskbar == tb && GetMonitorInfoW(h.monitor, &mi)) return h.monitor;
    if (ma == mb) return ma ? ma : MonitorFromWindow(tb, MONITOR_DEFAULTTONEAREST);
    if (!ma || !mb) return ma ? ma : mb;
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    UINT edge = ABE_BOTTOM;  // 资源管理器卡住时去问它会一直等，按最常见的底边算
    if (abd.hWnd && !IsHungAppWindow(abd.hWnd) && SHAppBarMessage(ABM_GETTASKBARPOS, &abd)) edge = abd.uEdge;
    return (horizontal ? edge != ABE_TOP : edge != ABE_LEFT) ? ma : mb;
}

// 这块屏幕上有任务栏（主屏的，或者副屏上的）
bool HasTaskbar(HMONITOR monitor) {
    for (HWND tb : FindTaskbars())
        if (TaskbarMonitor(tb) == monitor) return true;
    return false;
}

// ---- 诊断日志用的描述 ----

std::wstring RectText(const RECT& r) {
    wchar_t buf[80];
    swprintf(buf, 80, L"(%ld,%ld)-(%ld,%ld)", r.left, r.top, r.right, r.bottom);
    return buf;
}

std::wstring Describe(HWND hwnd) {
    if (!hwnd) return L"(无)";
    std::wstring exe = GetProcessPath(hwnd);
    size_t slash = exe.find_last_of(L'\\');
    if (slash != std::wstring::npos) exe = exe.substr(slash + 1);
    wchar_t buf[40];
    swprintf(buf, 40, L"%p", static_cast<void*>(hwnd));
    return std::wstring(buf) + L" " + GetClassNameStr(hwnd) + L" " + exe;
}

// ---- 拉伸最大化的窗口 ----

Attempt& AttemptFor(HWND hwnd) {
    for (Attempt& a : s_attempts)
        if (a.hwnd == hwnd) return a;
    s_attempts.push_back({hwnd, 0, {}, 0, 0, false, nullptr, 0, false, false});
    return s_attempts.back();
}

bool IsRefused(HWND hwnd) {
    for (const Attempt& a : s_attempts)
        if (a.hwnd == hwnd) return a.refused;
    return false;
}

// 探测消息的回音：窗口的线程处理到了这条消息，排在它前面的拉伸请求也处理过了
void CALLBACK OnPumped(HWND hwnd, UINT, ULONG_PTR serial, LRESULT) {
    for (Attempt& a : s_attempts)
        if (a.hwnd == hwnd && a.serial == serial) a.pumped = true;
}

// 窗口的尺寸是本程序改的（缩回去），之后再拉伸不算它拒绝
void ForgetRequests(HWND hwnd) {
    for (Attempt& a : s_attempts) {
        if (a.hwnd != hwnd) continue;
        a.last = 0;
        a.requests = 0;
    }
}

bool Refuses(HWND hwnd) {
    for (Attempt& a : s_attempts) {
        if (a.hwnd != hwnd || !a.refused) continue;
        // 用户把它还原 / 最小化了，或者挪到了别的屏幕：重新给它机会
        if (!IsZoomed(hwnd) || MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) != a.monitor) {
            a = {hwnd, 0, {}, 0, 0, false, nullptr, a.serial + 1, false, false};
            return false;
        }
        return true;
    }
    return false;
}

Stretched* FindStretched(HWND hwnd) {
    for (Stretched& s : s_stretched)
        if (s.hwnd == hwnd) return &s;
    return nullptr;
}

const Before* FindBefore(HMONITOR monitor) {
    for (const Before& b : s_before)
        if (b.monitor == monitor) return &b;
    return nullptr;
}

// 两个矩形每条边都相差不超过 tol
bool Near(const RECT& a, const RECT& b, LONG tol) {
    return std::abs(a.left - b.left) <= tol && std::abs(a.top - b.top) <= tol && std::abs(a.right - b.right) <= tol &&
           std::abs(a.bottom - b.bottom) <= tol;
}

LONG Tolerance(HMONITOR monitor) { return static_cast<LONG>(std::lround(4 * MonitorScale(monitor))); }
LONG BorderLimit(HMONITOR monitor) { return static_cast<LONG>(32 * MonitorScale(monitor)); }

RECT Inflate(const RECT& r, const RECT& by) {
    return {r.left - by.left, r.top - by.top, r.right + by.right, r.bottom + by.bottom};
}

// cur 伸出 work 的量。负的或太大说明窗口不是照 work 最大化的
bool BorderAround(const RECT& work, const RECT& cur, LONG limit, RECT& border) {
    border = {work.left - cur.left, work.top - cur.top, cur.right - work.right, cur.bottom - work.bottom};
    for (LONG b : {border.left, border.top, border.right, border.bottom})
        if (b < 0 || b > limit) return false;
    return true;
}

// 最大化窗口的边框伸出工作区的量。自动隐藏刚打开或刚关掉时，窗口可能还是照之前的工作区最大化的。
// 都对不上说明程序自己定了最大化的大小和位置，返回 false
bool MaximizedBorder(HMONITOR monitor, const MONITORINFO& mi, const RECT& cur, RECT& border) {
    LONG limit = BorderLimit(monitor);
    if (BorderAround(mi.rcWork, cur, limit, border)) return true;
    const Before* b = FindBefore(monitor);
    if (!b) return false;
    if (BorderAround(b->work, cur, limit, border)) return true;
    return !IsRectEmpty(&b->during) && BorderAround(b->during, cur, limit, border);
}

// 窗口要铺满的范围：工作区朝任务栏那一边扩到任务栏的外沿。
// 只动那一条边，放大镜之类停靠在别的边上的工具栏照样让开
RECT FillArea(HMONITOR monitor, const MONITORINFO& mi) {
    RECT area = mi.rcWork;
    LONG tol = Tolerance(monitor);
    for (HWND tb : FindTaskbars()) {
        RECT r;
        if (TaskbarMonitor(tb) != monitor || !GetWindowRect(tb, &r) ||
            !IntersectRect(&r, &r, &mi.rcMonitor))
            continue;
        if (r.top >= area.bottom - tol) area.bottom = std::max(area.bottom, r.bottom);
        else if (r.bottom <= area.top + tol) area.top = std::min(area.top, r.top);
        else if (r.left >= area.right - tol) area.right = std::max(area.right, r.right);
        else if (r.right <= area.left + tol) area.left = std::min(area.left, r.left);
    }
    return area;
}

DWORD TokenIntegrity(HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    std::vector<BYTE> buf(size);
    if (!size || !GetTokenInformation(token, TokenIntegrityLevel, buf.data(), size, &size))
        return SECURITY_MANDATORY_SYSTEM_RID;
    PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data())->Label.Sid;
    return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
}

// 窗口属于权限比本程序高的进程（以管理员身份运行的程序）：系统不让本程序挪它的窗口，
// 拉伸不了也缩不回来，所以不为它藏任务栏
bool OutranksUs(HWND hwnd) {
    static const DWORD mine = [] {
        DWORD level = SECURITY_MANDATORY_MEDIUM_RID;
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            level = TokenIntegrity(token);
            CloseHandle(token);
        }
        return level;
    }();
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE proc = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!proc) return true;
    bool higher = true;
    HANDLE token = nullptr;
    if (OpenProcessToken(proc, TOKEN_QUERY, &token)) {
        higher = TokenIntegrity(token) > mine;
        CloseHandle(token);
    }
    CloseHandle(proc);
    return higher;
}

// 最大化的窗口能不能铺满：本程序拉伸过的，或者正常最大化在工作区里的
bool CanFill(HWND hwnd, HMONITOR monitor) {
    MONITORINFO mi = {sizeof(mi)};
    RECT cur, border;
    if (!GetMonitorInfoW(monitor, &mi) || !GetWindowRect(hwnd, &cur)) return false;
    if (const Stretched* s = FindStretched(hwnd))
        if (s->monitor == monitor && Near(cur, s->rect, Tolerance(monitor))) return true;
    return MaximizedBorder(monitor, mi, cur, border);
}

// 算出最大化窗口铺满整块屏幕的位置。返回 false 表示这次不用发请求：
// 已经铺满了、本来就铺满（任务栏自动隐藏或不在这块屏上）、请求还在等程序处理、或者它不肯被拉伸
bool PlanStretch(HWND hwnd, HMONITOR monitor, RECT& want) {
    if (!IsZoomed(hwnd)) return false;  // 无边框全屏的窗口本来就铺满了
    MONITORINFO mi = {sizeof(mi)};
    RECT cur;
    if (!GetMonitorInfoW(monitor, &mi) || !GetWindowRect(hwnd, &cur)) return false;
    LONG tol = Tolerance(monitor);

    Attempt& a = AttemptFor(hwnd);
    if (a.refused) return false;
    Stretched* s = FindStretched(hwnd);
    if (s && s->monitor != monitor) {
        // 窗口被挪到了别的屏幕（Win+Shift+方向键），系统按那块屏重新最大化了：重新计数
        a.last = 0;
        a.requests = 0;
        a.windowStart = 0;
    }
    if (s && s->monitor == monitor && Near(cur, s->rect, tol)) {
        // 拉伸生效了（有的程序会按字符格之类把尺寸微调几个像素，也算）
        if (a.last) Log(L"拉伸生效 %ls %ls", Describe(hwnd).c_str(), RectText(cur).c_str());
        s->rect = cur;
        a.last = 0;
        return false;
    }

    // 最大化的窗口边框会伸出工作区几个像素，铺满屏幕时保持同样的伸出量
    RECT border;
    if (!MaximizedBorder(monitor, mi, cur, border)) return false;
    want = Inflate(FillArea(monitor, mi), border);
    if (Near(want, cur, tol)) {
        // 已经铺满了：打开自动隐藏以后，程序自己就照着新的工作区摆好了
        if (a.last) Log(L"已铺满 %ls %ls", Describe(hwnd).c_str(), RectText(cur).c_str());
        if (s) {
            s->rect = cur;
        } else if (const Before* b = FindBefore(monitor)) {
            // 自动隐藏打开期间才最大化的窗口也记下来，关掉以后照样缩回去
            // （有的程序照工作区自己定最大化大小、不带边框，不记的话会被当成铺满整块屏的游戏而不缩）
            RECT unused;
            if (!BorderAround(b->work, cur, BorderLimit(monitor), unused))
                s_stretched.push_back({hwnd, monitor, border, cur});
        }
        a.last = 0;
        return false;
    }

    DWORD now = GetTickCount();
    if (a.last) {
        // 自动隐藏还没打开、工作区还没变大：Edge 这类程序会把窗口改回去，这时没铺满不算，等打开以后重新计数
        if (s_ownAutoHide && !s_autoHideConfirmed) return false;
        if (now - a.last < kRetryMs) return false;  // 上次的请求可能还没处理完
        if (EqualRect(&cur, &a.before) && !a.pumped) {
            // 位置一点没变：可能是程序忙、请求还排着，也可能是处理了但不肯。
            // 给它的线程发一条探测消息，有回音才算处理过；没回音就接着等，不重发，也不算它拒绝
            if (!a.probing) {
                a.probing = true;
                if (!SendMessageCallbackW(hwnd, WM_NULL, 0, 0, OnPumped, a.serial)) a.pumped = true;
            }
            return false;
        }
        Log(L"拉伸没生效 %ls 现在 %ls", Describe(hwnd).c_str(), RectText(cur).c_str());
    }
    if (!a.windowStart || now - a.windowStart > kRequestWindowMs) {
        a.windowStart = now;
        a.requests = 0;
    }
    if (++a.requests > kMaxRequests) {
        a.refused = true;
        a.monitor = monitor;
        a.last = 0;
        Log(L"%ls 不肯铺满，在它还原或换屏幕之前不再为它隐藏任务栏", Describe(hwnd).c_str());
        return false;
    }
    a.last = now;
    a.before = cur;
    ++a.serial;
    a.probing = false;
    a.pumped = false;

    if (s) *s = {hwnd, monitor, border, want};
    else s_stretched.push_back({hwnd, monitor, border, want});
    Log(L"拉伸 %ls %ls -> %ls", Describe(hwnd).c_str(), RectText(cur).c_str(), RectText(want).c_str());
    return true;
}

// ---- 收拾跟着变大的窗口 ----

struct ShrinkContext {
    HMONITOR monitor;
    RECT screen, from, to;
    LONG limit;
};

// 贴边分屏（Win+方向键、分屏布局）的窗口
bool IsArranged(HWND hwnd) {
    using Fn = BOOL(WINAPI*)(HWND);
    static const auto fn = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "IsWindowArranged")));
    return fn && fn(hwnd);
}

// 照变大的工作区贴边分屏、系统又没替它挪回去的窗口：贴着 from 某条边的那一边挪到 to 的同一条边上
void FitArranged(HWND hwnd, const RECT& cur, const ShrinkContext& c) {
    RECT r = cur;
    if (c.from.left != c.to.left && std::abs(cur.left - c.from.left) <= c.limit) r.left += c.to.left - c.from.left;
    if (c.from.top != c.to.top && std::abs(cur.top - c.from.top) <= c.limit) r.top += c.to.top - c.from.top;
    if (c.from.right != c.to.right && std::abs(cur.right - c.from.right) <= c.limit) r.right += c.to.right - c.from.right;
    if (c.from.bottom != c.to.bottom && std::abs(cur.bottom - c.from.bottom) <= c.limit)
        r.bottom += c.to.bottom - c.from.bottom;
    if (EqualRect(&r, &cur)) return;
    SetWindowRectAsync(hwnd, r);
    Log(L"贴边窗口缩回 %ls -> %ls", Describe(hwnd).c_str(), RectText(r).c_str());
}

BOOL CALLBACK ShrinkProc(HWND hwnd, LPARAM param) {
    const ShrinkContext& c = *reinterpret_cast<const ShrinkContext*>(param);
    RECT cur, border;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsOwnProcess(hwnd)) return TRUE;
    if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) != c.monitor || !GetWindowRect(hwnd, &cur)) return TRUE;
    if (!IsZoomed(hwnd)) {
        if (IsArranged(hwnd)) FitArranged(hwnd, cur, c);
        return TRUE;
    }
    if (!BorderAround(c.from, cur, c.limit, border)) return TRUE;
    // 正好盖住整块屏、没有边框的是程序自己铺满的（游戏之类），不是照工作区最大化的，别动它
    if (EqualRect(&cur, &c.screen) && !FindStretched(hwnd)) return TRUE;
    RECT r = Inflate(c.to, border);
    if (!EqualRect(&r, &cur)) {
        SetWindowRectAsync(hwnd, r);
        Log(L"缩回 %ls -> %ls", Describe(hwnd).c_str(), RectText(r).c_str());
    }
    ForgetRequests(hwnd);
    return TRUE;
}

// 照工作区 from 最大化着、还没被系统或程序自己缩回去的窗口缩到工作区 to
void ShrinkMaximized(HMONITOR monitor, const RECT& screen, const RECT& from, const RECT& to) {
    ShrinkContext c = {monitor, screen, from, to, BorderLimit(monitor)};
    EnumWindows(ShrinkProc, reinterpret_cast<LPARAM>(&c));
}

// 本程序拉伸过的窗口，还停在拉伸后的大小的，缩回现在的工作区
void RestoreStretched() {
    for (const Stretched& s : s_stretched) {
        MONITORINFO mi = {sizeof(mi)};
        RECT cur;
        // 已经还原、最小化，或者挪到别的屏幕（系统会按那块屏重新最大化）的就不管了
        if (IsWindow(s.hwnd) && IsZoomed(s.hwnd) && MonitorFromWindow(s.hwnd, MONITOR_DEFAULTTONEAREST) == s.monitor &&
            GetMonitorInfoW(s.monitor, &mi) && GetWindowRect(s.hwnd, &cur) && Near(cur, s.rect, Tolerance(s.monitor))) {
            RECT r = Inflate(mi.rcWork, s.border);
            if (!EqualRect(&r, &cur)) {
                SetWindowRectAsync(s.hwnd, r);
                Log(L"缩回 %ls -> %ls", Describe(s.hwnd).c_str(), RectText(r).c_str());
            }
        }
        ForgetRequests(s.hwnd);
    }
    s_stretched.clear();
}

// 第三版自己扩大过工作区（SPI_SETWORKAREA），被强行结束时记在设置文件里。
// 资源管理器一般很快就会改回去，这里只是再确认一次，并清掉这条记录
void UndoLegacyWorkAreas() {
    wchar_t buf[2048] = {};
    GetPrivateProfileStringW(L"State", L"WorkAreas", L"", buf, ARRAYSIZE(buf), SettingsFile().c_str());
    if (!*buf) return;
    for (const wchar_t* p = buf; *p;) {
        RECT s, o, x;
        int n = swscanf(p, L"%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld", &s.left, &s.top, &s.right, &s.bottom,
                        &o.left, &o.top, &o.right, &o.bottom, &x.left, &x.top, &x.right, &x.bottom);
        if (n != 12) break;
        HMONITOR m = MonitorFromRect(&s, MONITOR_DEFAULTTONULL);
        MONITORINFO mi = {sizeof(mi)};
        if (m && GetMonitorInfoW(m, &mi) && EqualRect(&mi.rcMonitor, &s)) {
            if (EqualRect(&mi.rcWork, &x) && !Taskbar_AutoHideOn()) {
                Log(L"上次退出时没还原工作区，现在还原 %ls", RectText(o).c_str());
                SystemParametersInfoW(SPI_SETWORKAREA, 0, &o, SPIF_SENDCHANGE);
                GetMonitorInfoW(m, &mi);
            }
            if (!EqualRect(&mi.rcWork, &x)) ShrinkMaximized(m, s, x, mi.rcWork);
        }
        const wchar_t* next = wcschr(p, L';');
        if (!next) break;
        p = next + 1;
    }
    WritePrivateProfileStringW(L"State", L"WorkAreas", nullptr, SettingsFile().c_str());
}

// ---- 自动隐藏任务栏 ----

UINT NextSeq() {
    if (++s_autoHideSeq == 0) ++s_autoHideSeq;
    return s_autoHideSeq;
}

// 量一下自动隐藏打开期间的工作区：关掉以后靠它认出跟着变大的窗口
void SampleWorkAreas() {
    for (Before& b : s_before) {
        MONITORINFO mi = {sizeof(mi)};
        if (GetMonitorInfoW(b.monitor, &mi) && !EqualRect(&mi.rcWork, &b.work)) b.during = mi.rcWork;
    }
}

// 准备让动画线程打开自动隐藏：先记下各块屏幕的工作区和桌面图标的位置
void BeginAutoHide() {
    KillTimer(g_mainWnd, kTimerAfterAutoHide);
    // 上次关掉以后还没收拾完：那时记下的还是打开之前的样子，接着用
    if (s_before.empty()) {
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR m, HDC, LPRECT, LPARAM) -> BOOL {
                MONITORINFO mi = {sizeof(mi)};
                if (GetMonitorInfoW(m, &mi)) s_before.push_back({m, mi.rcMonitor, mi.rcWork, {}});
                return TRUE;
            },
            0);
    }
    DesktopIcons_BeginSession();
    s_ownAutoHide = true;
    s_autoHideConfirmed = false;
    NextSeq();  // 给动画线程的打开请求带上新编号（0 表示不开）
    SetRestoreAutoHideFlag(true);  // 被强行结束的话，下次启动时关掉
}

void CALLBACK OnRetryOff(HWND hwnd, UINT, UINT_PTR id, DWORD);

// 自动隐藏打开好一会儿了（资源管理器按新工作区排完桌面图标），而且还没要求关
bool AutoHideSteady() {
    return s_ownAutoHide && s_autoHideConfirmed && !s_offRequested && GetTickCount() - s_autoHideOnAt >= 2500;
}

// 桌面在前台期间读过图标位置：关自动隐藏之前再读一遍，用户挪过的图标按新位置记（动画线程关之前等它读完）
void FlushDesktopMark() {
    if (!s_desktopMarked) return;
    s_desktopMarked = false;
    // 刚记的（比如关掉最大化的窗口、桌面到了前台，同一次判断里就要关自动隐藏）：
    // 这么短的时间里不可能拖过图标，不用再读一遍，免得耽误关自动隐藏
    if (GetTickCount() - s_markedAt < 200) return;
    DesktopIcons_UpdateMoved();
}

// 让动画线程关掉本程序打开的自动隐藏。taskbar 不为空时等它的截图滑回原位再关。
// 资源管理器没在运行、卡住、或者刚关失败过时先不要求，过一会儿再试（功能刚被关掉、
// 或者前台一直是任务栏之类时，250 毫秒的定时器不会再来）
void RequestAutoHideOff(HWND taskbar) {
    bool backoff = s_offFailedAt && GetTickCount() - s_offFailedAt < 1000;
    if (!s_ownAutoHide || s_offRequested || backoff || !ExplorerRunning()) {
        if (s_ownAutoHide && !s_offRequested) SetTimer(g_mainWnd, kTimerRetryOff, 1100, OnRetryOff);
        if (taskbar) TaskbarAnim_Show(taskbar);
        return;
    }
    FlushDesktopMark();
    TaskbarAnim_WantAutoHide(0);
    SampleWorkAreas();
    s_offRequested = true;
    UINT seq = NextSeq();
    if (taskbar) TaskbarAnim_Show(taskbar, seq);
    else TaskbarAnim_AutoHideOff(seq);
}

// 自动隐藏关掉一会儿以后：资源管理器已经广播完工作区变化，程序和桌面都照新的工作区排好了。
// 没跟着缩回去的窗口缩回去，挪了的桌面图标摆回原位
void CALLBACK OnAfterAutoHide(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    if (s_ownAutoHide) return;
    for (const Before& b : s_before) {
        MONITORINFO mi = {sizeof(mi)};
        if (!GetMonitorInfoW(b.monitor, &mi) || !EqualRect(&mi.rcMonitor, &b.screen)) continue;  // 屏幕拔了或者改了分辨率
        RECT from = IsRectEmpty(&b.during) ? mi.rcMonitor : b.during;
        if (!EqualRect(&from, &mi.rcWork)) ShrinkMaximized(b.monitor, mi.rcMonitor, from, mi.rcWork);
    }
    s_before.clear();
    RestoreStretched();
    DesktopIcons_RestoreLater();
}

// 关自动隐藏失败以后再试一次。资源管理器卡住时接着等；进程没了的话等它重新起来（TaskbarCreated）再关
void CALLBACK OnRetryOff(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    if (!s_ownAutoHide || s_offRequested || s_targetMonitor) return;
    if (!ExplorerRunning()) {
        if (FindWindowW(L"Shell_TrayWnd", nullptr)) SetTimer(hwnd, id, 1100, OnRetryOff);
        return;
    }
    RequestAutoHideOff(nullptr);
}

// 等着处理别的线程发来的消息（资源管理器广播工作区变化时要等每个窗口处理完）
void PumpSentMessages(DWORD ms) {
    DWORD start = GetTickCount();
    for (DWORD elapsed = 0; elapsed < ms; elapsed = GetTickCount() - start) {
        MsgWaitForMultipleObjectsEx(0, nullptr, ms - elapsed, QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
}

// ---- 任务栏的显示 / 隐藏 ----

Hidden* FindHidden(HWND taskbar) {
    for (Hidden& h : s_hidden)
        if (h.taskbar == taskbar) return &h;
    return nullptr;
}

bool RemoveHidden(HWND taskbar) {
    auto it = std::find_if(s_hidden.begin(), s_hidden.end(), [&](const Hidden& h) { return h.taskbar == taskbar; });
    if (it == s_hidden.end()) return false;
    s_hidden.erase(it);
    return true;
}

// 藏起 monitor 上的任务栏、打开自动隐藏、拉伸 window；别的屏幕上被本程序藏起来的任务栏放回来。
// window 刚被判定不肯铺满时什么都不做，返回 false。
// dock：桌面在前台、迷你任务栏要停靠在这块屏上，window 为空。只藏任务栏，不为它打开自动隐藏
// （桌面用不着铺满），已经开着的接着用：任务栏藏着时关掉它，资源管理器可能会把任务栏重新显示出来
bool EnterHiddenMode(HMONITOR monitor, HWND window, bool dock = false) {
    // 资源管理器没在运行（崩溃、重启中）或者卡住了：什么都不动，开着的自动隐藏接着算本程序的，等它回来。
    // 刚重新启动时副屏的任务栏要过一会儿才出来，这时也别当成“这块屏没有任务栏”去关自动隐藏
    if (!ExplorerRunning()) return true;
    if (s_ownAutoHide && GetTickCount() - s_explorerStartedAt < 3000 && !HasTaskbar(monitor)) return true;
    RECT want = {};
    HWND stretch = !dock && PlanStretch(window, monitor, want) ? window : nullptr;
    if (!dock && IsRefused(window)) return false;

    // 这块屏上有任务栏、自动隐藏又不是用户自己开着的：要打开自动隐藏，工作区才会占满整块屏。
    // 无边框全屏的窗口（游戏、全屏视频）本来就盖住了任务栏，不为它去开；已经开着的接着用
    bool autoHide = HasTaskbar(monitor) && (dock ? s_ownAutoHide && !s_offRequested
                                                 : s_ownAutoHide || (IsZoomed(window) && !Taskbar_AutoHideOn()));
    // 要（重新）让动画线程打开自动隐藏：刚开始用，或者刚让它去关又反悔了（它可能已经关了）
    bool reissue = false;
    if (autoHide && !s_ownAutoHide) {
        BeginAutoHide();
        reissue = true;
    }
    if (autoHide && s_offRequested) {
        s_offRequested = false;  // 刚让动画线程去关的作废
        s_autoHideConfirmed = false;
        NextSeq();
        reissue = true;
    }
    UINT seq = autoHide ? s_autoHideSeq : 0;
    TaskbarAnim_WantAutoHide(seq);

    // 资源管理器重启后旧句柄失效
    s_hidden.erase(std::remove_if(s_hidden.begin(), s_hidden.end(), [](const Hidden& h) { return !IsWindow(h.taskbar); }),
                   s_hidden.end());
    s_homes.erase(std::remove_if(s_homes.begin(), s_homes.end(), [](const TaskbarHome& h) { return !IsWindow(h.taskbar); }),
                  s_homes.end());
    HWND first = nullptr;  // 这块屏上的任务栏
    for (HWND tb : FindTaskbars()) {
        if (TaskbarMonitor(tb) != monitor) {
            // 别的屏幕上藏着的放回来。用不着自动隐藏了的话，等它的截图滑回原位再关
            if (RemoveHidden(tb)) {
                if (autoHide) TaskbarAnim_Show(tb);
                else RequestAutoHideOff(tb);
            }
            continue;
        }
        if (!first) first = tb;
        Hidden* h = FindHidden(tb);
        DWORD now = GetTickCount();
        // 资源管理器有时会自己把任务栏重新显示出来，所以每次都检查
        if (h && !IsWindowVisible(tb)) {
            // 已经藏好了。要重新打开自动隐藏，或者自动隐藏被别人关掉了（比如在设置里，窗口会缩回去）：让动画线程去开。
            // 刚让它去开的不算（它可能正在开），过一会儿再查
            if (autoHide && (reissue || (now - s_autoHideAskedAt > 1500 && !Taskbar_AutoHideOn()))) {
                if (!reissue) Log(L"自动隐藏任务栏被关掉了，重新打开");
                s_autoHideConfirmed = false;
                s_autoHideAskedAt = now;
                TaskbarAnim_Hide(tb, stretch, want, false, seq);
                stretch = nullptr;
            }
            continue;
        }
        bool animate = true;
        if (h) {
            // 刚藏过又冒出来：再滑一遍看上去就是任务栏在反复伸缩，所以直接藏
            animate = now - h->at >= kReshowMs;
            Log(L"任务栏 %p 仍显示着，再次隐藏（%ls）", static_cast<void*>(tb), animate ? L"带动画" : L"不带动画");
            h->at = now;
        } else {
            s_hidden.push_back({tb, now});
            Log(L"隐藏任务栏 %p", static_cast<void*>(tb));
        }
        if (std::find(s_everHidden.begin(), s_everHidden.end(), tb) == s_everHidden.end()) s_everHidden.push_back(tb);
        if (autoHide) s_autoHideAskedAt = now;
        TaskbarAnim_Hide(tb, stretch, want, animate, seq);  // 截图盖住任务栏以后再开自动隐藏、拉伸窗口
        stretch = nullptr;
    }
    if (stretch) {
        // 任务栏早就藏好了（比如在两个最大化窗口之间切换）。自动隐藏还没确认打开的话，
        // 排在动画线程后面，等它打开了再拉伸，不然 Edge 这类程序会改回去
        if (autoHide && !s_autoHideConfirmed && first) TaskbarAnim_Hide(first, stretch, want, false, seq);
        else SetWindowRectAsync(stretch, want);
    }
    if (!autoHide) RequestAutoHideOff(nullptr);  // 换到了没有任务栏的屏幕、或者全屏窗口上：用不着了
    return true;
}

// 放回本程序藏起来的任务栏，关掉本程序打开的自动隐藏（第一条任务栏的截图滑回原位以后）
void LeaveHiddenMode() {
    TaskbarAnim_WantAutoHide(0);
    bool first = true;
    for (const Hidden& h : s_hidden) {
        if (!IsWindow(h.taskbar)) continue;
        Log(L"显示任务栏 %p", static_cast<void*>(h.taskbar));
        if (first) RequestAutoHideOff(h.taskbar);
        else TaskbarAnim_Show(h.taskbar);
        first = false;
    }
    s_hidden.clear();
    RequestAutoHideOff(nullptr);  // 已经要求过的不会重复
}

void ShowForShellUi() {
    for (const Hidden& h : s_hidden) {
        if (!IsWindow(h.taskbar) || IsWindowVisible(h.taskbar)) continue;
        Log(L"开始菜单等系统界面在前台，显示任务栏 %p", static_cast<void*>(h.taskbar));
        ShowWindowAsync(h.taskbar, SW_SHOWNA);
    }
}

// ---- 判断前台窗口 ----

bool IsTransient(HWND hwnd) {
    return IsOwnProcess(hwnd) || InList(GetClassNameStr(hwnd), kTransientClasses);
}

// 桌面，或者桌面弹出的右键菜单之类（和桌面同一个线程、没有标题栏）
bool IsDesktop(HWND hwnd) {
    if (InList(GetClassNameStr(hwnd), kDesktopClasses)) return true;
    HWND shell = GetShellWindow();
    return shell && GetWindowThreadProcessId(hwnd, nullptr) == GetWindowThreadProcessId(shell, nullptr) &&
           (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION) != WS_CAPTION;
}

bool IsTray(HWND hwnd) {
    std::wstring cls = GetClassNameStr(hwnd);
    return cls == L"Shell_TrayWnd" || cls == L"Shell_SecondaryTrayWnd";
}

// 有弹出菜单开着（比如托盘菜单）
bool MenuOpen() {
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, L"#32768", nullptr)) != nullptr)
        if (IsWindowVisible(h)) return true;
    return false;
}

// 前台停在一个看不见的窗口上，菜单也关了：托盘图标的菜单（本程序或者别的程序的）关掉以后常常这样
bool LeftOnHiddenWindow(HWND fg) {
    return fg && fg == s_seenFront && GetTickCount() - s_seenSince >= kHiddenFrontMs &&
           (!IsWindowVisible(fg) || IsCloaked(fg)) && !MenuOpen();
}

// 记下最近一个普通的前台窗口：不算开始菜单、任务栏这类临时界面、本程序和看不见的窗口
void RememberFront(HWND fg) {
    if (fg && !IsTransient(fg) && IsWindowVisible(fg) && !IsCloaked(fg)) s_lastFront = fg;
}

// 之前的前台窗口已经关掉、最小化或者藏起来了
bool LastFrontGone() {
    HWND last = s_lastFront;
    return !last || !IsWindow(last) || !IsWindowVisible(last) || IsIconic(last) || IsCloaked(last);
}

// 用户正用着任务栏：鼠标在它（或者它弹出的缩略图、菜单之类）上面，或者开着菜单
bool TrayInUse(HWND tray) {
    if (MenuOpen()) return true;
    POINT pt;
    if (!GetCursorPos(&pt)) return false;
    HWND under = WindowFromPoint(pt);
    under = under ? GetAncestor(under, GA_ROOT) : nullptr;
    return under && GetWindowThreadProcessId(under, nullptr) == GetWindowThreadProcessId(tray, nullptr);
}

// 记下前台换成了谁。前台换到显示着的任务栏上、鼠标正在上面，而且不是刚才的前台窗口没了落到它上面的：是用户点的。
// 点完过了 kTrayHoldMs、用户也不再用它了就算不用了（在开始菜单里点了任务栏上的应用时，等它的窗口出来；
// 点开始按钮关掉开始菜单时，过一会儿就回到停靠）
void NoteForeground(HWND fg) {
    if (fg != s_seenFront) {
        bool fell = s_seenFront && s_seenFront == s_lastFront && LastFrontGone();
        s_seenFront = fg;
        s_seenSince = GetTickCount();
        s_trayClicked = fg && IsTray(fg) && IsWindowVisible(fg) && !fell && TrayInUse(fg);
        s_goneAtClick = LastFrontGone();
        s_trayLeft = false;
        s_handedOff = false;
    } else if (s_trayClicked && !s_trayLeft && GetTickCount() - s_seenSince >= kTrayHoldMs && !TrayInUse(fg)) {
        s_trayLeft = true;
    }
}

// 用户面前已经没有窗口了：前台窗口最小化了（或者正在最小化）；或者前台落到了任务栏上（或者托盘菜单关掉后
// 留在看不见的窗口上），而之前的前台窗口已经不在了（关掉、最小化最后一个窗口以后常常这样）。
// 按 Win+T 时之前的窗口还在，不算；用户正用着点过的任务栏时，只算点完以后才没了的（比如点任务栏按钮把它最小化了）
bool NothingInFront(HWND fg) {
    if (!fg) return false;  // 锁屏、UAC 提示时没有前台窗口：保持现状
    if (IsIconic(fg) || (fg == s_minimizing && GetTickCount() - s_minimizingTick < 1000)) return true;
    if (IsTray(fg) && s_trayClicked && !s_trayLeft) return !s_goneAtClick && LastFrontGone();
    return (IsTray(fg) || LeftOnHiddenWindow(fg)) && LastFrontGone();
}

// 托盘菜单关掉后前台留在看不见的窗口上，或者落在点过、已经不用了的任务栏上，而之前在前台的是桌面
bool DesktopBehind(HWND fg) {
    return (LeftOnHiddenWindow(fg) || (fg && IsTray(fg) && s_trayClicked && s_trayLeft)) && !LastFrontGone() &&
           IsDesktop(s_lastFront);
}

// 要停靠迷你任务栏的屏幕：桌面在前台时；或者用户面前已经没有窗口（idle），而这块屏上也没有露着的窗口时。
// 停靠着的接着停在原来的屏幕上，刚回到桌面时停在鼠标所在（或者刚最小化的窗口所在）的屏幕上。否则返回 nullptr
HMONITOR DockMonitor(HWND fg, bool idle) {
    if (!g_settings.desktopDock) return nullptr;
    bool desktop = fg && (IsDesktop(fg) || DesktopBehind(fg));
    if (!idle && !desktop) return nullptr;
    HMONITOR mon = nullptr;
    MONITORINFO mi = {sizeof(mi)};
    POINT pt;
    if (s_dockMonitor && GetMonitorInfoW(s_dockMonitor, &mi)) {
        mon = s_dockMonitor;
    } else if (fg && (IsIconic(fg) || fg == s_minimizing)) {
        mon = MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY);  // 最小化的窗口按它原来的位置算
    } else if (GetCursorPos(&pt)) {
        mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    } else {
        return nullptr;  // 读不到鼠标位置：别的桌面（UAC 提示、锁屏）在前面
    }
    if (!HasTaskbar(mon)) return nullptr;
    if (idle && !desktop && AppWindowShownOn(mon, fg)) return nullptr;
    return mon;
}

// 窗口属于排除名单里的程序（最大化时不隐藏任务栏）。按进程记住上一次的结果，不用每次都查程序路径
bool IsExcluded(HWND hwnd) {
    static HWND lastApp = nullptr;
    static DWORD lastPid = 0;
    static std::wstring lastExe;
    if (g_settings.excludeApps.empty()) {
        lastApp = nullptr;
        lastPid = 0;
        return false;
    }
    // 窗口和进程都没变才用记下的：进程号会被新进程重用
    HWND app = AppWindowTarget(hwnd);
    DWORD pid = 0;
    GetWindowThreadProcessId(app, &pid);
    if (app != lastApp || pid != lastPid) {
        lastApp = app;
        lastPid = pid;
        lastExe = WindowExeName(app);
    }
    return IsExcludedExe(lastExe);
}

// 窗口最大化或铺满整个显示器时返回该显示器，否则返回 nullptr
HMONITOR GetTargetMonitor(HWND hwnd) {
    if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd) || IsCloaked(hwnd)) return nullptr;
    if (hwnd == GetShellWindow() || hwnd == GetDesktopWindow()) return nullptr;
    if (InList(GetClassNameStr(hwnd), kDesktopClasses)) return nullptr;
    if (hwnd == s_minimizing && GetTickCount() - s_minimizingTick < 1000) return nullptr;
    if (Refuses(hwnd)) return nullptr;
    if (IsExcluded(hwnd)) return nullptr;

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (!mon) return nullptr;
    // 程序自己定了最大化尺寸的窗口不算：藏了任务栏它也铺不满，只会在底下露出一条空白
    if (IsZoomed(hwnd) && !OutranksUs(hwnd) && CanFill(hwnd, mon)) return mon;

    // 无边框全屏：游戏、F11 浏览器、全屏视频
    RECT r;
    MONITORINFO mi = {sizeof(mi)};
    if (!GetWindowRect(hwnd, &r) || !GetMonitorInfoW(mon, &mi)) return nullptr;
    const RECT& m = mi.rcMonitor;
    bool covers = r.left <= m.left && r.top <= m.top && r.right >= m.right && r.bottom >= m.bottom;
    return covers ? mon : nullptr;
}

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD);
void Evaluate(bool enforce);

// 只订阅前台进程的位置变化事件：最大化、还原马上就能知道，又不用处理全系统的鼠标、光标移动
void WatchLocation(HWND hwnd) {
    DWORD pid = 0;
    if (hwnd) GetWindowThreadProcessId(hwnd, &pid);
    if (pid == s_locationPid) return;
    if (s_locationHook) UnhookWinEvent(s_locationHook);
    s_locationHook = nullptr;
    s_locationPid = pid;
    if (pid && pid != GetCurrentProcessId())
        s_locationHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnWinEvent, pid, 0,
                                         WINEVENT_OUTOFCONTEXT);
}

// enforce=false 时只在目标变化时动手；enforce=true 时每次都重新确认
// （定时器，顺便处理资源管理器自己把任务栏显示出来、窗口又被缩回去的情况）
void EvaluateNow(bool enforce) {
    // 主任务栏换了：资源管理器刚重新启动（TaskbarCreated 广播可能还没到）
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray && tray != s_lastTray) {
        if (s_lastTray) s_explorerStartedAt = GetTickCount();
        s_lastTray = tray;
    }
    HWND fg = GetForegroundWindow();
    // 自动隐藏开着期间用户在（另一块屏上的）桌面上可能拖过图标：桌面到前台时读一遍位置，离开时再读一遍，
    // 挪过的按新位置记，免得关掉自动隐藏以后拿旧位置把用户摆好的图标挪回去。
    // 要关自动隐藏时 RequestAutoHideOff 会先读完；不稳定的时候（正在开、正在关）读到的不算数
    bool steady = AutoHideSteady();
    if (steady && fg && InList(GetClassNameStr(fg), kDesktopClasses)) {
        if (!s_desktopMarked && DesktopIcons_Mark()) {
            s_desktopMarked = true;
            s_markedAt = GetTickCount();
        }
    } else if (s_desktopMarked) {
        if (steady) FlushDesktopMark();
        else s_desktopMarked = false;
    }
    NoteForeground(fg);
    bool idle = NothingInFront(fg);
    RememberFront(fg);
    HMONITOR dock = DockMonitor(fg, idle);
    bool transient = !dock && (!fg || IsTransient(fg));
    WatchLocation(transient ? s_targetWindow : fg);
    if (transient) {
        // 开始菜单、迷你任务栏这类临时界面在前台时不藏任务栏，也不关自动隐藏，
        // 除非让任务栏隐藏的那个窗口已经最小化、关掉或还原了：
        // 最小化最后一个窗口后，前台常常落到正隐藏着的任务栏上，不处理的话任务栏就一直出不来。
        // 设置里刚关掉的功能也不再保持（设置窗口本身就算临时界面）
        bool shellUi = fg && InList(GetClassNameStr(fg), kShellUiClasses);
        bool keepTarget = !s_targetMonitor ||
                          (g_settings.autoHideOnFullscreen && GetTargetMonitor(s_targetWindow) == s_targetMonitor);
        bool keepDock = !s_dockMonitor || g_settings.desktopDock;
        if (keepTarget && keepDock) {
            if ((s_targetMonitor || s_dockMonitor) && shellUi) ShowForShellUi();
            // 开始菜单贴着任务栏摆，停靠着的迷你任务栏会挡住它的下半截：先收起来，回到桌面再停靠。
            // 其余临时界面（比如点了停靠着的迷你任务栏）期间照样把任务栏藏好：资源管理器重新启动后新的任务栏会显示出来
            if (s_dockMonitor && shellUi) {
                Popup_SetDock(nullptr);
                s_dockAside = true;
            } else if (s_dockMonitor && enforce && !s_dockAside) {
                EnterHiddenMode(s_dockMonitor, nullptr, true);
            }
            return;
        }
        fg = nullptr;
        // 比如在迷你任务栏里关掉了最后一个最大化的窗口：桌面上没有别的窗口了就接着停靠
        if (!shellUi) dock = DockMonitor(nullptr, true);
    }

    HMONITOR target = fg && g_settings.autoHideOnFullscreen ? GetTargetMonitor(fg) : nullptr;
    HWND window = target ? fg : nullptr;
    // 前台换成了普通窗口（比如从迷你任务栏打开的应用），但让任务栏隐藏的那个窗口还最大化着、
    // 露在后面：接着按它来，任务栏照样藏着，不然它会缩回去。等它被最小化、还原或关掉再放出任务栏。
    // 前台是排除名单里的程序、而且它自己最大化着时除外：用户要的是它在前台时看得到任务栏
    bool excludedOnTop = fg && IsZoomed(fg) && IsExcluded(fg);
    if (!target && fg && !excludedOnTop && g_settings.autoHideOnFullscreen && s_targetWindow && fg != s_targetWindow &&
        IsWindow(s_targetWindow) && GetTargetMonitor(s_targetWindow) == s_targetMonitor) {
        target = s_targetMonitor;
        window = s_targetWindow;
    }
    if (target) dock = nullptr;  // 最大化的窗口还露着（比如在另一块屏上）：按它来
    if (dock && fg && IsTray(fg) && !s_handedOff) {
        // 要藏起的任务栏还在前台：先把前台交给桌面，等桌面到了前台再停靠。
        // 不然藏起它时系统会另找一个窗口激活（常常是后面露着的窗口），停靠马上又被撤掉
        s_handedOff = true;
        HWND desktop = GetShellWindow();
        if (desktop) {
            if (!SetForegroundWindow(desktop)) ForceForeground(desktop);
            if (GetForegroundWindow() == desktop) return;
        }
    }
    bool changed = target != s_targetMonitor || window != s_targetWindow || dock != s_dockMonitor;
    s_dockAside = false;
    Popup_SetDock(dock);
    if (!enforce && !changed) return;
    if (changed) {
        if (g_settings.debugLog)
            Log(L"目标 %ls（前台 %ls）%ls", Describe(window).c_str(), Describe(fg).c_str(),
                dock ? L"，桌面上停靠迷你任务栏" : L"");
        // 换了目标：旧目标还在等结果的拉伸请求作废，免得以后被当成它拒绝
        if (s_targetWindow && window != s_targetWindow)
            for (Attempt& a : s_attempts)
                if (a.hwnd == s_targetWindow) a.last = 0;
    }
    s_targetMonitor = target;
    s_targetWindow = window;
    s_dockMonitor = dock;
    if (target && !EnterHiddenMode(target, window)) {
        // 刚判定它不肯铺满：当作没有目标，任务栏放回来，自动隐藏关掉
        s_targetMonitor = nullptr;
        s_targetWindow = nullptr;
        LeaveHiddenMode();
    } else if (dock) {
        EnterHiddenMode(dock, nullptr, true);
    } else if (!target) {
        LeaveHiddenMode();
    }
}

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    switch (event) {
        case EVENT_SYSTEM_FOREGROUND:
            if (!s_enabled) RememberFront(GetForegroundWindow());
            Evaluate(false);
            break;
        case EVENT_SYSTEM_MINIMIZESTART:
            // 窗口开始缩下去时任务栏就开始往上滑，两段动画同时进行
            if (hwnd && hwnd == s_targetWindow) {
                s_minimizing = hwnd;
                s_minimizingTick = GetTickCount();
                Evaluate(false);
            }
            break;
        case EVENT_OBJECT_LOCATIONCHANGE:
            // 最大化、还原立刻响应；目标窗口自己的位置变了（比如又被缩回工作区）就重新确认一遍
            if (idObject == OBJID_WINDOW && idChild == CHILDID_SELF && hwnd && GetAncestor(hwnd, GA_ROOT) == hwnd)
                Evaluate(hwnd == s_targetWindow);
            break;
    }
}

void Evaluate(bool enforce) {
    // 记桌面图标位置是跨进程的 COM 调用，等结果时会处理 WinEvent 回调，别重入
    static bool busy = false;
    if (!s_enabled || busy) return;
    busy = true;
    EvaluateNow(enforce);
    busy = false;
}

void Unhook(HWINEVENTHOOK& hook) {
    if (hook) UnhookWinEvent(hook);
    hook = nullptr;
}

}  // namespace

void Fullscreen_Check() {
    // 顺便清理已经关掉的窗口的记录
    s_attempts.erase(std::remove_if(s_attempts.begin(), s_attempts.end(), [](const Attempt& a) { return !IsWindow(a.hwnd); }),
                     s_attempts.end());
    if (s_ownAutoHide && !s_offRequested) SampleWorkAreas();
    Evaluate(true);
}

void Fullscreen_OnAutoHideOff(UINT seq, bool done) {
    // 之后又要求打开了（动画线程会接着打开），这条回报作废
    if (!s_ownAutoHide || !s_offRequested || seq != s_autoHideSeq) return;
    s_offRequested = false;
    if (!done) {
        // 资源管理器没在运行，没关掉：仍算本程序开着的（设置文件里的记号也留着），等它回来再关。
        // 功能关着、或者前台是任务栏之类时定时器不会再来，单独排一次重试
        Log(L"没能关掉自动隐藏任务栏，过一会儿再试");
        s_offFailedAt = GetTickCount();
        SetTimer(g_mainWnd, kTimerRetryOff, 1100, OnRetryOff);
        return;
    }
    s_offFailedAt = 0;
    s_ownAutoHide = false;
    s_autoHideConfirmed = false;
    SetRestoreAutoHideFlag(false);
    // 等资源管理器广播完工作区的变化，程序和桌面照新的工作区排好以后再收拾
    SetTimer(g_mainWnd, kTimerAfterAutoHide, static_cast<UINT>(300 * TP_ANIM_SCALE), OnAfterAutoHide);
}

void Fullscreen_OnAutoHideOn(UINT seq, bool done) {
    // 之后又要求关、或者重新开过了，这条回报作废
    if (!s_ownAutoHide || s_offRequested || seq != s_autoHideSeq) return;
    if (!done) {
        // 资源管理器在运行却开不了（比如设置被锁住）：试两次还不行就当这个窗口铺不满，把任务栏放回来
        if (!ExplorerRunning() || ++s_onFailures < 2 || !s_targetWindow) return;
        s_onFailures = 0;
        Attempt& a = AttemptFor(s_targetWindow);
        a.refused = true;
        a.monitor = s_targetMonitor;
        Log(L"打不开自动隐藏任务栏，%ls 在它还原或换屏幕之前不再隐藏任务栏", Describe(s_targetWindow).c_str());
        Evaluate(true);
        return;
    }
    s_onFailures = 0;
    if (s_autoHideConfirmed) return;
    s_autoHideConfirmed = true;
    s_autoHideOnAt = GetTickCount();
    // 工作区现在才变大：拉伸请求从现在起重新计数，之前被程序改回去的不算它不肯铺满
    if (s_targetWindow) ForgetRequests(s_targetWindow);
}

void Fullscreen_OnTaskbarCreated() {
    Log(L"资源管理器启动了");
    s_explorerStartedAt = GetTickCount();
    s_offFailedAt = 0;  // 刚才是因为它没在运行才关不掉的，现在马上再试
    if (!s_ownAutoHide) UndoSavedAutoHide();                             // 之前关不掉留下的
    if (s_ownAutoHide && !s_targetMonitor) RequestAutoHideOff(nullptr);  // 功能关着时没有定时器重试
    Evaluate(true);
}

void Fullscreen_SetEnabled(bool enabled) {
    bool was = s_enabled;
    s_enabled = enabled;
    // 前台窗口一直盯着：功能关着时也记着最近的前台窗口，从托盘菜单打开停靠时才知道用户面前是什么
    if (!s_foregroundHook)
        s_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnWinEvent, 0, 0,
                                           WINEVENT_OUTOFCONTEXT);
    if (enabled) {
        if (!was) {
            s_seenFront = nullptr;
            s_trayClicked = false;
        }
        if (!s_minimizeHook)
            s_minimizeHook = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART, nullptr, OnWinEvent, 0,
                                             0, WINEVENT_OUTOFCONTEXT);
        SetTimer(g_mainWnd, kTimerFullscreen, 250, nullptr);
        Evaluate(true);
    } else {
        Unhook(s_minimizeHook);
        Unhook(s_locationHook);
        s_locationPid = 0;
        KillTimer(g_mainWnd, kTimerFullscreen);
        s_targetMonitor = nullptr;
        s_targetWindow = nullptr;
        s_dockMonitor = nullptr;
        s_dockAside = false;
        Popup_SetDock(nullptr);
        LeaveHiddenMode();
    }
}

void Fullscreen_LogState() {
    OSVERSIONINFOW ver = {sizeof(ver)};
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    if (rtlGetVersion) rtlGetVersion(&ver);
    Log(L"Windows %lu.%lu.%lu，隐藏任务栏 %ls，自动隐藏任务栏 %ls%ls", ver.dwMajorVersion, ver.dwMinorVersion,
        ver.dwBuildNumber, s_enabled ? L"开" : L"关", Taskbar_AutoHideOn() ? L"开" : L"关",
        s_ownAutoHide ? L"（本程序打开的）" : L"");
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR m, HDC, LPRECT, LPARAM) -> BOOL {
            MONITORINFO mi = {sizeof(mi)};
            if (GetMonitorInfoW(m, &mi))
                Log(L"屏幕 %p %ls 工作区 %ls 缩放 %.2f", static_cast<void*>(m), RectText(mi.rcMonitor).c_str(),
                    RectText(mi.rcWork).c_str(), MonitorScale(m));
            return TRUE;
        },
        0);
    for (HWND tb : FindTaskbars()) {
        RECT r = {};
        GetWindowRect(tb, &r);
        Log(L"任务栏 %p %ls %ls", static_cast<void*>(tb), RectText(r).c_str(), IsWindowVisible(tb) ? L"显示" : L"隐藏");
    }
}

bool Taskbar_AutoHideOn() { return AutoHideState() > 0; }

bool Taskbar_SetAutoHide(bool on) {
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    // 资源管理器在调用期间退出（卡死后被结束、重启）时调用会返回 0，不能当成“已经关掉”
    auto alive = [&] { return IsWindow(abd.hWnd) && FindWindowW(L"Shell_TrayWnd", nullptr) == abd.hWnd; };
    if (!abd.hWnd || IsHungAppWindow(abd.hWnd)) {
        Log(L"资源管理器没在运行或者没响应，没法%ls自动隐藏", on ? L"打开" : L"关闭");
        return false;
    }
    UINT state = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
    if (!alive()) return false;
    UINT want = on ? (state | ABS_AUTOHIDE) : (state & ~static_cast<UINT>(ABS_AUTOHIDE));
    if (want == state) return true;
    abd.lParam = want;
    double start = NowMs();
    SHAppBarMessage(ABM_SETSTATE, &abd);  // 资源管理器改完设置、广播完工作区的变化才返回
    // 再读一次确认改成了（万一资源管理器是稍后才改的，多看几次）
    bool done = false;
    for (int i = 0; i < 5 && !done && alive(); ++i) {
        if (i) Sleep(20);
        done = ((SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) != 0) == on && alive();
    }
    Log(L"%ls自动隐藏任务栏%ls，用了 %.0f 毫秒", on ? L"打开" : L"关闭", done ? L"" : L"失败", NowMs() - start);
    return done;
}

void Taskbar_RestoreAll() {
    TaskbarAnim_Stop();
    TaskbarAnim_WantAutoHide(0);
    if (g_mainWnd) {
        KillTimer(g_mainWnd, kTimerAfterAutoHide);
        KillTimer(g_mainWnd, kTimerRetryOff);
    }
    bool wasOn = s_ownAutoHide || GetRestoreAutoHideFlag();
    if (s_ownAutoHide && !s_offRequested) SampleWorkAreas();
    // 退出时还在桌面上：用户挪过的图标先按新位置记。前面 Fullscreen_SetEnabled(false) 可能已经发出了这次读取，
    // 而动画线程已经停了、不会替我们等，这里等它读完再关自动隐藏
    FlushDesktopMark();
    DesktopIcons_WaitUpdated(1000);
    DesktopIcons_AbandonUpdates();
    Taskbar_EmergencyRestore();  // 显示任务栏，关掉本程序打开的自动隐藏
    UndoSavedAutoHide();         // 上次异常退出时没来得及关的
    s_ownAutoHide = false;
    s_offRequested = false;
    s_autoHideConfirmed = false;
    s_offFailedAt = 0;
    s_onFailures = 0;
    s_hidden.clear();
    s_everHidden.clear();
    UndoLegacyWorkAreas();
    if (wasOn) {
        PumpSentMessages(400);  // 等资源管理器广播完、各程序把窗口缩回去、桌面排好图标
        for (const Before& b : s_before) {
            MONITORINFO mi = {sizeof(mi)};
            if (!GetMonitorInfoW(b.monitor, &mi) || !EqualRect(&mi.rcMonitor, &b.screen)) continue;
            RECT from = IsRectEmpty(&b.during) ? mi.rcMonitor : b.during;
            if (!EqualRect(&from, &mi.rcWork)) ShrinkMaximized(b.monitor, mi.rcMonitor, from, mi.rcWork);
        }
    }
    s_before.clear();
    RestoreStretched();
    DesktopIcons_Finish(2500);
}

void Taskbar_EmergencyRestore() {
    TaskbarAnim_Abort();  // 动画线程上排着的“藏起来、打开自动隐藏”不要再做了
    // 先关自动隐藏，任务栏回来以后最大化的窗口才不会钻到它下面。
    // 崩溃可能发生在别的线程上，再看一眼设置文件里的记号
    if (s_ownAutoHide || GetRestoreAutoHideFlag()) Taskbar_SetAutoHide(false);
    // 本程序藏过的不管现在看上去是否可见都发一次：藏的请求可能还排在资源管理器的队列里
    for (HWND h : FindTaskbars())
        if (!IsWindowVisible(h) || std::find(s_everHidden.begin(), s_everHidden.end(), h) != s_everHidden.end())
            ShowWindowAsync(h, SW_SHOWNA);
}

bool Taskbar_IsShownOn(HMONITOR monitor) {
    for (HWND h : FindTaskbars())
        if (TaskbarMonitor(h) == monitor) return IsWindowVisible(h) != FALSE;
    return false;
}

}  // namespace app
