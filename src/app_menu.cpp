// 迷你任务栏右键菜单用到的外壳操作：认出格子是哪个应用（AppUserModelID、程序路径、名称），
// 读它跳转列表里的“最近”项（IApplicationDocumentLists），以管理员身份运行，打开文件所在的位置。
#include "common.h"

namespace app {
namespace {

constexpr wchar_t kAppsFolderPrefix[] = L"shell:AppsFolder\\";
constexpr size_t kAppsFolderPrefixLen = ARRAYSIZE(kAppsFolderPrefix) - 1;

// MinGW 的 uuid 库里不一定有，自己定义
constexpr CLSID kClsidApplicationDocumentLists = {
    0x86BEC222, 0x30F2, 0x47E0, {0x9F, 0x25, 0x60, 0xD1, 0x1C, 0xD7, 0x5C, 0x28}};
// System.FileDescription、System.Link.TargetParsingPath、System.Title
constexpr PROPERTYKEY kPkeyFileDescription = {
    {0x0CEF7D53, 0xFA64, 0x11D1, {0xA2, 0x03, 0x00, 0x00, 0xF8, 0x1F, 0xED, 0xEE}}, 3};
constexpr PROPERTYKEY kPkeyLinkTargetParsingPath = {
    {0xB9B4B3FC, 0x2B51, 0x4A42, {0xB5, 0xD8, 0x32, 0x41, 0x46, 0xAF, 0xCF, 0x25}}, 2};
constexpr PROPERTYKEY kPkeyTitle = {{0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}}, 2};

// 没有显式 AppUserModelID 的程序，系统按程序路径算一个：开头是这些已知文件夹的换成文件夹的 GUID，
// 开始菜单“所有应用”里看到的就是这种（比如 {1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\notepad.exe）。
// System32、SysWOW64 要排在 Windows 前面
constexpr GUID kImplicitFolders[] = {
    {0x1AC14E77, 0x02E7, 0x4E5D, {0xB7, 0x44, 0x2E, 0xB1, 0xAE, 0x51, 0x98, 0xB7}},  // System
    {0xD65231B0, 0xB2F1, 0x4857, {0xA4, 0xCE, 0xA8, 0xE7, 0xC6, 0xEA, 0x7D, 0x27}},  // SystemX86
    {0x6D809377, 0x6AF0, 0x444B, {0x89, 0x57, 0xA3, 0x77, 0x3F, 0x02, 0x20, 0x0E}},  // ProgramFilesX64
    {0x7C5A40EF, 0xA0FB, 0x4BFC, {0x87, 0x4A, 0xC0, 0xF2, 0xE0, 0xB9, 0xFA, 0x8E}},  // ProgramFilesX86
    {0xF38BF404, 0x1D43, 0x42F2, {0x93, 0x05, 0x67, 0xDE, 0x0B, 0x28, 0xFC, 0x23}},  // Windows
};

bool StartsWithNoCase(const std::wstring& s, const wchar_t* prefix, size_t n) {
    return s.size() >= n && CompareStringOrdinal(s.c_str(), static_cast<int>(n), prefix, static_cast<int>(n), TRUE) ==
                                CSTR_EQUAL;
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring TakeString(PWSTR text) {
    std::wstring result = text ? text : L"";
    CoTaskMemFree(text);
    return result;
}

std::wstring ItemName(IShellItem* item, SIGDN form) {
    PWSTR text = nullptr;
    if (FAILED(item->GetDisplayName(form, &text))) return {};
    return TakeString(text);
}

std::wstring ItemProperty(IShellItem* item, const PROPERTYKEY& key) {
    std::wstring result;
    IShellItem2* item2 = nullptr;
    if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&item2)))) {
        PWSTR text = nullptr;
        if (SUCCEEDED(item2->GetString(key, &text))) result = TakeString(text);
        item2->Release();
    }
    return result;
}

std::wstring KnownFolderPath(const GUID& id) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path))) {
        CoTaskMemFree(path);
        return {};
    }
    return TakeString(path);
}

std::wstring ImplicitAppId(const std::wstring& exe) {
    for (const GUID& id : kImplicitFolders) {
        std::wstring folder = KnownFolderPath(id);
        if (folder.empty()) continue;
        if (folder.back() != L'\\') folder += L'\\';  // C:\Program Files 不能匹配到 C:\Program Files (x86)
        if (!StartsWithNoCase(exe, folder.c_str(), folder.size())) continue;
        wchar_t guid[64] = {};
        if (!StringFromGUID2(id, guid, ARRAYSIZE(guid))) break;
        return std::wstring(guid) + L"\\" + exe.substr(folder.size());
    }
    return exe;
}

