// 全局键盘钩子：区分 Win 键的短按、组合键和长按。
//
// Win 按下时先拦住不交给系统，然后：
//   - 在阈值内松开         → 补发一次 Win 单击，开始菜单照常打开
//   - 在阈值内按了其它键    → 补发 Win 按下 + 该键，Win+E、Win+D 等组合键照常工作
//   - 按住超过阈值          → 通知主线程弹出迷你任务栏，之后的 Win 松开也吞掉，系统完全感知不到这次按键
//
// 钩子跑在单独的线程上：主线程画弹窗、读窗口列表时，键盘不会因此变卡；
// 钩子回调超时太多次的话 Windows 会悄悄把钩子摘掉，单独线程也能避免这种情况。
//
// 设置里的快捷键有一部分也在这里认（鼠标键、长按、单独的 Ctrl / Shift / Alt、锁定键，见 Hook_SetBindings）：
//   - 只有单按：按下就触发，这次按下和松开都吞掉
//   - 要长按：按下先拦住，按够时长触发；没按够就松开的话，有单按的算单按，没有就把这次按键原样补发回去；
//     中间按了别的键（鼠标挪远了），也先补发回去
//   - 单独的 Ctrl / Shift / Alt 不拦，照常交给系统：中间没按别的键时才算数，触发了的话松开前补发一个没用的键，
//     免得单按 Alt 激活菜单栏、单按 Shift 切换输入法
#include "common.h"

#include <atomic>
#include <cstdlib>
#include <mutex>

