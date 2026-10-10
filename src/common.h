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
enum GlassStyle { kGlassLiquid, kGlassFrosted };            // 液态玻璃 / 毛玻璃
enum Theme { kThemeSystem, kThemeLight, kThemeDark };     // 跟随系统 / 浅色 / 深色
enum HotkeyId { kHotkeyPopup, kHotkeyPin, kHotkeyCount };  // 弹出迷你任务栏 / 固定前台的小窗口
constexpr int kMinLongPressMs = 200;
constexpr int kMaxLongPressMs = 5000;
constexpr int kDefaultLongPressMs = 1000;
struct Settings {
    bool autoHideOnFullscreen = true;  // 窗口最大化或全屏时隐藏任务栏
    bool longPressPopup = true;
    int longPressMs = 1000;
    bool showPinnedApps = true;  // 迷你任务栏里显示固定在任务栏的应用
    bool showLevels = true;      // 迷你任务栏里显示音量和亮度调节
    int popupScale = 100;        // 迷你任务栏大小，百分比（在显示器缩放的基础上再乘）
    bool debugLog = false;       // 诊断日志
    bool runAsAdmin = false;     // 以管理员身份运行（管理员权限的程序、游戏里也能长按 Win）
    bool checkUpdates = false;   // 自动检查更新
    bool keepFloatsOnTop = true; // 点全屏（最大化）的窗口时，浮在上面的小窗口不被盖住
    UINT hotkey[kHotkeyCount] = {};       // 快捷键（见 MakeHotkey），下标是 HotkeyId，0 = 没设
    bool hotkeyHold[kHotkeyCount] = {};   // 长按触发，否则单按
    int hotkeyHoldMs[kHotkeyCount] = {kDefaultLongPressMs, kDefaultLongPressMs};  // 长按要按住多久
    int glassStyle = kGlassLiquid;  // 迷你任务栏和设置窗口的材质
    int theme = kThemeSystem;       // 深色还是浅色
    std::vector<std::wstring> excludeApps;  // 最大化时不隐藏任务栏的程序：小写的程序文件名，如 notepad.exe
};

std::wstring NormalizeExeName(const std::wstring& text);  // 路径或名字 → 小写文件名，没写扩展名的补上 .exe
bool IsExcludedExe(const std::wstring& exeName);           // exeName 已经是 NormalizeExeName 的结果
void SetExcluded(const std::wstring& exeName, bool excluded);  // 改排除名单、保存并重新判断要不要藏任务栏

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
constexpr UINT_PTR kTimerUpdate = 6;         // 自动检查更新

// 主窗口消息
constexpr UINT WM_APP_TRAY = WM_APP + 1;       // 托盘图标的鼠标事件
constexpr UINT WM_APP_LONGPRESS = WM_APP + 2;  // 钩子线程通知：Win 键长按了
constexpr UINT WM_APP_AUTOHIDE_OFF = WM_APP + 3;  // 动画线程通知：关自动隐藏的结果，wParam 是请求编号，lParam 非 0 = 关掉了
constexpr UINT WM_APP_AUTOHIDE_ON = WM_APP + 4;   // 动画线程通知：开自动隐藏的结果，wParam 是请求编号，lParam 非 0 = 开了
constexpr UINT WM_APP_UPDATE = WM_APP + 5;        // 检查更新的线程通知：结果出来了，交给 Update_OnResult

// ---------------- 最大化 / 全屏时隐藏任务栏 ----------------
void Fullscreen_SetEnabled(bool enabled);
void Fullscreen_Check();
void Fullscreen_OnAutoHideOff(UINT seq, bool done);  // 收到 WM_APP_AUTOHIDE_OFF
void Fullscreen_OnAutoHideOn(UINT seq, bool done);   // 收到 WM_APP_AUTOHIDE_ON
void Fullscreen_OnTaskbarCreated();                  // 资源管理器（重新）启动了
void Fullscreen_QuietStartMenu();                    // 接下来打开的开始菜单是迷你任务栏点的：不把藏着的任务栏放出来
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
void DesktopIcons_Cover();             // 主线程，要回到桌面了（要关自动隐藏）：图标还藏着的话把桌面的照片重新垫好
void DesktopIcons_RevealLater();       // 主线程，自动隐藏刚关掉：图标排回原位就显示出来（打开前藏起来了的话）
void DesktopIcons_Recover();           // 上次藏起的桌面图标没显示回来（被强行结束）：现在显示
void DesktopIcons_Finish(DWORD ms);     // 退出时：摆回去并结束线程，最多等 ms 毫秒

