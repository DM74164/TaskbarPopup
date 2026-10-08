// 全局键盘钩子：区分 Win 键的短按、组合键和长按。
//
// Win 按下时先拦住不交给系统，然后：
//   - 在阈值内松开         → 补发一次 Win 单击，开始菜单照常打开
//   - 在阈值内按了其它键    → 补发 Win 按下 + 该键，Win+E、Win+D 等组合键照常工作
//   - 按住超过阈值          → 通知主线程弹出迷你任务栏，之后的 Win 松开也吞掉，系统完全感知不到这次按键
//
// 钩子跑在单独的线程上：主线程画弹窗、读窗口列表时，键盘不会因此变卡；
// 钩子回调超时太多次的话 Windows 会悄悄把钩子摘掉，单独线程也能避免这种情况。
#include "common.h"

#include <atomic>

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
            k.time - lastWinTime > 1500)
            s_state = State::Idle;
        lastWinTime = k.time;
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

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);

    StopTimer();
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

void Hook_Configure(bool enabled, int thresholdMs) {
    s_enabled = enabled;
    s_thresholdMs = thresholdMs;
}

void SendStartMenu() { SendWinTap(VK_LWIN); }

}  // namespace app