namespace app {
namespace {

enum class State { Idle, Pending, Passthrough, Triggered };

// 本程序注入的按键带这个标记，钩子见到就直接放行
constexpr ULONG_PTR kInjectMarker = 0x54425055;

// 主线程改设置，钩子线程读
std::atomic<bool> s_enabled{true};
std::atomic<int> s_thresholdMs{kDefaultLongPressMs};

// 以下只在钩子线程里访问
HHOOK s_hook = nullptr;
UINT_PTR s_timer = 0;
State s_state = State::Idle;
DWORD s_winVk = 0;

HANDLE s_thread = nullptr;
DWORD s_threadId = 0;

// 快捷键：主线程交过来的放在 s_newBindings，钩子线程收到 kMsgBindings 后换上
constexpr UINT kMsgBindings = WM_APP + 1;
constexpr DWORD kMaskVk = 0xE8;  // 没分配用途的键码，单独的 Alt / Shift 触发后补发它
std::mutex s_bindLock;
std::vector<KeyBinding> s_newBindings;

// 以下只在钩子线程里访问
std::vector<KeyBinding> s_bindings;
HHOOK s_mouseHook = nullptr;
UINT_PTR s_bindTimer = 0;
enum class Press { Idle, Pending, Fired, Swallow, Modifier };
struct {
    Press state = Press::Idle;
    KeyBinding binding;
    UINT vk = 0;
    DWORD scan = 0;
    bool extended = false;
    bool fired = false;  // 单独的修饰键：已经触发过了
    DWORD downTime = 0;
    DWORD lastTime = 0;  // 这个键最近一次事件（按住时的自动重复）的时间
    POINT pt = {};
    HWND target = nullptr;  // 鼠标键：按下时鼠标下的窗口
} s_press;

INPUT KeyInput(DWORD vk, DWORD scan, bool up, bool extended) {
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = static_cast<WORD>(vk);
    in.ki.wScan = static_cast<WORD>(scan ? scan : MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
    in.ki.dwExtraInfo = kInjectMarker;
    return in;
}

void SendWinTap(DWORD vk) {
    INPUT in[2] = {KeyInput(vk, 0, false, true), KeyInput(vk, 0, true, true)};
    SendInput(2, in, sizeof(INPUT));
}

bool ModifierHeld() {
    return ((GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU)) & 0x8000) != 0;
}

void StopTimer() {
    if (s_timer) KillTimer(nullptr, s_timer);
    s_timer = 0;
}

void CALLBACK OnLongPressTimer(HWND, UINT, UINT_PTR, DWORD) {
    StopTimer();
    if (s_state != State::Pending) return;
    s_state = State::Triggered;
    PostMessageW(g_mainWnd, WM_APP_LONGPRESS, 0, 0);
}

void StartTimer() {
    StopTimer();
    s_timer = SetTimer(nullptr, 0, static_cast<UINT>(s_thresholdMs.load()), OnLongPressTimer);
}

// 按住 Win 时两次自动重复之间最长隔多久。一般不到 1 秒；开了“筛选键”的话重复延迟、间隔可以设到 2 秒
DWORD RepeatGapLimit() {
    DWORD limit = 1500;
    FILTERKEYS fk = {sizeof(fk)};
    if (SystemParametersInfoW(SPI_GETFILTERKEYS, sizeof(fk), &fk, 0) && (fk.dwFlags & FKF_FILTERKEYSON))
        limit = std::max<DWORD>(limit, std::max(fk.iDelayMSec, fk.iRepeatMSec) + 500);
    return limit;
}

// ---- 快捷键 ----

INPUT MouseInput(UINT vk, bool up) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    switch (vk) {
        case VK_LBUTTON: in.mi.dwFlags = up ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN; break;
        case VK_RBUTTON: in.mi.dwFlags = up ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN; break;
        case VK_MBUTTON: in.mi.dwFlags = up ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN; break;
        default:
            in.mi.dwFlags = up ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN;
            in.mi.mouseData = vk == VK_XBUTTON1 ? XBUTTON1 : XBUTTON2;
            break;
    }
    in.mi.dwExtraInfo = kInjectMarker;
    return in;
}

// 拦下的那次按下（键盘或鼠标）
INPUT PressInput(bool up) {
    if (IsMouseVk(s_press.vk)) return MouseInput(s_press.vk, up);
    return KeyInput(s_press.vk, s_press.scan, up, s_press.extended);
}

void StopBindTimer() {
    if (s_bindTimer) KillTimer(nullptr, s_bindTimer);
    s_bindTimer = 0;
}

void Fire(int action) {
    if (action >= 0) PostMessageW(g_mainWnd, WM_APP_HOTKEY, static_cast<WPARAM>(action), reinterpret_cast<LPARAM>(s_press.target));
}

void CALLBACK OnBindTimer(HWND, UINT, UINT_PTR, DWORD) {
    StopBindTimer();
    if (s_press.state == Press::Pending) {
        s_press.state = Press::Fired;  // 之后的自动重复和松开都吞掉
        Fire(s_press.binding.hold);
    } else if (s_press.state == Press::Modifier && !s_press.fired) {
        s_press.fired = true;
        Fire(s_press.binding.hold);
    }
}

// 现在按着的 Ctrl / Shift / Alt（except 这个键不算：它自己就是快捷键的键）
UINT HeldMods(UINT except) {
    auto held = [except](int vk) { return static_cast<UINT>(vk) != except && (GetAsyncKeyState(vk) & 0x8000); };
    return (held(VK_LCONTROL) || held(VK_RCONTROL) ? MOD_CONTROL : 0) | (held(VK_LSHIFT) || held(VK_RSHIFT) ? MOD_SHIFT : 0) |
           (held(VK_LMENU) || held(VK_RMENU) ? MOD_ALT : 0);
}

const KeyBinding* Match(UINT vk) {
    UINT mods = HeldMods(vk);
    for (const KeyBinding& b : s_bindings)
        if (b.vk == vk && b.mods == mods) return &b;
    return nullptr;
}

// 开始处理一次按下：返回 true 表示吞掉
bool BeginPress(const KeyBinding& b, UINT vk, DWORD scan, bool extended, DWORD time, POINT pt, HWND target) {
    s_press.binding = b;
    s_press.vk = vk;
    s_press.scan = scan;
    s_press.extended = extended;
    s_press.fired = false;
    s_press.downTime = time;
    s_press.lastTime = time;
    s_press.pt = pt;
    s_press.target = target;
    if (IsModifierVk(vk)) {
        s_press.state = Press::Modifier;
        if (b.hold >= 0) s_bindTimer = SetTimer(nullptr, 0, static_cast<UINT>(s_thresholdMs.load()), OnBindTimer);
        return false;
    }
    if (b.hold < 0) {
        s_press.state = Press::Swallow;
        Fire(b.tap);
        return true;
    }
    s_press.state = Press::Pending;
    s_bindTimer = SetTimer(nullptr, 0, static_cast<UINT>(s_thresholdMs.load()), OnBindTimer);
    return true;
}

// 拦着的按下没等到长按就松开了：算单按，或者把这次按键原样补发回去
void ReleaseEarly() {
    StopBindTimer();
    s_press.state = Press::Idle;
    if (s_press.binding.tap >= 0) {
        Fire(s_press.binding.tap);
    } else {
        INPUT in[2] = {PressInput(false), PressInput(true)};
        SendInput(2, in, sizeof(INPUT));
    }
}

// 拦着的按下没等到长按就按了别的：先补发这次按下，再补发 next（吞掉原来的，保证先后顺序）
void GiveBack(const INPUT& next) {
    StopBindTimer();
    s_press.state = Press::Idle;
    INPUT in[2] = {PressInput(false), next};
    SendInput(2, in, sizeof(INPUT));
}

// 键盘事件：1 吞掉，0 放行，-1 不归快捷键管（接着按长按 Win 的逻辑走）
int BindKey(bool down, const KBDLLHOOKSTRUCT& k) {
    UINT vk = k.vkCode;
    bool mine = vk == s_press.vk && !IsMouseVk(s_press.vk);
    // 等这个键松开的时候隔了很久又有键按下：松开没收到（松开时前台是管理员权限的程序、切到了锁屏之类），从头开始
    bool waiting = s_press.state == Press::Fired || s_press.state == Press::Swallow ||
                   (s_press.state == Press::Modifier && s_press.fired);
    if (waiting && down && !IsMouseVk(s_press.vk) && k.time - s_press.lastTime > RepeatGapLimit()) {
        StopBindTimer();
        s_press.state = Press::Idle;
    }
    if (mine) s_press.lastTime = k.time;
    switch (s_press.state) {
        case Press::Idle: {
            if (!down || (k.flags & LLKHF_INJECTED)) return -1;
            const KeyBinding* b = Match(vk);
            if (!b) return -1;
            return BeginPress(*b, vk, k.scanCode, (k.flags & LLKHF_EXTENDED) != 0, k.time, {}, nullptr) ? 1 : 0;
        }
        case Press::Pending:
            if (mine) {
                if (down) return 1;  // 按住时的自动重复
                ReleaseEarly();
                return 1;
            }
            if (!down) return -1;  // 之前就按着的键松开
            GiveBack(KeyInput(vk, k.scanCode, false, (k.flags & LLKHF_EXTENDED) != 0));
            return 1;
        case Press::Fired:
        case Press::Swallow:
            if (!mine) return -1;
            if (!down) s_press.state = Press::Idle;
            return 1;
        case Press::Modifier:
            if (vk == s_press.vk) {
                if (down) return 0;  // 自动重复
                StopBindTimer();
                s_press.state = Press::Idle;
                if (!s_press.fired && s_press.binding.tap >= 0 && k.time - s_press.downTime < static_cast<DWORD>(s_thresholdMs.load())) {
                    s_press.fired = true;
                    Fire(s_press.binding.tap);
                }
                if (!s_press.fired) return 0;
                // 触发了：松开前补发一个没用的键，系统就不会当成单按了 Alt / Shift
                INPUT in[3] = {KeyInput(kMaskVk, 0, false, false), KeyInput(kMaskVk, 0, true, false),
                               KeyInput(vk, k.scanCode, true, (k.flags & LLKHF_EXTENDED) != 0)};
                SendInput(3, in, sizeof(INPUT));
                return 1;
            }
            if (down && !s_press.fired) {  // 按了组合键：这次不算，这个键照常看是不是快捷键
                StopBindTimer();
                s_press.state = Press::Idle;
                return BindKey(down, k);
            }
            return -1;
    }
    return -1;
}

// 鼠标事件：返回 true 表示吞掉
bool HandleMouse(WPARAM msg, const MSLLHOOKSTRUCT& m) {
    if (m.dwExtraInfo == kInjectMarker) return false;
    if (msg == WM_MOUSEMOVE) {
        // 拦着鼠标键时挪远了，是要拖动：把按下还回去
        if (s_press.state == Press::Pending && IsMouseVk(s_press.vk) &&
            (std::abs(m.pt.x - s_press.pt.x) > GetSystemMetrics(SM_CXDRAG) ||
             std::abs(m.pt.y - s_press.pt.y) > GetSystemMetrics(SM_CYDRAG))) {
            StopBindTimer();
            s_press.state = Press::Idle;
            INPUT in = PressInput(false);
            SendInput(1, &in, sizeof(INPUT));
        }
        return false;
    }
    UINT vk = 0;
    bool down = false;
    switch (msg) {
        case WM_LBUTTONDOWN: down = true; [[fallthrough]];
        case WM_LBUTTONUP: vk = VK_LBUTTON; break;
        case WM_RBUTTONDOWN: down = true; [[fallthrough]];
        case WM_RBUTTONUP: vk = VK_RBUTTON; break;
        case WM_MBUTTONDOWN: down = true; [[fallthrough]];
        case WM_MBUTTONUP: vk = VK_MBUTTON; break;
        case WM_XBUTTONDOWN: down = true; [[fallthrough]];
        case WM_XBUTTONUP: vk = HIWORD(m.mouseData) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2; break;
        default: return false;
    }
    bool mine = vk == s_press.vk;
    switch (s_press.state) {
        case Press::Idle: {
            if (!down || (m.flags & LLMHF_INJECTED) || s_state != State::Idle) return false;
            const KeyBinding* b = Match(vk);
            if (!b) return false;
            return BeginPress(*b, vk, 0, false, m.time, m.pt, GetAncestor(WindowFromPoint(m.pt), GA_ROOT));
        }
        case Press::Pending:
            if (mine) {
                if (!down) ReleaseEarly();
                return true;
            }
            if (!down) return false;
            GiveBack(MouseInput(vk, false));
            return true;
        case Press::Fired:
        case Press::Swallow:
            if (!mine) return false;
            if (down) {  // 又按下了：上次松开没收到，这次从头处理
                s_press.state = Press::Idle;
                return HandleMouse(msg, m);
            }
            s_press.state = Press::Idle;
            return true;
        case Press::Modifier:
            if (down && !s_press.fired) {  // Ctrl + 点击之类：这次不算
                StopBindTimer();
                s_press.state = Press::Idle;
            }
            return false;
    }
    return false;
}

LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && HandleMouse(wParam, *reinterpret_cast<MSLLHOOKSTRUCT*>(lParam))) return 1;
    return CallNextHookEx(s_mouseHook, code, wParam, lParam);
}