// ---------------- 键盘钩子（在单独的线程上运行）----------------
bool Hook_Install();
void Hook_Uninstall();
void Hook_Configure(bool enabled, int thresholdMs);
void SendStartMenu();
void SendWinX();  // 打开 Win+X 菜单（右键开始按钮的那个）

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
void ReadShortcut(const std::wstring& path, std::wstring& exePath, std::wstring& aumid);  // 读 .lnk，原样大小写

// ---------------- 以管理员身份运行（elevation.cpp） ----------------
constexpr wchar_t kRestartArg[] = L"--restart";  // 重新启动的新实例：等旧实例退出后再运行
bool IsElevated();
bool RunsAboveUs(HWND hwnd);  // 窗口所在进程的权限比本程序高（本程序的键盘钩子收不到它里面的按键）
void Elevation_Init();        // 前台换成权限更高的程序时提示一次
void Elevation_Shutdown();
bool RelaunchElevated();      // 弹 UAC，以管理员身份启动一个新实例（带 kRestartArg）
bool RelaunchUnelevated();    // 通过资源管理器以普通权限启动一个新实例
// 本程序是管理员时也以普通权限启动；args、dir 是参数和工作目录，可以为空
void LaunchAsUser(const std::wstring& target, const std::wstring& args = L"", const std::wstring& dir = L"");
bool AdminTask_Exists();      // 以管理员身份开机自启的计划任务
bool AdminTask_Set(bool enabled);
bool AdminTask_Run();
bool AdminTask_MatchesExe();  // 计划任务是用现在这个 exe 的路径建的
// 资源管理器没在运行时显示不了，返回 false。clickUrl 不为空：点气泡时以普通权限打开这个网址
bool ShowTrayBalloon(const wchar_t* title, const wchar_t* text, const wchar_t* clickUrl = nullptr);
void Pinned_ClearCache();

// ---------------- 检查更新（update.cpp，默认关闭）----------------
// 向 GitHub Releases 要最新版本，有新版本时弹托盘气泡，点了打开下载页面（只打开网页，不下载、不安装）
void Update_Configure();             // 按 g_settings.checkUpdates 开关自动检查：启动后 15 秒一次，之后每天一次
void Update_CheckNow(bool manual);   // 在后台线程上检查，同时只有一次；manual：结果不管怎样都告诉用户
void Update_OnResult();              // 主窗口收到 WM_APP_UPDATE
void Update_Stop();                  // 退出时：停掉计时器，稍等后台线程结束

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

// ---------------- 右键菜单用到的外壳操作（app_menu.cpp）----------------
// 格子是哪个应用：从窗口、固定的应用（.lnk 或 shell:AppsFolder\<ID>）凑出来
struct AppTarget {
    std::wstring name;      // 应用名：固定的应用用它的名字，否则查开始菜单、程序的文件说明
    std::wstring launch;    // 再开一个 / 打开：固定的应用的启动路径，应用商店应用的 shell:AppsFolder\<ID>，或者程序路径
    std::wstring runAs;     // 以管理员身份运行的对象（.lnk 或程序）；应用商店应用为空
    std::wstring location;  // 打开文件所在的位置时选中的文件；应用商店应用为空
    std::vector<std::wstring> appIds;  // 读“最近”项用的 AppUserModelID，按顺序试：显式的在前，按程序路径算的在后
};
AppTarget ResolveAppTarget(HWND hwnd, const std::wstring& launch, const std::wstring& pinName);
// 跳转列表里的“最近”项
struct RecentItem {
    std::wstring name;
    std::shared_ptr<ITEMIDLIST> pidl;  // 外壳项：按 IDList 打开
    std::wstring file, args, dir;      // 快捷方式：目标、参数、工作目录
};
std::vector<RecentItem> RecentItems(const std::vector<std::wstring>& appIds, size_t max);  // 第一个有结果的 ID 的
void OpenRecentItem(const RecentItem& item);              // 本程序是管理员时也以普通权限打开
void RunAsAdmin(HWND owner, const std::wstring& file);    // owner：UAC 提示框的父窗口，要在前台
void OpenFileLocation(const std::wstring& path);          // 在资源管理器里打开所在文件夹并选中它

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
// 设置窗口里调“迷你任务栏大小”时按 percent 弹出预览（不抢前台，点不到）。hold：还按着滑块，
// 先不收起；否则过一会儿自己收起
void Popup_Preview(int percent, bool hold);
void Popup_EndPreview();  // 马上收起预览

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

