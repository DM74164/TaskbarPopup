// 迷你任务栏里的系统托盘。
//
// Win11 的托盘区是资源管理器自己画的，没有公开接口能拿到别的程序的托盘图标。这里：
//   - 图标和名字：系统给每个托盘图标记在 HKCU\Control Panel\NotifyIconSettings 里（程序路径、
//     最初的提示文字、图标快照 PNG），只列出程序正在运行的
//   - 点击：先按 Win+B 让任务栏的通知区域拿到焦点（任务栏藏着时会跟着显示出来），
//     再用 UI 自动化在任务栏里找到对应的按钮，在它上面模拟一次真的鼠标左键 / 右键单击，
//     这样程序自己的窗口和右键菜单都照常弹出来。在任务栏上找不到（收在“隐藏的图标”里）就先展开再找；
//     还找不到的话，左键改成再启动一次这个程序（多数托盘程序会把已经开着的窗口调出来）
//   - 系统按钮：快速设置（Win+A）、通知中心和日历（Win+N）、展开隐藏的图标
#include "common.h"

#include <tlhelp32.h>
#include <uiautomation.h>

#include <cwctype>
#include <set>

namespace app {
namespace {

// 自己定义一份，免得依赖头文件里 GUID 的链接方式
const CLSID kClsidUIAutomation = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
const IID kIidUIAutomation = {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};

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
};

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// REG_EXPAND_SZ 也会展开成 REG_SZ 返回（同时写 RRF_RT_REG_EXPAND_SZ 反而会失败）
std::wstring RegString(HKEY key, const wchar_t* name) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return L"";
    return buf;
}

// "{已知文件夹 GUID}\相对路径" → 完整路径
std::wstring ResolvePath(const std::wstring& path) {
    if (path.size() < 40 || path[0] != L'{') return path;
    size_t close = path.find(L'}');
    if (close == std::wstring::npos) return path;
    GUID id;
    if (FAILED(CLSIDFromString(path.substr(0, close + 1).c_str(), &id))) return path;
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &folder))) return path;
    std::wstring full = std::wstring(folder) + path.substr(close + 1);
    CoTaskMemFree(folder);
    return full;
}

std::set<std::wstring> RunningExes() {
    std::set<std::wstring> exes;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return exes;
    PROCESSENTRY32W pe = {sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t path[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &size)) exes.insert(Lower(path));
        CloseHandle(h);
    }
    CloseHandle(snap);
    return exes;
}

// PNG 快照 → 自己持有像素的位图（从流解出来的位图要一直拿着流）
std::shared_ptr<Gdiplus::Bitmap> BitmapFromPng(const std::vector<BYTE>& png, int px) {
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, png.size());
    if (!mem) return nullptr;
    memcpy(GlobalLock(mem), png.data(), png.size());
    GlobalUnlock(mem);
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) {
        GlobalFree(mem);
        return nullptr;
    }
    std::shared_ptr<Gdiplus::Bitmap> result;
    {
        Gdiplus::Bitmap decoded(stream);
        if (decoded.GetLastStatus() == Gdiplus::Ok && decoded.GetWidth() > 0) {
            result = std::make_shared<Gdiplus::Bitmap>(px, px, PixelFormat32bppPARGB);
            Gdiplus::Graphics g(result.get());
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.DrawImage(&decoded, 0, 0, px, px);
        }
    }
    stream->Release();
    return result;
}

std::shared_ptr<Gdiplus::Bitmap> ExeIcon(const std::wstring& exe) {
    SHFILEINFOW info = {};
    if (!SHGetFileInfoW(exe.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_SMALLICON) || !info.hIcon)
        return nullptr;
    auto bmp = BitmapFromIcon(info.hIcon);
    DestroyIcon(info.hIcon);
    return bmp;
}

// 任务栏按钮的名字是程序现在的提示文字，和记下的最初提示文字、程序名比，越像分越高
int MatchScore(const std::wstring& buttonName, const TrayApp& app) {
    std::wstring name = Lower(buttonName), tip = Lower(app.name);
    if (name.empty()) return 0;
    if (!tip.empty()) {
        if (name == tip) return 100;
        if (tip.size() >= 2 && (name.compare(0, tip.size(), tip) == 0 || tip.compare(0, name.size(), name) == 0))
            return 80;
        if (tip.size() >= 2 && name.find(tip) != std::wstring::npos) return 60;
    }
    std::wstring stem = Lower(app.exe);
    size_t slash = stem.find_last_of(L"\\/");
    if (slash != std::wstring::npos) stem = stem.substr(slash + 1);
    if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, L".exe") == 0) stem.resize(stem.size() - 4);
    if (stem.size() >= 2 && name.find(stem) != std::wstring::npos) return 40;
    return 0;
}

