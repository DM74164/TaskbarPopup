// 读取用户固定在任务栏上的应用。
//
//   - 普通桌面程序：%APPDATA%\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar 里的 .lnk
//   - 应用商店应用：没有 .lnk，只以 AppUserModelID 的形式记录在注册表 Taskband\Favorites 里
//
// Favorites 是资源管理器内部的二进制格式，这里不完整解析，只在里面查找各个 .lnk 文件名和
// AppUserModelID 字符串，按它们第一次出现的位置排序，得到和任务栏上一致的顺序。
#include "common.h"

#include <commoncontrols.h>

#include <cwctype>
#include <map>

namespace app {
namespace {

constexpr wchar_t kTaskbandKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband";
constexpr size_t kNotFound = static_cast<size_t>(-1);
// IID_IImageList，MinGW 的 uuid 库里没有，自己定义
constexpr IID kIidImageList = {0x46EB5926, 0x582E, 0x4017, {0x9F, 0xDF, 0xE8, 0x99, 0x8D, 0xAA, 0x09, 0x50}};

struct Cache {
    FILETIME folderTime = {};
    std::vector<BYTE> favorites;
    int iconPx = 0;
    bool valid = false;
    std::vector<PinnedApp> apps;
};
Cache s_cache;
std::map<std::wstring, std::shared_ptr<Gdiplus::Bitmap>> s_iconCache;  // key: 启动路径 + 尺寸

std::wstring PinnedFolder() {
    wchar_t appdata[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return {};
    return std::wstring(appdata) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar";
}

FILETIME FolderWriteTime(const std::wstring& folder) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    GetFileAttributesExW(folder.c_str(), GetFileExInfoStandard, &data);
    return data.ftLastWriteTime;
}

std::vector<BYTE> ReadFavorites() {
    std::vector<BYTE> data;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kTaskbandKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return data;
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, L"Favorites", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_BINARY &&
        size > 0) {
        data.resize(size);
        if (RegQueryValueExW(key, L"Favorites", nullptr, nullptr, data.data(), &size) != ERROR_SUCCESS) data.clear();
        else data.resize(size);
    }
    RegCloseKey(key);
    return data;
}

wchar_t CharAt(const std::vector<BYTE>& blob, size_t offset) {
    return static_cast<wchar_t>(blob[offset] | (blob[offset + 1] << 8));
}

// 在二进制数据里找 UTF-16 字符串（不区分大小写，不要求两字节对齐），返回第一次出现的位置
size_t FindUtf16(const std::vector<BYTE>& blob, const std::wstring& needle) {
    size_t n = needle.size();
    if (n == 0 || blob.size() < n * 2) return kNotFound;
    for (size_t o = 0; o + n * 2 <= blob.size(); ++o) {
        size_t i = 0;
        while (i < n && std::towlower(CharAt(blob, o + 2 * i)) == std::towlower(needle[i])) ++i;
        if (i == n) return o;
    }
    return kNotFound;
}

bool IsAumidChar(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' ||
           c == L'_' || c == L'-' || c == L'!';
}

// 应用商店应用的 AppUserModelID 形如 Microsoft.WindowsStore_8wekyb3d8bbwe!App：
// 包名 + "_" + 13 位发布者哈希 + "!" + 应用 ID
bool LooksLikeAumid(const std::wstring& s) {
    size_t bang = s.find(L'!');
    if (bang == std::wstring::npos || bang + 1 >= s.size() || s.find(L'!', bang + 1) != std::wstring::npos) return false;
    size_t underscore = s.rfind(L'_', bang);
    if (underscore == std::wstring::npos || underscore == 0 || bang - underscore - 1 != 13) return false;
    for (size_t i = underscore + 1; i < bang; ++i)
        if (!((s[i] >= L'a' && s[i] <= L'z') || (s[i] >= L'0' && s[i] <= L'9'))) return false;
    return true;
}

// 找出 Favorites 里所有 AppUserModelID，返回 (位置, 小写 ID)
std::vector<std::pair<size_t, std::wstring>> FindAumids(const std::vector<BYTE>& blob) {
    std::map<std::wstring, size_t> found;
    auto flush = [&](std::wstring& run, size_t start) {
        if (run.size() >= 16 && LooksLikeAumid(run)) {
            std::wstring id = ToLower(run);
            auto it = found.find(id);
            if (it == found.end() || start < it->second) found[id] = start;
        }
        run.clear();
    };
    for (size_t align = 0; align < 2; ++align) {
        std::wstring run;
        size_t start = 0;
        for (size_t o = align; o + 1 < blob.size(); o += 2) {
            wchar_t c = CharAt(blob, o);
            if (IsAumidChar(c)) {
                if (run.empty()) start = o;
                run += c;
            } else {
                flush(run, start);
            }
        }
        flush(run, start);
    }
    std::vector<std::pair<size_t, std::wstring>> result;
    for (auto& [id, pos] : found) result.emplace_back(pos, id);
    return result;
}

// 读 .lnk 的目标程序和 AppUserModelID
void ReadLink(const std::wstring& path, std::wstring& exePath, std::wstring& aumid) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
        if (SUCCEEDED(file->Load(path.c_str(), STGM_READ))) {
            wchar_t target[MAX_PATH * 2] = {};
            if (SUCCEEDED(link->GetPath(target, ARRAYSIZE(target), nullptr, 0)) && target[0]) exePath = ToLower(target);
            IPropertyStore* store = nullptr;
            if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&store)))) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(store->GetValue(kPkeyAppUserModelId, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
                    aumid = ToLower(pv.pwszVal);
                PropVariantClear(&pv);
                store->Release();
            }
        }
        file->Release();
    }
    link->Release();
}

