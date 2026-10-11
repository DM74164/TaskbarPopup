// 桌面图标的位置。
//
// 打开 / 关闭自动隐藏任务栏会改变工作区，资源管理器接着让桌面按新的工作区重新排列图标，
// 关回去以后图标不一定落回原来的格子。所以打开前记下每个图标的位置，关掉以后把挪了的摆回去。
// 工作区一变，资源管理器就把桌面上所有图标按比例挪一小段（关掉网格对齐也一样）。打开自动隐藏时桌面被全屏窗口挡着，
// 看不见；关掉时桌面正露着，图标挪回去那一下看得清清楚楚。所以打开自动隐藏前把桌面图标藏起来，
// 关掉以后等图标都排回原位再显示（只有一块屏幕时这么做：多块屏幕的话别的屏上的桌面一直露着，不能藏）。
// 资源管理器排图标要几百毫秒，所以打开自动隐藏之前先给桌面拍张照（被窗口挡着也拍得到），藏起图标的同时把照片垫在桌面上面、
// 所有窗口下面（被全屏窗口挡着），回到桌面那一刻看到的就是照片；图标排好、显示出来以后再撤掉照片：
// 看上去图标一直在原处。
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
// 自动隐藏开着期间用户可能在（另一块屏上的）桌面上拖过图标：桌面到前台时读一遍位置（Mark），
// 离开桌面或者要关自动隐藏之前再读一遍（Update），两次之间挪过的图标按新位置记，免得之后被摆回旧位置。
// 只记用户挪过的：其余的还按打开自动隐藏之前的位置摆回去。wParam：会话编号
constexpr UINT kCmdMark = WM_APP + 4;
constexpr UINT kCmdUpdate = WM_APP + 5;
constexpr UINT kCmdRecover = WM_APP + 6;  // 上次藏起的桌面图标没来得及显示回来（被强行结束、崩溃）
constexpr UINT kCmdReveal = WM_APP + 7;   // 自动隐藏刚关掉：图标排回原位就显示。wParam：会话编号
constexpr UINT kCmdCover = WM_APP + 8;    // 要回到桌面了：照片重新垫一下（资源管理器可能改过桌面窗口的层次）
// 上次关掉以后还没摆完又打开了自动隐藏：记下的位置接着用，图标已经显示回来的话重新拍照、藏起
constexpr UINT kCmdRehide = WM_APP + 9;
constexpr UINT kCmdDisplay = WM_APP + 10;  // 显示器变了：接上了第二块屏的话藏着的图标马上显示（别的屏上的桌面露着）
constexpr wchar_t kCoverClass[] = L"TaskbarPopupDesktopCover";

struct IconPos {
    PITEMID_CHILD pidl;
    POINT pt;
};

HANDLE s_thread = nullptr;
DWORD s_threadId = 0;
HANDLE s_savedEvent = nullptr;  // 手动重置；记图标位置期间不发信号
HANDLE s_updatedEvent = nullptr;  // 手动重置；最后发出的那次 Update 还没读完时不发信号。关自动隐藏之前等它
UINT s_updatePosted = 0;          // 发出的 Update 的编号（s_lock 保护），读完的是最后一次才发信号
std::atomic<UINT> s_updateLate{0};  // 关自动隐藏那一刻最后发出的编号：这个编号及以前、还没读完的读到的位置不能用
std::mutex s_lock;              // 保护 s_session、s_snapshot
UINT s_session = 0;             // 每打开一次自动隐藏（和退出时）加一，旧的摆放请求看到变了就放弃
bool s_snapshot = false;        // 有一份打开自动隐藏之前记下的位置（可能还在记）
// 等记位置、拍照等得超时的那次会话的编号（等的一方不等了，自动隐藏已经打开）：这次及以前的会话记下的、拍下的都不能用。
// 只增不减：新会话开始时不清，免得把上一次还没记完的那份当成能用的
std::atomic<UINT> s_discardFrom{0};
bool Discarded(UINT session) { return s_discardFrom >= session; }
std::atomic<bool> s_quit{false};
std::vector<IconPos> s_icons;  // 只在图标线程上用
std::vector<POINT> s_layout;   // 关掉自动隐藏、桌面排好以后读到的位置（资源管理器排的），和 s_icons 一一对应
std::vector<POINT> s_mark;     // Mark 时读到的位置，和 s_icons 一一对应
HWND s_hiddenList = nullptr;   // 藏起来的桌面图标窗口，只在图标线程上用
std::atomic<HWND> s_hiddenNow{nullptr};  // 同上，给主线程看：退出等不及、崩溃时直接显示
RECT s_hiddenRect = {};        // 藏起时它的位置大小（工作区变了它可能跟着变）
double s_hiddenAt = 0;
// 桌面的照片，和盖在桌面上的照片窗口。只在图标线程上用
HDC s_shotDC = nullptr;
HBITMAP s_shot = nullptr;
HGDIOBJ s_shotOld = nullptr;
const DWORD* s_shotBits = nullptr;  // 照片的像素（s_shotWindow 那么大，从上到下）
// 照片只盖图标那些地方（s_shotArea 那么大，1 是要盖）：藏起图标后再拍一张只有壁纸的，和照片不一样的就是图标。空着就是整张盖
std::vector<BYTE> s_mask;
RECT s_shotWindow = {};          // 拍照时桌面窗口在屏幕上的范围（照片的原点）
RECT s_shotArea = {};            // 要盖住的范围：拍照时的工作区
HWND s_desktopRoot = nullptr;    // 放图标的顶层窗口（Progman / WorkerW），照片窗口紧贴在它上面
HWND s_cover = nullptr;
bool s_coverShown = false;
double s_coverAt = 0;
// 记位置时（打开自动隐藏之前）的图标窗口、它的位置大小和工作区：藏图标在开自动隐藏之后做，那时位置大小和工作区可能已经变了
HWND s_savedList = nullptr;
RECT s_savedListRect = {};
RECT s_savedWork = {};

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

