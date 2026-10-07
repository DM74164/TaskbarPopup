// 桌面图标的位置。
//
// 打开 / 关闭自动隐藏任务栏会改变工作区，资源管理器接着让桌面按新的工作区重新排列图标，
// 关回去以后图标不一定落回原来的格子。所以打开前记下每个图标的位置，关掉以后把挪了的摆回去。
// 桌面的 IFolderView 通过 IShellWindows 找到（Raymond Chen 介绍过的办法），调用都跨进程到资源管理器，
// 图标多的时候要几百毫秒，所以放在单独的线程上做，不耽误主线程和动画。
#include "common.h"

#include <exdisp.h>
#include <shlguid.h>

#include <atomic>
#include <cwchar>
#include <mutex>

namespace app {
namespace {

constexpr UINT kCmdSave = WM_APP + 1;
constexpr UINT kCmdRestore = WM_APP + 2;  // wParam：发出时的会话编号
constexpr UINT kCmdFinish = WM_APP + 3;

struct IconPos {
    PITEMID_CHILD pidl;
    POINT pt;
};

HANDLE s_thread = nullptr;
DWORD s_threadId = 0;
HANDLE s_savedEvent = nullptr;  // 手动重置；记图标位置期间不发信号
std::mutex s_lock;              // 保护 s_session、s_snapshot
UINT s_session = 0;             // 每打开一次自动隐藏（和退出时）加一，旧的摆放请求看到变了就放弃
bool s_snapshot = false;        // 有一份打开自动隐藏之前记下的位置（可能还在记）
std::atomic<bool> s_discard{false};  // 记得太慢，自动隐藏已经打开了，记下的不能用
std::atomic<bool> s_quit{false};
std::vector<IconPos> s_icons;  // 只在图标线程上用
std::vector<POINT> s_layout;   // 关掉自动隐藏、桌面排好以后读到的位置（资源管理器排的），和 s_icons 一一对应

// 用完自动 Release 的接口指针
template <class T>
struct Ref {
    T* p = nullptr;
    Ref() = default;
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    ~Ref() {
        if (p) p->Release();
    }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// 桌面的 IFolderView。资源管理器没在运行、桌面还没建好时返回 false
bool DesktopView(Ref<IFolderView>& view) {
    Ref<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows.p)))) return false;
    VARIANT loc;
    VariantInit(&loc);
    loc.vt = VT_I4;
    loc.lVal = CSIDL_DESKTOP;
    VARIANT empty;
    VariantInit(&empty);
    long hwnd = 0;
    Ref<IDispatch> disp;
    if (windows->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp.p) != S_OK || !disp) return false;
    Ref<IServiceProvider> provider;
    if (FAILED(disp->QueryInterface(IID_PPV_ARGS(&provider.p)))) return false;
    Ref<IShellBrowser> browser;
    if (FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser.p)))) return false;
    Ref<IShellView> shellView;
    if (FAILED(browser->QueryActiveShellView(&shellView.p)) || !shellView) return false;
    return SUCCEEDED(shellView->QueryInterface(IID_PPV_ARGS(&view.p)));
}

void Clear() {
    for (IconPos& icon : s_icons) CoTaskMemFree(icon.pidl);
    s_icons.clear();
    s_layout.clear();
}

// 用户正在桌面上按着鼠标主键（可能在拖图标）。在别的窗口上按着（拖窗口标题栏之类）不算。
// GetAsyncKeyState 看的是物理按键，左右键对调过的要看右键
bool DesktopPressed() {
    int key = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    if (!(GetAsyncKeyState(key) & 0x8000)) return false;
    POINT pt;
    if (!GetCursorPos(&pt)) return false;
    HWND root = GetAncestor(WindowFromPoint(pt), GA_ROOT);
    wchar_t cls[32] = {};
    if (!root || !GetClassNameW(root, cls, 32)) return false;
    return !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW");
}