struct Button {
    std::wstring name, cls, id;
    RECT rect;
};

// 窗口里所有按钮（UI 自动化）
std::vector<Button> ButtonsIn(HWND hwnd) {
    std::vector<Button> out;
    if (!hwnd) return out;
    Ref<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(kClsidUIAutomation, nullptr, CLSCTX_INPROC_SERVER, kIidUIAutomation,
                                reinterpret_cast<void**>(&uia.p))))
        return out;
    Ref<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandle(hwnd, &root.p)) || !root.p) return out;
    VARIANT type;
    VariantInit(&type);
    type.vt = VT_I4;
    type.lVal = UIA_ButtonControlTypeId;
    Ref<IUIAutomationCondition> cond;
    if (FAILED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &cond.p))) return out;
    Ref<IUIAutomationElementArray> all;
    if (FAILED(root->FindAll(TreeScope_Descendants, cond.p, &all.p)) || !all.p) return out;
    int n = 0;
    all->get_Length(&n);
    for (int i = 0; i < n; ++i) {
        Ref<IUIAutomationElement> e;
        if (FAILED(all->GetElement(i, &e.p)) || !e.p) continue;
        Button b = {};
        BSTR s = nullptr;
        if (SUCCEEDED(e->get_CurrentName(&s)) && s) b.name = s;
        SysFreeString(s);
        s = nullptr;
        if (SUCCEEDED(e->get_CurrentClassName(&s)) && s) b.cls = s;
        SysFreeString(s);
        s = nullptr;
        if (SUCCEEDED(e->get_CurrentAutomationId(&s)) && s) b.id = s;
        SysFreeString(s);
        e->get_CurrentBoundingRectangle(&b.rect);
        out.push_back(b);
    }
    return out;
}

bool OnScreen(const RECT& r) {
    if (r.right - r.left < 4 || r.bottom - r.top < 4) return false;
    POINT c = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    return MonitorFromPoint(c, MONITOR_DEFAULTTONULL) != nullptr;
}

bool IsChevron(const Button& b) {
    std::wstring all = Lower(b.cls + L" " + b.id + L" " + b.name);
    return all.find(L"chevron") != std::wstring::npos || all.find(L"隐藏的图标") != std::wstring::npos ||
           all.find(L"hidden icons") != std::wstring::npos;
}

const Button* BestMatch(const std::vector<Button>& buttons, const TrayApp& app) {
    const Button* best = nullptr;
    int bestScore = 0;
    for (const Button& b : buttons) {
        if (IsChevron(b) || !OnScreen(b.rect)) continue;
        int score = MatchScore(b.name, app);
        if (score > bestScore) {
            best = &b;
            bestScore = score;
        }
    }
    return best;
}

POINT s_savedCursor = {};

// 在 rect 中间模拟一次鼠标单击，过一会儿（Step 里）把鼠标挪回原处
void ClickAt(const RECT& rect, bool right) {
    GetCursorPos(&s_savedCursor);
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1), vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
    LONG x = (rect.left + rect.right) / 2, y = (rect.top + rect.bottom) / 2;
    INPUT in[3] = {};
    for (INPUT& i : in) {
        i.type = INPUT_MOUSE;
        i.mi.dx = static_cast<LONG>((x - vx) * 65535LL / vw);
        i.mi.dy = static_cast<LONG>((y - vy) * 65535LL / vh);
        i.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
    }
    in[1].mi.dwFlags |= right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    in[2].mi.dwFlags |= right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    SendInput(3, in, sizeof(INPUT));
}

HWND OverflowWindow() {
    HWND h = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr);
    return h && IsWindowVisible(h) ? h : nullptr;
}

// ---- 点击的几个步骤，用定时器一步步走，不卡主线程（任务栏显示出来要靠主线程处理前台变化） ----

enum class Job { None, App, Overflow };
Job s_job = Job::None;
TrayApp s_target;
bool s_right = false;
int s_step = 0;

void Finish() {
    s_job = Job::None;
    KillTimer(g_mainWnd, kTimerTray);
}