// 藏起图标时也记在设置文件里：被强行结束的话，下次启动时显示回来
bool SavedHidden() { return GetPrivateProfileIntW(L"State", L"DesktopIconsHidden", 0, SettingsFile().c_str()) != 0; }

void SaveHidden(bool hidden) {
    EnsureUnicodeIni();
    WritePrivateProfileStringW(L"State", L"DesktopIconsHidden", hidden ? L"1" : nullptr, SettingsFile().c_str());
}

// 桌面上放图标的那个列表窗口
HWND IconList(IFolderView* view) {
    Ref<IShellView> shellView;
    HWND defView = nullptr;
    if (FAILED(view->QueryInterface(IID_PPV_ARGS(&shellView.p))) || FAILED(shellView->GetWindow(&defView)) || !defView)
        return nullptr;
    return FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
}

std::vector<POINT> Read();
bool AtSaved(const std::vector<POINT>& now);

// 等 ms 毫秒，期间照样处理别的线程发来的消息：资源管理器广播工作区变化时要等每个顶层窗口（包括这个线程的照片窗口
// 和 COM 的隐藏窗口）处理完，这里干睡会拖慢它
void Pause(DWORD ms) {
    for (DWORD start = GetTickCount(), elapsed = 0; elapsed < ms; elapsed = GetTickCount() - start) {
        if (MsgWaitForMultipleObjectsEx(0, nullptr, ms - elapsed, QS_SENDMESSAGE, 0) != WAIT_OBJECT_0) break;
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
    }
}

void ReleaseMask();

void ReleaseShot() {
    ReleaseMask();
    if (s_shotDC) {
        SelectObject(s_shotDC, s_shotOld);
        DeleteDC(s_shotDC);
    }
    if (s_shot) DeleteObject(s_shot);
    s_shotDC = nullptr;
    s_shot = nullptr;
    s_shotOld = nullptr;
    s_shotBits = nullptr;
}

// 一块 32 位的内存位图
struct Dib {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    DWORD* bits = nullptr;
    int w = 0, h = 0;
    Dib() = default;
    Dib(const Dib&) = delete;
    Dib& operator=(const Dib&) = delete;
    ~Dib() {
        if (dc) {
            SelectObject(dc, old);
            DeleteDC(dc);
        }
        if (bmp) DeleteObject(bmp);
    }
    bool Create(int width, int height) {
        if (bits && w == width && h == height) return true;
        BITMAPINFO bi = {};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
        void* p = nullptr;
        if (!dc) dc = CreateCompatibleDC(nullptr);
        HBITMAP b = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, nullptr, 0) : nullptr;
        if (!b) return false;
        HGDIOBJ prev = SelectObject(dc, b);
        if (bmp) DeleteObject(bmp);
        else old = prev;
        bmp = b;
        bits = static_cast<DWORD*>(p);
        w = width;
        h = height;
        return true;
    }
};

// 两个像素看上去一样：每个颜色分量差得不多
bool Near(DWORD a, DWORD b) {
    for (int shift = 0; shift < 24; shift += 8) {
        int d = static_cast<int>((a >> shift) & 0xFF) - static_cast<int>((b >> shift) & 0xFF);
        if (d > 6 || d < -6) return false;
    }
    return true;
}

// 两张和 s_shotWindow 一样大的图在照片盖住的地方有多少个点不一样（隔一个点比一个）
long CountDiff(const DWORD* a, const DWORD* b, int w) {
    long diff = 0;
    LONG ox = s_shotArea.left - s_shotWindow.left, oy = s_shotArea.top - s_shotWindow.top;
    LONG aw = s_shotArea.right - s_shotArea.left, ah = s_shotArea.bottom - s_shotArea.top;
    bool masked = s_mask.size() == static_cast<size_t>(aw) * ah;
    for (LONG y = 0; y < ah; y += 2) {
        const DWORD* pa = a + static_cast<size_t>(y + oy) * w + ox;
        const DWORD* pb = b + static_cast<size_t>(y + oy) * w + ox;
        const BYTE* pm = masked ? s_mask.data() + static_cast<size_t>(y) * aw : nullptr;
        for (LONG x = 0; x < aw; x += 2)
            if ((!pm || pm[x]) && !Near(pa[x], pb[x])) ++diff;
    }
    return diff;
}

std::unique_ptr<Dib> s_maskShot;  // 照片里只留要盖的地方（其余全透明），盖着的就是它

void ReleaseMask() {
    s_mask.clear();
    s_maskShot.reset();
}

LRESULT CALLBACK CoverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void HideCover() {
    if (!s_coverShown) return;
    ShowWindow(s_cover, SW_HIDE);
    s_coverShown = false;
}

