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
// 液态玻璃 / Win11 原版（平的亚克力，细边框）。以前的毛玻璃（1）由 Win11 原版顶替
enum GlassStyle { kGlassLiquid, kGlassWin11 };
enum Theme { kThemeSystem, kThemeLight, kThemeDark };     // 跟随系统 / 浅色 / 深色
enum HotkeyId { kHotkeyPopup, kHotkeyPin, kHotkeyCount };  // 弹出迷你任务栏 / 固定前台的小窗口
enum TrayStyle { kTrayStacked, kTrayRow };                  // 托盘图标：小图标两行堆叠 / 和应用一样大横着排
// 在底边时的对齐：跟随系统任务栏的对齐方式 / 居中 / 靠左 / 靠右 / 自定义（编辑模式里拖到的位置，见 popupOffset）。
// 竖排时只看是不是自定义，别的都是上下居中
enum PopupAlign { kAlignSystem, kAlignCenter, kAlignLeft, kAlignRight, kAlignCustom };
enum Language { kLangSystem, kLangChinese, kLangEnglish };  // 界面语言
enum PopupEdge { kEdgeBottom, kEdgeLeft, kEdgeRight };  // 迷你任务栏弹在屏幕哪条边：底边横排 / 左边、右边竖排
enum PopupMonitor { kPopupAtCursor, kPopupAtActive };  // 迷你任务栏弹在哪块屏幕上：鼠标所在的 / 前台窗口所在的
// 某块屏幕自己的迷你任务栏位置和对齐（“每块屏幕分别设置”开着时用）
struct ScreenPlace {
    std::wstring key;  // MonitorKey
    int edge = kEdgeBottom;
    int align = kAlignSystem;
    int offset = 500;  // 自定义位置：面板中心在那条屏幕边上的千分之几处
};
inline bool operator==(const ScreenPlace& a, const ScreenPlace& b) {
    return a.key == b.key && a.edge == b.edge && a.align == b.align && a.offset == b.offset;
}
// 迷你任务栏里能排顺序的模块（编辑模式里拖着换位置），开始按钮总在最前面；后面几样（输入法、电量、时钟）挨着时中间没有分隔线
enum Module { kModWindows, kModTray, kModMedia, kModIme, kModBattery, kModClock, kModuleCount };
constexpr int kMinLongPressMs = 200;
constexpr int kMaxLongPressMs = 5000;
constexpr int kDefaultLongPressMs = 1000;
constexpr int kDefaultGlassTransparency = 60;  // 液态玻璃的透明度（0 – 100%），60 是原来的样子
constexpr int kMaxPopupOffset = 1000;
constexpr int kMinIconSize = 80, kMaxIconSize = 150;  // 图标大小，百分比
constexpr int kMaxIconGap = 16;                       // 图标间距，像素（按 100% 大小算）
constexpr int kDefaultIconGap = 4;
// 迷你任务栏显示哪些、按什么顺序：放在底部和侧边时各一份（Settings::modules 的下标见 ModuleSide）
struct ModuleSet {
    bool tray = true;     // 系统托盘“隐藏的图标”里的那些
    bool levels = true;   // 音量和亮度调节
    bool radios = true;   // Wi-Fi 和蓝牙开关
    bool media = true;    // 正在播放的媒体
    bool ime = true;      // 输入法的中英文状态
    bool battery = true;  // 电量（没有电池时不显示）
    bool clock = true;    // 时钟
    int order[kModuleCount] = {kModWindows, kModTray, kModMedia, kModIme, kModBattery, kModClock};  // 模块的顺序，见 Module
};
enum ModuleSide { kSideBottom, kSideEdge };
inline int ModuleSideOf(int edge) { return edge == kEdgeBottom ? kSideBottom : kSideEdge; }
struct Settings {
    bool autoHideOnFullscreen = true;  // 窗口最大化或全屏时隐藏任务栏
    bool alwaysHideTaskbar = false;    // 一直隐藏任务栏：没有窗口最大化时也藏着，鼠标碰到屏幕边上也不弹出
    bool longPressPopup = true;
    int longPressMs = 1000;
    bool showPinnedApps = true;  // 迷你任务栏里显示固定在任务栏的应用
    ModuleSet modules[2];        // 迷你任务栏放在底部、侧边时各显示哪些，见 ModuleSide
    int trayStyle = 0;           // 托盘图标的样式，见 TrayStyle
    int popupScale = 100;        // 迷你任务栏大小，百分比（在显示器缩放的基础上再乘），编辑模式里拖面板的边改
    int popupRadius = 22;        // 迷你任务栏的圆角半径，像素（按 100% 大小算）
    int popupLength = 0;         // 迷你任务栏的长度（横排是宽，竖排是高）：占那条屏幕边的千分之几，0 = 跟着内容
    int popupEdge = kEdgeBottom;    // 迷你任务栏弹在哪条边，见 PopupEdge
    int popupAlign = kAlignSystem;  // 在底边时的对齐，见 PopupAlign
    int popupOffset = 500;          // 自定义位置：面板中心在那条屏幕边上的千分之几处（横排从左数，竖排从上数）
    int popupMonitor = kPopupAtCursor;  // 弹在哪块屏幕上，见 PopupMonitor
    bool popupPerScreen = false;        // 每块屏幕分别设置位置和对齐（screens），没设过的屏幕用上面两项
    std::vector<ScreenPlace> screens;
    int language = kLangSystem;     // 界面语言，见 Language
    bool debugLog = false;       // 诊断日志
    bool runAsAdmin = false;     // 以管理员身份运行（管理员权限的程序、游戏里也能长按 Win）
    bool checkUpdates = false;   // 自动检查更新
    bool keepFloatsOnTop = true; // 点全屏（最大化）的窗口时，浮在上面的小窗口不被盖住
    bool pinTopmost = false;     // 固定的小窗口在所有窗口上面（置顶），不只是全屏窗口
    UINT hotkey[kHotkeyCount] = {};       // 快捷键（见 MakeHotkey），下标是 HotkeyId，0 = 没设
    bool hotkeyHold[kHotkeyCount] = {};   // 长按触发，否则单按
    int hotkeyHoldMs[kHotkeyCount] = {kDefaultLongPressMs, kDefaultLongPressMs};  // 长按要按住多久
    int glassStyle = kGlassLiquid;  // 迷你任务栏和设置窗口的材质
    int theme = kThemeSystem;       // 深色还是浅色
    int glassTransparency = kDefaultGlassTransparency;  // 迷你任务栏液态玻璃的透明度，百分比，深浅色共用
    bool flashAlert = true;    // 任务栏藏着时提醒新消息：程序闪烁时屏幕边亮起，迷你任务栏里它的图标变色
    bool edgePopup = false;    // 任务栏藏着时，鼠标停在迷你任务栏那条屏幕边上就弹出
    bool clockSeconds = false; // 时钟显示秒
    bool clockWeekday = false; // 时钟显示星期
    bool clockDate = true;     // 时钟显示日期
    int iconSize = 100;               // 迷你任务栏的图标（格子）大小，百分比，在“大小”的基础上再乘
    int iconGap = kDefaultIconGap;    // 格子之间的间距，像素（按 100% 大小算）
    bool showTitle = true;            // 横排时顶上有一行显示名字；关掉时名字显示在面板上方的小牌子里
    bool customTint = false;          // 迷你任务栏用自己选的底色（tintColor），不跟随深浅色
    COLORREF tintColor = RGB(32, 32, 36);
    bool customAccent = false;        // 用自己选的强调色（accentColor），不跟随系统
    COLORREF accentColor = RGB(0, 120, 212);
    // 名单里是小写的程序文件名，如 notepad.exe。“最大化或全屏时隐藏任务栏”开着时用 excludeApps，关着时用 includeApps
    std::vector<std::wstring> excludeApps;  // 最大化时不隐藏任务栏的程序
    std::vector<std::wstring> includeApps;  // 最大化时隐藏任务栏的程序
};

