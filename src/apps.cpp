// 打字筛选时用到的“所有应用”：开始菜单“所有应用”里的程序（shell:AppsFolder，桌面程序和应用商店应用都有）。
// 读一遍要一两百毫秒，所以在后台线程上读，读完通知迷你任务栏；结果缓存 5 分钟。
#include "common.h"

#include <atomic>
#include <mutex>
#include <thread>

namespace app {
namespace {

// MinGW 的 uuid 库里不一定有，自己定义
constexpr GUID kFolderIdAppsFolder = {0x1e87508d, 0x89c2, 0x42f0, {0x8a, 0x7e, 0x64, 0x5a, 0x0f, 0x50, 0xca, 0x58}};
constexpr GUID kBhidEnumItems = {0x94f60519, 0x2850, 0x4924, {0xaa, 0x5a, 0xd1, 0x5e, 0x84, 0x86, 0x80, 0x39}};
constexpr DWORD kRefreshMs = 5 * 60 * 1000;

std::mutex s_lock;
std::vector<InstalledApp> s_apps;
DWORD s_loadedAt = 0;  // 0 = 还没读过
std::atomic<bool> s_loading{false};
std::thread s_thread;

std::wstring TakeName(IShellItem* item, SIGDN form) {
    PWSTR text = nullptr;
    std::wstring result;
    if (SUCCEEDED(item->GetDisplayName(form, &text)) && text) {
        result = text;
        CoTaskMemFree(text);
    }
    return result;
}

std::vector<InstalledApp> Enumerate() {
    std::vector<InstalledApp> apps;
    IShellItem* folder = nullptr;
    if (FAILED(SHGetKnownFolderItem(kFolderIdAppsFolder, KF_FLAG_DEFAULT, nullptr, IID_IShellItem,
                                    reinterpret_cast<void**>(&folder))))
        return apps;
    IEnumShellItems* items = nullptr;
    if (SUCCEEDED(folder->BindToHandler(nullptr, kBhidEnumItems, IID_IEnumShellItems, reinterpret_cast<void**>(&items)))) {
        IShellItem* item = nullptr;
        while (items->Next(1, &item, nullptr) == S_OK) {
            InstalledApp app;
            app.name = TakeName(item, SIGDN_NORMALDISPLAY);
            std::wstring id = TakeName(item, SIGDN_PARENTRELATIVEPARSING);  // AppUserModelID，或者 {已知文件夹}\程序路径
            item->Release();
            std::wstring lower = ToLower(app.name);
            // 卸载程序、说明文档之类的条目搜出来也没用
            if (app.name.empty() || id.empty() || lower.find(L"uninstall") != std::wstring::npos ||
                lower.find(L"卸载") != std::wstring::npos)
                continue;
            app.launch = L"shell:AppsFolder\\" + id;
            app.search = lower + L"\n" + ToLower(id);
            apps.push_back(std::move(app));
        }
        items->Release();
    }
    folder->Release();
    return apps;
}

}  // namespace

void Apps_Refresh(HWND notify) {
    if (s_loading) return;
    {
        std::lock_guard<std::mutex> guard(s_lock);
        if (s_loadedAt && GetTickCount() - s_loadedAt < kRefreshMs) return;
    }
    if (s_thread.joinable()) s_thread.join();
    s_loading = true;
    s_thread = std::thread([notify] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        double start = NowMs();
        std::vector<InstalledApp> apps = Enumerate();
        Log(L"读到 %d 个应用，用了 %.0f 毫秒", static_cast<int>(apps.size()), NowMs() - start);
        {
            std::lock_guard<std::mutex> guard(s_lock);
            s_apps = std::move(apps);
            s_loadedAt = std::max<DWORD>(1, GetTickCount());
        }
        s_loading = false;
        PostMessageW(notify, WM_APP_APPS, 0, 0);
        if (SUCCEEDED(co)) CoUninitialize();
    });
}

std::vector<InstalledApp> Apps_Match(const std::wstring& filter, size_t max) {
    struct Hit {
        int score;
        const InstalledApp* app;
    };
    std::vector<InstalledApp> result;
    std::lock_guard<std::mutex> guard(s_lock);
    std::vector<Hit> hits;
    for (const InstalledApp& a : s_apps) {
        size_t pos = a.search.find(filter);
        if (pos == std::wstring::npos) continue;
        size_t nameEnd = a.search.find(L'\n');
        // 名字开头 > 名字里某个词的开头 > 名字中间 > 只有程序 ID 里有
        int score = pos == 0 ? 0 : pos > nameEnd ? 3 : a.search[pos - 1] == L' ' ? 1 : 2;
        hits.push_back({score, &a});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) {
        return x.score != y.score ? x.score < y.score : x.app->name.size() < y.app->name.size();
    });
    for (size_t i = 0; i < hits.size() && i < max; ++i) result.push_back(*hits[i].app);
    return result;
}

void Apps_Stop() {
    if (!s_thread.joinable()) return;
    // 后台还在读开始菜单（某个应用或外壳扩展卡住时可能一直读不完）：最多等半秒，等不到就不等了，
    // 进程最后用 TerminateProcess 结束
    for (int i = 0; i < 50 && s_loading; ++i) Sleep(10);
    if (s_loading) {
        Log(L"读应用列表的线程没有及时结束");
        s_thread.detach();
    } else {
        s_thread.join();
    }
}

}  // namespace app