// 给桌面拍照（被窗口挡着也拍得到），只要工作区那一块。拍到的是全黑（没拍成）就不要
void Capture(HWND list) {
    HideCover();
    ReleaseShot();
    HWND root = GetAncestor(list, GA_ROOT);
    RECT rr;
    RECT area;
    if (!root || !GetWindowRect(root, &rr) || !IntersectRect(&area, &s_savedWork, &rr)) return;
    int w = rr.right - rr.left, h = rr.bottom - rr.top;
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bmp) {
        if (dc) DeleteDC(dc);
        return;
    }
    HGDIOBJ old = SelectObject(dc, bmp);
    double start = NowMs();
    bool ok = PrintWindow(root, dc, PW_RENDERFULLCONTENT) != FALSE;
    GdiFlush();
    if (ok) {
        ok = false;
        const DWORD* px = static_cast<const DWORD*>(bits);
        for (int y = area.top - rr.top; y < area.bottom - rr.top && !ok; y += 37)
            for (int x = area.left - rr.left; x < area.right - rr.left && !ok; x += 41)
                ok = (px[static_cast<size_t>(y) * w + x] & 0xFFFFFF) != 0;
    }
    if (!ok) {
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
        Log(L"没拍到桌面的照片，回到桌面时图标会晚一下出现");
        return;
    }
    s_shotDC = dc;
    s_shot = bmp;
    s_shotOld = old;
    s_shotBits = static_cast<const DWORD*>(bits);
    s_shotWindow = rr;
    s_shotArea = area;
    s_desktopRoot = root;
    Log(L"给桌面拍了照，用了 %.0f 毫秒", NowMs() - start);
}

// 把照片盖在桌面上（已经盖着的话重新贴到桌面窗口上面）。照片窗口紧贴在桌面窗口上面，别的窗口都在它上面，鼠标点得穿。
// 没有照片、没盖成时返回 false
bool ShowCover() {
    if (!s_shot || !IsWindow(s_desktopRoot)) return false;
    if (!s_cover) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = CoverProc;
        wc.hInstance = g_instance;
        wc.lpszClassName = kCoverClass;
        RegisterClassExW(&wc);
        s_cover = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kCoverClass, L"",
                                  WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_instance, nullptr);
        if (!s_cover) return false;
        BOOL disable = TRUE;  // 不要系统给弹出窗口加的淡入淡出
        DwmSetWindowAttribute(s_cover, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));
    }
    // 已经盖着就不再传一遍照片（十几 MB，正回到桌面时传会掉帧）。用整体透明度不透明的 ULW_ALPHA（不看逐像素透明度），
    // 撤掉时只改透明度就能淡出
    if (!s_coverShown) {
        POINT dst = {s_shotArea.left, s_shotArea.top};
        SIZE size = {s_shotArea.right - s_shotArea.left, s_shotArea.bottom - s_shotArea.top};
        POINT src = {s_shotArea.left - s_shotWindow.left, s_shotArea.top - s_shotWindow.top};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, 0};
        HDC from = s_shotDC;
        if (s_maskShot) {  // 只盖图标的：逐像素透明
            from = s_maskShot->dc;
            src = {0, 0};
            blend.AlphaFormat = AC_SRC_ALPHA;
        }
        if (!UpdateLayeredWindow(s_cover, nullptr, &dst, &size, from, &src, 0, &blend, ULW_ALPHA)) return false;
    }
    HWND above = GetWindow(s_desktopRoot, GW_HWNDPREV);  // 紧挨在桌面窗口上面的那个，照片放在它下面
    if (above == s_cover) above = GetWindow(s_cover, GW_HWNDPREV);
    SetWindowPos(s_cover, above ? above : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (!s_coverShown) s_coverAt = NowMs();
    s_coverShown = true;
    return true;
}

// 淡出照片再藏起来：照片和真桌面稍有不同（颜色、文字阴影）也不会一下跳变
void FadeOutCover() {
    if (!s_coverShown) return;
    constexpr double kFadeMs = 150;
    for (double start = NowMs(), t = 0; t < kFadeMs; t = NowMs() - start) {
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, static_cast<BYTE>(255 * (1 - t / kFadeMs)),
                               static_cast<BYTE>(s_maskShot ? AC_SRC_ALPHA : 0)};
        if (!UpdateLayeredWindow(s_cover, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &blend, ULW_ALPHA)) break;
        WaitForVBlank();
    }
    HideCover();
}

// 要藏这个图标窗口：只有一块屏幕，用户也没关“显示桌面图标”
bool WillHide(HWND list) { return list && GetSystemMetrics(SM_CMONITORS) == 1 && IsWindowVisible(list); }

// 藏起记位置时找到的桌面图标窗口。照片在记位置时已经拍好了
void HideIcons() {
    HWND list = s_savedList;
    if (s_quit || s_hiddenList || !IsWindow(list) || !WillHide(list)) return;
    SaveHidden(true);  // 先记下再藏，藏的这一刻被强行结束也能找回来
    s_hiddenRect = s_savedListRect;
    s_hiddenNow = list;
    ShowWindow(list, SW_HIDE);
    if (s_quit) ShowWindowAsync(list, SW_SHOWNA);  // 藏的同时主线程不等了：它可能没看到这次
    s_hiddenList = list;
    s_hiddenAt = NowMs();
    Log(L"藏起桌面图标，任务栏回来、图标排回原位以后再显示");
}

// 打开自动隐藏之前：有照片的话先把照片盖上、等它上屏再藏图标（这时最大化动画可能还露着一部分桌面，先藏会看到图标闪没）。
// 图标还藏着（上一轮没显示回来）的话照片重新贴一下
void CoverAndHide() {
    if (s_hiddenList) {
        ShowCover();
        return;
    }
    if (!IsWindow(s_savedList) || !WillHide(s_savedList)) return;
    if (ShowCover()) WaitForVBlank();
    HideIcons();
    if (!s_hiddenList) HideCover();
}