void Save() {
    Clear();
    double start = NowMs();
    Ref<IFolderView> view;
    if (!DesktopView(view)) {
        Log(L"找不到桌面，不记图标位置");
    } else if (view->GetAutoArrange() == S_OK) {
        // 自动排列时图标的位置由顺序决定，工作区变回来以后自己就排回去了
        Log(L"桌面图标是自动排列的，不用记位置");
    } else {
        Ref<IEnumIDList> items;
        if (SUCCEEDED(view->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&items.p))) && items) {
            PITEMID_CHILD child = nullptr;
            while (items->Next(1, &child, nullptr) == S_OK) {
                POINT pt;
                if (SUCCEEDED(view->GetItemPosition(child, &pt))) s_icons.push_back({child, pt});
                else CoTaskMemFree(child);
            }
        }
        Log(L"记下 %d 个桌面图标的位置，用了 %.0f 毫秒", static_cast<int>(s_icons.size()), NowMs() - start);
    }
    // 等的一方已经不等了：自动隐藏可能已经打开，读到的也许是重新排过的位置
    if (s_discard.exchange(false)) {
        Log(L"记图标位置太慢，这次不摆回去");
        Clear();
    }
}

// 记下的图标现在的位置。读不到桌面时返回空
std::vector<POINT> Read() {
    std::vector<POINT> now;
    Ref<IFolderView> view;
    if (!DesktopView(view)) return now;
    for (const IconPos& icon : s_icons) {
        POINT pt = {LONG_MIN, LONG_MIN};
        view->GetItemPosition(icon.pidl, &pt);
        now.push_back(pt);
    }
    return now;
}

bool Same(const std::vector<POINT>& a, const std::vector<POINT>& b) {
    return !a.empty() && a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](const POINT& p, const POINT& q) { return p.x == q.x && p.y == q.y; });
}

// 只挪位置变了的，返回挪了几个。
// onlyAt 不为空时只挪还停在 onlyAt 里那个位置上的（资源管理器又排了一遍）：位置又变了的是用户自己拖过的，不动
int Restore(const std::vector<POINT>* onlyAt) {
    if (s_icons.empty()) return 0;
    int moved = 0;
    Ref<IFolderView> view;
    if (!DesktopView(view) || view->GetAutoArrange() == S_OK) return 0;
    for (size_t i = 0; i < s_icons.size(); ++i) {
        const IconPos& icon = s_icons[i];
        POINT cur;
        if (FAILED(view->GetItemPosition(icon.pidl, &cur))) continue;  // 图标已经删掉了
        if (cur.x == icon.pt.x && cur.y == icon.pt.y) continue;
        if (onlyAt && (i >= onlyAt->size() || (*onlyAt)[i].x != cur.x || (*onlyAt)[i].y != cur.y)) continue;
        PCUITEMID_CHILD one[1] = {icon.pidl};
        POINT pt = icon.pt;
        if (SUCCEEDED(view->SelectAndPositionItems(1, one, &pt, SVSI_POSITIONITEM))) {
            // 诊断日志里记一个例子，看得出挪了多远
            if (!moved) Log(L"桌面图标 (%ld,%ld) 摆回 (%ld,%ld)", cur.x, cur.y, pt.x, pt.y);
            ++moved;
        }
    }
    return moved;
}

bool Current(UINT session) {
    std::lock_guard<std::mutex> guard(s_lock);
    return session == s_session && !s_quit;
}

// 等 ms 毫秒。中途又打开了自动隐藏（会话变了）或者要退出时返回 false。
// pressed 不为空时顺便看用户有没有在桌面上按过鼠标
bool Wait(DWORD ms, UINT session, bool* pressed = nullptr) {
    for (DWORD start = GetTickCount(); GetTickCount() - start < ms;) {
        if (!Current(session)) return false;
        if (pressed && DesktopPressed()) *pressed = true;
        Sleep(50);
    }
    return Current(session);
}

// 用户正在桌面上按着鼠标：等松开，最多 10 秒
bool WaitReleased(UINT session, bool& pressed) {
    for (DWORD start = GetTickCount(); DesktopPressed() && GetTickCount() - start < 10000;) {
        pressed = true;
        if (!Wait(100, session)) return false;
    }
    return !DesktopPressed();
}

// 桌面重新排列图标是资源管理器过一会儿才做的：每 150 毫秒读一次位置，连续两次没变（或者等了 2 秒）再摆回去。
// 用户在桌面上按着鼠标（可能在拖图标）时不算排好，等松开。
// 期间用户在桌面上按过鼠标（pressed）的话，只摆从第一次读到现在没动过的：动过的可能是用户自己拖的
bool RestoreWhenSettled(UINT session, bool& pressed) {
    s_layout.clear();
    if (s_icons.empty()) return true;
    std::vector<POINT> first = Read();
    std::vector<POINT> last = first;
    int stable = 0;
    for (DWORD start = GetTickCount(); stable < 2 && GetTickCount() - start < 2000;) {
        if (!Wait(150, session, &pressed)) return false;
        std::vector<POINT> now = Read();
        stable = Same(now, last) && !DesktopPressed() ? stable + 1 : 0;
        last = std::move(now);
    }
    if (!WaitReleased(session, pressed)) return false;
    if (pressed) last = Read();  // 松开以后的位置
    s_layout = std::move(last);
    if (pressed) Log(L"用户在桌面上按过鼠标，只摆没被拖过的图标");
    Log(L"桌面图标摆回原位 %d 个", Restore(pressed ? &first : nullptr));
    return true;
}

