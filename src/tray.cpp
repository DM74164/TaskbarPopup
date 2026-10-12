// 迷你任务栏里的托盘图标：系统托盘“显示隐藏的图标”里收着的那些（可以在设置里关掉）。
//
// Win11 的托盘区是资源管理器自己画的，没有公开接口能拿到别的程序的托盘图标。这里：
//   - 图标和名字：系统给每个托盘图标记在 HKCU\Control Panel\NotifyIconSettings 里（程序路径、最初的提示文字、
//     是否在任务栏上一直显示、图标快照 PNG），只列出程序正在运行、没设成一直显示的。在后台线程上读，
//     迷你任务栏弹出时直接用读好的（每 15 秒、每次收起后重新读一遍）
//   - 点击：在后台线程上用 UI 自动化在系统托盘里找到对应的按钮。左键先直接“调用”它（和在托盘里按回车一样，
//     不用把系统任务栏、隐藏的图标弹出来）；不行或者是右键，就展开“隐藏的图标”，在它上面模拟一次真的鼠标单击，
//     这样程序自己的窗口和右键菜单照常弹出来（右键先试像按菜单键那样让它弹菜单，鼠标不挪过去，
//     按鼠标位置弹菜单的程序菜单就出在迷你任务栏的图标上方；没弹出来再真点）。任务栏被本程序藏着时 UI 自动化看不到里面的按钮：先从它的子窗口找，
//     还找不到就临时把它显示出来（自动隐藏开着，只在屏幕底边露一条线），点完、菜单关了再藏回去。
//     都找不到时左键改成再启动一次这个程序（多数托盘程序会把开着的窗口调出来），
//     右键改成弹出迷你任务栏自己的菜单（打开、打开文件所在的位置、结束程序）
#include "common.h"

#include <tlhelp32.h>
#include <uiautomation.h>

#include <atomic>
#include <cwctype>
#include <map>
#include <mutex>
#include <set>

namespace app {
namespace {

// 自己定义一份，免得依赖头文件里 GUID 的链接方式
const CLSID kClsidUIAutomation = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
const IID kIidUIAutomation = {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
const IID kIidInvokePattern = {0xfb377fbe, 0x8ea6, 0x46d5, {0x9c, 0x73, 0x64, 0x99, 0x64, 0x2d, 0x30, 0x59}};
const IID kIidTogglePattern = {0x94cf8058, 0x9b8d, 0x4ab9, {0x8b, 0xfd, 0x4c, 0xd0, 0xa3, 0x3c, 0x8c, 0x70}};
const IID kIidExpandCollapsePattern = {0x619be086, 0x1f4e, 0x4ee4, {0xba, 0xfa, 0x21, 0x01, 0x28, 0x73, 0x87, 0x30}};
const IID kIidElement3 = {0x8471df34, 0xaee0, 0x4a01, {0xa7, 0xde, 0x7d, 0xb9, 0xaf, 0x12, 0xc2, 0x96}};

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

// ---- 读托盘图标 ----

std::mutex s_lock;
std::vector<TrayApp> s_apps;       // 最近一次读到的（s_lock 保护）
std::atomic<bool> s_loading{false};
// 解好的图标：快照没变就接着用，不用每次都解 PNG（只在读的线程里用，同一时间只有一个在读）
std::map<std::wstring, std::pair<std::vector<BYTE>, std::shared_ptr<Gdiplus::Bitmap>>> s_iconCache;

// REG_EXPAND_SZ 也会展开成 REG_SZ 返回（同时写 RRF_RT_REG_EXPAND_SZ 反而会失败）
std::wstring RegString(HKEY key, const wchar_t* name) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
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
    if (!SHGetFileInfoW(exe.c_str(), 0, &info, sizeof(info), SHGFI_ICON) || !info.hIcon) return nullptr;
    auto bmp = BitmapFromIcon(info.hIcon);
    DestroyIcon(info.hIcon);
    return bmp;
}

std::vector<TrayApp> Load() {
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
            // 提示文字可能有好几行（比如“OneDrive\n已是最新”），名字只要第一行
            app.name = RegString(key, L"InitialTooltip");
            app.name = app.name.substr(0, app.name.find_first_of(L"\r\n"));
            while (!app.name.empty() && std::iswspace(app.name.back())) app.name.pop_back();
            DWORD promoted = 0, size = sizeof(promoted);
            RegGetValueW(key, nullptr, L"IsPromoted", RRF_RT_REG_DWORD, nullptr, &promoted, &size);
            app.promoted = promoted != 0;
            std::vector<BYTE> png;
            DWORD bytes = 0;
            if (RegQueryValueExW(key, L"IconSnapshot", nullptr, nullptr, nullptr, &bytes) == ERROR_SUCCESS && bytes > 0) {
                png.resize(bytes);
                if (RegQueryValueExW(key, L"IconSnapshot", nullptr, nullptr, png.data(), &bytes) == ERROR_SUCCESS) png.resize(bytes);
                else png.clear();
            }
            auto& cached = s_iconCache[lower];
            if (!cached.second || cached.first != png) {
                std::shared_ptr<Gdiplus::Bitmap> icon = png.empty() ? nullptr : BitmapFromPng(png, 48);
                if (!icon) icon = ExeIcon(app.exe);
                cached = {std::move(png), icon};
            }
            app.icon = cached.second;
            if (app.name.empty()) {
                size_t slash = app.exe.find_last_of(L"\\/");
                app.name = slash == std::wstring::npos ? app.exe : app.exe.substr(slash + 1);
            }
            apps.push_back(std::move(app));
        }
        RegCloseKey(key);
    }
    RegCloseKey(root);
    return apps;
}