// 上次关掉以后还没摆完又要打开自动隐藏（kCmdRehide），打开之前做：图标还藏着的话照片也接着用；已经显示回来的话，
// 图标都在记下的位置、图标窗口和工作区也和记位置时一样才重新拍照，不然不拍（回桌面时图标晚一下出现，不会盖一张不对的照片）
void Reshoot(UINT session) {
    if (s_hiddenList) return;
    ReleaseShot();
    HWND list = s_savedList;
    if (!IsWindow(list) || !WillHide(list)) return;
    RECT now;
    MONITORINFO mi = {sizeof(mi)};
    if (!GetWindowRect(list, &now) || !EqualRect(&now, &s_savedListRect) ||
        !GetMonitorInfoW(MonitorFromWindow(list, MONITOR_DEFAULTTONEAREST), &mi) || !EqualRect(&mi.rcWork, &s_savedWork) ||
        !AtSaved(Read())) {
        Log(L"桌面图标还没排回原位，这次不拍照");
        return;
    }
    Capture(list);
    // 等的一方已经不等了：自动隐藏可能已经打开，照片也许拍到了挪动中的图标
    if (Discarded(session) && s_shot) {
        ReleaseShot();
        Log(L"拍照太慢，照片不用");
    }
}

// 图标窗口刚显示出来：等资源管理器把它画完（最多 300 毫秒，不会卡住），再等两帧让 DWM 合成上屏
void WaitPainted(HWND list) {
    for (double start = NowMs(); IsWindow(list) && GetUpdateRect(list, nullptr, FALSE) && NowMs() - start < 300;) Pause(5);
    DWORD_PTR result;
    SendMessageTimeoutW(list, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 200, &result);  // 处理到它说明画图的消息已经处理完了
    WaitForVBlank();
    WaitForVBlank();
}

// 图标刚藏起来、照片整张盖着（被全屏窗口挡着，看不见）：再给桌面拍一张，这张只有壁纸。和照片比，不一样的地方
// （只看图标所在的格子）就是图标和文字，向外多留两个点；照片改成只盖这些地方，其余透明。动态壁纸、幻灯片壁纸照常露着动，
// 换回真图标时也只换图标那一块，不会看到壁纸跳一下
void MaskCover() {
    if (!s_shotBits || !s_coverShown || !s_hiddenList || s_maskShot || !IsWindow(s_desktopRoot)) return;
    RECT rr;
    if (!GetWindowRect(s_desktopRoot, &rr) || !EqualRect(&rr, &s_shotWindow)) return;
    int w = rr.right - rr.left, h = rr.bottom - rr.top;
    LONG aw = s_shotArea.right - s_shotArea.left, ah = s_shotArea.bottom - s_shotArea.top;
    LONG ox = s_shotArea.left - s_shotWindow.left, oy = s_shotArea.top - s_shotWindow.top;
    double start = NowMs();
    WaitForVBlank();  // 图标窗口藏起来要等一帧才从画面上消失
    Dib bare;
    if (!bare.Create(w, h) || !PrintWindow(s_desktopRoot, bare.dc, PW_RENDERFULLCONTENT)) return;
    GdiFlush();

    // 图标所在的格子（屏幕坐标转成 s_shotArea 里的坐标）。读不到间距时整块都算
    std::vector<BYTE> cell(static_cast<size_t>(aw) * ah, 0);
    POINT spacing = {};
    Ref<IFolderView> view;
    bool cells = !s_icons.empty() && DesktopView(view) && SUCCEEDED(view->GetSpacing(&spacing)) && spacing.x > 0 &&
                 spacing.y > 0;
    if (cells) {
        for (const IconPos& icon : s_icons) {
            POINT pt = icon.pt;
            MapWindowPoints(s_hiddenList, nullptr, &pt, 1);
            LONG l = std::max<LONG>(0, pt.x - 8 - s_shotArea.left), t = std::max<LONG>(0, pt.y - 8 - s_shotArea.top);
            LONG r = std::min<LONG>(aw, pt.x + spacing.x + 8 - s_shotArea.left);
            LONG b = std::min<LONG>(ah, pt.y + spacing.y + 8 - s_shotArea.top);
            for (LONG y = t; y < b; ++y) std::fill(cell.begin() + y * aw + l, cell.begin() + y * aw + std::max(l, r), 1);
        }
    } else {
        std::fill(cell.begin(), cell.end(), 1);
    }
    // 不一样的点，再向外扩两个点（先横着扩、再竖着扩）
    std::vector<BYTE> diff(cell.size(), 0), wide(cell.size(), 0);
    for (LONG y = 0; y < ah; ++y) {
        const DWORD* p = s_shotBits + static_cast<size_t>(y + oy) * w + ox;
        const DWORD* q = bare.bits + static_cast<size_t>(y + oy) * w + ox;
        for (LONG x = 0; x < aw; ++x) {
            size_t i = static_cast<size_t>(y) * aw + x;
            if (cell[i] && !Near(p[x], q[x])) diff[i] = 1;
        }
    }
    constexpr LONG kGrow = 2;
    for (LONG y = 0; y < ah; ++y)
        for (LONG x = 0; x < aw; ++x)
            if (diff[static_cast<size_t>(y) * aw + x])
                for (LONG k = std::max<LONG>(0, x - kGrow); k <= std::min<LONG>(aw - 1, x + kGrow); ++k)
                    wide[static_cast<size_t>(y) * aw + k] = 1;
    std::fill(diff.begin(), diff.end(), 0);
    long covered = 0;
    for (LONG y = 0; y < ah; ++y)
        for (LONG x = 0; x < aw; ++x)
            if (wide[static_cast<size_t>(y) * aw + x])
                for (LONG k = std::max<LONG>(0, y - kGrow); k <= std::min<LONG>(ah - 1, y + kGrow); ++k) {
                    size_t i = static_cast<size_t>(k) * aw + x;
                    if (!diff[i]) ++covered;
                    diff[i] = 1;
                }
    if (!covered) {  // 没拍出图标（比如桌面上没有图标）：照片没用了
        Log(L"藏起图标后的桌面和照片一样，不盖照片");
        HideCover();
        ReleaseShot();
        return;
    }

    auto masked = std::make_unique<Dib>();
    if (!masked->Create(aw, ah)) return;
    for (LONG y = 0; y < ah; ++y) {
        const DWORD* p = s_shotBits + static_cast<size_t>(y + oy) * w + ox;
        DWORD* d = masked->bits + static_cast<size_t>(y) * aw;
        const BYTE* m = diff.data() + static_cast<size_t>(y) * aw;
        for (LONG x = 0; x < aw; ++x) d[x] = m[x] ? (p[x] | 0xFF000000) : 0;  // 只有全透明和不透明，不用预乘
    }
    GdiFlush();
    POINT dst = {s_shotArea.left, s_shotArea.top}, src = {0, 0};
    SIZE size = {aw, ah};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    if (!UpdateLayeredWindow(s_cover, nullptr, &dst, &size, masked->dc, &src, 0, &blend, ULW_ALPHA)) return;
    s_mask = std::move(diff);
    s_maskShot = std::move(masked);
    Log(L"照片改成只盖图标（%.1f%% 的地方%ls），用了 %.0f 毫秒", 100.0 * covered / (static_cast<double>(aw) * ah),
        cells ? L"" : L"，没读到图标格子", NowMs() - start);
}

