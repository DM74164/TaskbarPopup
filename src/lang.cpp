// 界面语言：代码里的界面文字都写中文，Tr(L"中文") 在英文界面时查表换成英文（表里没有的原样显示）。
// 诊断日志只用中文，不翻译。
#include "common.h"

#include <atomic>
#include <cstdarg>
#include <string_view>
#include <unordered_map>

namespace app {
namespace {

std::atomic<bool> s_english{false};

const wchar_t* const kEnglish[][2] = {
    // 设置窗口
    {L"TaskbarPopup 设置", L"TaskbarPopup Settings"},
    {L"任务栏", L"Taskbar"},
    {L"迷你任务栏", L"Mini taskbar"},
    {L"外观", L"Appearance"},
    {L"快捷键", L"Hotkeys"},
    {L"启动和更新", L"Startup & updates"},
    {L"全屏", L"Fullscreen"},
    {L"小窗口", L"Small windows"},
    {L"最大化时不隐藏任务栏的程序", L"Apps that keep the taskbar"},
    {L"最大化时隐藏任务栏的程序", L"Apps that hide the taskbar"},
    {L"弹出", L"Opening"},
    {L"显示", L"Contents"},
    {L"界面", L"Interface"},
    {L"最大化或全屏时隐藏任务栏", L"Hide taskbar when maximized or fullscreen"},
    {L"窗口铺满整块屏幕，要用任务栏时长按 Win", L"Windows fill the whole screen; hold Win for the taskbar"},
    {L"点全屏窗口时小窗口留在上面", L"Keep small windows above fullscreen ones"},
    {L"双击小窗口或按快捷键固定它，最小化就取消", L"Double-click or use the hotkey to pin; minimize to unpin"},
    {L"固定的小窗口在所有窗口上面", L"Keep pinned windows above all windows"},
    {L"长按 Win 键弹出迷你任务栏", L"Hold Win to show the mini taskbar"},
    {L"左键单击托盘图标也能弹出", L"Clicking the tray icon works too"},
    {L"显示固定在任务栏的应用", L"Show pinned apps"},
    {L"显示音量和亮度调节", L"Show volume and brightness"},
    {L"显示 Wi-Fi 和蓝牙开关", L"Show Wi-Fi and Bluetooth toggles"},
    {L"右键按钮打开系统设置", L"Right-click a toggle to open its settings"},
    {L"显示托盘里隐藏的图标", L"Show hidden tray icons"},
    {L"左键、右键和在系统托盘里点一样", L"Left and right clicks work like in the system tray"},
    {L"开机自动启动", L"Start with Windows"},
    {L"自动检查更新", L"Auto-check for updates"},
    {L"每天看一次 GitHub 上有没有新版本", L"Checks GitHub once a day"},
    {L"长按时长", L"Hold time"},
    {L"迷你任务栏大小", L"Mini taskbar size"},
    {L"透明度", L"Transparency"},
    {L"%d 毫秒", L"%d ms"},
    {L"材质", L"Material"},
    {L"液态玻璃", L"Liquid glass"},
    {L"毛玻璃", L"Frosted"},
    {L"颜色", L"Theme"},
    {L"跟随系统", L"System"},
    {L"浅色", L"Light"},
    {L"深色", L"Dark"},
    {L"托盘图标", L"Tray icons"},
    {L"小图标堆叠", L"Stacked"},
    {L"横向完整展示", L"In a row"},
    {L"对齐", L"Alignment"},
    {L"居中", L"Center"},
    {L"靠左", L"Left"},
    {L"靠右", L"Right"},
    {L"位置", L"Position"},
    {L"底部", L"Bottom"},
    {L"左侧", L"Left"},
    {L"右侧", L"Right"},
    {L"语言", L"Language"},
    {L"弹出迷你任务栏", L"Show mini taskbar"},
    {L"再按一次收起", L"Press again to hide"},
    {L"固定前台的小窗口", L"Pin active small window"},
    {L"再按一次取消；鼠标键固定鼠标下的窗口", L"Press again to unpin; mouse buttons pin the window under the cursor"},
    {L"单按", L"Press"},
    {L"长按", L"Hold"},
    {L"按下按键或鼠标键，Esc 取消", L"Press a key or mouse button, Esc to cancel"},
    {L"被别的程序占用了，换一个吧", L"In use by another app, try another"},
    {L"按下按键…", L"Press a key…"},
    {L"未设置", L"Not set"},
    {L"和另一个快捷键重复了", L"Same as the other hotkey"},
    {L"＋ 添加程序", L"+ Add app"},
    {L"也可以在迷你任务栏里右键运行中的程序来添加或去掉",
     L"You can also add or remove an app by right-clicking it in the mini taskbar"},
    {L"立即检查", L"Check now"},
    {L"版本", L"Version"},
    {L"确定", L"OK"},
    {L"取消", L"Cancel"},
    {L"程序 (*.exe)", L"Programs (*.exe)"},
    {L"选择最大化时不隐藏任务栏的程序", L"Choose an app that keeps the taskbar"},
    {L"选择最大化时隐藏任务栏的程序", L"Choose an app that hides the taskbar"},
    {L"没有其它开着窗口的程序", L"No other apps with open windows"},
    {L"浏览...", L"Browse..."},

    // 快捷键
    {L"左 Ctrl", L"Left Ctrl"},
    {L"右 Ctrl", L"Right Ctrl"},
    {L"左 Shift", L"Left Shift"},
    {L"右 Shift", L"Right Shift"},
    {L"左 Alt", L"Left Alt"},
    {L"右 Alt", L"Right Alt"},
    {L"鼠标左键", L"Left Mouse"},
    {L"鼠标右键", L"Right Mouse"},
    {L"鼠标中键", L"Middle Mouse"},
    {L"鼠标后退键", L"Mouse Back"},
    {L"鼠标前进键", L"Mouse Forward"},
    {L"这个键不能用，换一个", L"This key can't be used, try another"},
    {L"鼠标左键要配上 Ctrl、Shift 或 Alt", L"Left click needs Ctrl, Shift or Alt"},
    {L"单按右键的话右键菜单就打不开了，配上修饰键或改成长按",
     L"Right click alone would block context menus; add a modifier or use Hold"},
    {L"单按会打不了字，配上 Ctrl / Alt 或改成长按", L"Alone it would block typing; add Ctrl / Alt or use Hold"},
    {L"F12 单按留给系统了，配上修饰键或改成长按", L"F12 alone is reserved by Windows; add a modifier or use Hold"},
    {L"快捷键 %ls 被别的程序占用了，在设置里换一个吧。", L"Hotkey %ls is in use by another app. Pick another one in Settings."},

    // 托盘图标和菜单
    {L"TaskbarPopup：全屏隐藏任务栏 / 长按 Win 弹出", L"TaskbarPopup: fullscreen without taskbar / hold Win"},
    {L"长按 Win 键弹出迷你任务栏（%d 毫秒）", L"Hold Win to show the mini taskbar (%d ms)"},
    {L"显示迷你任务栏", L"Show mini taskbar"},
    {L"设置...", L"Settings..."},
    {L"检查更新", L"Check for updates"},
    {L"窗口最大化或全屏时隐藏任务栏", L"Hide taskbar when maximized or fullscreen"},
    {L"以管理员身份运行（游戏里也能长按 Win）", L"Run as administrator (hold Win works in games)"},
    {L"记录诊断日志", L"Record diagnostic log"},
    {L"打开诊断日志所在文件夹", L"Open diagnostic log folder"},
    {L"退出", L"Exit"},
    {L"无法安装键盘钩子，长按 Win 功能不可用。", L"Couldn't install the keyboard hook. Holding Win won't work."},

    // 迷你任务栏
    {L"第 %d/%d 页", L"Page %d/%d"},
    {L"%ls（第 %d/%d 页）", L"%ls (page %d/%d)"},
    {L"没有可用的播放设备", L"No playback device"},
    {L"已静音（点喇叭取消）", L"Muted (click the speaker to unmute)"},
    {L"音量 %d%%", L"Volume %d%%"},
    {L"正在读取亮度…", L"Reading brightness…"},
    {L"这块屏幕不支持调节亮度", L"This display doesn't support brightness control"},
    {L"亮度 %d%%", L"Brightness %d%%"},
    {L"蓝牙", L"Bluetooth"},
    {L"Wi-Fi：%ls", L"Wi-Fi: %ls"},
    {L"Wi-Fi 已打开", L"Wi-Fi on"},
    {L"Wi-Fi 已关闭", L"Wi-Fi off"},
    {L"蓝牙已打开", L"Bluetooth on"},
    {L"蓝牙已关闭", L"Bluetooth off"},
    {L"正在打开 Wi-Fi…", L"Turning Wi-Fi on…"},
    {L"正在关闭 Wi-Fi…", L"Turning Wi-Fi off…"},
    {L"正在打开蓝牙…", L"Turning Bluetooth on…"},
    {L"正在关闭蓝牙…", L"Turning Bluetooth off…"},
    {L"Wi-Fi 切换失败", L"Couldn't switch Wi-Fi"},
    {L"蓝牙切换失败", L"Couldn't switch Bluetooth"},
    {L"没有 Wi-Fi", L"No Wi-Fi"},
    {L"没有蓝牙", L"No Bluetooth"},
    {L"Wi-Fi 不可用", L"Wi-Fi unavailable"},
    {L"蓝牙不可用", L"Bluetooth unavailable"},
    {L"开始", L"Start"},
    {L"没有打开的窗口", L"No open windows"},
    {L"没有匹配的窗口或应用", L"No matching windows or apps"},
    {L"最近", L"Recent"},
    {L"打开", L"Open"},
    {L"以管理员身份运行", L"Run as administrator"},
    {L"打开文件所在的位置", L"Open file location"},
    {L"还原", L"Restore"},
    {L"最小化", L"Minimize"},
    {L"最大化", L"Maximize"},
    {L"最大化时不隐藏任务栏", L"Don't hide taskbar when maximized"},
    {L"最大化时隐藏任务栏", L"Hide taskbar when maximized"},
    {L"关闭窗口", L"Close window"},
    {L"关闭全部 %d 个窗口", L"Close all %d windows"},
    {L"程序菜单", L"App menu"},
    {L"结束程序", L"End task"},
    {L"结束“%ls”？\n它没保存的内容会丢失。", L"End \"%ls\"?\nAnything it hasn't saved will be lost."},

    // 固定小窗口的提示
    {L"已取消固定", L"Unpinned"},
    {L"这个窗口固定不了", L"This window can't be pinned"},
    {L"最大化、全屏的窗口不用固定", L"Maximized or fullscreen windows don't need pinning"},
    {L"这个窗口本来就在最上面", L"This window is already always on top"},
    {L"已固定，留在所有窗口上面", L"Pinned: stays above all windows"},
    {L"已固定，点全屏窗口时它留在上面", L"Pinned: stays above fullscreen windows"},

    // 管理员权限
    {L"这个程序里长按 Win 不起作用", L"Holding Win doesn't work in this app"},
    {L"前台程序以管理员身份运行（不少游戏是这样），普通权限的 TaskbarPopup 收不到它里面的按键。"
     L"在托盘菜单里勾选“以管理员身份运行”即可。",
     L"The app in front runs as administrator (many games do), so TaskbarPopup can't see its keys. "
     L"Turn on \"Run as administrator\" in the tray menu."},
    {L"资源管理器还没准备好，稍后再试一次。", L"File Explorer isn't ready yet. Try again in a moment."},
    {L"暂时无法启动", L"Can't start right now"},

    // 检查更新
    {L"连接超时", L"Connection timed out"},
    {L"找不到服务器，可能没有联网", L"Server not found; you may be offline"},
    {L"连不上 GitHub", L"Can't reach GitHub"},
    {L"安全连接失败", L"Secure connection failed"},
    {L"网络错误 %lu", L"Network error %lu"},
    {L"还没有发布过正式版本", L"No release has been published yet"},
    {L"GitHub 暂时限制了请求次数，稍后再试", L"GitHub is limiting requests for now; try again later"},
    {L"服务器返回 %lu", L"Server returned %lu"},
    {L"服务器的回应不对", L"Unexpected server response"},
    {L"发现新版本 %ls", L"New version %ls available"},
    {L"点这里打开下载页面", L"Click to open the download page"},
    {L"发现新版本 %ls，现在打开下载页面吗？", L"New version %ls is available. Open the download page now?"},
    {L"检查更新失败：无法启动后台线程", L"Update check failed: couldn't start a background thread"},
    {L"检查更新失败：%ls", L"Update check failed: %ls"},
    {L"发现新版本 %ls，下载文件还没准备好，过一会儿再检查",
     L"New version %ls found, but its download isn't ready yet. Check again later."},
    {L"已经是最新版本（%ls）", L"You're up to date (%ls)"},
};

}  // namespace

bool LanguageIsEnglish(int language) {
    if (language == kLangEnglish) return true;
    if (language == kLangChinese) return false;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_CHINESE;  // 跟随系统：不是中文系统就用英文
}

void SetUiEnglish(bool english) { s_english = english; }
bool UiEnglish() { return s_english; }

const wchar_t* Tr(const wchar_t* zh) {
    if (!zh || !s_english) return zh;
    static const std::unordered_map<std::wstring_view, const wchar_t*> table = [] {
        std::unordered_map<std::wstring_view, const wchar_t*> map;
        for (const auto& pair : kEnglish) map.emplace(pair[0], pair[1]);
        return map;
    }();
    auto it = table.find(zh);
    return it == table.end() ? zh : it->second;
}

std::wstring TrF(const wchar_t* zhFormat, ...) {
    wchar_t text[1024];
    va_list args;
    va_start(args, zhFormat);
    vswprintf(text, ARRAYSIZE(text), Tr(zhFormat), args);
    va_end(args);
    text[ARRAYSIZE(text) - 1] = L'\0';
    return text;
}

}  // namespace app
