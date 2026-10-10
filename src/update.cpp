// 检查更新（默认关闭）：向 GitHub Releases 要最新发布的版本号，比现在的新就弹托盘气泡，点了打开下载页面。
// 只打开网页，不下载、不安装任何东西。请求在后台线程上做，WinHTTP 每一步最多等 5 秒，主线程从不等网络。
#include "common.h"
#include "version.h"

#include <winhttp.h>

#include <atomic>
#include <cstring>
#include <cwchar>
#include <mutex>

// Windows 8.1 起才有：按系统（Internet 选项）的代理设置走，旧的 SDK 头文件里没有
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace app {
namespace {

constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kApiPath[] = L"/repos/DM74164/TaskbarPopup/releases/latest";
constexpr wchar_t kRepoUrl[] = L"https://github.com/DM74164/TaskbarPopup/";  // 下载页面只认这个仓库下的地址
constexpr wchar_t kReleasesUrl[] = L"https://github.com/DM74164/TaskbarPopup/releases/latest";
constexpr wchar_t kUserAgent[] = L"TaskbarPopup/" TP_VERSION_STR;  // GitHub API 不接受没有 User-Agent 的请求
constexpr UINT kFirstCheckMs = 15 * 1000;            // 启动后先等一会儿，别和开机时的其他程序抢网络
constexpr UINT kIntervalMs = 24 * 60 * 60 * 1000;   // 之后每天一次
constexpr int kNetTimeoutMs = 5000;                  // 解析域名、连接、发送、接收各自最多等这么久
constexpr size_t kMaxResponse = 1 << 20;             // 正常的回应只有几 KB
constexpr DWORD kStopWaitMs = 1000;

struct Version {
    int part[3] = {};
};

struct Result {
    bool ok = false;
    Version latest;
    std::wstring url;    // 下载页面
    std::wstring error;  // 失败原因，给用户看的
};

// 后台线程把结果放在这里，再发 WM_APP_UPDATE 让主线程来取
std::mutex s_lock;
Result s_result;
bool s_hasResult = false;
std::atomic<bool> s_stopping{false};  // 退出时没等到线程结束：它别再碰这些静态变量

// 以下只在主线程上用
HANDLE s_thread = nullptr;
bool s_busy = false;       // 正在检查（结果还没取走）
bool s_manual = false;     // 这次检查里用户点过“检查更新”：结果都要告诉用户
bool s_scheduled = false;  // 自动检查的计时器开着

int Compare(const Version& a, const Version& b) {
    for (int i = 0; i < 3; ++i)
        if (a.part[i] != b.part[i]) return a.part[i] < b.part[i] ? -1 : 1;
    return 0;
}

Version CurrentVersion() {
    Version v;
    v.part[0] = TP_VERSION_MAJOR;
    v.part[1] = TP_VERSION_MINOR;
    v.part[2] = TP_VERSION_PATCH;
    return v;
}

// "v1.0.3"、"1.0.3"、"v1.1"（缺的部分算 0），后面可以跟 -beta 之类的后缀；别的格式返回 false
template <class Char>
bool ParseVersion(const std::basic_string<Char>& text, Version& out) {
    size_t i = 0;
    if (i < text.size() && (text[i] == 'v' || text[i] == 'V')) ++i;
    Version v;
    for (int n = 0; n < 3; ++n) {
        size_t start = i;
        long value = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            value = value * 10 + (text[i] - '0');
            if (value > 999999) return false;
            ++i;
        }
        if (i == start) return false;  // 点的前后都要有数字
        v.part[n] = static_cast<int>(value);
        if (n < 2 && i < text.size() && text[i] == '.') {
            ++i;
            continue;
        }
        break;
    }
    if (i < text.size() && text[i] != '-' && text[i] != '+') return false;
    out = v;
    return true;
}

std::wstring VersionText(const Version& v) {
    wchar_t buf[48];
    swprintf(buf, ARRAYSIZE(buf), L"v%d.%d.%d", v.part[0], v.part[1], v.part[2]);
    return buf;
}

// ---- 只认最外层的 "tag_name" 和 "html_url"（author 等嵌套对象里也有 html_url）----

// s[i] 是开头的引号；读完 i 停在结尾引号后面。只关心 ASCII 的值，\u 转义出的非 ASCII 字符记成 '?'
bool ReadJsonString(const std::string& s, size_t& i, std::string& out) {
    out.clear();
    for (++i; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') {
            ++i;
            return true;
        }
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i >= s.size()) return false;
        switch (s[i]) {
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (i + 4 >= s.size()) return false;
                unsigned code = 0;
                for (int k = 1; k <= 4; ++k) {
                    char h = s[i + k];
                    int d = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10
                            : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                    if (d < 0) return false;
                    code = code * 16 + static_cast<unsigned>(d);
                }
                out += code > 0 && code < 0x80 ? static_cast<char>(code) : '?';
                i += 4;
                break;
            }
            default: out += s[i]; break;  // \" \\ \/
        }
    }
    return false;
}

void SkipSpace(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
}