std::wstring NormalizeExeName(const std::wstring& text);  // 路径或名字 → 小写文件名，没写扩展名的补上 .exe
bool InAppList(const std::vector<std::wstring>& list, const std::wstring& exeName);  // exeName 已经是 NormalizeExeName 的结果
// 现在用的名单：“最大化或全屏时隐藏任务栏”开着时是不隐藏的程序，关着时是隐藏的程序
std::vector<std::wstring>& ExceptionApps();
bool IsExceptionExe(const std::wstring& exeName);              // 在现在用的名单里
void SetException(const std::wstring& exeName, bool listed);  // 改现在用的名单、保存并重新判断要不要藏任务栏

ScreenPlace* FindScreenPlace(std::vector<ScreenPlace>& list, const std::wstring& key);
// 迷你任务栏在这块屏幕上弹在哪条边、怎么对齐，自定义位置时在边上的千分之几处
void PopupPlaceFor(HMONITOR monitor, int& edge, int& align, int& offset);
// 系统的强调色：深色时用浅一些的，浅色时用深一些的（和系统的开关一样）。读不到时是默认的蓝色
Gdiplus::Color SystemAccent(bool light);
float ColorLuma(COLORREF c);  // 0 – 255

constexpr int kMinPopupScale = 50;
constexpr int kMaxPopupScale = 200;
constexpr int kDefaultPopupScale = 100;
constexpr int kMaxPopupRadius = 40;
constexpr int kMaxPopupLength = 1000;
// 各材质默认的圆角：Win11 原版和系统的弹出面板一样是 8
inline int DefaultPopupRadius(int style) { return style == kGlassWin11 ? 8 : 22; }