DWORD WINAPI LoadThread(LPVOID) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // SHGetFileInfo 要
    std::vector<TrayApp> apps = Load();
    {
        std::lock_guard<std::mutex> guard(s_lock);
        s_apps = std::move(apps);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    s_loading = false;
    return 0;
}

// ---- 点托盘图标（后台线程） ----

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
    RECT rect = {};
    std::shared_ptr<IUIAutomationElement> element;
};

std::shared_ptr<IUIAutomationElement> Hold(IUIAutomationElement* e) {
    return std::shared_ptr<IUIAutomationElement>(e, [](IUIAutomationElement* p) { p->Release(); });
}

// 窗口里所有按钮（UI 自动化）
std::vector<Button> ButtonsIn(IUIAutomation* uia, HWND hwnd) {
    std::vector<Button> out;
    if (!hwnd) return out;
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
        IUIAutomationElement* e = nullptr;
        if (FAILED(all->GetElement(i, &e)) || !e) continue;
        Button b;
        b.element = Hold(e);
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
        out.push_back(std::move(b));
    }
    return out;
}

// 任务栏里所有按钮。任务栏被藏起来时从它往下一个都找不到（UI 自动化不进看不见的窗口），
// 就从它的每个子窗口分别找（Win11 任务栏的内容在 XAML 的子窗口里）
std::vector<Button> TaskbarButtons(IUIAutomation* uia, HWND tray) {
    std::vector<Button> out = ButtonsIn(uia, tray);
    if (!out.empty() || !tray) return out;
    std::vector<HWND> children;
    EnumChildWindows(
        tray,
        [](HWND h, LPARAM lp) -> BOOL {
            reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&children));
    std::set<std::wstring> seen;  // 子窗口套着子窗口时同一个按钮会找到两次
    std::wstring found;
    for (HWND child : children) {
        std::vector<Button> buttons = ButtonsIn(uia, child);
        found += GetClassNameStr(child) + L" " + std::to_wstring(buttons.size()) + L" 个；";
        for (Button& b : buttons) {
            wchar_t key[96];
            swprintf(key, 96, L"%ld,%ld,%ld,%ld|", b.rect.left, b.rect.top, b.rect.right, b.rect.bottom);
            if (seen.insert(key + b.cls + L"|" + b.name).second) out.push_back(std::move(b));
        }
    }
    if (g_settings.debugLog)
        Log(L"托盘：任务栏%ls，从它的 %d 个子窗口里找：%ls", IsWindowVisible(tray) ? L"显示着" : L"藏着",
            static_cast<int>(children.size()), found.c_str());
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

const Button* FindChevron(const std::vector<Button>& buttons) {
    const Button* chevron = nullptr;
    for (const Button& b : buttons)
        if (IsChevron(b)) chevron = &b;
    return chevron;
}

const Button* BestMatch(const std::vector<Button>& buttons, const TrayApp& app, bool needOnScreen) {
    const Button* best = nullptr;
    int bestScore = 0;
    for (const Button& b : buttons) {
        if (IsChevron(b) || (needOnScreen && !OnScreen(b.rect))) continue;
        int score = MatchScore(b.name, app);
        if (score > bestScore) {
            best = &b;
            bestScore = score;
        }
    }
    return best;
}

// 先试“调用”，再试“切换”“展开”（“显示隐藏的图标”在不同版本里可能是这几种按钮之一）
bool Press(const Button& b) {
    IUIAutomationElement* e = b.element.get();
    if (!e) return false;
    IUIAutomationInvokePattern* invoke = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_InvokePatternId, kIidInvokePattern, reinterpret_cast<void**>(&invoke))) &&
        invoke) {
        HRESULT hr = invoke->Invoke();
        invoke->Release();
        if (SUCCEEDED(hr)) return true;
    }
    IUIAutomationTogglePattern* toggle = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_TogglePatternId, kIidTogglePattern, reinterpret_cast<void**>(&toggle))) &&
        toggle) {
        HRESULT hr = toggle->Toggle();
        toggle->Release();
        if (SUCCEEDED(hr)) return true;
    }
    IUIAutomationExpandCollapsePattern* expand = nullptr;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, kIidExpandCollapsePattern,
                                         reinterpret_cast<void**>(&expand))) &&
        expand) {
        HRESULT hr = expand->Expand();
        expand->Release();
        if (SUCCEEDED(hr)) return true;
    }
    return false;
}

