#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <objidl.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

// GDI+ 头文件依赖 min/max 宏，而上面定义了 NOMINMAX
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

namespace app {

// ---------------- 设置 ----------------
struct Settings {
    bool autoHideOnFullscreen = true;  // 窗口最大化或全屏时隐藏任务栏
    bool longPressPopup = true;
    int longPressMs = 1000;
    bool showPinnedApps = true;  // 迷你任务栏里显示固定在任务栏的应用
    bool showLevels = true;      // 迷你任务栏里显示音量和亮度调节
    int popupScale = 100;        // 迷你任务栏大小，百分比（在显示器缩放的基础上再乘）
    bool debugLog = false;       // 诊断日志
    bool runAsAdmin = false;     // 以管理员身份运行（管理员权限的程序、游戏里也能长按 Win）
    std::vector<std::wstring> excludeApps;  // 最大化时不隐藏任务栏的程序：小写的程序文件名，如 notepad.exe
};

std::wstring NormalizeExeName(const std::wstring& text);  // 路径或名字 → 小写文件名，没写扩展名的补上 .exe
bool IsExcludedExe(const std::wstring& exeName);           // exeName 已经是 NormalizeExeName 的结果
void SetExcluded(const std::wstring& exeName, bool excluded);  // 改排除名单、保存并重新判断要不要藏任务栏

constexpr int kMinLongPressMs = 200;
constexpr int kMaxLongPressMs = 5000;
constexpr int kDefaultLongPressMs = 1000;
constexpr int kMinPopupScale = 50;
constexpr int kMaxPopupScale = 200;
constexpr int kDefaultPopupScale = 100;

extern HINSTANCE g_instance;
extern HWND g_mainWnd;  // 隐藏的主消息窗口，托盘和计时器都挂在它上面
extern Settings g_settings;

std::wstring SettingsDir();
std::wstring SettingsFile();  // SettingsDir() 下的 settings.ini
void EnsureUnicodeIni();      // 写 settings.ini 之前调用：保证它是 UTF-16 编码
void LoadSettings();
void SaveSettings();
bool IsAutoStartEnabled();
void SetAutoStart(bool enabled);
void ApplySettings();

// 本程序打开了系统的“自动隐藏任务栏”时记在设置文件里，异常退出后下次启动时把它关回去
bool GetRestoreAutoHideFlag();
void SetRestoreAutoHideFlag(bool value);

// 测试时可以用 -DTP_ANIM_SCALE=10 把所有动画放慢 10 倍，方便截图检查
#ifndef TP_ANIM_SCALE
#define TP_ANIM_SCALE 1
#endif

// 主窗口上的计时器 ID
constexpr UINT_PTR kTimerFullscreen = 2;
constexpr UINT_PTR kTimerShowPopup = 3;
constexpr UINT_PTR kTimerAfterAutoHide = 4;  // 关掉自动隐藏以后收拾窗口和桌面图标
constexpr UINT_PTR kTimerRetryOff = 5;       // 没能关掉自动隐藏，过一会儿再试

// 主窗口消息
constexpr UINT WM_APP_TRAY = WM_APP + 1;       // 托盘图标的鼠标事件
constexpr UINT WM_APP_LONGPRESS = WM_APP + 2;  // 钩子线程通知：Win 键长按了
constexpr UINT WM_APP_AUTOHIDE_OFF = WM_APP + 3;  // 动画线程通知：关自动隐藏的结果，wParam 是请求编号，lParam 非 0 = 关掉了
constexpr UINT WM_APP_AUTOHIDE_ON = WM_APP + 4;   // 动画线程通知：开自动隐藏的结果，wParam 是请求编号，lParam 非 0 = 开了

// ---------------- 最大化 / 全屏时隐藏任务栏 ----------------
void Fullscreen_SetEnabled(bool enabled);
void Fullscreen_Check();
void Fullscreen_OnAutoHideOff(UINT seq, bool done);  // 收到 WM_APP_AUTOHIDE_OFF
void Fullscreen_OnAutoHideOn(UINT seq, bool done);   // 收到 WM_APP_AUTOHIDE_ON
void Fullscreen_OnTaskbarCreated();                  // 资源管理器（重新）启动了
void Taskbar_RestoreAll();        // 启动、退出时：显示所有任务栏，关掉本程序打开的自动隐藏，窗口和桌面图标还原
void Taskbar_EmergencyRestore();  // 崩溃时：只把任务栏显示出来、关掉自动隐藏
bool Taskbar_IsShownOn(HMONITOR monitor);  // 这块屏幕上的任务栏现在是否显示着（没被本程序藏起来）
void Fullscreen_LogState();                // 把系统版本、各屏幕的工作区、任务栏位置写进诊断日志
// 系统的“自动隐藏任务栏”设置（对所有屏幕上的任务栏都生效）。可以在任何线程上调用。
// Taskbar_SetAutoHide 返回设置现在是否就是要的状态（资源管理器没在运行时返回 false）
bool Taskbar_AutoHideOn();
bool Taskbar_SetAutoHide(bool on);

// ---------------- 任务栏滑出 / 滑入动画（在单独的线程上运行）----------------
// stretch：截图盖住任务栏以后要拉伸到 stretchRect 的窗口，可以为空。
// animate=false：任务栏刚藏过又被资源管理器显示出来时用，直接藏，不再滑一遍。
// autoHideSeq 不为 0：真任务栏藏好以后打开自动隐藏，让工作区占满整块屏（最大化的窗口这才铺得满），
// 打开了再拉伸 stretch，并发 WM_APP_AUTOHIDE_ON(autoHideSeq) 给主窗口。
// 轮到它时主线程要的编号（TaskbarAnim_WantAutoHide）已经不是它了，就不开也不拉伸
void TaskbarAnim_Hide(HWND taskbar, HWND stretch, const RECT& stretchRect, bool animate, UINT autoHideSeq);
void TaskbarAnim_WantAutoHide(UINT seq);  // 主线程现在要开的请求编号，0 = 要关
// offSeq 不为 0：截图滑回原位、盖住任务栏的位置以后关掉自动隐藏，再发 WM_APP_AUTOHIDE_OFF(offSeq) 给主窗口
void TaskbarAnim_Show(HWND taskbar, UINT offSeq = 0);
void TaskbarAnim_AutoHideOff(UINT seq);  // 没有要滑回来的任务栏时关掉自动隐藏（正在滑回来的话等它滑完）
void TaskbarAnim_Stop();  // 结束动画线程，去掉所有截图
void TaskbarAnim_Abort();  // 崩溃时：让动画线程不再做任何事（只改原子变量，不等），可以在任何线程上调用

// ---------------- 桌面图标（跨进程调用在单独的线程上做）----------------
// 工作区变化时桌面会按新的工作区重新排列图标，打开自动隐藏前记下位置，关掉以后摆回去
void DesktopIcons_BeginSession();       // 主线程，要打开自动隐藏时：开始记位置（上次的还没摆完就接着用）
void DesktopIcons_WaitSaved(DWORD ms);  // 动画线程，打开自动隐藏之前：等位置记完，超时的话这次不摆
void DesktopIcons_RestoreLater();       // 主线程，关掉自动隐藏以后：等桌面排完再摆回去，又打开的话作废
bool DesktopIcons_Mark();               // 主线程，自动隐藏开着期间桌面到了前台：读一遍图标位置
void DesktopIcons_UpdateMoved();        // 主线程，离开桌面或要关自动隐藏前：Mark 以后挪过的图标按新位置记
void DesktopIcons_WaitUpdated(DWORD ms);  // 关自动隐藏之前：等上面那次读完
void DesktopIcons_AbandonUpdates();       // 马上要关自动隐藏：还没读完的不用了（读到的可能是重新排过的位置）
void DesktopIcons_Finish(DWORD ms);     // 退出时：摆回去并结束线程，最多等 ms 毫秒

// ---------------- 键盘钩子（在单独的线程上运行）----------------
bool Hook_Install();
void Hook_Uninstall();
void Hook_Configure(bool enabled, int thresholdMs);
void SendStartMenu();

// ---------------- 窗口列表 ----------------
struct WindowEntry {
    HWND hwnd = nullptr;
    std::wstring title;
    std::shared_ptr<Gdiplus::Bitmap> icon;
    std::wstring exePath;  // 小写，用来和固定的应用对应
    std::wstring aumid;    // AppUserModelID，小写，可能为空
    bool active = false;
};
std::vector<WindowEntry> EnumerateWindows(HWND foreground, int iconPx);
void Windows_ClearCache();

// ---------------- 固定在任务栏的应用 ----------------
struct PinnedApp {
    std::wstring name;
    std::wstring launch;   // 启动用：.lnk 路径，或 shell:AppsFolder\<AUMID>
    std::wstring exePath;  // 目标程序路径，小写，可能为空
    std::wstring aumid;    // AppUserModelID，小写，可能为空
    std::shared_ptr<Gdiplus::Bitmap> icon;
};
std::vector<PinnedApp> LoadPinnedApps(int iconPx);
bool PinMatchesWindow(const PinnedApp& pin, const WindowEntry& window);
void LaunchApp(const std::wstring& target);

// ---------------- 以管理员身份运行（elevation.cpp） ----------------
constexpr wchar_t kRestartArg[] = L"--restart";  // 重新启动的新实例：等旧实例退出后再运行
bool IsElevated();
bool RunsAboveUs(HWND hwnd);  // 窗口所在进程的权限比本程序高（本程序的键盘钩子收不到它里面的按键）
void Elevation_Init();        // 前台换成权限更高的程序时提示一次
void Elevation_Shutdown();
bool RelaunchElevated();      // 弹 UAC，以管理员身份启动一个新实例（带 kRestartArg）
bool RelaunchUnelevated();    // 通过资源管理器以普通权限启动一个新实例
void LaunchAsUser(const std::wstring& target);  // 本程序是管理员时也以普通权限启动
bool AdminTask_Exists();      // 以管理员身份开机自启的计划任务
bool AdminTask_Set(bool enabled);
bool AdminTask_Run();
bool AdminTask_MatchesExe();  // 计划任务是用现在这个 exe 的路径建的
bool ShowTrayBalloon(const wchar_t* title, const wchar_t* text);  // 资源管理器没在运行时显示不了，返回 false
void Pinned_ClearCache();

// ---------------- 所有应用：打字筛选时搜索用（apps.cpp）----------------
constexpr UINT WM_APP_APPS = WM_APP + 31;  // 后台读完了，发给 Apps_Refresh 的 notify
struct InstalledApp {
    std::wstring name;
    std::wstring launch;  // shell:AppsFolder\<ID>
    std::wstring search;  // 小写的名字 + 换行 + ID
};
void Apps_Refresh(HWND notify);  // 没读过或者读过超过 5 分钟就在后台重新读
std::vector<InstalledApp> Apps_Match(const std::wstring& filter, size_t max);  // filter 要小写，按匹配程度排序
void Apps_Stop();                // 退出前等后台线程结束
std::shared_ptr<Gdiplus::Bitmap> ShellItemIcon(const std::wstring& parsingName, int iconPx);  // pinned.cpp，带缓存

// ---------------- 窗口缩略图（thumbnail.cpp）----------------
// 在 (centerX, bottomY) 上方居中显示 source 的实时缩略图，不超出 bounds；DWM 不给缩略图时返回 false
bool Thumb_Show(HWND source, int centerX, int bottomY, const RECT& bounds, float scale, bool light);
void Thumb_Hide();
void Thumb_Destroy();

// ---------------- 迷你任务栏 ----------------
void Popup_Init();
void Popup_Destroy();
void Popup_Show();
void Popup_Hide();
void Popup_Toggle();

// ---------------- 音量和亮度 ----------------
// 音量：默认播放设备的主音量（Core Audio）。在调用线程上同步执行（线程需已初始化 COM），很快
bool Volume_Get(float& level, bool& muted);  // level 0~1；没有可用的播放设备时返回 false
void Volume_Set(float level);                 // 同时取消静音
void Volume_SetMute(bool muted);
void Volume_Release();                        // 退出前在主线程上释放缓存的 COM 对象

// 亮度：笔记本内屏走 WMI，外接显示器走 DDC/CI。这两种都慢，所以放在后台线程上做，
// 查询结果用 PostMessage(notify, WM_APP_BRIGHTNESS, 百分比 0~100（不支持时为 -1）, (LPARAM)monitor) 发回
constexpr UINT WM_APP_BRIGHTNESS = WM_APP + 30;
void Brightness_Query(HWND notify, HMONITOR monitor);
void Brightness_Set(HMONITOR monitor, int percent);  // 拖动时连续调用，只执行最新的一次
void Brightness_Stop();                              // 退出前结束后台线程

// ---------------- 设置窗口 ----------------
void SettingsDialog_Show();
HWND SettingsDialog_Hwnd();

// ---------------- 液态玻璃材质 ----------------
// 截下面板后面的屏幕内容，模糊并提高饱和度；按圆角矩形算出边缘的折射、色散、高光和阴影。
// 画出来的玻璃是不透明的（背景已经画进去了），所以要和截图时的屏幕位置对齐。
class Glass {
public:
    // window：要画玻璃的窗口在屏幕上的位置；panel：面板静止时在屏幕上的位置（用来判断背景明暗）
    void Capture(const RECT& window, const RECT& panel, float scale);
    bool Light() const { return m_light; }  // 背景偏亮，用浅色玻璃配深色文字
    // 按面板静止时的形状预先算好每个像素的材质（覆盖率、阴影、折射位移、高光），尺寸不变时直接返回。
    // width×height：窗口像素大小；panel：面板在窗口里的位置；radius：圆角半径
    void Prepare(int width, int height, const Gdiplus::RectF& panel, float radius);
    // 在预乘 BGRA 像素上画阴影和玻璃面板。origin：窗口左上角在屏幕上的位置；
    // slide：面板往下平移的像素数（弹出 / 收起动画），背景不跟着动
    void Render(DWORD* pixels, POINT origin, int slide) const;

private:
    struct Texel {
        float dx = 0, dy = 0;  // 折射：去背景的哪里取色（相对本像素，绿色通道的位移）
        BYTE cover = 0;        // 面板覆盖率
        BYTE shadow = 0;       // 阴影的不透明度
        BYTE light = 0;        // 叠加的白光
        BYTE bevel = 0;        // 1 = 在边缘弯曲的一圈里，要折射取样
    };
    void SampleBevel(float x, float y, const Texel& t, float rgb[3]) const;
    DWORD Fetch(int x, int y) const;