// ---------------- 小窗口留在全屏窗口上面 ----------------
void FloatWindows_Configure(bool enabled);
void FloatWindows_TogglePin(HWND target);  // 固定小窗口（和双击一样），已经固定的取消，在窗口上方提示一下。target 为空时用前台窗口

// ---------------- 快捷键（hotkeys.cpp）----------------
// 一个快捷键存成 (修饰键 MOD_CONTROL / MOD_SHIFT / MOD_ALT << 16) | 虚拟键码，0 = 没设。
// 键可以是键盘上的键（单独的左右 Ctrl / Shift / Alt 用 VK_LCONTROL 这类分左右的键码），也可以是鼠标键（VK_LBUTTON 这类）
constexpr UINT MakeHotkey(UINT mods, UINT vk) { return (mods << 16) | vk; }
constexpr UINT HotkeyMods(UINT hotkey) { return hotkey >> 16; }
constexpr UINT HotkeyVk(UINT hotkey) { return hotkey & 0xFFFF; }
constexpr bool IsMouseVk(UINT vk) { return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2; }
constexpr bool IsModifierVk(UINT vk) { return vk >= VK_LSHIFT && vk <= VK_RMENU; }  // 分左右的 Shift / Ctrl / Alt
std::wstring KeyName(UINT vk, bool display = false);  // 空 = 不能用在快捷键里的键。display：给人看的（鼠标键、左右 Ctrl 写中文）
std::wstring HotkeyText(UINT hotkey, bool display = false);  // 如 Ctrl+Alt+Z；0 时为空
UINT ParseHotkey(const std::wstring& text);  // HotkeyText(hotkey) 反过来，认不出时为 0
const wchar_t* HotkeyProblem(UINT hotkey, bool hold);  // 这个键为什么不能这样用（单按 / 长按），能用时为 nullptr
void Hotkeys_Apply();                // 按 g_settings 注册；注册不上的（被别的程序占用）弹托盘气泡说一次
void Hotkeys_Suspend(bool suspend);  // 设置窗口录快捷键时先全部停掉，不然按下已经设的键录不到
bool Hotkeys_Failed(int id);         // 现在设的这个注册不上
void Hotkeys_OnHotkey(WPARAM id);    // 主窗口收到 WM_HOTKEY
void Hotkeys_Run(int id, HWND target);  // 做快捷键对应的事。target：鼠标键触发时鼠标下的窗口，键盘触发时为空
constexpr UINT WM_APP_HOTKEY = WM_APP + 6;  // 键盘钩子线程通知：快捷键触发了，wParam 是 HotkeyId，lParam 是 target

// RegisterHotKey 管不了的快捷键（鼠标键、长按、单独的 Ctrl / Shift / Alt、锁定键）由键盘钩子线程来认。
// tap / hold：单按 / 长按时做哪件事（HotkeyId），-1 = 没有；两样都有时没按够长按时长就松开算单按
struct KeyBinding {
    UINT mods = 0;
    UINT vk = 0;
    int tap = -1;
    int hold = -1;
    int holdMs = kDefaultLongPressMs;  // 长按要按住多久
};
void Hook_SetBindings(const std::vector<KeyBinding>& bindings);
HWND SettingsDialog_Hwnd();