// 像按键盘上的菜单键那样让按钮弹出右键菜单，鼠标不用挪过去。系统不支持时返回 false
bool ShowMenuOf(const Button& b) {
    IUIAutomationElement3* e3 = nullptr;
    if (!b.element || FAILED(b.element->QueryInterface(kIidElement3, reinterpret_cast<void**>(&e3))) || !e3) return false;
    HRESULT hr = e3->ShowContextMenu();
    e3->Release();
    return SUCCEEDED(hr);
}

// 在 rect 中间模拟一次鼠标单击，再把鼠标挪回原处
void ClickAt(const RECT& rect, bool right) {
    POINT saved;
    GetCursorPos(&saved);
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
    Sleep(150);
    SetCursorPos(saved.x, saved.y);
}

HWND OverflowWindow() { return FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr); }

bool OverflowShowing() {
    HWND overflow = OverflowWindow();
    return overflow && IsWindowVisible(overflow);
}

// 有弹出菜单开着（系统画的菜单）
bool MenuShowing() {
    for (HWND h = FindWindowExW(nullptr, nullptr, L"#32768", nullptr); h; h = FindWindowExW(nullptr, h, L"#32768", nullptr))
        if (IsWindowVisible(h)) return true;
    return false;
}

// 前台窗口属于这个程序（它弹菜单时会把前台给自己的窗口，自己画菜单的程序也一样）
bool ForegroundIsApp(const std::wstring& exe) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    HANDLE h = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!h) return false;
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    bool same = QueryFullProcessImageNameW(h, 0, path, &size) && Lower(path) == Lower(exe);
    CloseHandle(h);
    return same;
}

struct Job {
    TrayApp app;
    bool right = false;
    bool overflowOnly = false;  // 只展开“隐藏的图标”
    HWND notify = nullptr;      // 右键弹不出程序自己的菜单时通知谁
};
std::atomic<bool> s_clicking{false};

void LaunchInstead(const TrayApp& app) {
    Log(L"托盘：找不到 %ls 的托盘图标，改成再启动一次它", app.name.c_str());
    PostMessageW(g_mainWnd, WM_APP_TRAY_LAUNCH, 0, reinterpret_cast<LPARAM>(new std::wstring(app.exe)));
}

void MenuInstead(const Job& job) {
    Log(L"托盘：弹不出 %ls 自己的菜单，改用迷你任务栏的菜单", job.app.name.c_str());
    HWND to = job.notify && IsWindow(job.notify) ? job.notify : g_mainWnd;
    auto* app = new TrayApp(job.app);
    if (!PostMessageW(to, WM_APP_TRAY_MENU, 0, reinterpret_cast<LPARAM>(app))) delete app;
}