    std::vector<DWORD> m_blur;  // 模糊后的背景
    RECT m_area = {};           // 背景截图在屏幕上的范围
    int m_w = 0, m_h = 0;
    float m_scale = 1;
    bool m_light = false;

    std::vector<Texel> m_texels;  // 面板静止时窗口里每个像素的材质
    int m_texW = 0, m_texH = 0;
    Gdiplus::RectF m_texPanel;
    float m_texRadius = 0;
};

// ---------------- 工具函数 ----------------
std::wstring ToLower(std::wstring s);
std::wstring GetClassNameStr(HWND hwnd);
std::wstring GetWindowTitle(HWND hwnd);
std::wstring GetProcessPath(HWND hwnd);
HWND AppWindowTarget(HWND hwnd);  // UWP 应用返回承载它的 CoreWindow，其余原样返回
std::wstring WindowExeName(HWND hwnd);  // 窗口所属程序的小写文件名（UWP 应用取应用自己的进程）
std::wstring GetWindowAumid(HWND hwnd);
bool IsCloaked(HWND hwnd);
bool IsOwnProcess(HWND hwnd);
void ForceForeground(HWND hwnd);
float MonitorScale(HMONITOR monitor);
void AddRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius);
std::shared_ptr<Gdiplus::Bitmap> BitmapFromIcon(HICON hIcon);
double NowMs();        // 高精度时钟，毫秒
// 诊断日志：g_settings.debugLog 打开时追加一行到 SettingsDir() 下的 debug.log，可以在任何线程上调用
void Log(const wchar_t* format, ...);
std::wstring LogFile();
void WaitForVBlank();  // 等到下一次屏幕刷新，让动画按显示器的节奏走
bool CaptureScreen(const RECT& area, std::vector<DWORD>& pixels);  // 截屏，BGRA，不透明
void SetWindowRectAsync(HWND hwnd, const RECT& rect);  // 挪别的程序的窗口：异步、不激活、不改层次

// System.AppUserModel.ID，自己定义一份，免得依赖 propkey.h 的链接方式
constexpr PROPERTYKEY kPkeyAppUserModelId = {
    {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

}  // namespace app
