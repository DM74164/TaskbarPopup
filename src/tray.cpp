// 迷你任务栏里的托盘图标：系统托盘“显示隐藏的图标”里收着的那些（可以在设置里关掉）。
//
// 托盘区是资源管理器自己的，没有公开接口能拿到别的程序的托盘图标。这里：
//   - 图标和名字（在后台线程上读，迷你任务栏弹出时直接用读好的，每 15 秒、每次收起后重新读一遍）：
//     Win11：系统给每个托盘图标记在 HKCU\Control Panel\NotifyIconSettings 里（程序路径、最初的提示文字、
//     是否在任务栏上一直显示、图标 GUID、图标快照 PNG），只列出程序正在运行、没设成一直显示的。
//     Win10 没有这些记录：“隐藏的图标”是资源管理器里的一个工具栏，直接读它的按钮（图标、提示文字、属于哪个窗口）
//   - 点击：在后台线程上用 UI 自动化在系统托盘里找到对应的按钮。左键先直接“调用”它（和在托盘里按回车一样，
//     不用把系统任务栏、隐藏的图标弹出来，Win10 不试这一步）；不行或者是右键，就展开“隐藏的图标”，在它上面模拟一次真的鼠标单击，
//     右键仍由程序自己弹菜单，再把新弹出的菜单移到迷你任务栏的图标旁。
//     任务栏被本程序藏着时 UI 自动化看不到里面的按钮：先从它的子窗口找，
//     还找不到就临时把它显示出来（自动隐藏开着，只在屏幕底边露一条线），点完、菜单关了再藏回去。
//     找不到时只在左键操作中尝试启动程序（多数托盘程序会把开着的窗口调出来）。
#include "common.h"

#include <tlhelp32.h>
#include <uiautomation.h>

#include <atomic>
#include <cstdlib>
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
struct CachedIcon {
    std::vector<BYTE> png;
    std::shared_ptr<Gdiplus::Bitmap> icon, large;
};
std::map<std::wstring, CachedIcon> s_iconCache;
// 程序文件里的图标，按“程序|大小”存（只在读的线程里比较、复制，不交出去：交出去的是复制的那份，画的线程同时用也不冲突）
std::map<std::wstring, std::shared_ptr<Gdiplus::Bitmap>> s_exeIconCache;
constexpr int kLargeIconPx = 96;

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

// PNG 快照 → 自己持有像素的位图（从流解出来的位图要一直拿着流），原来的大小
std::shared_ptr<Gdiplus::Bitmap> BitmapFromPng(const std::vector<BYTE>& png) {
    if (png.empty()) return nullptr;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, png.size());
    if (!mem) return nullptr;
    void* bytes = GlobalLock(mem);
    if (!bytes) {
        GlobalFree(mem);
        return nullptr;
    }
    memcpy(bytes, png.data(), png.size());
    GlobalUnlock(mem);
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) {
        GlobalFree(mem);
        return nullptr;
    }
    std::shared_ptr<Gdiplus::Bitmap> result;
    {
        Gdiplus::Bitmap decoded(stream);
        int w = static_cast<int>(decoded.GetWidth()), h = static_cast<int>(decoded.GetHeight());
        if (decoded.GetLastStatus() == Gdiplus::Ok && w > 0 && h > 0) {
            result = std::make_shared<Gdiplus::Bitmap>(w, h, PixelFormat32bppPARGB);
            Gdiplus::Graphics g(result.get());
            g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
            g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.DrawImage(&decoded, Gdiplus::Rect(0, 0, w, h));
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

// 程序文件里的主图标，按 px 大小取（有大图的程序取大的是清楚的）。读的线程自己留着，不交出去
Gdiplus::Bitmap* ExeIconAt(const std::wstring& exe, int px) {
    std::wstring key = Lower(exe) + L"|" + std::to_wstring(px);
    auto found = s_exeIconCache.find(key);
    if (found != s_exeIconCache.end()) return found->second.get();
    std::shared_ptr<Gdiplus::Bitmap> bmp;
    HICON icon = nullptr;
    if (SHDefExtractIconW(exe.c_str(), 0, 0, &icon, nullptr, px) == S_OK && icon) {
        bmp = BitmapFromIcon(icon);
        DestroyIcon(icon);
    }
    s_exeIconCache[key] = bmp;
    return bmp.get();
}

// 两个图标是不是同一张图：large 缩到 small 的大小（一样大就不缩），按预乘过透明度的颜色逐像素比
bool LooksSame(Gdiplus::Bitmap* bmpSmall, Gdiplus::Bitmap* bmpLarge) {
    int w = static_cast<int>(bmpSmall->GetWidth()), h = static_cast<int>(bmpSmall->GetHeight());
    if (w < 8 || h < 8) return false;
    Gdiplus::Bitmap a(w, h, PixelFormat32bppPARGB), b(w, h, PixelFormat32bppPARGB);
    {
        Gdiplus::Graphics g(&a);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.DrawImage(bmpSmall, Gdiplus::Rect(0, 0, w, h));
    }
    {
        Gdiplus::Graphics g(&b);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        Gdiplus::ImageAttributes wrap;
        wrap.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
        g.DrawImage(bmpLarge, Gdiplus::Rect(0, 0, w, h), 0, 0, static_cast<int>(bmpLarge->GetWidth()),
                    static_cast<int>(bmpLarge->GetHeight()), Gdiplus::UnitPixel, &wrap);
    }
    Gdiplus::Rect rect(0, 0, w, h);
    Gdiplus::BitmapData da, db;
    if (a.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &da) != Gdiplus::Ok) return false;
    if (b.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &db) != Gdiplus::Ok) {
        a.UnlockBits(&da);
        return false;
    }
    long long diff = 0, covered = 0;
    for (int y = 0; y < h; ++y) {
        const BYTE* pa = static_cast<const BYTE*>(da.Scan0) + y * da.Stride;
        const BYTE* pb = static_cast<const BYTE*>(db.Scan0) + y * db.Stride;
        for (int x = 0; x < w * 4; x += 4) {
            if (pa[x + 3] < 16 && pb[x + 3] < 16) continue;
            ++covered;
            for (int c = 0; c < 4; ++c) diff += std::abs(pa[x + c] - pb[x + c]);
        }
    }
    a.UnlockBits(&da);
    b.UnlockBits(&db);
    long long avg = covered ? diff / (covered * 4) : -1;
    if (g_settings.debugLog) Log(L"托盘：%d×%d 的图标和程序本身同样大的图标平均差 %lld", w, h, avg);
    // 系统画快照时可能有一点点出入；换了颜色、换了图案的（状态图标）差得多
    return covered >= w * h / 8 && avg < 12;
}