// 点不中时：左键再启动一次，右键弹迷你任务栏自己的菜单
void Instead(const Job& job) {
    if (job.right) MenuInstead(job);
    else LaunchInstead(job.app);
}

void Names(const wchar_t* where, const std::vector<Button>& buttons) {
    if (!g_settings.debugLog) return;
    std::wstring list;
    for (const Button& b : buttons) {
        if (!list.empty()) list += L" | ";
        list += b.name + L"[" + b.cls + L"]" + (OnScreen(b.rect) ? L"" : L"(不在屏幕上)");
    }
    Log(L"托盘：%ls里的按钮 %d 个：%ls", where, static_cast<int>(buttons.size()), list.c_str());
}

// 返回 true = 在系统托盘里点了（或者展开了隐藏的图标），false = 改成了别的办法
bool RunJob(const Job& job) {
    Ref<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(kClsidUIAutomation, nullptr, CLSCTX_INPROC_SERVER, kIidUIAutomation,
                                reinterpret_cast<void**>(&uia.p))) || !uia.p) {
        Log(L"托盘：用不了 UI 自动化");
        if (!job.overflowOnly) Instead(job);
        return false;
    }
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    std::vector<Button> onTaskbar = TaskbarButtons(uia.p, tray);
    std::vector<Button> inOverflow = ButtonsIn(uia.p, OverflowWindow());
    Names(L"任务栏", onTaskbar);
    Names(L"隐藏的图标", inOverflow);

    if (!job.overflowOnly) {
        // 左键：直接“调用”按钮，什么都不用弹出来
        if (!job.right) {
            const Button* b = BestMatch(inOverflow, job.app, false);
            if (!b) b = BestMatch(onTaskbar, job.app, false);
            if (b && Press(*b)) {
                Log(L"托盘：调用了 %ls 的托盘图标", b->name.c_str());
                return true;
            }
        }
        // 任务栏露着、图标就在上面：直接点
        if (tray && IsWindowVisible(tray))
            if (const Button* b = BestMatch(onTaskbar, job.app, true)) {
                Log(L"托盘：点任务栏上的 %ls", b->name.c_str());
                ClickAt(b->rect, job.right);
                return true;
            }
    }

    // 展开“隐藏的图标”，在里面点
    HWND overflow = OverflowWindow();
    if (!overflow || !IsWindowVisible(overflow)) {
        const Button* chevron = FindChevron(onTaskbar);
        DWORD_PTR ours = 0;
        if (!chevron && tray && !IsWindowVisible(tray) &&
            SendMessageTimeoutW(g_mainWnd, WM_APP_TRAY_PEEK, 1, reinterpret_cast<LPARAM>(tray), SMTO_ABORTIFHUNG, 1000,
                                &ours) &&
            ours) {
            // 任务栏是本程序藏的：临时显示出来再找（点完、菜单关了以后 ClickThread 让主线程藏回去）
            for (int i = 0; i < 20 && !IsWindowVisible(tray); ++i) Sleep(50);
            for (int i = 0; i < 5 && !chevron; ++i) {  // 显示出来以后 XAML 可能还要一会儿才把按钮报出来
                Sleep(100);
                onTaskbar = TaskbarButtons(uia.p, tray);
                chevron = FindChevron(onTaskbar);
            }
            Log(L"托盘：临时显示任务栏以后%ls“显示隐藏的图标”按钮", chevron ? L"找到了" : L"还是找不到");
            Names(L"临时显示的任务栏", onTaskbar);
        }
        if (!chevron || !Press(*chevron)) {
            Log(L"托盘：没能展开隐藏的图标（%ls）", chevron ? L"按钮按不动" : L"找不到按钮");
            if (!job.overflowOnly) Instead(job);
            return false;
        }
        for (int i = 0; i < 20; ++i) {  // 最多等 1 秒
            Sleep(50);
            overflow = OverflowWindow();
            if (overflow && IsWindowVisible(overflow)) break;
        }
        if (!overflow || !IsWindowVisible(overflow)) {
            Log(L"托盘：按了展开隐藏的图标，但没有展开");
            if (!job.overflowOnly) Instead(job);
            return false;
        }
        Sleep(200);  // 等展开动画放完，按钮到位
    }
    if (job.overflowOnly) return true;
    inOverflow = ButtonsIn(uia.p, overflow);
    Names(L"展开后隐藏的图标", inOverflow);
    if (const Button* b = BestMatch(inOverflow, job.app, true)) {
        RECT r = b->rect;
        if (job.right && ShowMenuOf(*b)) {
            for (int i = 0; i < 12; ++i) {  // 最多等 0.6 秒
                Sleep(50);
                if (MenuShowing() || ForegroundIsApp(job.app.exe)) {
                    Log(L"托盘：像按菜单键那样弹出了 %ls 的菜单", b->name.c_str());
                    return true;
                }
            }
            Log(L"托盘：像按菜单键那样没弹出 %ls 的菜单，改成真的右键点一下", b->name.c_str());
        }
        Log(L"托盘：%ls隐藏图标里的 %ls（%ld,%ld - %ld,%ld）", job.right ? L"右键" : L"点", b->name.c_str(), r.left,
            r.top, r.right, r.bottom);
        ClickAt(r, job.right);
        return true;
    }
    Log(L"托盘：隐藏的图标里找不到 %ls", job.app.name.c_str());
    Instead(job);
    return false;
}