// 反过来：{已知文件夹}\相对路径 → 完整路径；不是这种形式返回空
std::wstring PathFromImplicitAppId(const std::wstring& id) {
    if (id.size() < 40 || id[0] != L'{' || id[37] != L'}' || id[38] != L'\\') return {};
    GUID folder;
    if (FAILED(CLSIDFromString(id.substr(0, 38).c_str(), &folder))) return {};
    std::wstring base = KnownFolderPath(folder);
    return base.empty() ? L"" : base + L"\\" + id.substr(39);
}

// 开始菜单“所有应用”里的一项：名称、原样大小写的 ID、桌面程序的目标路径
struct AppsEntry {
    std::wstring name, id, target;
};

bool LookupAppsFolder(const std::wstring& id, AppsEntry& out) {
    if (id.empty()) return false;
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName((kAppsFolderPrefix + id).c_str(), nullptr, IID_PPV_ARGS(&item)))) return false;
    AppsEntry e;
    e.name = ItemName(item, SIGDN_NORMALDISPLAY);
    e.id = ItemProperty(item, kPkeyAppUserModelId);
    if (e.id.empty()) e.id = ItemName(item, SIGDN_PARENTRELATIVEPARSING);
    e.target = ItemProperty(item, kPkeyLinkTargetParsingPath);
    item->Release();
    // 名字就是 ID 本身的，是外壳给认不出的 ID 临时建的项：当作没有
    if (e.name.empty() || EqualsNoCase(e.name, id)) return false;
    if (!EqualsNoCase(e.id, id)) e.id = id;  // 拿到的不是同一个 ID，还用原来的
    out = std::move(e);
    return true;
}

std::wstring FileDescription(const std::wstring& path) {
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) return {};
    std::wstring result = ItemProperty(item, kPkeyFileDescription);
    item->Release();
    return result;
}

// 路径 → 不带 .exe 的文件名
std::wstring FileStem(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (name.size() > 4 && EqualsNoCase(name.substr(name.size() - 4), L".exe")) name.resize(name.size() - 4);
    return name;
}

bool FileExists(const std::wstring& path) {
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::shared_ptr<ITEMIDLIST> HoldIdList(PIDLIST_ABSOLUTE pidl) {
    if (!pidl) return nullptr;
    return std::shared_ptr<ITEMIDLIST>(pidl, [](ITEMIDLIST* p) { CoTaskMemFree(p); });
}

// 跳转列表给的是 IShellItem（文件等外壳项）或者 IShellLink（带参数的快捷方式）
bool DescribeRecent(IUnknown* object, RecentItem& out) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&item)))) {
        out.name = ItemName(item, SIGDN_NORMALDISPLAY);
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHGetIDListFromObject(item, &pidl))) out.pidl = HoldIdList(pidl);
        item->Release();
        return !out.name.empty() && out.pidl;
    }

    IShellLinkW* link = nullptr;
    if (FAILED(object->QueryInterface(IID_PPV_ARGS(&link)))) return false;
    wchar_t text[INFOTIPSIZE] = {};
    if (SUCCEEDED(link->GetPath(text, ARRAYSIZE(text), nullptr, 0))) out.file = text;
    text[0] = L'\0';
    if (SUCCEEDED(link->GetArguments(text, ARRAYSIZE(text)))) out.args = text;
    text[0] = L'\0';
    if (SUCCEEDED(link->GetWorkingDirectory(text, ARRAYSIZE(text)))) out.dir = text;
    text[0] = L'\0';
    if (SUCCEEDED(link->GetDescription(text, ARRAYSIZE(text)))) out.name = text;
    if (out.name.empty()) {
        IPropertyStore* store = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&store)))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(store->GetValue(kPkeyTitle, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) out.name = pv.pwszVal;
            PropVariantClear(&pv);
            store->Release();
        }
    }
    if (out.file.empty()) {
        // 目标不是文件（比如控制面板里的项）：按 IDList 打开，参数就顾不上了
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(link->GetIDList(&pidl))) out.pidl = HoldIdList(pidl);
        if (out.name.empty() && out.pidl) {
            PWSTR name = nullptr;
            if (SUCCEEDED(SHGetNameFromIDList(out.pidl.get(), SIGDN_NORMALDISPLAY, &name))) out.name = TakeString(name);
        }
    } else if (out.name.empty()) {
        out.name = FileStem(out.file);
    }
    link->Release();
    return !out.name.empty() && (!out.file.empty() || out.pidl);
}

}  // namespace

