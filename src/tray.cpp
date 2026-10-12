// 迷你任务栏里的托盘图标：系统托盘“显示隐藏的图标”里收着的那些（可以在设置里关掉）。
//
// Win11 的托盘区是资源管理器自己画的，没有公开接口能拿到别的程序的托盘图标。这里：
//   - 图标和名字：系统给每个托盘图标记在 HKCU\Control Panel\NotifyIconSettings 里（程序路径、最初的提示文字、
//     是否在任务栏上一直显示、图标快照 PNG），只列出程序正在运行、没设成一直显示的。在后台线程上读，
//     迷你任务栏弹出时直接用读好的（每 15 秒、每次收起后重新读一遍）
//   - 点击：在后台线程上用 UI 自动化在系统托盘里找到对应的按钮。左键先直接“调用”它（和在托盘里按回车一样，
//     不用把系统任务栏、隐藏的图标弹出来）；不行或者是右键，就展开“隐藏的图标”，在它上面模拟一次真的鼠标单击，
//     这样程序自己的窗口和右键菜单照常弹出来。都找不到时左键改成再启动一次这个程序（多数托盘程序会把开着的窗口调出来）
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

struct Job {
    TrayApp app;
    bool right = false;
    bool overflowOnly = false;  // 只展开“隐藏的图标”
};
std::atomic<bool> s_clicking{false};

void LaunchInstead(const TrayApp& app) {
    Log(L"托盘：找不到 %ls 的托盘图标，改成再启动一次它", app.name.c_str());
    PostMessageW(g_mainWnd, WM_APP_TRAY_LAUNCH, 0, reinterpret_cast<LPARAM>(new std::wstring(app.exe)));
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

void RunJob(const Job& job) {
    Ref<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(kClsidUIAutomation, nullptr, CLSCTX_INPROC_SERVER, kIidUIAutomation,
                                reinterpret_cast<void**>(&uia.p))) || !uia.p) {
        Log(L"托盘：用不了 UI 自动化");
        if (!job.overflowOnly && !job.right) LaunchInstead(job.app);
        return;
    }
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    std::vector<Button> onTaskbar = ButtonsIn(uia.p, tray);
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
                return;
            }
        }
        // 任务栏露着、图标就在上面：直接点
        if (tray && IsWindowVisible(tray))
            if (const Button* b = BestMatch(onTaskbar, job.app, true)) {
                Log(L"托盘：点任务栏上的 %ls", b->name.c_str());
                ClickAt(b->rect, job.right);
                return;
            }
    }

    // 展开“隐藏的图标”，在里面点
    HWND overflow = OverflowWindow();
    if (!overflow || !IsWindowVisible(overflow)) {
        const Button* chevron = nullptr;
        for (const Button& b : onTaskbar)
            if (IsChevron(b)) chevron = &b;
        if (!chevron || !Press(*chevron)) {
            Log(L"托盘：没能展开隐藏的图标（%ls）", chevron ? L"按钮按不动" : L"找不到按钮");
            if (!job.overflowOnly && !job.right) LaunchInstead(job.app);
            return;
        }
        for (int i = 0; i < 20; ++i) {  // 最多等 1 秒
            Sleep(50);
            overflow = OverflowWindow();
            if (overflow && IsWindowVisible(overflow)) break;
        }
        if (!overflow || !IsWindowVisible(overflow)) {
            Log(L"托盘：按了展开隐藏的图标，但没有展开");
            if (!job.overflowOnly && !job.right) LaunchInstead(job.app);
            return;
        }
        Sleep(200);  // 等展开动画放完，按钮到位
    }
    if (job.overflowOnly) return;
    inOverflow = ButtonsIn(uia.p, overflow);
    Names(L"展开后隐藏的图标", inOverflow);
    if (const Button* b = BestMatch(inOverflow, job.app, true)) {
        Log(L"托盘：%ls隐藏图标里的 %ls", job.right ? L"右键" : L"点", b->name.c_str());
        ClickAt(b->rect, job.right);
        return;
    }
    if (!job.right) LaunchInstead(job.app);
    else Log(L"托盘：隐藏的图标里找不到 %ls", job.app.name.c_str());
}

DWORD WINAPI ClickThread(LPVOID param) {
    std::unique_ptr<Job> job(static_cast<Job*>(param));
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    RunJob(*job);
    if (SUCCEEDED(co)) CoUninitialize();
    s_clicking = false;
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

}  // namespace

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

void Tray_Click(const TrayApp& app, bool right) { StartJob(new Job{app, right, false}); }

void Tray_ShowHidden() { StartJob(new Job{TrayApp{}, false, true}); }

bool Tray_OnMessage(UINT msg, LPARAM lParam) {
    if (msg != WM_APP_TRAY_LAUNCH) return false;
    std::unique_ptr<std::wstring> exe(reinterpret_cast<std::wstring*>(lParam));
    if (exe && !exe->empty()) LaunchAsUser(*exe);
    return true;
}

}  // namespace app