// 换上主线程交过来的快捷键。有鼠标键、单独的修饰键（要看中间点没点鼠标）时才装鼠标钩子
void TakeBindings() {
    {
        std::lock_guard<std::mutex> guard(s_bindLock);
        s_bindings = s_newBindings;
    }
    bool mouse = std::any_of(s_bindings.begin(), s_bindings.end(),
                             [](const KeyBinding& b) { return IsMouseVk(b.vk) || IsModifierVk(b.vk); });
    if (mouse && !s_mouseHook) {
        s_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
        if (!s_mouseHook) Log(L"快捷键：鼠标钩子装不上（错误 %lu），鼠标键的快捷键用不了", GetLastError());
    } else if (!mouse && s_mouseHook) {
        UnhookWindowsHookEx(s_mouseHook);
        s_mouseHook = nullptr;
    }
}

// 返回 true 表示吞掉这个按键事件
bool HandleKey(WPARAM msg, const KBDLLHOOKSTRUCT& k) {
    if (k.dwExtraInfo == kInjectMarker) return false;

    bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    bool isWin = k.vkCode == VK_LWIN || k.vkCode == VK_RWIN;
    if (isWin) {
        // 等 Win 松开的状态下，隔了很久又来一个 Win 按下：不是按住时的自动重复（最慢也几百毫秒一次），
        // 是上次松开没收到（松开时前台是管理员权限的程序、或者切到了锁屏 / UAC 界面）。从头开始，这次照常处理
        static DWORD lastWinTime = 0;
        if (down && k.vkCode == s_winVk && (s_state == State::Triggered || s_state == State::Passthrough) &&
            k.time - lastWinTime > 1500 && k.time - lastWinTime > RepeatGapLimit())
            s_state = State::Idle;
        lastWinTime = k.time;
    }

    // 快捷键：正在处理的那次按下要看所有按键（按了别的键就补发回去）；没有的话 Win 键的事不归它管
    if (s_press.state != Press::Idle || (s_state == State::Idle && !isWin)) {
        int r = BindKey(down, k);
        if (r >= 0) return r == 1;
    }

    switch (s_state) {
        case State::Idle:
            if (!s_enabled || !isWin || !down) return false;
            s_winVk = k.vkCode;
            // 别的软件注入的 Win，或者先按住了 Ctrl/Shift/Alt 的组合键：不干预
            if ((k.flags & LLKHF_INJECTED) || ModifierHeld()) {
                s_state = State::Passthrough;
                return false;
            }
            s_state = State::Pending;
            StartTimer();
            return true;

        case State::Pending:
            if (k.vkCode == s_winVk) {
                if (down) return true;  // 按住时的自动重复
                StopTimer();
                s_state = State::Idle;
                SendWinTap(s_winVk);  // 短按：还给系统一次完整的 Win 单击
                return true;
            }
            if (!down) return false;  // 之前就按着的键松开，与 Win 无关
            {
                // 组合键：按顺序补发 Win 按下和当前这个键
                StopTimer();
                s_state = State::Passthrough;
                INPUT in[2] = {KeyInput(s_winVk, 0, false, true),
                               KeyInput(k.vkCode, k.scanCode, false, (k.flags & LLKHF_EXTENDED) != 0)};
                SendInput(2, in, sizeof(INPUT));
            }
            return true;

        case State::Passthrough:
            if (k.vkCode == s_winVk && !down) s_state = State::Idle;
            return false;

        case State::Triggered:
            if (k.vkCode != s_winVk) return false;  // 其它键放给弹窗（方向键、回车等）
            if (!down) s_state = State::Idle;
            return true;
    }
    return false;
}