// 照片盖住的地方一共比多少个点（和 CountDiff 一样隔一个点比一个）
long CountSamples() {
    LONG aw = s_shotArea.right - s_shotArea.left, ah = s_shotArea.bottom - s_shotArea.top;
    bool masked = s_mask.size() == static_cast<size_t>(aw) * ah;
    long n = 0;
    for (LONG y = 0; y < ah; y += 2)
        for (LONG x = 0; x < aw; x += 2)
            if (!masked || s_mask[static_cast<size_t>(y) * aw + x]) ++n;
    return n;
}

// 图标显示出来以后（照片还盖着）：隔一会儿给桌面再拍一张，和照片一样了才撤照片。图标是一个个画出来的（图标、缩略图、
// 小箭头可能晚一点），没画完就撤照片看上去就是一闪。资源管理器有时显示出图标窗口好几百毫秒都不画，这时桌面不变但和照片
// 差得远，不能当成画好了：差得不多（一成以内，比如鼠标停在某个图标上有高亮、图标边上的动态壁纸在动）并且不再变好才算，
// 一直不画就让它重画一次。最多等 ms 毫秒。trace 记下拍到的和照片差多少（百分比，变了才记），返回还有多少个点不一样，拍不到返回 -1
long WaitLikeShot(HWND list, DWORD ms, std::wstring& trace) {
    if (!s_shotBits || !IsWindow(s_desktopRoot)) return -1;
    RECT rr;
    if (!GetWindowRect(s_desktopRoot, &rr) || !EqualRect(&rr, &s_shotWindow)) return -1;
    int w = rr.right - rr.left, h = rr.bottom - rr.top;
    long tiny = (s_shotArea.right - s_shotArea.left) * (s_shotArea.bottom - s_shotArea.top) / 4 / 2000;
    long total = std::max(1L, CountSamples());
    Dib now;
    long diff = -1, last = -1, shownPct = -1;
    bool nudged = false;
    for (double start = NowMs();;) {
        if (!now.Create(w, h) || !PrintWindow(s_desktopRoot, now.dc, PW_RENDERFULLCONTENT)) return diff;
        GdiFlush();
        diff = CountDiff(now.bits, s_shotBits, w);
        long pct = (diff * 100 + total - 1) / total;
        if (pct != shownPct) {  // 一样的不重复记
            wchar_t step[24];
            swprintf(step, 24, trace.empty() ? L"%ld%%" : L"→%ld%%", pct);
            trace += step;
            shownPct = pct;
        }
        if (diff <= tiny) return diff;
        bool stuck = last >= 0 && diff >= last - total / 200;  // 和上一张比没怎么变好
        if (stuck && diff * 10 <= total) return diff;
        double elapsed = NowMs() - start;
        if (elapsed >= ms) return diff;
        if (stuck && !nudged && elapsed >= 150 && IsWindow(list)) {
            RedrawWindow(list, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            trace += L"（让它重画）";
            shownPct = -1;
            nudged = true;
        }
        last = diff;
        Pause(25);
    }
}

// 诊断日志开着时：照片撤掉以后，把屏幕上露着的桌面和照片比一比，两张图存到设置文件夹里（desktop-photo.png、
// desktop-screen.png），看照片和真桌面哪里不一样
void DiagnoseShot() {
    if (!g_settings.debugLog || !s_shotBits) return;
    int w = s_shotArea.right - s_shotArea.left, h = s_shotArea.bottom - s_shotArea.top;
    int sw = s_shotWindow.right - s_shotWindow.left;
    const DWORD* photo = s_shotBits + static_cast<size_t>(s_shotArea.top - s_shotWindow.top) * sw +
                         (s_shotArea.left - s_shotWindow.left);
    Dib screen;
    if (!screen.Create(w, h)) return;
    HDC sdc = GetDC(nullptr);
    BitBlt(screen.dc, 0, 0, w, h, sdc, s_shotArea.left, s_shotArea.top, SRCCOPY);
    ReleaseDC(nullptr, sdc);
    GdiFlush();
    long seen = 0, bad = 0;
    double sum = 0;
    for (int y = 0; y < h; y += 16)
        for (int x = 0; x < w; x += 16) {
            POINT pt = {s_shotArea.left + x, s_shotArea.top + y};
            if (GetAncestor(WindowFromPoint(pt), GA_ROOT) != s_desktopRoot) continue;  // 被别的窗口挡着
            if (!s_mask.empty() && !s_mask[static_cast<size_t>(y) * w + x]) continue;    // 照片没盖这里
            DWORD a = screen.bits[static_cast<size_t>(y) * w + x], b = photo[static_cast<size_t>(y) * sw + x];
            ++seen;
            if (!Near(a, b)) ++bad;
            for (int shift = 0; shift < 24; shift += 8)
                sum += std::abs(static_cast<int>((a >> shift) & 0xFF) - static_cast<int>((b >> shift) & 0xFF));
        }
    MSG pending;  // 又要开始下一轮了（存图要几百毫秒）：不存了
    bool busy = PeekMessageW(&pending, reinterpret_cast<HWND>(-1), 0, 0, PM_NOREMOVE) != FALSE;
    Log(L"诊断：屏幕上露着的桌面（照片盖着的地方）取了 %ld 个点，和照片不一样的 %ld 个，平均每个颜色分量差 %.2f", seen, bad,
        seen ? sum / seen / 3 : 0.0);
    if (busy) return;
    CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    Gdiplus::Bitmap a(w, h, sw * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(const_cast<DWORD*>(photo)));
    Gdiplus::Bitmap b(w, h, w * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(screen.bits));
    std::wstring dir = SettingsDir() + L"\\";
    a.Save((dir + L"desktop-photo.png").c_str(), &png, nullptr);
    b.Save((dir + L"desktop-screen.png").c_str(), &png, nullptr);
}

// 显示图标；盖着照片的话等资源管理器把图标画出来、和照片对上了，再淡出照片
void ShowIcons() {
    if (!s_hiddenList) return;
    HWND list = s_hiddenList;
    if (IsWindow(list)) ShowWindow(list, SW_SHOWNA);
    s_hiddenList = nullptr;
    s_hiddenNow = nullptr;
    SaveHidden(false);
    if (s_coverShown) {
        double start = NowMs();
        WaitPainted(list);
        std::wstring trace;
        long diff = WaitLikeShot(list, s_maskShot ? 1500 : 600, trace);
        WaitForVBlank();  // 拍到的是画好的，等它上屏
        WaitForVBlank();
        double painted = NowMs() - start;
        FadeOutCover();
        Log(L"显示桌面图标，等图标画好、和照片对上用了 %.0f 毫秒（和照片差 %ls，还差 %ld 个点），淡出照片（照片盖了 %.0f 毫秒）",
            painted, trace.c_str(), diff, NowMs() - s_coverAt);
        DiagnoseShot();
    } else {
        Log(L"显示桌面图标");
    }
    ReleaseShot();  // 下一轮重新拍
}

// 上次藏起、没来得及显示回来的
void Recover() {
    if (s_hiddenList || !SavedHidden()) return;
    Ref<IFolderView> view;
    if (!DesktopView(view)) return;  // 资源管理器没在运行：记号留着，它起来以后再试
    HWND list = IconList(view.p);
    if (list && !IsWindowVisible(list)) {
        ShowWindow(list, SW_SHOWNA);
        Log(L"上次藏起的桌面图标没显示回来，现在显示");
    }
    SaveHidden(false);
}

void Clear() {
    for (IconPos& icon : s_icons) CoTaskMemFree(icon.pidl);
    s_icons.clear();
    s_layout.clear();
    s_mark.clear();
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

void Save(UINT session) {
    Clear();
    s_savedList = nullptr;
    double start = NowMs();
    Recover();
    Ref<IFolderView> view;
    if (!DesktopView(view)) {
        Log(L"找不到桌面，不记图标位置");
    } else {
        // 自动排列时图标的位置由顺序决定，工作区变回来以后自己就排回去了，不用摆；位置照样记下，用来判断排好了没有
        if (view->GetAutoArrange() == S_OK) Log(L"桌面图标是自动排列的，不用摆回去");
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
        SetRectEmpty(&s_savedListRect);
        SetRectEmpty(&s_savedWork);
        s_savedList = IconList(view.p);
        if (s_savedList) {
            MONITORINFO mi = {sizeof(mi)};
            GetWindowRect(s_savedList, &s_savedListRect);
            if (GetMonitorInfoW(MonitorFromWindow(s_savedList, MONITOR_DEFAULTTONEAREST), &mi)) s_savedWork = mi.rcWork;
            // 照片也要在打开自动隐藏之前拍：一打开资源管理器就开始挪图标，这时再找桌面、拍照都要排队等它，拍到的已经挪了
            if (WillHide(s_savedList)) Capture(s_savedList);
        } else if (GetSystemMetrics(SM_CMONITORS) == 1) {
            Log(L"找不到放桌面图标的窗口，不藏图标");
        }
    }
    // 等的一方已经不等了：自动隐藏可能已经打开，读到的也许是重新排过的位置，照片也许拍到了挪动中的图标
    if (Discarded(session)) {
        Log(L"记图标位置太慢，这次不摆回去");
        Clear();
        ReleaseShot();
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
        Pause(50);
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
// 期间用户在桌面上按过鼠标（pressed）的话，只摆从按下之前到现在没动过的：动过的可能是用户自己拖的
bool RestoreWhenSettled(UINT session, bool& pressed) {
    s_layout.clear();
    if (s_icons.empty()) return true;
    std::vector<POINT> last = Read();
    std::vector<POINT> beforePress = last;  // 第一次按下之前最后读到的（资源管理器那时已经排过的也在里面）
    int stable = 0;
    for (DWORD start = GetTickCount(); stable < 2 && GetTickCount() - start < 2000;) {
        if (!Wait(150, session, &pressed)) return false;
        std::vector<POINT> now = Read();
        stable = Same(now, last) && !DesktopPressed() ? stable + 1 : 0;
        if (!pressed) beforePress = now;
        last = std::move(now);
    }
    if (!WaitReleased(session, pressed)) return false;
    if (pressed) {
        last = Read();  // 松开以后的位置
        // 和按下之前不一样的可能是用户刚拖过的：第二遍别把它当成“还停在资源管理器排的位置上”
        for (size_t i = 0; i < last.size(); ++i)
            if (i >= beforePress.size() || last[i].x != beforePress[i].x || last[i].y != beforePress[i].y)
                last[i] = {LONG_MIN, LONG_MIN};
    }
    s_layout = std::move(last);
    if (pressed) Log(L"用户在桌面上按过鼠标，只摆没被拖过的图标");
    Log(L"桌面图标摆回原位 %d 个", Restore(pressed ? &beforePress : nullptr));
    return true;
}

// 现在的位置都是记下的位置（删掉的图标不算）
bool AtSaved(const std::vector<POINT>& now) {
    if (now.size() != s_icons.size()) return false;
    for (size_t i = 0; i < now.size(); ++i)
        if (now[i].x != LONG_MIN && (now[i].x != s_icons[i].pt.x || now[i].y != s_icons[i].pt.y)) return false;
    return true;
}

// 图标窗口的位置大小和藏起时一样了
bool ListBack() {
    RECT r;
    return !IsWindow(s_hiddenList) || (GetWindowRect(s_hiddenList, &r) && EqualRect(&r, &s_hiddenRect));
}

// 自动隐藏刚关掉，资源管理器正按原来的工作区把图标挪回去：图标窗口变回原样，而且图标都回到记下的位置、
// 或者挪完不再动了（用户挪过的图标不会回到记下的位置），就把图标显示出来，最多等 1.5 秒。又打开了自动隐藏的话接着藏着
void RevealWhenBack(UINT session) {
    if (!s_hiddenList) return;
    ShowCover();  // 万一前面没盖上
    double start = NowMs();
    std::vector<POINT> first = Read();
    std::vector<POINT> last = first;
    bool changed = false;
    int stable = 0;
    for (DWORD start = GetTickCount(); GetTickCount() - start < 1500;) {
        if (ListBack() && (AtSaved(last) || stable >= 2)) break;
        Pause(15);
        if (!Current(session)) return;
        std::vector<POINT> now = Read();
        if (!Same(now, first)) changed = true;
        stable = changed && Same(now, last) ? stable + 1 : 0;
        last = std::move(now);
    }
    if (!Current(session)) return;  // 又打开了自动隐藏：图标接着藏着
    Log(L"关掉自动隐藏后 %.0f 毫秒图标排回原位", NowMs() - start);
    ShowIcons();
}

// 这一轮用完了：会话没变（没有又打开自动隐藏）就丢掉记下的位置，下次打开前重新记
void Done(UINT session) {
    {
        std::lock_guard<std::mutex> guard(s_lock);
        if (session != s_session) return;
        Clear();
        s_snapshot = false;
    }
}

DWORD WINAPI ThreadProc(LPVOID ready) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // 建好消息队列以后才能收 PostThreadMessage
    SetEvent(static_cast<HANDLE>(ready));
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.hwnd) {  // 照片窗口的消息
            DispatchMessageW(&msg);
            continue;
        }
        if (s_quit) continue;
        switch (msg.message) {
            case kCmdSave: {
                Save(static_cast<UINT>(msg.wParam));
                // 照片现在就垫上：回到桌面时不管多快都不会先露出没有图标的桌面。藏好了再让动画线程去开自动隐藏，
                // 免得资源管理器在图标还露着时就挪它们。已经要退出（会话变了）就不藏
                if (Current(static_cast<UINT>(msg.wParam))) CoverAndHide();
                SetEvent(s_savedEvent);
                MaskCover();  // 自动隐藏可以去开了，这时图标已经藏好、照片盖着，再把照片改成只盖图标
                break;
            }
            case kCmdRehide:
                Reshoot(static_cast<UINT>(msg.wParam));
                if (Current(static_cast<UINT>(msg.wParam))) CoverAndHide();
                SetEvent(s_savedEvent);
                MaskCover();
                break;
            case kCmdDisplay:
                if (s_hiddenList && GetSystemMetrics(SM_CMONITORS) > 1) {
                    Log(L"接上了另一块屏幕，藏着的桌面图标现在显示");
                    ShowIcons();  // 位置照常等关掉自动隐藏以后摆
                }
                break;
            case kCmdCover:
                if (Current(static_cast<UINT>(msg.wParam)) && s_hiddenList) ShowCover();
                break;
            case kCmdRestore: {
                UINT session = static_cast<UINT>(msg.wParam);
                // 资源管理器有时过一会儿又排一次，1 秒后再查一遍。
                // 这期间用户在桌面上按过鼠标的话，只摆还停在资源管理器排的位置上的，别的可能是用户拖过的
                bool pressed = false;
                if (!RestoreWhenSettled(session, pressed) || !Current(session)) {
                    Done(session);  // 又打开了自动隐藏：图标接着藏着
                    break;
                }
                ShowIcons();
                if (Wait(1000, session, &pressed) && WaitReleased(session, pressed))
                    if (int moved = Restore(pressed ? &s_layout : nullptr)) Log(L"又摆回 %d 个桌面图标", moved);
                // 用户一直按着没摆完也丢掉，免得下次拿旧位置把用户拖过的图标挪回去。又打开了自动隐藏（会话变了）时留着接着用
                Done(session);
                break;
            }
            case kCmdMark:
                s_mark.clear();
                if (Current(static_cast<UINT>(msg.wParam)) && !s_icons.empty()) s_mark = Read();
                break;
            case kCmdUpdate: {
                UINT number = static_cast<UINT>(msg.lParam);
                if (Current(static_cast<UINT>(msg.wParam)) && !s_mark.empty()) {
                    std::vector<POINT> now = Read();
                    // 等的一方已经不等了：自动隐藏可能正在关，读到的也许是资源管理器重新排的位置
                    if (number > s_updateLate && now.size() == s_mark.size()) {
                        int moved = 0;
                        for (size_t i = 0; i < now.size(); ++i) {
                            const POINT& a = s_mark[i];
                            const POINT& b = now[i];
                            if (a.x == LONG_MIN || b.x == LONG_MIN || (a.x == b.x && a.y == b.y)) continue;
                            s_icons[i].pt = b;
                            ++moved;
                        }
                        if (moved) Log(L"自动隐藏期间用户挪过 %d 个桌面图标，按新位置记", moved);
                    }
                }
                s_mark.clear();
                std::lock_guard<std::mutex> guard(s_lock);
                if (number == s_updatePosted) SetEvent(s_updatedEvent);  // 后面还排着一次的话等它
                break;
            }
            case kCmdFinish: {
                UINT session = static_cast<UINT>(msg.wParam);
                bool pressed = false;
                RestoreWhenSettled(session, pressed);
                ShowIcons();
                Done(session);
                break;
            }
            case kCmdReveal:
                RevealWhenBack(static_cast<UINT>(msg.wParam));
                break;
            case kCmdRecover:
                Recover();
                break;
        }
    }
    if (!s_quit) ShowIcons();
    else if (IsWindow(s_hiddenList)) ShowWindowAsync(s_hiddenList, SW_SHOWNA);  // 主线程不等了：直接显示，不淡出
    HideCover();
    if (s_cover) DestroyWindow(s_cover);
    s_cover = nullptr;
    ReleaseShot();
    Clear();
    CoUninitialize();
    return 0;
}

bool EnsureThread() {
    if (s_thread) return true;
    if (!s_savedEvent) s_savedEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!s_updatedEvent) s_updatedEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s_savedEvent || !s_updatedEvent || !ready) {
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
    // 上次关掉以后还没摆完：记下的还是打开之前的样子，接着用；图标可能已经显示回来了，要重新藏起
    UINT cmd = s_snapshot ? kCmdRehide : kCmdSave;
    if (!EnsureThread()) return;
    s_snapshot = true;
    ResetEvent(s_savedEvent);
    if (!PostThreadMessageW(s_threadId, cmd, s_session, 0)) {
        if (cmd == kCmdSave) s_snapshot = false;
        SetEvent(s_savedEvent);
    }
}