// 托盘图标比较小时，看它是不是就是程序本身的图标（取程序同样大的图标逐像素比），是就交出一份高清的
std::shared_ptr<Gdiplus::Bitmap> LargeFor(const std::wstring& exe, Gdiplus::Bitmap* icon) {
    if (exe.empty() || !icon) return nullptr;
    Gdiplus::Bitmap* large = ExeIconAt(exe, kLargeIconPx);
    if (!large || large->GetWidth() <= icon->GetWidth()) return nullptr;
    Gdiplus::Bitmap* same = ExeIconAt(exe, static_cast<int>(icon->GetWidth()));
    if (!same || !LooksSame(icon, same)) return nullptr;
    Gdiplus::Rect all(0, 0, static_cast<int>(large->GetWidth()), static_cast<int>(large->GetHeight()));
    return std::shared_ptr<Gdiplus::Bitmap>(large->Clone(all, PixelFormat32bppPARGB));
}

void LogIcon(const TrayApp& app) {
    if (!g_settings.debugLog) return;
    Log(L"托盘：%ls 的图标 %u×%u%ls", app.name.c_str(), app.icon ? app.icon->GetWidth() : 0,
        app.icon ? app.icon->GetHeight() : 0, app.iconLarge ? L"，放大时用程序本身的高清图标" : L"");
}

// Win10 及以前的托盘：老式工具栏，不是 Win11 的 XAML
bool LegacyTray() {
    DWORD build = WindowsBuild();
    return build && build < 22000;
}

// 提示文字可能有好几行（比如“OneDrive\n已是最新”），名字只要第一行
std::wstring FirstLine(const std::wstring& text) {
    std::wstring line = text.substr(0, text.find_first_of(L"\r\n"));
    while (!line.empty() && std::iswspace(line.back())) line.pop_back();
    return line;
}