extern HINSTANCE g_instance;
extern HWND g_mainWnd;  // 隐藏的主消息窗口，托盘和计时器都挂在它上面
extern Settings g_settings;
// 底部、侧边随便哪一份开着这一项（托盘、媒体这些在后台读的，有一份要就得读）
inline bool AnyModuleSet(bool ModuleSet::*item) { return g_settings.modules[0].*item || g_settings.modules[1].*item; }

std::wstring SettingsDir();
std::wstring SettingsFile();  // SettingsDir() 下的 settings.ini
void EnsureUnicodeIni();      // 写 settings.ini 之前调用：保证它是 UTF-16 编码
void LoadSettings();
void SaveSettings();
// 外观方案：设置文件里的 [Look1] ~ [Look8]，存着外观、模块和位置相关的那些键
constexpr int kMaxLookPresets = 8;
std::vector<int> LookPresets();  // 存着的方案编号，从小到大
int SaveLookPreset(int slot);    // 把现在的外观存成第 slot 个方案（0 = 新的一个），返回编号，存满了返回 0
void ApplyLookPreset(int slot);  // 换上第 slot 个方案（保存并生效）
void DeleteLookPreset(int slot);
// 设置导出成一个文件 / 从文件导入（导入后马上生效；以管理员身份运行、诊断日志这些跟着电脑走的不导入）
bool ExportSettings(const std::wstring& path);
bool ImportSettings(const std::wstring& path);
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
constexpr UINT WM_APP_TRAY_LAUNCH = WM_APP + 7;   // 点托盘图标的线程通知：找不到图标，再启动一次程序，交给 Tray_OnMessage
constexpr UINT WM_APP_WINTAP = WM_APP + 8;        // 钩子线程通知：任务栏藏着时短按了 Win（wParam 是键码），交给 Popup_StartMenu
constexpr UINT WM_APP_TRAY_MENU = WM_APP + 9;     // 点托盘图标的线程通知：程序自己的菜单弹不出来（发给 Tray_Click 的 notify）
constexpr UINT WM_APP_TRAY_PEEK = WM_APP + 10;    // 点托盘图标的线程要求：wParam 非 0 = 临时显示藏着的任务栏 lParam，0 = 藏回去。
                                                  // 交给 Fullscreen_PeekTaskbar，返回 1 = 是本程序藏的

