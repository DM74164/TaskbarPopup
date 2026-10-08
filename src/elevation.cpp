// 以管理员身份运行。
//
// 前台是以管理员身份运行的程序（不少游戏、带反作弊的游戏、从管理员 Steam 启动的游戏）时，
// Windows 不把它收到的按键交给普通权限程序的键盘钩子：长按 Win 本程序看不到，按键直接交给系统、打开开始菜单。
// 本程序自己也以管理员身份运行就能收到。开机自启改用“任务计划程序”（最高权限、不弹 UAC），
// 从迷你任务栏启动的程序通过资源管理器以普通权限启动，免得它们也变成管理员权限。
#include "common.h"

#include <exdisp.h>
#include <shldisp.h>
#include <shlguid.h>

namespace app {
namespace {

constexpr wchar_t kTaskName[] = L"TaskbarPopup";

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

std::wstring ExePath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

// 进程令牌的完整性级别（RID）。读不到返回 -1
int IntegrityOf(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return -1;
    int level = -1;
    BYTE buf[64];
    DWORD size = 0;
    if (GetTokenInformation(token, TokenIntegrityLevel, buf, sizeof(buf), &size)) {
        PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf)->Label.Sid;
        level = static_cast<int>(*GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1));
    }
    CloseHandle(token);
    return level;
}

// 隐藏窗口运行 schtasks.exe，返回它的退出码（没能运行返回 -1）
int RunSchtasks(const std::wstring& args) {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\schtasks.exe\" " + args;
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    DWORD code = static_cast<DWORD>(-1);
    if (WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

std::wstring XmlEscape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        switch (c) {
            case L'&': out += L"&amp;"; break;
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'"': out += L"&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

// 通过资源管理器（普通权限）调用 ShellExecute：
// 桌面的 IShellView → IShellFolderViewDual → IShellDispatch2::ShellExecute
bool ShellExecuteViaExplorer(const std::wstring& file, const std::wstring& args) {
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
    if (windows->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp.p) != S_OK || !disp.p) return false;
    Ref<IServiceProvider> provider;
    if (FAILED(disp->QueryInterface(IID_PPV_ARGS(&provider.p)))) return false;
    Ref<IShellBrowser> browser;
    if (FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser.p)))) return false;
    Ref<IShellView> view;
    if (FAILED(browser->QueryActiveShellView(&view.p)) || !view.p) return false;
    Ref<IDispatch> background;
    if (FAILED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&background.p)))) return false;
    Ref<IShellFolderViewDual> folderView;
    if (FAILED(background->QueryInterface(IID_PPV_ARGS(&folderView.p)))) return false;
    Ref<IDispatch> appDisp;
    if (FAILED(folderView->get_Application(&appDisp.p)) || !appDisp.p) return false;
    Ref<IShellDispatch2> shell;
    if (FAILED(appDisp->QueryInterface(IID_PPV_ARGS(&shell.p)))) return false;

    BSTR bFile = SysAllocString(file.c_str());
    VARIANT vArgs, vDir, vOp, vShow;
    VariantInit(&vArgs);
    VariantInit(&vDir);
    VariantInit(&vOp);
    VariantInit(&vShow);
    if (!args.empty()) {
        vArgs.vt = VT_BSTR;
        vArgs.bstrVal = SysAllocString(args.c_str());
    }
    vShow.vt = VT_I4;
    vShow.lVal = SW_SHOWNORMAL;
    HRESULT hr = shell->ShellExecute(bFile, vArgs, vDir, vOp, vShow);
    VariantClear(&vArgs);
    SysFreeString(bFile);
    return SUCCEEDED(hr);
}

HWINEVENTHOOK s_fgHook = nullptr;
bool s_hinted = false;

void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (s_hinted || IsElevated() || !g_settings.longPressPopup || !hwnd) return;
    if (!RunsAboveUs(hwnd)) return;
    s_hinted = true;
    Log(L"前台程序 %ls 的权限比本程序高，长按 Win 在它里面收不到", GetProcessPath(hwnd).c_str());
    ShowTrayBalloon(L"这个程序里长按 Win 不起作用",
                    L"前台程序以管理员身份运行（不少游戏是这样），普通权限的 TaskbarPopup 收不到它里面的按键。"
                    L"在托盘菜单里勾选“以管理员身份运行”即可。");
}

}  // namespace

bool IsElevated() {
    static int cached = -1;
    if (cached < 0) {
        cached = 0;
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            TOKEN_ELEVATION e = {};
            DWORD size = 0;
            if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size)) cached = e.TokenIsElevated ? 1 : 0;
            CloseHandle(token);
        }
    }
    return cached == 1;
}