// IShellItemImageFactory 给的 32 位 DIB（预乘 alpha）→ GDI+ 位图
std::shared_ptr<Gdiplus::Bitmap> BitmapFromDib(HBITMAP hbmp) {
    DIBSECTION ds = {};
    if (GetObjectW(hbmp, sizeof(ds), &ds) != sizeof(ds) || ds.dsBm.bmBitsPixel != 32 || !ds.dsBm.bmBits) return nullptr;
    int w = ds.dsBm.bmWidth, h = ds.dsBm.bmHeight;
    bool bottomUp = ds.dsBmih.biHeight > 0;
    auto bmp = std::make_shared<Gdiplus::Bitmap>(w, h, PixelFormat32bppPARGB);
    Gdiplus::Rect rect(0, 0, w, h);
    Gdiplus::BitmapData data;
    if (bmp->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &data) != Gdiplus::Ok) return nullptr;
    bool anyAlpha = false;
    for (int y = 0; y < h; ++y) {
        int srcRow = bottomUp ? h - 1 - y : y;
        BYTE* dst = static_cast<BYTE*>(data.Scan0) + y * data.Stride;
        memcpy(dst, static_cast<const BYTE*>(ds.dsBm.bmBits) + static_cast<size_t>(srcRow) * ds.dsBm.bmWidthBytes, w * 4);
        for (int x = 0; x < w && !anyAlpha; ++x) anyAlpha = dst[x * 4 + 3] != 0;
    }
    // 没有 alpha 通道：全黑说明没取到图（交给调用方换别的办法），否则当成不透明
    bool blank = !anyAlpha;
    if (!anyAlpha) {
        for (int y = 0; y < h; ++y) {
            BYTE* row = static_cast<BYTE*>(data.Scan0) + y * data.Stride;
            for (int x = 0; x < w; ++x) {
                if (row[x * 4] || row[x * 4 + 1] || row[x * 4 + 2]) blank = false;
                row[x * 4 + 3] = 0xFF;
            }
        }
    }
    bmp->UnlockBits(&data);
    return blank ? nullptr : bmp;
}

// 备用：从系统图像列表取图标（32 或 48 像素）
std::shared_ptr<Gdiplus::Bitmap> IconFromImageList(IShellItem* item, int iconPx) {
    std::shared_ptr<Gdiplus::Bitmap> result;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHGetIDListFromObject(item, &pidl))) return result;
    SHFILEINFOW sfi = {};
    if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl), 0, &sfi, sizeof(sfi), SHGFI_PIDL | SHGFI_SYSICONINDEX)) {
        IImageList* list = nullptr;
        if (SUCCEEDED(SHGetImageList(iconPx > 32 ? SHIL_EXTRALARGE : SHIL_LARGE, kIidImageList,
                                     reinterpret_cast<void**>(&list)))) {
            HICON icon = nullptr;
            if (SUCCEEDED(list->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &icon)) && icon) {
                result = BitmapFromIcon(icon);
                DestroyIcon(icon);
            }
            list->Release();
        }
    }
    CoTaskMemFree(pidl);
    return result;
}