// ---------------- 最大化 / 全屏时隐藏任务栏 ----------------
void Fullscreen_SetEnabled(bool enabled);
void Fullscreen_Check();
void Fullscreen_OnAutoHideOff(UINT seq, bool done);  // 收到 WM_APP_AUTOHIDE_OFF
void Fullscreen_OnAutoHideOn(UINT seq, bool done);   // 收到 WM_APP_AUTOHIDE_ON
void Fullscreen_OnTaskbarCreated();                  // 资源管理器（重新）启动了
void Fullscreen_QuietStartMenu();                    // 接下来打开的开始菜单是迷你任务栏点的：不把藏着的任务栏放出来
bool Fullscreen_TaskbarHidden();                     // 本程序现在藏着任务栏（有窗口最大化或全屏）
// 托盘图标那边要去“隐藏的图标”里点：show 时临时显示本程序藏着的任务栏 tb（UI 自动化看不到藏着的窗口里的按钮），
// 否则把它藏回去。都不放动画。tb 不是本程序藏着的返回 false
bool Fullscreen_PeekTaskbar(HWND tb, bool show);
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
void DesktopIcons_DisplayChanged();    // 主线程，显示器变了（WM_DISPLAYCHANGE）：接上了第二块屏的话藏着的图标马上显示
// 一直隐藏任务栏时自动隐藏一直开着、桌面一直要用，不藏图标（位置照样记、照样摆回去）；藏着的马上显示
void DesktopIcons_KeepShown(bool keep);
void DesktopIcons_EmergencyShow();     // 崩溃时：藏着的桌面图标直接显示（不摆位置）
void DesktopIcons_Finish(DWORD ms);     // 退出时：摆回去并结束线程，最多等 ms 毫秒

// ---------------- 键盘钩子（在单独的线程上运行）----------------
bool Hook_Install();
void Hook_Uninstall();
void Hook_Configure(bool enabled, int thresholdMs);
// 任务栏藏着时短按 Win 不直接交给系统，通知主线程（WM_APP_WINTAP）先弹迷你任务栏再打开开始菜单
void Hook_SetTaskbarHidden(bool hidden);
void SendStartMenu();
void SendWinTap(DWORD vk);  // 补发一次 Win 单击（钩子不拦）
void SendText(const std::wstring& text);  // 把字（\r 是回车）照打字那样发给前台窗口
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
// monitor：迷你任务栏弹在哪块屏幕上。系统设置成多块屏幕上的任务栏只显示自己屏幕上的窗口时，照样只列这块屏上的
std::vector<WindowEntry> EnumerateWindows(HWND foreground, int iconPx, HMONITOR monitor);
bool IsTaskbarWindow(HWND h);  // 按任务栏 / Alt+Tab 的规则算是应用窗口
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
    std::wstring exe;       // 打开“最近”里的文件用的程序（桌面程序的程序路径）；应用商店应用、找不到程序时为空
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
void OpenRecentItem(const RecentItem& item, const std::wstring& exe);  // 文件用 exe 打开（为空时用默认程序）；本程序是管理员时也以普通权限打开
void RunAsAdmin(HWND owner, const std::wstring& file);    // owner：UAC 提示框的父窗口，要在前台
void OpenFileLocation(const std::wstring& path);          // 在资源管理器里打开所在文件夹并选中它

// ---------------- 窗口缩略图（thumbnail.cpp）----------------
// 在 at 旁边显示 source 的实时缩略图（见 ThumbPlace），不超出 bounds；DWM 不给缩略图时返回 false
// 缩略图放在哪：kThumbAbove 底边中点对着 at；kThumbRight 左边对着 at.x、顶边在 at.y；kThumbLeft 右边对着 at.x、顶边在 at.y
enum ThumbPlace { kThumbAbove, kThumbRight, kThumbLeft };
bool Thumb_Show(HWND source, POINT at, int place, const RECT& bounds, float scale, bool light);
void Thumb_Hide();
void Thumb_Destroy();

// ---------------- 系统托盘里隐藏的图标（tray.cpp）----------------
struct TrayApp {
    std::wstring name, exe;
    std::shared_ptr<Gdiplus::Bitmap> icon;
    bool promoted = false;  // 用户设成在任务栏上一直显示
};
void Tray_Refresh();                    // 在后台重新读一遍（已经在读就不管）
std::vector<TrayApp> Tray_Hidden();     // 最近读到的、正在运行、收在“隐藏的图标”里的
// 在后台线程上去系统托盘里点它。右键弹不出程序自己的菜单时给 notify（为空则是主窗口）发 WM_APP_TRAY_MENU，
// lParam 是 new 出来的 TrayApp，收到的一方负责 delete
void Tray_Click(const TrayApp& app, bool right, HWND notify = nullptr);
void Tray_ShowHidden();                 // 展开系统的“隐藏的图标”
bool Tray_Busy();                       // 还在系统托盘里找、点
// 迷你任务栏自己的菜单：打开、打开文件所在的位置、结束程序，appMenu 时再加一项“程序菜单”。
// 由 owner 在屏幕坐标 at 弹出（默认底边对着它）。选了别的项时先调用 picked（可以为空，比如收起迷你任务栏）再去做；
// 返回 true = 选了“程序菜单”，由调用的一方去系统托盘里右键它
bool Tray_Menu(const TrayApp& app, HWND owner, POINT at, void (*picked)(), bool appMenu = false,
               UINT align = TPM_BOTTOMALIGN);  // align：TrackPopupMenu 的对齐方式