void DesktopIcons_WaitSaved(DWORD ms) {
    if (!s_savedEvent) return;
    if (WaitForSingleObject(s_savedEvent, ms) != WAIT_TIMEOUT) return;
    std::lock_guard<std::mutex> guard(s_lock);
    s_discardFrom = s_session;
}

bool DesktopIcons_Mark() {
    std::lock_guard<std::mutex> guard(s_lock);
    return s_thread && s_snapshot && PostThreadMessageW(s_threadId, kCmdMark, s_session, 0);
}

void DesktopIcons_UpdateMoved() {
    std::lock_guard<std::mutex> guard(s_lock);
    if (!s_thread || !s_snapshot || !s_updatedEvent) return;
    UINT number = ++s_updatePosted;
    ResetEvent(s_updatedEvent);
    if (!PostThreadMessageW(s_threadId, kCmdUpdate, s_session, number)) SetEvent(s_updatedEvent);
}

void DesktopIcons_WaitUpdated(DWORD ms) {
    if (s_updatedEvent) WaitForSingleObject(s_updatedEvent, ms);
}

void DesktopIcons_AbandonUpdates() {
    std::lock_guard<std::mutex> guard(s_lock);
    s_updateLate = s_updatePosted;
}

void DesktopIcons_RestoreLater() {
    std::lock_guard<std::mutex> guard(s_lock);
    if (s_thread && s_snapshot) PostThreadMessageW(s_threadId, kCmdRestore, s_session, 0);
}