DWORD WINAPI ClickThread(LPVOID param) {
    std::unique_ptr<Job> job(static_cast<Job*>(param));
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    bool trayHidden = tray && !IsWindowVisible(tray);
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool clicked = RunJob(*job);
    if (SUCCEEDED(co)) CoUninitialize();
    s_clicking = false;
    // 任务栏本来藏着：为了点隐藏的图标临时显示了，或者资源管理器展开隐藏的图标时自己把它显示了出来。
    // 点了的话等隐藏的图标和程序的菜单都关了，没点成就马上，让主线程藏回去（不是本程序藏的、已经不用藏了的主线程不管）
    if (trayHidden) {
        for (int i = 0; clicked && i < 600 && (OverflowShowing() || MenuShowing()); ++i) Sleep(100);  // 最多等 1 分钟
        if (!s_clicking && IsWindowVisible(tray))
            PostMessageW(g_mainWnd, WM_APP_TRAY_PEEK, 0, reinterpret_cast<LPARAM>(tray));
    }
    return 0;
}

void StartJob(Job* job) {
    if (s_clicking.exchange(true)) {  // 上一次还没点完
        delete job;
        return;
    }
    Fullscreen_QuietStartMenu();  // 展开的隐藏图标在前台时别把藏着的系统任务栏放出来
    if (HANDLE h = CreateThread(nullptr, 0, ClickThread, job, 0, nullptr)) {
        CloseHandle(h);
    } else {
        delete job;
        s_clicking = false;
    }
}

// ---- 迷你任务栏自己的菜单 ----

std::vector<DWORD> ProcessesOf(const std::wstring& exe) {
    std::vector<DWORD> pids;
    std::wstring want = Lower(exe);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe = {sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t path[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &size) && Lower(path) == want) pids.push_back(pe.th32ProcessID);
        CloseHandle(h);
    }
    CloseHandle(snap);
    return pids;
}

struct EndJob {
    std::wstring exe, name;
};

// 结束程序：先给它的窗口发关闭（藏着的也发，多数托盘程序收到会自己退出），3 秒后还在的强行结束
DWORD WINAPI EndThread(LPVOID param) {
    std::unique_ptr<EndJob> job(static_cast<EndJob*>(param));
    std::vector<DWORD> pids = ProcessesOf(job->exe);
    std::vector<HANDLE> handles;
    for (DWORD pid : pids)
        if (HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid)) handles.push_back(h);
    if (handles.size() < pids.size()) Log(L"托盘：%ls 有 %d 个进程没有权限结束（管理员权限的程序）", job->name.c_str(),
                                          static_cast<int>(pids.size() - handles.size()));
    struct Ctx {
        const std::vector<DWORD>* pids;
    } ctx = {&pids};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            const auto* list = reinterpret_cast<Ctx*>(lp)->pids;
            if (std::find(list->begin(), list->end(), pid) != list->end()) PostMessageW(h, WM_CLOSE, 0, 0);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    int killed = 0;
    if (!handles.empty() &&
        WaitForMultipleObjects(static_cast<DWORD>(std::min<size_t>(handles.size(), MAXIMUM_WAIT_OBJECTS)), handles.data(),
                               TRUE, 3000) == WAIT_TIMEOUT)
        for (HANDLE h : handles)
            if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT && TerminateProcess(h, 1)) ++killed;
    for (HANDLE h : handles) CloseHandle(h);
    Log(L"托盘：结束 %ls（%d 个进程，强行结束了 %d 个）", job->name.c_str(), static_cast<int>(pids.size()), killed);
    if (pids.empty() || handles.size() < pids.size()) MessageBeep(MB_ICONWARNING);
    return 0;
}