bool Tray_OnMessage(UINT msg, LPARAM lParam);  // 主窗口收到 WM_APP_TRAY_LAUNCH、WM_APP_TRAY_MENU 时交给它

// ---------------- 迷你任务栏 ----------------
void Popup_Init();
void Popup_Destroy();
void Popup_Show();
void Popup_Hide();
void Popup_Toggle();
// 任务栏藏着时短按了 Win：先弹出迷你任务栏，弹完再打开开始菜单（开始菜单开着时迷你任务栏一直在）；
// 任务栏没藏着、开始菜单已经开着时照常补发 Win
void Popup_StartMenu(DWORD vk);
// 设置窗口里调“迷你任务栏大小”“位置”“对齐”时按 percent、edge（PopupEdge）、align（PopupAlign）弹出预览
// （不抢前台，点不到）。hold：还按着滑块，先不收起；否则过一会儿自己收起
void Popup_Preview(int percent, int edge, int align, bool hold);
// 预览用的外观。在 Popup_Preview 之前调用，预览开着时马上换上
struct PopupLook {
    int style = kGlassLiquid;  // GlassStyle
    bool light = false;        // 浅色玻璃（自定义底色时按底色的深浅定）
    int transparency = kDefaultGlassTransparency;
    int radius = 22;
    int iconSize = 100, iconGap = kDefaultIconGap;
    bool showTitle = true;
    bool customTint = false, customAccent = false;
    COLORREF tint = 0, accent = 0;
};
PopupLook LookFromSettings();  // 现在的设置对应的外观
void Popup_PreviewLook(const PopupLook& look);
void Popup_EndPreview();  // 马上收起预览
void Popup_Configure();   // 按设置开关“鼠标停在屏幕边上时弹出”
void Popup_Refresh();     // 开着的话重画（比如要提醒的窗口变了）
HMONITOR Popup_Monitor();  // 迷你任务栏开着（不算预览）时在哪块屏幕上，没开着为空

// ---------------- 消息提醒（attention.cpp）----------------
// 程序在任务栏上闪烁（来了新消息）时记下它：迷你任务栏里它的格子变色；任务栏被本程序藏着时，
// 那块屏幕放任务栏的那条边上再亮起一道光。窗口到了前台、关掉或者藏起来以后不再提醒
void Attention_Configure();  // 按 g_settings.flashAlert 开关，主窗口建好以后调用
bool Attention_OnMessage(UINT msg, WPARAM wParam, LPARAM lParam);  // 主窗口收到的是外壳钩子消息就处理掉，返回 true
bool Attention_IsUrgent(HWND hwnd);  // 这个窗口闪过、还没去看
void Attention_Update();     // 迷你任务栏弹出、收起以后：马上重新看光要不要亮
void Attention_Shutdown();

// ---------------- 电量和输入法（indicators.cpp）----------------
struct BatteryInfo {
    bool present = false;  // 有电池
    int percent = -1;      // 0 – 100，-1 = 不知道
    bool charging = false;
    bool plugged = false;  // 接着电源
    int secondsLeft = -1;  // 还能用多久，-1 = 不知道
};
BatteryInfo Battery_Get();
struct ImeInfo {
    std::wstring label;      // 格子里显示的：中 / 英 / あ / A / ENG
    std::wstring text;       // 鼠标指着时的标题
    bool canToggle = false;  // 点一下能切换中英文
};
ImeInfo Ime_Get(HWND window);  // window 所在线程的输入法；读不到中英文时按键盘布局的语言显示
bool Ime_Toggle(HWND window);  // 切换中英文，成功返回 true