void DesktopIcons_Cover() {
    std::lock_guard<std::mutex> guard(s_lock);
    if (s_thread && s_snapshot) PostThreadMessageW(s_threadId, kCmdCover, s_session, 0);
}

void DesktopIcons_RevealLater() {
    std::lock_guard<std::mutex> guard(s_lock);
    if (s_thread && s_snapshot) PostThreadMessageW(s_threadId, kCmdReveal, s_session, 0);
}

void DesktopIcons_Recover() {
    if (!SavedHidden()) return;
    std::lock_guard<std::mutex> guard(s_lock);
    if (EnsureThread()) PostThreadMessageW(s_threadId, kCmdRecover, 0, 0);
}

void DesktopIcons_DisplayChanged() {
    std::lock_guard<std::mutex> guard(s_lock);
    if (s_thread) PostThreadMessageW(s_threadId, kCmdDisplay, 0, 0);
}

void DesktopIcons_EmergencyShow() {
    if (HWND list = s_hiddenNow.exchange(nullptr)) ShowWindowAsync(list, SW_SHOWNA);  // 记号留着，下次启动时再看一眼
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
        DesktopIcons_EmergencyShow();  // 图标还藏着的话先显示出来，不摆位置了
        Log(L"摆桌面图标超时，不等了");
    } else {
        s_quit = false;
    }
    CloseHandle(s_thread);
    s_thread = nullptr;
    s_threadId = 0;
    if (s_savedEvent) SetEvent(s_savedEvent);
    if (s_updatedEvent) SetEvent(s_updatedEvent);
}

}  // namespace app