bool ParseRelease(const std::string& json, std::string& tag, std::string& url) {
    size_t i = 0;
    SkipSpace(json, i);
    if (i >= json.size() || json[i] != '{') return false;
    int depth = 0;
    while (i < json.size()) {
        char c = json[i];
        if (c == '{' || c == '[') {
            ++depth;
            ++i;
        } else if (c == '}' || c == ']') {
            if (--depth <= 0) break;  // 最外层的对象结束了
            ++i;
        } else if (c == '"') {
            std::string key;
            if (!ReadJsonString(json, i, key)) return false;
            if (depth != 1) continue;
            // 最外层对象里后面跟冒号的字符串就是键；值是字符串时顺便读掉，不是的话交给外面的循环
            SkipSpace(json, i);
            if (i >= json.size() || json[i] != ':') continue;
            SkipSpace(json, ++i);
            if (i >= json.size() || json[i] != '"') continue;
            std::string value;
            if (!ReadJsonString(json, i, value)) return false;
            if (key == "tag_name") tag = value;
            else if (key == "html_url") url = value;
        } else {
            ++i;
        }
    }
    return !tag.empty();
}

// 下载页面要交给浏览器打开：只接受这个仓库下、只含普通字符的地址，免得引号之类混进命令行
bool IsSafeReleaseUrl(const std::string& url) {
    size_t prefix = wcslen(kRepoUrl);
    if (url.size() <= prefix || url.size() > 512 || url.find("..") != std::string::npos) return false;
    for (size_t k = 0; k < prefix; ++k)
        if (url[k] != static_cast<char>(kRepoUrl[k])) return false;
    for (char c : url.substr(prefix)) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  (c != '\0' && std::strchr("-._~/%+", c) != nullptr);
        if (!ok) return false;
    }
    return true;
}

struct NetHandle {
    HINTERNET h = nullptr;
    NetHandle() = default;
    NetHandle(const NetHandle&) = delete;
    NetHandle& operator=(const NetHandle&) = delete;
    ~NetHandle() {
        if (h) WinHttpCloseHandle(h);
    }
};

std::wstring NetError(const wchar_t* step) {
    DWORD err = GetLastError();
    Log(L"检查更新：%ls 失败（%lu）", step, err);
    switch (err) {
        case ERROR_WINHTTP_TIMEOUT: return L"连接超时";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return L"找不到服务器，可能没有联网";
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR: return L"连不上 GitHub";
        case ERROR_WINHTTP_SECURE_FAILURE: return L"安全连接失败";
    }
    wchar_t buf[64];
    swprintf(buf, ARRAYSIZE(buf), L"网络错误 %lu", err);
    return buf;
}

// 在后台线程上运行
Result Fetch() {
    Result r;
    NetHandle session, connect, request;
    session.h = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h)  // Windows 8.1 以前
        session.h = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) {
        r.error = NetError(L"WinHttpOpen");
        return r;
    }
    WinHttpSetTimeouts(session.h, kNetTimeoutMs, kNetTimeoutMs, kNetTimeoutMs, kNetTimeoutMs);
    connect.h = WinHttpConnect(session.h, kApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connect.h)
        request.h = WinHttpOpenRequest(connect.h, L"GET", kApiPath, nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request.h) {
        r.error = NetError(L"打开请求");
        return r;
    }
    const wchar_t headers[] = L"Accept: application/vnd.github+json\r\n";
    if (!WinHttpSendRequest(request.h, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        r.error = NetError(L"发送请求");
        return r;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        Log(L"检查更新：服务器返回 %lu", status);
        if (status == 404) {
            r.error = L"还没有发布过正式版本";
        } else if (status == 403 || status == 429) {
            r.error = L"GitHub 暂时限制了请求次数，稍后再试";
        } else {
            wchar_t buf[64];
            swprintf(buf, ARRAYSIZE(buf), L"服务器返回 %lu", status);
            r.error = buf;
        }
        return r;
    }
    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail)) {
            r.error = NetError(L"接收");
            return r;
        }
        if (avail == 0) break;
        if (body.size() + avail > kMaxResponse) {
            Log(L"检查更新：回应超过 %u 字节", static_cast<unsigned>(kMaxResponse));
            r.error = L"服务器的回应不对";
            return r;
        }
        size_t old = body.size();
        body.resize(old + avail);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, &body[old], avail, &read)) {
            r.error = NetError(L"接收");
            return r;
        }
        body.resize(old + read);
        if (read == 0) break;
    }

    std::string tag, url;
    if (!ParseRelease(body, tag, url) || !ParseVersion(tag, r.latest)) {
        Log(L"检查更新：读不懂回应（%u 字节）", static_cast<unsigned>(body.size()));
        r.error = L"服务器的回应不对";
        return r;
    }
    r.url = IsSafeReleaseUrl(url) ? std::wstring(url.begin(), url.end()) : kReleasesUrl;
    r.ok = true;
    return r;
}

DWORD WINAPI CheckThread(void*) {
    Result r = Fetch();
    if (s_stopping) return 0;
    {
        std::lock_guard<std::mutex> guard(s_lock);
        s_result = std::move(r);
        s_hasResult = true;
    }
    PostMessageW(g_mainWnd, WM_APP_UPDATE, 0, 0);
    return 0;
}