// ---------------- 正在播放的媒体（media.cpp）----------------
// 走 WinRT 的 GlobalSystemMediaTransportControlsSessionManager（和系统音量弹窗里的媒体控制是同一个），
// 读和发命令都在后台线程上做，读到的和上次不一样（或者是要求读的）时 PostMessage(notify, WM_APP_MEDIA, 0, 0)
constexpr UINT WM_APP_MEDIA = WM_APP + 33;
enum MediaCommand { kMediaPrevious, kMediaPlayPause, kMediaNext };
struct MediaInfo {
    bool present = false;  // 有在播放或者暂停着的
    bool playing = false;
    bool canPrevious = false, canNext = false, canPlayPause = false;
    std::wstring title, artist;
    std::shared_ptr<Gdiplus::Bitmap> cover;  // 封面（裁成正方形、缩小过），没有为空
};
void Media_Configure(bool enabled, HWND notify);  // 开着时后台每隔几秒读一遍，弹出时就知道有没有在放
void Media_Query();                // 马上读一遍
void Media_Command(int command);   // MediaCommand
MediaInfo Media_Get();             // 最近读到的
void Media_Stop();                 // 退出前结束后台线程

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

// ---------------- Wi-Fi 和蓝牙（radios.cpp）----------------
// 读和切换都在后台线程上做，做完 PostMessage(notify, WM_APP_RADIOS, 切换失败的开关（1 << RadioId，没失败为 0）, 0)，
// 收到后用 Radios_Get 取最新的样子
constexpr UINT WM_APP_RADIOS = WM_APP + 32;
enum RadioId { kRadioWifi, kRadioBluetooth, kRadioCount };
enum RadioStatus {
    kRadioReading,      // 还没读到
    kRadioMissing,      // 这台电脑上没有
    kRadioUnavailable,  // 读不到，或者被禁用了（硬件开关、设备管理器）
    kRadioOff,
    kRadioOn,
};
struct RadioInfo {
    int status = kRadioReading;  // 见 RadioStatus
    bool switching = false;      // 正在切换，status 是要切成的样子
    std::wstring network;        // Wi-Fi 连着的网络
};
void Radios_Query(HWND notify);
void Radios_Set(int radio, bool on, HWND notify);
RadioInfo Radios_Get(int radio);
void Radios_Stop();  // 退出前结束后台线程

// ---------------- 设置窗口 ----------------
void SettingsDialog_Show();
void SettingsDialog_SettingsChanged();  // 在别处（迷你任务栏的编辑模式里）改了设置：窗口开着的话没动过的项跟着变

// ---------------- 小窗口留在全屏窗口上面 ----------------
void FloatWindows_Configure(bool enabled, bool topmost = false);  // topmost：固定的小窗口在所有窗口上面
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
    // 截 window 四周（再多一圈）的屏幕，模糊、提高饱和度（style：GlassStyle，Win11 原版模糊得更厉害）。
    // 不碰任何成员，可以在后台线程里调用。
    // same 不为空、范围相同、截到的和它一模一样（或者截图失败）时不做后面的处理，返回 false
    static bool Shoot(const RECT& window, float scale, int style, GlassShot& out, const GlassShot* same = nullptr);
    // 截下 window（要画玻璃的窗口在屏幕上的位置）后面的背景，按现在的材质处理
    void Capture(const RECT& window, float scale);
    // 背景换成更大的 window 后面的：旧截图范围里那块用旧的原图（窗口自己正盖在上面），其余从屏幕上截。
    // 已经截过这么大就不再截
    void Extend(const RECT& window);
    // 换上一张截好的背景
    void Adopt(GlassShot&& shot, float scale);
    const std::vector<DWORD>& Raw() const { return m_raw; }
    void SetLight(bool light) { m_light = light; }        // 浅色玻璃配深色文字，深色玻璃配浅色文字
    bool Light() const { return m_light; }
    void SetScale(float scale) { m_scale = scale; }  // 只换边缘、阴影等的尺寸，背景不重新截
    // 材质（GlassStyle）。Win11 原版：没有边缘折射和高光，再模糊一些，浓浓的系统灰色着色，一圈细边框
    void SetStyle(int style) { m_style = style; }
    int Style() const { return m_style; }
    // 液态玻璃的透明度（0 – 100%）：越透明着色越淡。默认 kDefaultGlassTransparency
    void SetTransparency(int percent) { m_transparency = percent; }
    // 自己选的底色（着色），custom 为 false 时按深浅色用默认的
    void SetTint(bool custom, COLORREF color) {
        m_customTint = custom;
        m_tint = color;
    }
    const RECT& Area() const { return m_area; }  // 背景截图在屏幕上的范围
    // 按面板静止时的形状预先算好每个像素的材质（覆盖率、阴影、折射位移、高光），尺寸不变时直接返回。
    // width×height：窗口像素大小；panel：面板在窗口里的位置；radius：圆角半径
    void Prepare(int width, int height, const Gdiplus::RectF& panel, float radius);
    // 在预乘 BGRA 像素上画阴影和玻璃面板。origin：窗口左上角在屏幕上的位置；
    // slide / slideX：面板往下 / 往右平移的像素数（弹出 / 收起动画），背景不跟着动
    void Render(DWORD* pixels, POINT origin, int slide, int slideX = 0) const;