std::wstring FileName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Win11：系统给每个托盘图标记下的设置
std::vector<TrayApp> LoadFromSettings() {
    std::vector<TrayApp> apps;
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return apps;
    std::set<std::wstring> running = RunningExes();
    std::map<std::pair<std::wstring, bool>, size_t> entries;
    wchar_t sub[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(root, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        HKEY key;
        if (RegOpenKeyExW(root, sub, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        TrayApp app;
        app.exe = ResolvePath(RegString(key, L"ExecutablePath"));
        std::wstring lower = Lower(app.exe);
        DWORD promoted = 0, size = sizeof(promoted);
        RegGetValueW(key, nullptr, L"IsPromoted", RRF_RT_REG_DWORD, nullptr, &promoted, &size);
        if (app.exe.empty() || !running.count(lower)) {
            RegCloseKey(key);
            continue;
        }
        // 一直显示的和收起来的各显示一份，但保留两组里每个图标的 GUID。
        app.promoted = promoted != 0;
        auto [entry, added] = entries.try_emplace(std::make_pair(lower, app.promoted), apps.size());
        if (added) {
            app.name = FirstLine(RegString(key, L"InitialTooltip"));
            std::vector<BYTE> png;
            DWORD bytes = 0;
            if (RegQueryValueExW(key, L"IconSnapshot", nullptr, nullptr, nullptr, &bytes) == ERROR_SUCCESS && bytes > 0) {
                png.resize(bytes);
                if (RegQueryValueExW(key, L"IconSnapshot", nullptr, nullptr, png.data(), &bytes) == ERROR_SUCCESS) png.resize(bytes);
                else png.clear();
            }
            if (app.name.empty()) app.name = FileName(app.exe);
            auto& cached = s_iconCache[lower];
            if (!cached.icon || cached.png != png) {
                std::shared_ptr<Gdiplus::Bitmap> icon = png.empty() ? nullptr : BitmapFromPng(png);
                if (!icon) icon = ExeIcon(app.exe);
                cached = {std::move(png), icon, icon ? LargeFor(app.exe, icon.get()) : nullptr};
                app.icon = cached.icon;
                app.iconLarge = cached.large;
                LogIcon(app);
            }
            app.icon = cached.icon;
            app.iconLarge = cached.large;
            apps.push_back(std::move(app));
        }
        std::wstring iconGuid = RegString(key, L"IconGuid");
        GUID guid{};
        if (!iconGuid.empty() && SUCCEEDED(CLSIDFromString(iconGuid.c_str(), &guid)) && !IsEqualGUID(guid, GUID{})) {
            auto& guids = apps[entry->second].iconGuids;
            if (std::find(guids.begin(), guids.end(), guid) == guids.end()) guids.push_back(guid);
        }
        RegCloseKey(key);
    }
    RegCloseKey(root);
    return apps;
}

// 资源管理器给托盘工具栏每个按钮带的数据（没有公开，Win7 到 Win10 一直是这样）。
// 按本机指针大小排，只在本程序和资源管理器位数相同时能用
struct TrayButtonData {
    HWND hwnd;  // 托盘图标属于的窗口
    UINT id;
    UINT callbackMessage;
    DWORD reserved[2];
    HICON icon;
};

// Win10：“隐藏的图标”是 NotifyIconOverflowWindow 里的工具栏（没展开时也在），按钮信息在资源管理器的内存里。
// 在资源管理器那边借一小块内存，让工具栏把按钮、提示文字写进去，再读回来（只读，不往它那边写东西）。
// 图标句柄各个进程通用，直接拿来画。找不到工具栏、读不了返回 false
bool LoadFromToolbar(std::vector<TrayApp>& apps) {
#ifndef _WIN64
    BOOL wow64 = FALSE;
    if (IsWow64Process(GetCurrentProcess(), &wow64) && wow64) return false;  // 32 位的本程序读不了 64 位资源管理器的内存
#endif
    HWND overflow = FindWindowW(L"NotifyIconOverflowWindow", nullptr);
    HWND toolbar = overflow ? FindWindowExW(overflow, nullptr, L"ToolbarWindow32", nullptr) : nullptr;
    DWORD pid = 0;
    if (!toolbar || !GetWindowThreadProcessId(toolbar, &pid) || !pid) return false;
    HANDLE explorer = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ, FALSE, pid);
    if (!explorer) {
        Log(L"托盘：打不开资源管理器进程（错误 %lu），读不到隐藏的图标", GetLastError());
        return false;
    }
    // 资源管理器卡住时别一直等
    auto ask = [&](UINT msg, WPARAM wParam, LPARAM lParam, DWORD_PTR& result) {
        return SendMessageTimeoutW(toolbar, msg, wParam, lParam, SMTO_ABORTIFHUNG, 500, &result) != 0;
    };
    constexpr size_t kTextAt = 64;      // 借来的内存：前面放按钮，后面放提示文字
    constexpr size_t kTextChars = 512;  // 提示文字最多 128 个字，足够了
    BYTE* remote = static_cast<BYTE*>(VirtualAllocEx(explorer, nullptr, kTextAt + kTextChars * sizeof(wchar_t),
                                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    DWORD_PTR count = 0;
    bool ok = remote && ask(TB_BUTTONCOUNT, 0, 0, count);
    std::set<std::wstring> seen;  // 同一个图标只列一次
    for (DWORD_PTR i = 0; ok && i < count; ++i) {
        TBBUTTON button = {};
        TrayButtonData data = {};
        DWORD_PTR got = 0;
        if (!ask(TB_GETBUTTON, i, reinterpret_cast<LPARAM>(remote), got) || !got ||
            !ReadProcessMemory(explorer, remote, &button, sizeof(button), nullptr))
            continue;
        // 藏起来的按钮是程序设成不显示的图标；程序退出了没来得及删的图标，窗口已经没了
        if ((button.fsState & TBSTATE_HIDDEN) || !button.dwData ||
            !ReadProcessMemory(explorer, reinterpret_cast<const void*>(button.dwData), &data, sizeof(data), nullptr) ||
            !IsWindow(data.hwnd))
            continue;
        TrayApp app;
        app.exe = GetProcessPath(data.hwnd);
        if (app.exe.empty()) continue;
        // 先问长度，放得下才让它写（它不管缓冲区多大）
        DWORD_PTR length = 0;
        std::wstring tip;
        if (ask(TB_GETBUTTONTEXTW, static_cast<WPARAM>(button.idCommand), 0, length) && length > 0 &&
            length < kTextChars &&
            ask(TB_GETBUTTONTEXTW, static_cast<WPARAM>(button.idCommand), reinterpret_cast<LPARAM>(remote + kTextAt),
                length)) {
            tip.resize(length);
            if (!ReadProcessMemory(explorer, remote + kTextAt, tip.data(), length * sizeof(wchar_t), nullptr)) tip.clear();
        }
        app.name = FirstLine(tip);
        if (app.name.empty()) app.name = FileName(app.exe);
        if (!seen.insert(Lower(app.exe) + L"|" + app.name).second) continue;
        if (data.icon) app.icon = BitmapFromIcon(data.icon);  // 句柄归资源管理器，不能 DestroyIcon
        if (!app.icon) app.icon = ExeIcon(app.exe);
        if (app.icon) app.iconLarge = LargeFor(app.exe, app.icon.get());
        apps.push_back(std::move(app));  // 都在隐藏的图标里，promoted 是 false
    }
    if (!ok) Log(L"托盘：读不了隐藏的图标的工具栏（错误 %lu）", GetLastError());
    if (remote) VirtualFreeEx(explorer, remote, 0, MEM_RELEASE);
    CloseHandle(explorer);
    return ok;
}

std::vector<TrayApp> Load() {
    std::vector<TrayApp> apps;
    if (LegacyTray() && LoadFromToolbar(apps)) return apps;
    return LoadFromSettings();
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

POINT Center(const RECT& r) { return {r.left + (r.right - r.left) / 2, r.top + (r.bottom - r.top) / 2}; }

bool OnScreen(const RECT& r) {
    if (r.right - r.left < 4 || r.bottom - r.top < 4) return false;
    return MonitorFromPoint(Center(r), MONITOR_DEFAULTTONULL) != nullptr;
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

// Win10 的“显示隐藏的图标”是托盘区里一个普通的按钮窗口，名字各种语言不一样：按名字认不出来时按窗口找，放进 legacy。
// 任务栏藏着时它也看不见，算找不到（接着临时显示任务栏再找）
const Button* FindChevron(IUIAutomation* uia, HWND tray, const std::vector<Button>& buttons, Button& legacy) {
    if (const Button* b = FindChevron(buttons)) return b;
    if (!LegacyTray() || !tray) return nullptr;
    HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
    HWND button = notify ? FindWindowExW(notify, nullptr, L"Button", nullptr) : nullptr;
    IUIAutomationElement* e = nullptr;
    if (!button || !IsWindowVisible(button) || FAILED(uia->ElementFromHandle(button, &e)) || !e) return nullptr;
    legacy = Button{};
    legacy.element = Hold(e);
    return &legacy;
}

// 任务栏上程序窗口的按钮（名字是“程序名 - 1 个正在运行的窗口”，也会对上程序名），不是托盘图标
bool IsTaskButton(const Button& b) { return Lower(b.cls).find(L"tasklist") != std::wstring::npos; }

const Button* ButtonAtIcon(const std::vector<Button>& buttons, const RECT& icon) {
    if (!OnScreen(icon)) return nullptr;
    POINT center = Center(icon);
    const Button* match = nullptr;
    for (const Button& b : buttons) {
        if (!PtInRect(&b.rect, center) || !PtInRect(&icon, Center(b.rect))) continue;
        // 溢出面板没打开时，系统可能返回展开按钮的位置。
        if (IsChevron(b) || IsTaskButton(b)) return nullptr;
        if (match && !EqualRect(&match->rect, &b.rect)) return nullptr;
        match = &b;
    }
    return match;
}

const Button* BestMatch(const std::vector<Button>& buttons, const TrayApp& app, bool needOnScreen) {
    if (buttons.empty()) return nullptr;
    // 提示文字可能只有实时状态。用登记的图标标识找屏幕上的按钮。
    for (const GUID& guid : app.iconGuids) {
        NOTIFYICONIDENTIFIER id{};
        id.cbSize = sizeof(id);
        id.guidItem = guid;
        RECT icon{};
        if (FAILED(Shell_NotifyIconGetRect(&id, &icon))) continue;
        if (const Button* b = ButtonAtIcon(buttons, icon)) {
            Log(L"托盘：按图标 GUID 找到 %ls", app.name.c_str());
            return b;
        }
    }

    const Button* best = nullptr;
    int bestScore = 0;
    for (const Button& b : buttons) {
        if (IsChevron(b) || IsTaskButton(b) || (needOnScreen && !OnScreen(b.rect))) continue;
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

bool SendClick(POINT at, bool right, const POINT* returnTo) {
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1), vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
    if (GetSystemMetrics(SM_SWAPBUTTON)) right = !right;  // 左右键对调了：发的是物理按键
    auto move = [&](POINT p) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = static_cast<LONG>((p.x - vx) * 65535LL / vw);
        input.mi.dy = static_cast<LONG>((p.y - vy) * 65535LL / vh);
        input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
        return input;
    };
    const INPUT atIcon = move(at);
    INPUT in[4] = {atIcon, atIcon, atIcon};
    in[1].mi.dwFlags |= right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    in[2].mi.dwFlags |= right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    // 同一批输入里右键并回到原位，不在系统托盘停留等待菜单。
    UINT count = 3;
    if (returnTo) in[count++] = move(*returnTo);
    UINT sent = SendInput(count, in, sizeof(INPUT));
    if (sent >= 2 && sent < count) sent += SendInput(count - sent, in + sent, sizeof(INPUT));
    if (returnTo && sent && sent < count && SendInput(1, in + count - 1, sizeof(INPUT)) != 1) {
        POINT now{};
        if (GetCursorPos(&now) && std::abs(now.x - at.x) <= 2 && std::abs(now.y - at.y) <= 2)
            SetCursorPos(returnTo->x, returnTo->y);
    }
    return sent >= 3;
}

// 展开的“隐藏的图标”：Win11 是 XAML 的窗口，Win10 是老的 NotifyIconOverflowWindow。两种都有时用显示着的那个
HWND OverflowWindow() {
    HWND found = nullptr;
    for (const wchar_t* cls : {L"TopLevelWindowForOverflowXamlIsland", L"NotifyIconOverflowWindow"}) {
        HWND h = FindWindowW(cls, nullptr);
        if (h && IsWindowVisible(h)) return h;
        if (!found) found = h;
    }
    return found;
}

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
    return !exe.empty() && Lower(GetProcessPath(GetForegroundWindow())) == Lower(exe);
}

// ---- 点的时候遮住 ----
// 任务栏藏着时，要点隐藏的图标只能临时把任务栏显示出来、展开“隐藏的图标”（资源管理器这时还会把任务栏升上来），
// 屏幕上会闪一下。点之前把这两个窗口调成几乎全透明（不透明度 1/255：看不见，鼠标照样点得中），点完、藏回去以后再放回来。
// 崩溃、被结束时没放回的，下次启动、退出时按这个特别的不透明度认出来放回（Tray_RepairVeil）
constexpr BYTE kVeilAlpha = 1;

struct Veiled {
    HWND hwnd;
    bool hadLayered;  // 本来就是分层窗口：放回原来的透明设置；不是：去掉分层
    COLORREF key;
    BYTE alpha;
    DWORD flags;
};
std::mutex s_veilLock;
std::vector<Veiled> s_veiled;
std::atomic<bool> s_clicking{false};

std::wstring ClassOf(HWND h) {
    wchar_t cls[96] = L"";
    GetClassNameW(h, cls, ARRAYSIZE(cls));
    return cls;
}

void Veil(HWND h) {
    if (!h || !IsWindow(h)) return;
    std::lock_guard<std::mutex> guard(s_veilLock);
    for (const Veiled& v : s_veiled) {
        if (v.hwnd != h) continue;
        LONG_PTR style = GetWindowLongPtrW(h, GWL_EXSTYLE);
        BYTE alpha = 255;
        DWORD flags = 0;
        if (!(style & WS_EX_LAYERED)) SetWindowLongPtrW(h, GWL_EXSTYLE, style | WS_EX_LAYERED);
        if (!GetLayeredWindowAttributes(h, nullptr, &alpha, &flags) || !(flags & LWA_ALPHA) || alpha != kVeilAlpha)
            SetLayeredWindowAttributes(h, 0, kVeilAlpha, LWA_ALPHA);
        return;
    }
    Veiled v{h, false, 0, 255, 0};
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_LAYERED) {
        if (!GetLayeredWindowAttributes(h, &v.key, &v.alpha, &v.flags)) {  // 自己用 UpdateLayeredWindow 画的：不动它
            Log(L"托盘：%ls 是自己画透明的分层窗口，没法遮住", ClassOf(h).c_str());
            return;
        }
        v.hadLayered = true;
    } else {
        SetWindowLongPtrW(h, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    }
    if (!SetLayeredWindowAttributes(h, 0, kVeilAlpha, LWA_ALPHA)) {
        DWORD err = GetLastError();
        if (!v.hadLayered) SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) & ~WS_EX_LAYERED);
        Log(L"托盘：%ls 调不成透明（错误 %lu），会闪一下", ClassOf(h).c_str(), err);
        return;
    }
    s_veiled.push_back(v);
    Log(L"托盘：先把 %ls 调成透明再点", ClassOf(h).c_str());
}

void Unveil(const Veiled& v) {
    if (!IsWindow(v.hwnd)) return;
    if (v.hadLayered) {
        SetLayeredWindowAttributes(v.hwnd, v.key, v.alpha, v.flags);
    } else {
        SetWindowLongPtrW(v.hwnd, GWL_EXSTYLE, GetWindowLongPtrW(v.hwnd, GWL_EXSTYLE) & ~WS_EX_LAYERED);
        RedrawWindow(v.hwnd, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }
}

bool AnyVeiled() {
    std::lock_guard<std::mutex> guard(s_veilLock);
    return !s_veiled.empty();
}

bool IsVeiled(HWND h) {
    std::lock_guard<std::mutex> guard(s_veilLock);
    for (const Veiled& v : s_veiled)
        if (v.hwnd == h) return true;
    return false;
}

void UnveilAll() {
    std::lock_guard<std::mutex> guard(s_veilLock);
    if (s_clicking) return;  // 开始新点击和恢复透明度共用这把锁
    for (const Veiled& v : s_veiled) Unveil(v);
    if (!s_veiled.empty()) Log(L"托盘：点完了，任务栏和隐藏的图标放回不透明");
    s_veiled.clear();
}

struct Job {
    TrayApp app;
    bool right = false;
    bool overflowOnly = false;  // 只展开“隐藏的图标”
    TrayMenuAnchor anchor;
};
std::atomic<bool> s_hideOwed{false};  // 临时显示的任务栏还没藏回去时又点了一个：留给那一次点完再藏
std::atomic<HWND> s_openMenu{nullptr};

std::vector<HWND> TrayMenus() {
    std::vector<HWND> menus;
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        if (Tray_IsMenuWindow(hwnd)) reinterpret_cast<std::vector<HWND>*>(param)->push_back(hwnd);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&menus));
    return menus;
}

POINT TrayMenuPosition(const TrayMenuAnchor& anchor, const RECT& work, SIZE menu) {
    POINT pos = anchor.at;
    if (anchor.edge == kEdgeBottom) pos.y -= menu.cy;
    if (anchor.edge == kEdgeRight) pos.x -= menu.cx;
    pos.x = std::clamp(pos.x, work.left, std::max(work.left, work.right - menu.cx));
    pos.y = std::clamp(pos.y, work.top, std::max(work.top, work.bottom - menu.cy));
    return pos;
}

bool PlaceTrayMenu(HWND hwnd, const TrayMenuAnchor& anchor) {
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromPoint(anchor.at, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    constexpr UINT flags = SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER;
    int moves = 0;
    bool confirmed = false;
    for (;;) {
        RECT frame{};
        if (!IsWindowVisible(hwnd) || !GetWindowRect(hwnd, &frame) || IsRectEmpty(&frame)) return false;
        POINT pos = TrayMenuPosition(anchor, monitor.rcWork, {frame.right - frame.left, frame.bottom - frame.top});
        bool atAnchor = pos.x == frame.left && pos.y == frame.top;
        if (atAnchor && confirmed) return true;
        confirmed = atAnchor;
        if (!atAnchor && (++moves > 3 || !SetWindowPos(hwnd, nullptr, pos.x, pos.y, 0, 0, flags))) return false;
        // 等目标线程应用位置。跨屏 DPI 变化也可能改变菜单大小。
        Sleep(30);
    }
}

bool MenuBelongsToApp(HWND menu, const std::wstring& exe) {
    if (Lower(GetProcessPath(menu)) == exe) return true;
    for (HWND owner = GetWindow(menu, GW_OWNER); owner; owner = GetWindow(owner, GW_OWNER))
        if (Lower(GetProcessPath(owner)) == exe) return true;
    if (ClassOf(menu) != L"#32768") return false;
    GUITHREADINFO info{sizeof(info)};
    DWORD thread = GetWindowThreadProcessId(menu, nullptr);
    return thread && GetGUIThreadInfo(thread, &info) && Lower(GetProcessPath(info.hwndMenuOwner)) == exe;
}

bool MenuAppearedNearClick(HWND menu, POINT trayPoint, POINT panelPoint) {
    if (ClassOf(menu) == L"#32768") return true;
    RECT frame{};
    if (!GetWindowRect(menu, &frame) || IsRectEmpty(&frame)) return false;
    const int margin = static_cast<int>(96 * MonitorScale(MonitorFromRect(&frame, MONITOR_DEFAULTTONEAREST)));
    InflateRect(&frame, margin, margin);
    return PtInRect(&frame, trayPoint) || PtInRect(&frame, panelPoint);
}

// 窗口显示事件唤醒检查。定时检查也覆盖只报内部控件事件的程序。
void CALLBACK WakeOnShow(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {}

void WaitForTrayMenu(std::vector<HWND> seen, const Job& job, POINT trayPoint) {
    const auto exe = Lower(job.app.exe);
    const auto name = FileName(job.app.exe);
    const ULONGLONG start = GetTickCount64();
    do {
        for (HWND menu : TrayMenus()) {
            if (std::find(seen.begin(), seen.end(), menu) != seen.end() ||
                !MenuAppearedNearClick(menu, trayPoint, job.anchor.at) || !MenuBelongsToApp(menu, exe)) continue;
            seen.push_back(menu);
            s_openMenu = menu;
            if (PlaceTrayMenu(menu, job.anchor)) {
                Log(L"托盘：已确认 %ls 的菜单定位（%ls）", name.c_str(), ClassOf(menu).c_str());
                return;
            }
            Log(L"托盘：%ls 的菜单位置未确认（%ls）", name.c_str(), ClassOf(menu).c_str());
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    } while (GetTickCount64() - start < 1200);
    if (!s_openMenu.load()) Log(L"托盘：%ls 右键后未识别到可定位的弹出菜单", name.c_str());
}

bool ClickAt(const RECT& rect, const Job& job) {
    const bool placeMenu = job.right && job.anchor.edge >= 0 && !job.app.exe.empty();
    auto before = placeMenu ? TrayMenus() : std::vector<HWND>{};
    const POINT target = Center(rect);
    HWINEVENTHOOK hook = placeMenu ? SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, WakeOnShow,
                                                    0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNTHREAD) : nullptr;
    POINT saved{};
    bool haveCursor = GetCursorPos(&saved) != FALSE;
    if (job.right && !haveCursor) {
        if (hook) UnhookWinEvent(hook);
        Log(L"托盘：读取不到光标坐标，无法右键 %ls", FileName(job.app.exe).c_str());
        return false;
    }
    bool clicked = SendClick(target, job.right, job.right ? &saved : nullptr);
    if (clicked && placeMenu) {
        // 只移动这次点击后出现、属于该应用的菜单。
        WaitForTrayMenu(std::move(before), job, target);
    }
    if (hook) UnhookWinEvent(hook);
    if (!job.right && haveCursor) {
        if (clicked) Sleep(150);
        SetCursorPos(saved.x, saved.y);
    }
    if (!clicked) Log(L"托盘：无法模拟点击 %ls", FileName(job.app.exe).c_str());
    return clicked;
}

void LaunchOnClickFailure(const Job& job) {
    if (job.right || job.overflowOnly) return;
    Log(L"托盘：找不到 %ls 的托盘图标，改成再启动一次它", job.app.name.c_str());
    auto exe = std::make_unique<std::wstring>(job.app.exe);
    if (PostMessageW(g_mainWnd, WM_APP_TRAY_LAUNCH, 0, reinterpret_cast<LPARAM>(exe.get()))) exe.release();
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

// 再按一次“显示隐藏的图标”，把开着的隐藏的图标收起来
void Collapse(IUIAutomation* uia, HWND tray) {
    if (!OverflowShowing()) return;
    std::vector<Button> buttons = TaskbarButtons(uia, tray);
    Button legacyChevron;
    if (const Button* chevron = FindChevron(uia, tray, buttons, legacyChevron)) Press(*chevron);
}

// 返回是否在系统托盘里点到图标或展开了隐藏的图标。
bool RunJob(const Job& job) {
    Ref<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(kClsidUIAutomation, nullptr, CLSCTX_INPROC_SERVER, kIidUIAutomation,
                                reinterpret_cast<void**>(&uia.p))) || !uia.p) {
        Log(L"托盘：用不了 UI 自动化");
        LaunchOnClickFailure(job);
        return false;
    }
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    std::vector<Button> onTaskbar = TaskbarButtons(uia.p, tray);
    Names(L"任务栏", onTaskbar);

    // Win10 的托盘是老式工具栏：“调用”它不一定会把点击转给程序，任务栏上同名的程序按钮也分不出来。
    // 列出来的本来就都在隐藏的图标里，直接展开它真点
    if (!job.overflowOnly && !LegacyTray()) {
        // 左键：直接“调用”按钮，什么都不用弹出来
        if (!job.right) {
            std::vector<Button> inOverflow = ButtonsIn(uia.p, OverflowWindow());
            Names(L"隐藏的图标", inOverflow);
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
                if (ClickAt(b->rect, job)) return true;
            }
    }

    // 展开“隐藏的图标”，在里面点。只是去点里面的一个图标（不是要看隐藏的图标）时先遮住：
    // 藏着的任务栏（临时显示、资源管理器展开时自己升上来都看不见）和隐藏的图标那个窗口
    HWND overflow = OverflowWindow();
    bool veil = !job.overflowOnly;
    bool expanded = false;  // 隐藏的图标是这次展开的
    if (veil && tray && !IsWindowVisible(tray)) Veil(tray);
    if (!overflow || !IsWindowVisible(overflow)) {
        if (veil) Veil(overflow);
        Button legacyChevron;
        const Button* chevron = FindChevron(uia.p, tray, onTaskbar, legacyChevron);
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
                chevron = FindChevron(uia.p, tray, onTaskbar, legacyChevron);
            }
            Log(L"托盘：临时显示任务栏以后%ls“显示隐藏的图标”按钮", chevron ? L"找到了" : L"还是找不到");
            Names(L"临时显示的任务栏", onTaskbar);
        }
        // 展开隐藏的图标时资源管理器会把藏着的任务栏升上来：先告诉主线程这是点托盘临时显示的（遮着），
        // 别当成“又冒出来了”带动画地再藏（动画用的截图会闪一下），点完由 ClickThread 藏回去
        if (chevron && veil && tray && !IsWindowVisible(tray) && IsVeiled(tray))
            SendMessageTimeoutW(g_mainWnd, WM_APP_TRAY_PEEK, 2, reinterpret_cast<LPARAM>(tray), SMTO_ABORTIFHUNG, 1000,
                                &ours);
        if (!chevron || !Press(*chevron)) {
            Log(L"托盘：没能展开隐藏的图标（%ls）", chevron ? L"按钮按不动" : L"找不到按钮");
            LaunchOnClickFailure(job);
            return false;
        }
        for (int i = 0; i < 200; ++i) {  // 最多等 1 秒。勤着看：展开时才建出来的窗口要赶在画出第一帧前遮上
            overflow = OverflowWindow();
            if (veil) Veil(overflow);
            if (overflow && IsWindowVisible(overflow)) break;
            Sleep(5);
        }
        if (!overflow || !IsWindowVisible(overflow)) {
            Log(L"托盘：按了展开隐藏的图标，但没有展开");
            LaunchOnClickFailure(job);
            return false;
        }
        expanded = true;
        // 等按钮到位。展开动画期间资源管理器可能重设窗口样式和不透明度。
        const bool coveredOverflow = veil && IsVeiled(overflow);
        const bool coveredTray = veil && IsVeiled(tray);
        if (coveredOverflow || coveredTray) {
            const ULONGLONG settleUntil = GetTickCount64() + 200;
            while (GetTickCount64() < settleUntil) {
                Sleep(16);
                if (coveredOverflow) Veil(overflow);
                if (coveredTray) Veil(tray);
            }
        } else {
            Sleep(200);
        }
    }
    if (job.overflowOnly) return true;
    std::vector<Button> inOverflow = ButtonsIn(uia.p, overflow);
    Names(L"展开后隐藏的图标", inOverflow);
    if (const Button* b = BestMatch(inOverflow, job.app, true)) {
        RECT r = b->rect;
        // 左键：展开以后按钮能“调用”了，先这样点，鼠标不用挪过去（Win10 的老式托盘“调用”不一定转给程序，照旧真点）
        if (!job.right && !LegacyTray() && Press(*b)) {
            Log(L"托盘：展开后调用了 %ls 的托盘图标", b->name.c_str());
            // 真点时隐藏的图标会自己收起，“调用”的有时还开着：等一会儿（收起动画放完）还开着、程序也没弹菜单，就再按一次收起来
            for (int i = 0; i < 8 && OverflowShowing() && !MenuShowing(); ++i) Sleep(50);
            if (OverflowShowing() && !MenuShowing() && !ForegroundIsApp(job.app.exe)) Collapse(uia.p, tray);
            return true;
        }
        Log(L"托盘：%ls隐藏图标里的 %ls（%ld,%ld - %ld,%ld）", job.right ? L"右键" : L"点", b->name.c_str(), r.left,
            r.top, r.right, r.bottom);
        if (ClickAt(r, job)) return true;
    } else {
        Log(L"托盘：隐藏的图标里找不到 %ls", job.app.name.c_str());
    }
    // 这次展开的：趁还遮着收起来（ClickThread 等它收好再放回不透明），不然一放回就露出来
    if (expanded) Collapse(uia.p, tray);
    LaunchOnClickFailure(job);
    return false;
}

DWORD WINAPI ClickThread(LPVOID param) {
    std::unique_ptr<Job> job(static_cast<Job*>(param));
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    bool trayHidden = tray && !IsWindowVisible(tray);
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool clicked = RunJob(*job);
    HWND menu = s_openMenu.load();
    if (SUCCEEDED(co)) CoUninitialize();
    s_clicking = false;
    // 任务栏本来藏着：为了点隐藏的图标临时显示了，或者资源管理器展开隐藏的图标时自己把它显示了出来。
    // 点了的话等隐藏的图标和程序的菜单都关了，没点成就等这次展开的收起来，让主线程藏回去（不是本程序藏的、已经不用藏了的主线程不管）
    bool owed = s_hideOwed.exchange(false);
    if (trayHidden || owed) {
        // 点了：等隐藏的图标和程序的菜单都关了（最多 1 分钟）。没点成：等遮着的隐藏的图标收起来（最多 2 秒）
        for (int i = 0; i < (clicked ? 600 : 20); ++i) {
            bool open = clicked ? OverflowShowing() || MenuShowing() || Tray_IsMenuWindow(menu)
                                : OverflowShowing() && IsVeiled(OverflowWindow());
            if (!open) break;
            Sleep(100);
        }
        if (s_clicking) {
            s_hideOwed = true;
        } else {
            // 藏回去了再放回不透明。资源管理器收起隐藏的图标以后还可能自己把任务栏又显示出来：
            // 藏好以后再遮半秒，冒出来就再藏（不是本程序藏的了就不管，马上放回）
            for (int round = 0; round < 3 && !s_clicking; ++round) {
                if (IsWindowVisible(tray) || round == 0) {  // 没显示出来也告诉主线程点完了（展开前记下的作废）
                    if (round) Log(L"托盘：任务栏藏回去以后又被显示出来，遮着再藏一次");
                    DWORD_PTR ours = 0;
                    if (!SendMessageTimeoutW(g_mainWnd, WM_APP_TRAY_PEEK, 0, reinterpret_cast<LPARAM>(tray),
                                             SMTO_ABORTIFHUNG, 1000, &ours) ||
                        !ours)
                        break;
                    for (int i = 0; i < 20 && IsWindowVisible(tray); ++i) Sleep(50);
                    if (IsWindowVisible(tray)) break;
                }
                if (!AnyVeiled()) break;  // 没遮（只是展开看看）：不用再等
                int i = 0;
                while (i < 10 && !IsWindowVisible(tray) && !s_clicking) {
                    Sleep(50);
                    ++i;
                }
                if (i == 10) break;  // 一直藏着
            }
            if (s_clicking) s_hideOwed = true;  // 等的时候又点了一个：留给那一次藏
        }
    } else {
        for (int i = 0; AnyVeiled() && i < 600 && (OverflowShowing() || Tray_IsMenuWindow(menu)); ++i) Sleep(100);
    }
    UnveilAll();  // 又点了一个时由那一次放回
    return 0;
}

void StartJob(Job* job) {
    {
        std::lock_guard<std::mutex> guard(s_veilLock);
        if (s_clicking.exchange(true)) {  // 上一次还没点完
            delete job;
            return;
        }
        s_openMenu = nullptr;
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

void Tray_Click(const TrayApp& app, bool right, TrayMenuAnchor anchor) { StartJob(new Job{app, right, false, anchor}); }

bool Tray_IsMenuWindow(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return false;
    std::wstring cls = GetClassNameStr(hwnd);
    if (cls == L"#32768") return true;
    if (cls == TOOLTIPS_CLASSW || cls == L"TopLevelWindowForOverflowXamlIsland" || cls == L"NotifyIconOverflowWindow" ||
        cls == L"Shell_TrayWnd" || cls == L"Shell_SecondaryTrayWnd") return false;
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE), ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    return (style & WS_POPUP) && (style & WS_CAPTION) != WS_CAPTION && !(style & WS_DISABLED) &&
           !(ex & (WS_EX_TRANSPARENT | WS_EX_APPWINDOW)) &&
           ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) || GetWindow(hwnd, GW_OWNER)) && !IsCloaked(hwnd);
}

bool Tray_MenuShowing() { return Tray_IsMenuWindow(s_openMenu.load()); }

void Tray_ShowHidden() { StartJob(new Job{TrayApp{}, false, true}); }

bool Tray_Busy() { return s_clicking; }

void Tray_RepairVeil() {
    for (const wchar_t* cls : {L"Shell_TrayWnd", L"TopLevelWindowForOverflowXamlIsland", L"NotifyIconOverflowWindow"}) {
        HWND h = FindWindowW(cls, nullptr);
        if (!h || !(GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_LAYERED)) continue;
        COLORREF key = 0;
        BYTE alpha = 255;
        DWORD flags = 0;
        if (!GetLayeredWindowAttributes(h, &key, &alpha, &flags) || !(flags & LWA_ALPHA) || alpha != kVeilAlpha) continue;
        SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
        SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) & ~WS_EX_LAYERED);
        RedrawWindow(h, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
        Log(L"托盘：%ls 还是上次点托盘图标时调的透明，放回来", cls);
    }
}

}  // namespace app