void CALLBACK Step(HWND, UINT, UINT_PTR, DWORD) {
    KillTimer(g_mainWnd, kTimerTray);
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    switch (s_step++) {
        case 0: {  // 通知区域已经拿到焦点、任务栏露出来了
            std::vector<Button> buttons = ButtonsIn(tray);
            if (s_job == Job::App) {
                if (const Button* b = BestMatch(buttons, s_target)) {
                    Log(L"托盘：点任务栏上的 %ls", b->name.c_str());
                    ClickAt(b->rect, s_right);
                    s_step = 10;  // 等一下把鼠标挪回去
                    SetTimer(g_mainWnd, kTimerTray, 120, Step);
                    return;
                }
            }
            // 收在“隐藏的图标”里：展开它
            for (const Button& b : buttons)
                if (IsChevron(b) && OnScreen(b.rect)) {
                    ClickAt(b.rect, false);
                    SetTimer(g_mainWnd, kTimerTray, 400, Step);
                    return;
                }
            Log(L"托盘：任务栏上找不到展开隐藏图标的按钮");
            if (s_job == Job::App && !s_right) LaunchAsUser(s_target.exe);
            Finish();
            return;
        }
        case 1: {  // 隐藏的图标已经展开
            SetCursorPos(s_savedCursor.x, s_savedCursor.y);
            if (s_job == Job::Overflow) {
                Finish();
                return;
            }
            std::vector<Button> buttons = ButtonsIn(OverflowWindow());
            if (const Button* b = BestMatch(buttons, s_target)) {
                Log(L"托盘：点隐藏图标里的 %ls", b->name.c_str());
                ClickAt(b->rect, s_right);
                s_step = 10;
                SetTimer(g_mainWnd, kTimerTray, 120, Step);
                return;
            }
            // 找不到对应的按钮：左键就再启动一次这个程序；右键把展开的隐藏图标留给用户自己点
            Log(L"托盘：找不到 %ls 的托盘图标", s_target.name.c_str());
            if (!s_right) LaunchAsUser(s_target.exe);
            Finish();
            return;
        }
        default:  // 点完了，鼠标挪回原处
            SetCursorPos(s_savedCursor.x, s_savedCursor.y);
            Finish();
            return;
    }
}

void Start(Job job) {
    s_job = job;
    s_step = 0;
    GetCursorPos(&s_savedCursor);
    SendWinCombo('B');  // 通知区域拿到焦点：任务栏藏着的话会显示出来，按钮才在屏幕上
    SetTimer(g_mainWnd, kTimerTray, 350, Step);
}

}  // namespace

std::vector<TrayApp> Tray_Load(int iconPx) {
    std::vector<TrayApp> apps;
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return apps;
    std::set<std::wstring> running = RunningExes();
    std::set<std::wstring> seen;  // 同一个程序只列一次
    wchar_t sub[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(root, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY key;
        if (RegOpenKeyExW(root, sub, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        TrayApp app;
        app.exe = ResolvePath(RegString(key, L"ExecutablePath"));
        std::wstring lower = Lower(app.exe);
        if (!app.exe.empty() && running.count(lower) && !seen.count(lower)) {
            seen.insert(lower);
            app.name = RegString(key, L"InitialTooltip");
            DWORD promoted = 0, size = sizeof(promoted);
            RegGetValueW(key, nullptr, L"IsPromoted", RRF_RT_REG_DWORD, nullptr, &promoted, &size);
            app.promoted = promoted != 0;
            DWORD bytes = 0;
            if (RegQueryValueExW(key, L"IconSnapshot", nullptr, nullptr, nullptr, &bytes) == ERROR_SUCCESS && bytes > 0) {
                std::vector<BYTE> png(bytes);
                if (RegQueryValueExW(key, L"IconSnapshot", nullptr, nullptr, png.data(), &bytes) == ERROR_SUCCESS)
                    app.icon = BitmapFromPng(png, iconPx);
            }
            if (!app.icon) app.icon = ExeIcon(app.exe);
            if (app.name.empty()) {
                size_t slash = app.exe.find_last_of(L"\\/");
                app.name = slash == std::wstring::npos ? app.exe : app.exe.substr(slash + 1);
            }
            apps.push_back(std::move(app));
        }
        RegCloseKey(key);
    }
    RegCloseKey(root);
    // 任务栏上露着的排前面，和系统任务栏的顺序接近
    std::stable_sort(apps.begin(), apps.end(), [](const TrayApp& a, const TrayApp& b) { return a.promoted > b.promoted; });
    return apps;
}

void Tray_Click(const TrayApp& app, bool right) {
    s_target = app;
    s_right = right;
    Start(Job::App);
}

void Tray_ShowHidden() { Start(Job::Overflow); }

void Tray_QuickSettings() { SendWinCombo('A'); }

void Tray_Notifications() { SendWinCombo('N'); }

}  // namespace app