private:
    struct Texel {
        float dx = 0, dy = 0;  // 折射：去背景的哪里取色（相对本像素，绿色通道的位移）
        BYTE cover = 0;        // 面板覆盖率
        BYTE shadow = 0;       // 阴影的不透明度
        BYTE light = 0;        // 叠加的白光（边缘高光）；Win11 原版是边框的浓淡
        BYTE gloss = 0;        // 顶部光泽的位置权重，乘上深浅色各自的强度
        BYTE bevel = 0;        // 1 = 在边缘弯曲的一圈里，要折射取样
    };
    void RenderRows(DWORD* pixels, POINT origin, int slide, int slideX, int rowBegin, int rowEnd) const;
    void SampleBevel(float x, float y, const Texel& t, float rgb[3]) const;
    DWORD Fetch(int x, int y) const;

    std::vector<DWORD> m_raw;   // 截下来的原图，再截时比较有没有变
    std::vector<DWORD> m_blur;  // 模糊后的背景
    RECT m_area = {};           // 背景截图在屏幕上的范围
    int m_w = 0, m_h = 0;
    float m_scale = 1;
    bool m_light = false;
    int m_style = kGlassLiquid;
    int m_transparency = kDefaultGlassTransparency;
    bool m_customTint = false;
    COLORREF m_tint = 0;

    std::vector<Texel> m_texels;  // 面板静止时窗口里每个像素的材质
    int m_texW = 0, m_texH = 0;
    Gdiplus::RectF m_texPanel;
    float m_texRadius = 0;
    float m_texScale = 0;
    int m_texStyle = -1;
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
// 游戏、全屏视频这类盖住整块屏幕又不是最大化的窗口（最大化的窗口铺满整块屏不算）
bool IsFullscreenApp(HWND hwnd);
void ForceForeground(HWND hwnd);
float MonitorScale(HMONITOR monitor);
std::wstring MonitorKey(HMONITOR monitor);    // 认出同一块屏幕用的：接口名（换了排列、重新接上都不变），取不到时用设备名
std::wstring MonitorLabel(HMONITOR monitor);  // 给人看的名字：屏幕 2（主屏幕）
void AddRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius);
std::shared_ptr<Gdiplus::Bitmap> BitmapFromIcon(HICON hIcon);
double NowMs();        // 高精度时钟，毫秒
DWORD WindowsBuild();  // 系统的内部版本号（Win10 2004 是 19041，Win11 从 22000 起），取不到时为 0
// 诊断日志：g_settings.debugLog 打开时追加一行到 SettingsDir() 下的 debug.log，可以在任何线程上调用
void Log(const wchar_t* format, ...);

// lang.cpp：界面文字。代码里写中文，英文界面时 Tr 查表换成英文
bool LanguageIsEnglish(int language);  // 这个设置（Language）下用不用英文
void SetUiEnglish(bool english);
bool UiEnglish();
const wchar_t* Tr(const wchar_t* zh);
std::wstring TrF(const wchar_t* zhFormat, ...);  // Tr 以后按格式填进参数
std::wstring LogFile();
void WaitForVBlank();  // 等到下一次屏幕刷新，让动画按显示器的节奏走
bool CaptureScreen(const RECT& area, std::vector<DWORD>& pixels);  // 截屏，BGRA，不透明
void SetWindowRectAsync(HWND hwnd, const RECT& rect);  // 挪别的程序的窗口：异步、不激活、不改层次

// System.AppUserModel.ID，自己定义一份，免得依赖 propkey.h 的链接方式
constexpr PROPERTYKEY kPkeyAppUserModelId = {
    {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

}  // namespace app