AppTarget ResolveAppTarget(HWND hwnd, const std::wstring& launch, const std::wstring& pinName) {
    AppTarget t;
    std::wstring windowId, exe, lnk, linkId, appsId;
    if (hwnd && IsWindow(hwnd)) {
        windowId = GetWindowAumid(hwnd, true);
        exe = GetProcessPath(AppWindowTarget(hwnd));
        // 应用商店应用最小化时偶尔找不到它自己的进程，ApplicationFrameHost 不是它
        if (NormalizeExeName(exe) == L"applicationframehost.exe") exe.clear();
    }
    if (StartsWithNoCase(launch, kAppsFolderPrefix, kAppsFolderPrefixLen)) appsId = launch.substr(kAppsFolderPrefixLen);
    else if (!launch.empty()) lnk = launch;
    if (!lnk.empty()) {
        std::wstring target;
        ReadShortcut(lnk, target, linkId);
        if (exe.empty()) exe = target;
    }
    if (exe.empty()) exe = PathFromImplicitAppId(appsId);

    // 有显式 ID 的先查开始菜单：拿原样大小写的 ID（固定的应用记的是小写）、名称，桌面程序还有目标路径
    std::wstring id = !windowId.empty() ? windowId : !linkId.empty() ? linkId : appsId;
    AppsEntry entry;
    bool listed = LookupAppsFolder(id, entry);
    if (listed) id = entry.id;
    bool packaged = id.find(L'!') != std::wstring::npos;  // 应用商店应用：包名_发布者!应用
    if (exe.empty() && listed && !packaged && FileExists(entry.target)) exe = entry.target;

    auto addId = [&](const std::wstring& s) {
        if (s.empty()) return;
        for (const std::wstring& known : t.appIds)
            if (EqualsNoCase(known, s)) return;
        t.appIds.push_back(s);
    };
    addId(id);
    addId(windowId);
    addId(linkId);
    addId(appsId);
    std::wstring implicitId = packaged || exe.empty() ? L"" : ImplicitAppId(exe);
    addId(implicitId);
    if (!packaged) addId(exe);

    t.name = pinName;
    if (t.name.empty() && listed) t.name = entry.name;
    if (t.name.empty() && implicitId != exe && LookupAppsFolder(implicitId, entry)) t.name = entry.name;
    if (t.name.empty() && !exe.empty()) t.name = FileDescription(exe);
    if (t.name.empty() && !exe.empty()) t.name = FileStem(exe);
    if (t.name.empty() && hwnd) t.name = GetWindowTitle(hwnd);

    t.launch = !launch.empty() ? launch : packaged ? kAppsFolderPrefix + id : exe;
    if (!packaged) {
        t.runAs = !lnk.empty() ? lnk : exe;
        t.location = FileExists(exe) ? exe : lnk;
    }
    return t;
}

std::vector<RecentItem> RecentItems(const std::vector<std::wstring>& appIds, size_t max) {
    std::vector<RecentItem> result;
    for (const std::wstring& id : appIds) {
        IApplicationDocumentLists* lists = nullptr;
        if (FAILED(CoCreateInstance(kClsidApplicationDocumentLists, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&lists))))
            break;  // 系统不支持，换别的 ID 也一样
        IObjectArray* array = nullptr;
        if (SUCCEEDED(lists->SetAppID(id.c_str())) &&
            SUCCEEDED(lists->GetList(ADLT_RECENT, static_cast<UINT>(max), IID_PPV_ARGS(&array)))) {
            UINT count = 0;
            if (FAILED(array->GetCount(&count))) count = 0;
            for (UINT i = 0; i < count && result.size() < max; ++i) {
                IUnknown* object = nullptr;
                if (FAILED(array->GetAt(i, IID_PPV_ARGS(&object)))) continue;
                RecentItem item;
                if (DescribeRecent(object, item)) result.push_back(std::move(item));
                object->Release();
            }
            array->Release();
        }
        lists->Release();
        if (!result.empty()) break;
    }
    return result;
}

void OpenRecentItem(const RecentItem& item) {
    if (!item.file.empty()) {
        LaunchAsUser(item.file, item.args, item.dir);
        return;
    }
    if (!item.pidl) return;
    if (IsElevated()) {
        // 本程序是管理员时直接打开的话，打开它的程序也是管理员权限：换成解析名交给资源管理器
        PWSTR name = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(item.pidl.get(), SIGDN_DESKTOPABSOLUTEPARSING, &name))) {
            std::wstring parsing = TakeString(name);
            if (!parsing.empty()) LaunchAsUser(parsing);
        }
        return;
    }
    AllowSetForegroundWindow(ASFW_ANY);  // 让打开它的程序能拿到前台
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_IDLIST;
    sei.lpIDList = item.pidl.get();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) Log(L"打开最近项失败（%lu）：%ls", GetLastError(), item.name.c_str());
}

void RunAsAdmin(HWND owner, const std::wstring& file) {
    AllowSetForegroundWindow(ASFW_ANY);
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.hwnd = owner;  // UAC 提示框要有前台的父窗口，不然只在任务栏上闪
    sei.lpVerb = L"runas";
    sei.lpFile = file.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) && GetLastError() != ERROR_CANCELLED)
        Log(L"以管理员身份运行失败（%lu）：%ls", GetLastError(), file.c_str());
}

void OpenFileLocation(const std::wstring& path) {
    // 交给资源管理器：本程序是管理员时也以普通权限打开
    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    LaunchAsUser(std::wstring(windows) + L"\\explorer.exe", L"/select,\"" + path + L"\"");
}

}  // namespace app
