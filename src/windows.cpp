// 按任务栏 / Alt+Tab 的规则列出当前桌面上的应用窗口，并取图标。
#include "common.h"

#include <map>
#include <unordered_map>
#include <unordered_set>

namespace app {
namespace {

// 记住每个窗口第一次出现的顺序，避免图标随 Z 序每次跳来跳去
std::unordered_map<HWND, long long> s_order;
long long s_seq = 0;
std::map<std::wstring, std::shared_ptr<Gdiplus::Bitmap>> s_exeIconCache;

HICON QueryIcon(HWND h, WPARAM type) {
    DWORD_PTR result = 0;
    SendMessageTimeoutW(h, WM_GETICON, type, 0, SMTO_ABORTIFHUNG, 50, &result);
    return reinterpret_cast<HICON>(result);
}

std::shared_ptr<Gdiplus::Bitmap> GetIcon(HWND h, const std::wstring& exePath) {
    HICON icon = QueryIcon(h, ICON_BIG);
    if (!icon) icon = QueryIcon(h, ICON_SMALL2);
    if (!icon) icon = QueryIcon(h, ICON_SMALL);
    if (!icon) icon = reinterpret_cast<HICON>(GetClassLongPtrW(h, GCLP_HICON));
    if (!icon) icon = reinterpret_cast<HICON>(GetClassLongPtrW(h, GCLP_HICONSM));
    if (icon) {
        // 这些句柄归目标窗口所有，不能 DestroyIcon
        if (auto bmp = BitmapFromIcon(icon)) return bmp;
    }

    if (exePath.empty()) return nullptr;
    auto it = s_exeIconCache.find(exePath);
    if (it != s_exeIconCache.end()) return it->second;

    std::shared_ptr<Gdiplus::Bitmap> bmp;
    SHFILEINFOW sfi = {};
    if (SHGetFileInfoW(exePath.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon) {
        bmp = BitmapFromIcon(sfi.hIcon);
        DestroyIcon(sfi.hIcon);
    }
    s_exeIconCache[exePath] = bmp;
    return bmp;
}

// 系统设置里“在所有显示器上显示任务栏”开着、“任务栏应用显示在”不是“所有任务栏”时，
// 这块屏幕上的任务栏只显示自己屏幕上的窗口（选的是“主任务栏和窗口所在的任务栏”时主屏照样显示全部）
bool OnlyOwnScreen(HMONITOR monitor) {
    if (!monitor || GetSystemMetrics(SM_CMONITORS) < 2) return false;
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
    DWORD enabled = 0, mode = 0, size = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"MMTaskbarEnabled", RRF_RT_REG_DWORD, nullptr, &enabled, &size) != ERROR_SUCCESS ||
        !enabled)
        return false;
    size = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"MMTaskbarMode", RRF_RT_REG_DWORD, nullptr, &mode, &size) != ERROR_SUCCESS)
        return false;
    if (mode == 0) return false;
    MONITORINFO mi = {sizeof(mi)};
    bool primary = GetMonitorInfoW(monitor, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY);
    return !(mode == 1 && primary);
}

}  // namespace

bool IsTaskbarWindow(HWND h) {
    if (!IsWindowVisible(h)) return false;
    if (GetWindowTextLengthW(h) == 0) return false;
    if (IsOwnProcess(h)) return false;

    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    bool appWindow = (ex & WS_EX_APPWINDOW) != 0;
    if (!appWindow) {
        if (ex & WS_EX_TOOLWINDOW) return false;
        if (ex & WS_EX_NOACTIVATE) return false;
        if (GetWindow(h, GW_OWNER)) return false;
    }

    // 其它虚拟桌面上的窗口、后台挂起的 UWP 窗口都是 cloaked 状态
    if (IsCloaked(h)) return false;

    std::wstring cls = GetClassNameStr(h);
    return cls != L"Progman" && cls != L"WorkerW" && cls != L"Shell_TrayWnd" && cls != L"Shell_SecondaryTrayWnd";
}

std::vector<WindowEntry> EnumerateWindows(HWND foreground, int iconPx, HMONITOR monitor) {
    std::vector<HWND> handles;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            if (IsTaskbarWindow(h)) reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&handles));

    // EnumWindows 按 Z 序从上到下，新出现的窗口倒序编号，让较早的窗口靠左。
    // 别的屏幕上的窗口也编号，弹在不同的屏幕上时顺序不跳
    for (auto it = handles.rbegin(); it != handles.rend(); ++it)
        if (!s_order.count(*it)) s_order[*it] = s_seq++;
    std::unordered_set<HWND> alive(handles.begin(), handles.end());
    for (auto it = s_order.begin(); it != s_order.end();)
        it = alive.count(it->first) ? std::next(it) : s_order.erase(it);

    // 最小化的窗口按它还原后的位置算
    if (OnlyOwnScreen(monitor))
        handles.erase(std::remove_if(handles.begin(), handles.end(),
                                     [&](HWND h) { return MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) != monitor; }),
                      handles.end());

    std::sort(handles.begin(), handles.end(), [](HWND a, HWND b) { return s_order[a] < s_order[b]; });

    HWND activeRoot = foreground ? GetAncestor(foreground, GA_ROOTOWNER) : nullptr;
    std::vector<WindowEntry> result;
    result.reserve(handles.size());
    for (HWND h : handles) {
        WindowEntry e;
        e.hwnd = h;
        e.title = GetWindowTitle(h);
        e.exePath = ToLower(GetProcessPath(AppWindowTarget(h)));
        e.aumid = GetWindowAumid(h);
        // 应用商店应用的框架窗口给不出应用自己的图标，按 AppUserModelID 向外壳要（带缓存）
        if (!e.aumid.empty() && GetClassNameStr(h) == L"ApplicationFrameWindow")
            e.icon = ShellItemIcon(L"shell:AppsFolder\\" + e.aumid, iconPx);
        if (!e.icon) e.icon = GetIcon(h, e.exePath);
        e.active = h == foreground || h == activeRoot;
        result.push_back(std::move(e));
    }
    return result;
}

void Windows_ClearCache() { s_exeIconCache.clear(); }

}  // namespace app