// 这一轮用完了：会话没变（没有又打开自动隐藏）就丢掉记下的位置，下次打开前重新记
void Done(UINT session) {
    std::lock_guard<std::mutex> guard(s_lock);
    if (session != s_session) return;
    Clear();
    s_snapshot = false;
}

DWORD WINAPI ThreadProc(LPVOID ready) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // 建好消息队列以后才能收 PostThreadMessage
    SetEvent(static_cast<HANDLE>(ready));
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (s_quit) continue;
        switch (msg.message) {
            case kCmdSave:
                Save();
                SetEvent(s_savedEvent);
                break;
            case kCmdRestore: {
                UINT session = static_cast<UINT>(msg.wParam);
                // 资源管理器有时过一会儿又排一次，1 秒后再查一遍。
                // 这期间用户在桌面上按过鼠标的话，只摆还停在资源管理器排的位置上的，别的可能是用户拖过的
                bool pressed = false;
                if (RestoreWhenSettled(session, pressed) && Wait(1000, session, &pressed) && WaitReleased(session, pressed))
                    if (int moved = Restore(pressed ? &s_layout : nullptr)) Log(L"又摆回 %d 个桌面图标", moved);
                // 用户一直按着没摆完也丢掉，免得下次拿旧位置把用户拖过的图标挪回去。又打开了自动隐藏（会话变了）时留着接着用
                Done(session);
                break;
            }
            case kCmdFinish: {
                UINT session = static_cast<UINT>(msg.wParam);
                bool pressed = false;
                RestoreWhenSettled(session, pressed);
                Done(session);
                break;
            }
        }
    }
    Clear();
    CoUninitialize();
    return 0;
}

bool EnsureThread() {
    if (s_thread) return true;
    if (!s_savedEvent) s_savedEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s_savedEvent || !ready) {
        if (ready) CloseHandle(ready);
        return false;
    }
    s_quit = false;
    s_thread = CreateThread(nullptr, 0, ThreadProc, ready, 0, &s_threadId);
    if (s_thread) WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);
    return s_thread != nullptr;
}

}  // namespace

void DesktopIcons_BeginSession() {
    std::lock_guard<std::mutex> guard(s_lock);
    ++s_session;
    if (s_snapshot) return;  // 上次关掉以后还没摆完：记下的还是打开之前的样子，接着用
    if (!EnsureThread()) return;
    s_snapshot = true;
    s_discard = false;
    ResetEvent(s_savedEvent);
    if (!PostThreadMessageW(s_threadId, kCmdSave, 0, 0)) {
        s_snapshot = false;
        SetEvent(s_savedEvent);
    }
}

void DesktopIcons_WaitSaved(DWORD ms) {
    if (!s_savedEvent) return;
    if (WaitForSingleObject(s_savedEvent, ms) == WAIT_TIMEOUT) s_discard = true;
}

void DesktopIcons_RestoreLater() {
    std::lock_guard<std::mutex> guard(s_lock);
    if (s_thread && s_snapshot) PostThreadMessageW(s_threadId, kCmdRestore, s_session, 0);
}

void DesktopIcons_Finish(DWORD ms) {
    if (!s_thread) return;
    {
        std::lock_guard<std::mutex> guard(s_lock);
        ++s_session;  // 还在排队、在等的摆放请求都作废
        if (s_snapshot) PostThreadMessageW(s_threadId, kCmdFinish, s_session, 0);
        s_snapshot = false;
    }
    PostThreadMessageW(s_threadId, WM_QUIT, 0, 0);
    if (WaitForSingleObject(s_thread, ms) == WAIT_TIMEOUT) {
        s_quit = true;  // 资源管理器卡住了，不等了；线程读完这次就退出
        Log(L"摆桌面图标超时，不等了");
    } else {
        s_quit = false;
    }
    CloseHandle(s_thread);
    s_thread = nullptr;
    s_threadId = 0;
    if (s_savedEvent) SetEvent(s_savedEvent);
}

}  // namespace app