// ---------------- 液态玻璃材质 ----------------
// 截好、模糊好的一块背景
struct GlassShot {
    RECT area = {};  // 在屏幕上的范围
    int w = 0, h = 0;
    std::vector<DWORD> raw;   // 原图，用来比较背景变了没有
    std::vector<DWORD> blur;  // 模糊、提高饱和度以后的
};

// 截下面板后面的屏幕内容，模糊并提高饱和度；按圆角矩形算出边缘的折射、色散、高光和阴影。
// 画出来的玻璃是不透明的（背景已经画进去了），所以要和截图时的屏幕位置对齐。
class Glass {
public:
    // 截 window 四周（再多一圈）的屏幕，模糊、提高饱和度（frosted：毛玻璃，模糊得更厉害）。
    // 不碰任何成员，可以在后台线程里调用。
    // same 不为空、范围相同、截到的和它一模一样（或者截图失败）时不做后面的处理，返回 false
    static bool Shoot(const RECT& window, float scale, bool frosted, GlassShot& out, const GlassShot* same = nullptr);
    // 截下 window（要画玻璃的窗口在屏幕上的位置）后面的背景，按现在的材质处理
    void Capture(const RECT& window, float scale);
    // 换上一张截好的背景
    void Adopt(GlassShot&& shot, float scale);
    const std::vector<DWORD>& Raw() const { return m_raw; }
    void SetLight(bool light) { m_light = light; }        // 浅色玻璃配深色文字，深色玻璃配浅色文字
    bool Light() const { return m_light; }
    void SetScale(float scale) { m_scale = scale; }  // 只换边缘、阴影等的尺寸，背景不重新截
    void SetFrosted(bool frosted) { m_frosted = frosted; }  // 毛玻璃：没有边缘折射和高光，着色更浓
    bool Frosted() const { return m_frosted; }
    const RECT& Area() const { return m_area; }  // 背景截图在屏幕上的范围
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
        BYTE light = 0;        // 叠加的白光（边缘高光）
        BYTE gloss = 0;        // 顶部光泽的位置权重，乘上深浅色各自的强度
        BYTE bevel = 0;        // 1 = 在边缘弯曲的一圈里，要折射取样
    };
    void RenderRows(DWORD* pixels, POINT origin, int slide, int rowBegin, int rowEnd) const;
    void SampleBevel(float x, float y, const Texel& t, float rgb[3]) const;
    DWORD Fetch(int x, int y) const;

    std::vector<DWORD> m_raw;   // 截下来的原图，再截时比较有没有变
    std::vector<DWORD> m_blur;  // 模糊后的背景
    RECT m_area = {};           // 背景截图在屏幕上的范围
    int m_w = 0, m_h = 0;
    float m_scale = 1;
    bool m_light = false;
    bool m_frosted = false;

    std::vector<Texel> m_texels;  // 面板静止时窗口里每个像素的材质
    int m_texW = 0, m_texH = 0;
    Gdiplus::RectF m_texPanel;
    float m_texRadius = 0;
    float m_texScale = 0;
    bool m_texFrosted = false;
};

// 按颜色选项（Theme）决定用不用浅色玻璃。taskbar：跟随系统时看“Windows 模式”（任务栏、开始菜单），
// 否则看“应用模式”
bool ThemeIsLight(int theme, bool taskbar);

// ---------------- 工具函数 ----------------
std::wstring ToLower(std::wstring s);
std::wstring GetClassNameStr(HWND hwnd);
std::wstring GetWindowTitle(HWND hwnd);
std::wstring GetProcessPath(HWND hwnd);
HWND AppWindowTarget(HWND hwnd);  // UWP 应用返回承载它的 CoreWindow，其余原样返回
std::wstring WindowExeName(HWND hwnd);  // 窗口所属程序的小写文件名（UWP 应用取应用自己的进程）
std::wstring GetWindowAumid(HWND hwnd, bool keepCase = false);  // 默认转成小写
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