LRESULT CALLBACK HookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && HandleKey(wParam, *reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam))) return 1;
    return CallNextHookEx(s_hook, code, wParam, lParam);
}

struct StartInfo {
    HANDLE ready;
    bool ok;
};

DWORD WINAPI HookThread(LPVOID param) {
    auto* start = static_cast<StartInfo*>(param);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // 先建好消息队列
    s_hook = SetWindowsHookExW(WH_KEYBOARD_LL, HookProc, GetModuleHandleW(nullptr), 0);
    start->ok = s_hook != nullptr;
    SetEvent(start->ready);
    if (!s_hook) return 1;
    TakeBindings();

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!msg.hwnd && msg.message == kMsgBindings) TakeBindings();
        else DispatchMessageW(&msg);
    }

    StopTimer();
    StopBindTimer();
    if (s_mouseHook) UnhookWindowsHookEx(s_mouseHook);
    s_mouseHook = nullptr;
    UnhookWindowsHookEx(s_hook);
    s_hook = nullptr;
    return 0;
}

}  // namespace

bool Hook_Install() {
    if (s_thread) return true;
    StartInfo start = {CreateEventW(nullptr, TRUE, FALSE, nullptr), false};
    s_thread = CreateThread(nullptr, 0, HookThread, &start, 0, &s_threadId);
    if (s_thread) WaitForSingleObject(start.ready, INFINITE);
    CloseHandle(start.ready);
    if (s_thread && !start.ok) {
        WaitForSingleObject(s_thread, INFINITE);
        CloseHandle(s_thread);
        s_thread = nullptr;
    }
    return s_thread != nullptr;
}