Version NotifiedVersion() {
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(L"State", L"UpdateNotified", L"", buf, ARRAYSIZE(buf), SettingsFile().c_str());
    Version v;
    ParseVersion(std::wstring(buf), v);  // 没记过或读不懂就是 0.0.0
    return v;
}

void SetNotifiedVersion(const Version& v) {
    EnsureUnicodeIni();
    WritePrivateProfileStringW(L"State", L"UpdateNotified", VersionText(v).c_str(), SettingsFile().c_str());
}

// 只在手动检查时用：资源管理器没在运行、气泡出不来的话改用对话框
void Tell(const std::wstring& text, bool failed) {
    if (ShowTrayBalloon(L"检查更新", text.c_str())) return;
    MessageBoxW(nullptr, text.c_str(), L"TaskbarPopup",
                (failed ? MB_ICONWARNING : MB_ICONINFORMATION) | MB_TOPMOST | MB_SETFOREGROUND);
}

void AnnounceNewer(const Result& r, bool manual) {
    std::wstring title = L"发现新版本 " + VersionText(r.latest);
    if (ShowTrayBalloon(title.c_str(), L"点这里打开下载页面", r.url.c_str())) {
        SetNotifiedVersion(r.latest);
        return;
    }
    if (!manual) return;  // 下次自动检查再说
    std::wstring text = title + L"，现在打开下载页面吗？";
    SetNotifiedVersion(r.latest);
    if (MessageBoxW(nullptr, text.c_str(), L"TaskbarPopup", MB_ICONINFORMATION | MB_YESNO | MB_TOPMOST | MB_SETFOREGROUND) ==
        IDYES)
        LaunchAsUser(r.url);
}

void CALLBACK OnTimer(HWND, UINT, UINT_PTR, DWORD) {
    SetTimer(g_mainWnd, kTimerUpdate, kIntervalMs, OnTimer);  // 第一次是启动后 15 秒，之后每天一次
    Update_CheckNow(false);
}

}  // namespace

void Update_Configure() {
    if (g_settings.checkUpdates == s_scheduled) return;  // 改别的设置时也会调用，别把计时重新开始
    s_scheduled = g_settings.checkUpdates;
    if (s_scheduled) {
        SetTimer(g_mainWnd, kTimerUpdate, kFirstCheckMs, OnTimer);
    } else {
        KillTimer(g_mainWnd, kTimerUpdate);
    }
}

void Update_CheckNow(bool manual) {
    if (s_stopping) return;
    // 结果的消息万一丢了（线程已经结束却还没取走）：现在取，免得以后再也检查不了
    if (s_busy && s_thread && WaitForSingleObject(s_thread, 0) == WAIT_OBJECT_0) Update_OnResult();
    if (manual) s_manual = true;
    if (s_busy) return;  // 正在检查：等它的结果，用户点过的话结果照样告诉用户
    if (s_thread) CloseHandle(s_thread);
    Log(manual ? L"检查更新（手动）" : L"检查更新（自动）");
    s_busy = true;
    s_thread = CreateThread(nullptr, 0, CheckThread, nullptr, 0, nullptr);
    if (!s_thread) {
        s_busy = false;
        if (s_manual) Tell(L"检查更新失败：无法启动后台线程", true);
        s_manual = false;
    }
}

void Update_OnResult() {
    Result r;
    {
        std::lock_guard<std::mutex> guard(s_lock);
        if (!s_hasResult) return;
        r = std::move(s_result);
        s_result = Result();
        s_hasResult = false;
    }
    bool manual = s_manual;
    s_manual = false;
    s_busy = false;
    if (!r.ok) {
        Log(L"检查更新失败：%ls", r.error.c_str());
        if (manual) Tell(L"检查更新失败：" + r.error, true);
        return;
    }
    Log(L"检查更新：最新版本 %ls，下载页面 %ls", VersionText(r.latest).c_str(), r.url.c_str());
    if (Compare(r.latest, CurrentVersion()) > 0) {
        // 自动检查不为同一个版本反复提醒
        if (manual || Compare(r.latest, NotifiedVersion()) > 0) AnnounceNewer(r, manual);
    } else if (manual) {
        Tell(L"已经是最新版本（v" TP_VERSION_STR L"）", false);
    }
}

void Update_Stop() {
    s_stopping = true;
    if (s_scheduled) KillTimer(g_mainWnd, kTimerUpdate);
    s_scheduled = false;
    if (!s_thread) return;
    // WinHTTP 的同步调用打断不了，每一步最多 5 秒；等不到就不等了，线程看到 s_stopping 后不再碰共享的状态，
    // 进程最后用 TerminateProcess 结束，它用到的静态变量不会被析构
    if (WaitForSingleObject(s_thread, kStopWaitMs) != WAIT_OBJECT_0) Log(L"检查更新的线程没有及时结束");
    CloseHandle(s_thread);
    s_thread = nullptr;
}

}  // namespace app