// 通过外壳取显示名称和图标；.lnk 和 shell:AppsFolder\<AUMID> 都能用
bool DescribeItem(const std::wstring& parsingName, int iconPx, std::wstring& name,
                  std::shared_ptr<Gdiplus::Bitmap>& icon) {
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(parsingName.c_str(), nullptr, IID_PPV_ARGS(&item)))) return false;

    PWSTR display = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &display)) && display) {
        name = display;
        CoTaskMemFree(display);
    }

    std::wstring key = ToLower(parsingName) + L"|" + std::to_wstring(iconPx);
    auto cached = s_iconCache.find(key);
    if (cached != s_iconCache.end()) {
        icon = cached->second;
    } else {
        IShellItemImageFactory* factory = nullptr;
        if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&factory)))) {
            HBITMAP hbmp = nullptr;
            if (SUCCEEDED(factory->GetImage(SIZE{iconPx, iconPx}, SIIGBF_ICONONLY, &hbmp)) && hbmp) {
                icon = BitmapFromDib(hbmp);
                DeleteObject(hbmp);
            }
            factory->Release();
        }
        if (!icon) icon = IconFromImageList(item, iconPx);
        s_iconCache[key] = icon;
    }
    item->Release();
    return true;
}

std::vector<PinnedApp> Load(const std::wstring& folder, const std::vector<BYTE>& favorites, int iconPx) {
    struct Entry {
        size_t order;
        std::wstring sortName;
        PinnedApp app;
    };
    std::vector<Entry> entries;

    // 1. 文件夹里的 .lnk
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((folder + L"\\*.lnk").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            Entry e;
            e.sortName = ToLower(fd.cFileName);
            e.order = FindUtf16(favorites, fd.cFileName);
            e.app.launch = folder + L"\\" + fd.cFileName;
            ReadLink(e.app.launch, e.app.exePath, e.app.aumid);
            if (!DescribeItem(e.app.launch, iconPx, e.app.name, e.app.icon) || e.app.name.empty()) {
                e.app.name = fd.cFileName;
                e.app.name.resize(e.app.name.size() - 4);  // 去掉 .lnk
            }
            entries.push_back(std::move(e));
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }

    // 2. Favorites 里的应用商店应用（.lnk 已经代表了的跳过）
    for (auto& [pos, id] : FindAumids(favorites)) {
        bool dup = std::any_of(entries.begin(), entries.end(), [&](const Entry& e) { return e.app.aumid == id; });
        if (dup) continue;
        Entry e;
        e.order = pos;
        e.sortName = id;
        e.app.aumid = id;
        e.app.launch = L"shell:AppsFolder\\" + id;
        // 拿不到名称说明这个应用已经卸载了
        if (!DescribeItem(e.app.launch, iconPx, e.app.name, e.app.icon) || e.app.name.empty()) continue;
        entries.push_back(std::move(e));
    }

    // 在 Favorites 里找不到位置的排到最后，按名称排
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.order != b.order) return a.order < b.order;
        return a.sortName < b.sortName;
    });
    std::vector<PinnedApp> apps;
    for (auto& e : entries) apps.push_back(std::move(e.app));
    return apps;
}

}  // namespace

std::vector<PinnedApp> LoadPinnedApps(int iconPx) {
    std::wstring folder = PinnedFolder();
    FILETIME folderTime = FolderWriteTime(folder);
    std::vector<BYTE> favorites = ReadFavorites();

    // 固定的应用没变就直接用上次的结果，弹出更快
    if (s_cache.valid && s_cache.iconPx == iconPx && CompareFileTime(&s_cache.folderTime, &folderTime) == 0 &&
        s_cache.favorites == favorites)
        return s_cache.apps;

    s_cache.apps = Load(folder, favorites, iconPx);
    s_cache.folderTime = folderTime;
    s_cache.favorites = std::move(favorites);
    s_cache.iconPx = iconPx;
    s_cache.valid = true;
    return s_cache.apps;
}

// 判断一个窗口是不是这个固定应用的：两边都有 AppUserModelID 时比较 ID，否则比较程序路径
bool PinMatchesWindow(const PinnedApp& pin, const WindowEntry& window) {
    if (!pin.aumid.empty() && !window.aumid.empty()) return pin.aumid == window.aumid;
    return !pin.exePath.empty() && pin.exePath == window.exePath;
}

void LaunchApp(const std::wstring& target) { LaunchAsUser(target); }

std::shared_ptr<Gdiplus::Bitmap> ShellItemIcon(const std::wstring& parsingName, int iconPx) {
    // 打字筛选时每打一个字都要取一遍，取过的（包括没取到的）直接用缓存，不再创建外壳对象
    auto cached = s_iconCache.find(ToLower(parsingName) + L"|" + std::to_wstring(iconPx));
    if (cached != s_iconCache.end()) return cached->second;
    std::wstring name;
    std::shared_ptr<Gdiplus::Bitmap> icon;
    DescribeItem(parsingName, iconPx, name, icon);
    return icon;
}

void Pinned_ClearCache() {
    s_cache = Cache();
    s_iconCache.clear();
}

}  // namespace app