void EndApp(const TrayApp& app) {
    wchar_t text[512];
    swprintf(text, 512, L"结束“%ls”？\n它没保存的内容会丢失。", app.name.c_str());
    if (MessageBoxW(nullptr, text, L"TaskbarPopup", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST |
                                                         MB_SETFOREGROUND) != IDOK)
        return;
    if (HANDLE h = CreateThread(nullptr, 0, EndThread, new EndJob{app.exe, app.name}, 0, nullptr)) CloseHandle(h);
}

enum TrayCmd : UINT { kTrayOpen = 1, kTrayAppMenu, kTrayLocation, kTrayEnd };

}  // namespace

bool Tray_Menu(const TrayApp& app, HWND owner, POINT at, void (*picked)(), bool appMenu) {
    std::wstring name = app.name.size() > 40 ? app.name.substr(0, 39) + L"…" : app.name;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, name.c_str());  // 第一行是哪个程序
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayOpen, L"打开");
    if (appMenu) AppendMenuW(menu, MF_STRING, kTrayAppMenu, L"程序菜单");
    if (GetFileAttributesW(app.exe.c_str()) != INVALID_FILE_ATTRIBUTES)
        AppendMenuW(menu, MF_STRING, kTrayLocation, L"打开文件所在的位置");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayEnd, L"结束程序");
    SetMenuDefaultItem(menu, kTrayOpen, FALSE);
    if (!SetForegroundWindow(owner)) ForceForeground(owner);  // 不然点别处菜单不会消失
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_BOTTOMALIGN, at.x, at.y, 0,
                              owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (cmd == kTrayAppMenu) return true;
    if (!cmd) return false;
    if (picked) picked();
    switch (cmd) {
        case kTrayOpen: Tray_Click(app, false); break;
        case kTrayLocation: OpenFileLocation(app.exe); break;
        case kTrayEnd: EndApp(app); break;
    }
    return false;
}

void Tray_Refresh() {
    if (s_loading.exchange(true)) return;
    if (HANDLE h = CreateThread(nullptr, 0, LoadThread, nullptr, 0, nullptr)) CloseHandle(h);
    else s_loading = false;
}

std::vector<TrayApp> Tray_Hidden() {
    std::lock_guard<std::mutex> guard(s_lock);
    std::vector<TrayApp> hidden;
    for (const TrayApp& app : s_apps)
        if (!app.promoted) hidden.push_back(app);
    return hidden;
}

void Tray_Click(const TrayApp& app, bool right, HWND notify) { StartJob(new Job{app, right, false, notify}); }

void Tray_ShowHidden() { StartJob(new Job{TrayApp{}, false, true, nullptr}); }

bool Tray_Busy() { return s_clicking; }

bool Tray_OnMessage(UINT msg, LPARAM lParam) {
    if (msg == WM_APP_TRAY_MENU) {
        std::unique_ptr<TrayApp> app(reinterpret_cast<TrayApp*>(lParam));
        POINT pt;
        GetCursorPos(&pt);
        if (app) Tray_Menu(*app, g_mainWnd, pt, nullptr);
        return true;
    }
    if (msg != WM_APP_TRAY_LAUNCH) return false;
    std::unique_ptr<std::wstring> exe(reinterpret_cast<std::wstring*>(lParam));
    if (exe && !exe->empty()) LaunchAsUser(*exe);
    return true;
}

}  // namespace app