void Hook_Uninstall() {
    if (!s_thread) return;
    PostThreadMessageW(s_threadId, WM_QUIT, 0, 0);
    WaitForSingleObject(s_thread, 2000);
    CloseHandle(s_thread);
    s_thread = nullptr;
}

void Hook_SetBindings(const std::vector<KeyBinding>& bindings) {
    {
        std::lock_guard<std::mutex> guard(s_bindLock);
        s_newBindings = bindings;
    }
    if (s_thread) PostThreadMessageW(s_threadId, kMsgBindings, 0, 0);  // 还没启动的话启动时会取
}

void Hook_Configure(bool enabled, int thresholdMs) {
    s_enabled = enabled;
    s_thresholdMs = thresholdMs;
}

void SendStartMenu() { SendWinTap(VK_LWIN); }

void SendWinX() {
    // Shift+F10 打开的菜单：Shift 还按着，先松开，不然成了 Win+Shift+X
    std::vector<INPUT> in;
    for (DWORD vk : {VK_LSHIFT, VK_RSHIFT})
        if (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) in.push_back(KeyInput(vk, 0, true, false));
    in.push_back(KeyInput(VK_LWIN, 0, false, true));
    in.push_back(KeyInput('X', 0, false, false));
    in.push_back(KeyInput('X', 0, true, false));
    in.push_back(KeyInput(VK_LWIN, 0, true, true));
    SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT));
}

}  // namespace app