bool RunsAboveUs(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || pid == GetCurrentProcessId()) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;  // 系统进程之类，判断不了
    int theirs = IntegrityOf(process);
    bool denied = theirs < 0 && GetLastError() == ERROR_ACCESS_DENIED;
    CloseHandle(process);
    if (denied) return true;  // 连令牌都不让读：比本程序高（或者受保护的进程）
    int mine = IntegrityOf(GetCurrentProcess());
    return theirs >= 0 && mine >= 0 && theirs > mine;
}

void Elevation_Init() {
    s_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnForeground, 0, 0,
                               WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

void Elevation_Shutdown() {
    if (s_fgHook) UnhookWinEvent(s_fgHook);
    s_fgHook = nullptr;
}

bool RelaunchElevated() {
    std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = kRestartArg;
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) return true;
    Log(L"以管理员身份重新启动失败（%lu）", GetLastError());
    return false;
}

bool RelaunchUnelevated() {
    if (ShellExecuteViaExplorer(ExePath(), kRestartArg)) return true;
    Log(L"以普通权限重新启动失败");
    return false;
}

void LaunchAsUser(const std::wstring& target) {
    AllowSetForegroundWindow(ASFW_ANY);  // 让新启动的程序能拿到前台
    if (IsElevated()) {
        if (ShellExecuteViaExplorer(target, L"")) return;
        // 资源管理器没在运行（正在重启）时不能退回到自己启动：那样启动的程序也是管理员权限
        Log(L"通过资源管理器以普通权限启动失败：%ls", target.c_str());
        MessageBeep(MB_ICONWARNING);
        ShowTrayBalloon(L"暂时无法启动", L"资源管理器还没准备好，稍后再试一次。");
        return;
    }
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.lpFile = target.c_str();
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

bool AdminTask_Exists() { return RunSchtasks(std::wstring(L"/Query /TN \"") + kTaskName + L"\"") == 0; }

bool AdminTask_Run() { return RunSchtasks(std::wstring(L"/Run /TN \"") + kTaskName + L"\"") == 0; }

// 建任务时用的程序路径记在设置文件里，程序换了位置时就知道任务过时了
bool AdminTask_MatchesExe() {
    wchar_t path[MAX_PATH * 2] = {};
    GetPrivateProfileStringW(L"State", L"AdminTaskExe", L"", path, ARRAYSIZE(path), SettingsFile().c_str());
    return *path && CompareStringOrdinal(path, -1, ExePath().c_str(), -1, TRUE) == CSTR_EQUAL;
}

namespace {
void RememberTaskExe(const wchar_t* path) {
    EnsureUnicodeIni();
    WritePrivateProfileStringW(L"State", L"AdminTaskExe", path, SettingsFile().c_str());
}
}  // namespace

bool AdminTask_Set(bool enabled) {
    if (!enabled) {
        bool gone = !AdminTask_Exists() || RunSchtasks(std::wstring(L"/Delete /TN \"") + kTaskName + L"\" /F") == 0;
        if (gone) RememberTaskExe(nullptr);
        return gone;
    }
    wchar_t domain[256] = {}, user[256] = {};
    GetEnvironmentVariableW(L"USERDOMAIN", domain, 256);
    GetEnvironmentVariableW(L"USERNAME", user, 256);
    std::wstring account = XmlEscape(std::wstring(domain) + L"\\" + user);
    // 不能用 schtasks 的默认设置：用电池时不启动、运行 72 小时后强制结束、优先级偏低
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"<RegistrationInfo><Description>TaskbarPopup: start at logon as administrator</Description></RegistrationInfo>\r\n"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + account + L"</UserId></LogonTrigger></Triggers>\r\n"
        L"<Principals><Principal id=\"Author\"><UserId>" + account +
        L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>false</StartWhenAvailable>"
        L"<IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>"
        L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>4</Priority></Settings>\r\n"
        L"<Actions Context=\"Author\"><Exec><Command>\"" + XmlEscape(ExePath()) + L"\"</Command></Exec></Actions>\r\n"
        L"</Task>\r\n";

    wchar_t tempDir[MAX_PATH] = {}, file[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    if (!GetTempFileNameW(tempDir, L"tbp", 0, file)) return false;
    HANDLE h = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const WORD bom = 0xFEFF;
    WriteFile(h, &bom, sizeof(bom), &written, nullptr);
    WriteFile(h, xml.c_str(), static_cast<DWORD>(xml.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
    int code = RunSchtasks(std::wstring(L"/Create /TN \"") + kTaskName + L"\" /XML \"" + file + L"\" /F");
    DeleteFileW(file);
    if (code != 0) Log(L"创建开机自启的计划任务失败（%d）", code);
    if (code == 0) RememberTaskExe(ExePath().c_str());
    return code == 0;
}

}  // namespace app
