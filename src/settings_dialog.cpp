// 设置窗口：和迷你任务栏一样的液态玻璃面板，开关、滑块、名单都是自己画的。
//
// 分层窗口贴的是“玻璃 + 内容”的整张图：玻璃按窗口后面的屏幕内容折射。后面的画面在后台线程里
// 一遍遍截（截图时排除本窗口），变了就重画；拖动时按新位置去截好的背景里取色，并不停地截窗口四周
// 一大片，玻璃跟着实时变。界面线程只管画，不等截图。
#include "common.h"
#include "version.h"

#include <cmath>
#include <memory>

using namespace Gdiplus;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace app {
namespace {

constexpr wchar_t kClassName[] = L"TaskbarPopupSettings";
constexpr UINT_PTR kTimerLive = 2;  // 定时刷新玻璃后面的背景
constexpr UINT_PTR kTimerSave = 3;  // 改了设置过一小会儿存下、生效（按住方向键调滑块时不用每下都存）
constexpr UINT kSaveDelayMs = 150;
constexpr UINT WM_APP_SHOT = WM_APP + 40;   // 后台截好背景了，lParam 是 ShotJob*
constexpr UINT WM_APP_FRAME = WM_APP + 41;  // 淡入、开关滑块动画的下一帧
constexpr double kFadeMs = 180.0 * TP_ANIM_SCALE;
constexpr double kKnobMs = 150.0 * TP_ANIM_SCALE;

// ---- 尺寸（96 DPI 下的像素，乘 s_scale 得到物理像素） ----
constexpr float kShadow = 28;   // 面板外面留给阴影的一圈
constexpr float kPad = 20;      // 面板内边距
constexpr float kColW = 360;    // 右边内容的宽度（英文界面宽一些，见 ColW）
constexpr float kColWEnglish = 430;
constexpr float kNavW = 140;    // 左边导航至少这么宽，字长的话跟着宽
constexpr float kNavGap = 16;   // 导航和内容之间
constexpr float kNavItemH = 38;
constexpr float kHeaderH = 52;  // 标题栏（可以拖动）
constexpr float kGroupTitleH = 24;
constexpr float kGroupGap = 12;
constexpr float kRowPad = 14;  // 卡片里左右的边距
constexpr float kChipH = 28;

// 分类：左边导航一项一页
enum Page {
    kPageTaskbar, kPageMini, kPageControls, kPageSize, kPageScreens, kPageLook, kPagePresets, kPageHotkeys, kPageStartup,
    kPageCount
};
const wchar_t* const kPageNames[kPageCount] = {L"任务栏", L"迷你任务栏", L"状态和控制", L"大小和位置", L"多屏幕",
                                               L"外观",   L"方案和备份", L"快捷键",     L"启动和更新"};

// 开关
enum Toggle { kAutoHide, kKeepFloats, kPinTopmost, kLongPress, kPinned, kLevels, kRadios, kTray, kAutoStart, kUpdates, kPerScreen,
             kAlwaysHide, kFlashAlert, kEdgePopup, kMedia, kIme, kBattery, kClock, kClockSeconds, kClockWeekday, kClockDate,
             kShowTitle, kKeepOtherBars, kMarkScreens, kBrightAll, kToggleCount };
struct ToggleText {
    const wchar_t* label;
    const wchar_t* sub;  // 可以为空
};
const ToggleText kToggleText[kToggleCount] = {
    {L"最大化或全屏时隐藏任务栏", L"窗口铺满整块屏幕，要用任务栏时长按 Win"},
    {L"点全屏窗口时小窗口留在上面", L"双击小窗口或按快捷键固定它，最小化就取消"},
    {L"固定的小窗口在所有窗口上面", nullptr},
    {L"长按 Win 键弹出迷你任务栏", L"左键单击托盘图标也能弹出"},
    {L"显示固定在任务栏的应用", nullptr},
    {L"显示音量和亮度调节", nullptr},
    {L"显示 Wi-Fi 和蓝牙开关", L"右键按钮弹出网络、蓝牙列表"},
    {L"显示托盘里隐藏的图标", L"左键、右键和在系统托盘里点一样"},
    {L"开机自动启动", nullptr},
    {L"自动检查更新", L"每天看一次 GitHub 上有没有新版本"},
    {L"每块屏幕分别设置位置和对齐", L"改的是设置窗口所在的那块屏幕"},
    {L"一直隐藏任务栏", L"没有窗口最大化时也藏着，鼠标碰到屏幕边上也不弹出"},
    {L"任务栏藏着时提醒新消息", L"程序闪烁时屏幕边亮起，迷你任务栏里的图标也变色"},
    {L"鼠标停在屏幕边上时弹出", L"任务栏藏着时，鼠标在迷你任务栏那条边上停一下"},
    {L"显示正在播放的媒体", L"点封面展开上一首、下一首"},
    {L"显示输入法中英文", L"点一下切换中英文"},
    {L"显示电量", L"没有电池时不显示"},
    {L"显示时钟", nullptr},
    {L"秒", nullptr},  // 时钟的这三项是“同时显示”那一排小胶囊，见 ClockPartsRow
    {L"星期", nullptr},
    {L"日期", nullptr},
    {L"显示标题行", L"关掉时名字显示在面板上方"},
    {L"其它屏幕的任务栏照常显示", L"Edge、Chrome 这类程序最大化时还是一起藏"},
    {L"标出其它屏幕上的窗口", L"图标右上角显示屏幕编号"},
    {L"所有屏幕亮度一起调", nullptr},
};

// 滑块
// 快捷键的长按时长：kPopupHoldMs + HotkeyId
enum SliderId { kLongPressMs, kRadius, kPopupHoldMs, kPinHoldMs, kMoveHoldMs, kNextHoldMs, kGlassAlpha, kIconSize, kIconGap, kSliderCount };
static_assert(kPopupHoldMs + kHotkeyCount == kGlassAlpha, "每个快捷键一个长按时长");
struct SliderSpec {
    const wchar_t* label;
    int minValue, maxValue, step;
};
const SliderSpec kSliderSpec[kSliderCount] = {
    {L"长按时长", kMinLongPressMs, kMaxLongPressMs, 100},
    {L"圆角", 0, kMaxPopupRadius, 2},
    {L"长按时长", kMinLongPressMs, kMaxLongPressMs, 100},
    {L"长按时长", kMinLongPressMs, kMaxLongPressMs, 100},
    {L"长按时长", kMinLongPressMs, kMaxLongPressMs, 100},
    {L"长按时长", kMinLongPressMs, kMaxLongPressMs, 100},
    {L"透明度", 0, 100, 5},
    {L"图标大小", kMinIconSize, kMaxIconSize, 5},
    {L"图标间距", 0, kMaxIconGap, 1},
};

// 几选一（分段按钮）
enum SegId {
    kSegStyle, kSegTheme, kSegTray, kSegAlign, kSegEdge, kSegLang, kSegMonitor, kSegTint, kSegAccent, kSegSide, kSegWindows,
    kSegCount
};
struct SegSpec {
    const wchar_t* label;
    std::vector<const wchar_t*> options;  // 下标就是存进设置的值
};
const SegSpec kSegSpec[kSegCount] = {
    {L"材质", {L"液态玻璃", L"Win11"}},  // 下标是 GlassStyle
    {L"颜色", {L"跟随系统", L"浅色", L"深色"}},
    {L"托盘图标", {L"小图标堆叠", L"横向完整展示"}},  // 下标是 TrayStyle
    {L"对齐", {L"跟随系统", L"居中", L"靠左", L"靠右", L"自定义"}},  // 下标是 PopupAlign；“自定义”只在编辑模式里拖过时出现
    {L"位置", {L"底部", L"左侧", L"右侧"}},  // 下标是 PopupEdge
    {L"语言", {L"跟随系统", L"中文", L"English"}},  // 下标是 Language；“中文”“English”不翻译
    {L"弹在", {L"鼠标所在屏幕", L"前台窗口所在屏幕"}},  // 下标是 PopupMonitor
    {L"底色", {L"跟随颜色", L"自定义"}},
    {L"强调色", {L"跟随系统", L"自定义"}},
    {L"迷你任务栏在", {L"底部", L"侧边"}},  // 下标是 ModuleSide：“状态和控制”页的开关改的是哪一份（只是换着看，不存）
    {L"显示的窗口", {L"跟随系统", L"所有屏幕", L"这块屏幕"}},  // 下标是 PopupWindows
};
constexpr int kSegStride = 8;  // 分段按钮的 Item::index = 第几组 * kSegStride + 第几项

// 快捷键：下标是 HotkeyId
const ToggleText kHotkeyText[kHotkeyCount] = {
    {L"弹出迷你任务栏", L"再按一次收起"},
    {L"固定前台的小窗口", L"再按一次取消；鼠标键固定鼠标下的窗口"},
    {L"把前台窗口移到下一块屏幕", L"鼠标键移的是鼠标下的窗口"},
    {L"在下一块屏幕弹出迷你任务栏", L"再按一次换到再下一块"},
};
const wchar_t* const kHotkeyModes[2] = {L"单按", L"长按"};  // 快捷键的 kHotkeyMode：Item::index = 第几个 * kSegStride + 这个下标

// 能点的东西
enum Kind {
    kNoItem, kToggleRow, kSliderRow, kSegment, kHotkeyRow, kHotkeyClear, kHotkeyMode, kCheckNow, kChip, kAddChip, kClose,
    kNav, kSwatch, kPreset, kAddPreset, kFile, kToggleChip, kScreenChip
};
struct Item {
    Kind kind = kNoItem;
    int index = 0;  // 第几个开关 / 滑块 / 程序；分段按钮见 kSegStride
    RectF rect;     // 窗口里的位置（物理像素）
    bool operator==(const Item& o) const { return kind == o.kind && index == o.index; }
};

// 窗口里的各项值。改了马上存（见 ScheduleSave），只改用户在窗口里动过的项：在迷你任务栏或托盘菜单里改的设置不会被盖掉
struct Values {
    bool on[kToggleCount] = {};  // 底部、侧边各一份的那几个开关显示的是 seg[kSegSide] 那份，存的是 sides
    ModuleSet sides[2];          // 只用里面显示哪些，顺序在迷你任务栏的编辑模式里排
    int slider[kSliderCount] = {};
    int seg[kSegCount] = {};  // 位置、对齐两项是窗口里显示的（分屏设置时是设置窗口所在屏幕的），存的是下面三项
    int edge = kEdgeBottom, align = kAlignSystem;  // 所有屏幕共用的位置和对齐
    int offset = 500;                              // 所有屏幕共用的自定义位置（在迷你任务栏的编辑模式里拖的）
    std::vector<ScreenPlace> screens;              // 每块屏幕自己的
    COLORREF tint = 0, accent = 0;                 // 自定义的底色、强调色
    UINT hotkey[kHotkeyCount] = {};
    bool hold[kHotkeyCount] = {};
    std::vector<std::wstring> exclude;  // 最大化时不隐藏任务栏的程序
    std::vector<std::wstring> include;  // 最大化时隐藏任务栏的程序（关着“最大化或全屏时隐藏任务栏”时用）
    std::vector<std::wstring> noHide;   // 不隐藏任务栏的屏幕（MonitorKey）
};
Values s_v, s_initial;

// 底部、侧边各一份的开关
struct SideToggle {
    int toggle;
    bool ModuleSet::*item;
};
const SideToggle kSideToggles[] = {{kTray, &ModuleSet::tray},       {kLevels, &ModuleSet::levels},
                                   {kRadios, &ModuleSet::radios},   {kMedia, &ModuleSet::media},
                                   {kIme, &ModuleSet::ime},         {kBattery, &ModuleSet::battery},
                                   {kClock, &ModuleSet::clock}};
const SideToggle* FindSideToggle(int toggle) {
    for (const SideToggle& t : kSideToggles)
        if (t.toggle == toggle) return &t;
    return nullptr;
}

// “迷你任务栏在”换到 side：开关显示那一份
void ShowSide(int side) {
    s_v.seg[kSegSide] = side;
    for (const SideToggle& t : kSideToggles) {
        s_v.on[t.toggle] = s_v.sides[side].*t.item;
        s_initial.on[t.toggle] = s_initial.sides[side].*t.item;
    }
}

// 窗口里现在显示的名单：跟着“最大化或全屏时隐藏任务栏”那个开关换
std::vector<std::wstring>& ShownApps() { return s_v.on[kAutoHide] ? s_v.exclude : s_v.include; }
const wchar_t* AppsTitle() { return s_v.on[kAutoHide] ? L"最大化时不隐藏任务栏的程序" : L"最大化时隐藏任务栏的程序"; }

// 设置窗口所在的屏幕：“每块屏幕分别设置”开着时位置和对齐改的是它的
HMONITOR s_screen = nullptr;
std::wstring s_screenKey;

bool MultiScreen() { return GetSystemMetrics(SM_CMONITORS) > 1; }

// 分段按钮显示现在要改的位置和对齐
void ShowPlace() {
    s_v.seg[kSegEdge] = s_v.edge;
    s_v.seg[kSegAlign] = s_v.align;
    if (!s_v.on[kPerScreen]) return;
    if (const ScreenPlace* p = FindScreenPlace(s_v.screens, s_screenKey)) {
        s_v.seg[kSegEdge] = p->edge;
        s_v.seg[kSegAlign] = p->align;
    }
}

// 点了位置或对齐：记到所有屏幕共用的，或者设置窗口所在屏幕自己的那一份上
void StorePlace() {
    if (!s_v.on[kPerScreen]) {
        s_v.edge = s_v.seg[kSegEdge];
        s_v.align = s_v.seg[kSegAlign];
        return;
    }
    ScreenPlace* p = FindScreenPlace(s_v.screens, s_screenKey);
    if (!p) {
        s_v.screens.push_back({s_screenKey, s_v.edge, s_v.align, s_v.offset});
        p = &s_v.screens.back();
    }
    p->edge = s_v.seg[kSegEdge];
    p->align = s_v.seg[kSegAlign];
}

struct Palette {
    Color text, subtle, card, cardRim, separator, accent, onAccent, trackOff, knob, button, buttonHot, textShadow, hover,
        warn;
};
const Palette kDark = {Color(240, 255, 255, 255), Color(165, 255, 255, 255), Color(20, 255, 255, 255),
                       Color(46, 255, 255, 255),  Color(32, 255, 255, 255),  Color(255, 96, 205, 255),
                       Color(255, 16, 22, 30),    Color(60, 255, 255, 255),  Color(255, 255, 255, 255),
                       Color(30, 255, 255, 255),  Color(56, 255, 255, 255),  Color(90, 0, 0, 0),
                       Color(22, 255, 255, 255),  Color(255, 255, 150, 120)};
const Palette kLight = {Color(235, 18, 18, 22),    Color(160, 18, 18, 22),    Color(105, 255, 255, 255),
                        Color(210, 255, 255, 255), Color(30, 0, 0, 0),        Color(255, 0, 95, 184),
                        Color(255, 255, 255, 255), Color(50, 0, 0, 0),        Color(255, 255, 255, 255),
                        Color(120, 255, 255, 255), Color(190, 255, 255, 255), Color(110, 255, 255, 255),
                        Color(16, 0, 0, 0),        Color(255, 196, 43, 28)};

HWND s_hwnd = nullptr;
float s_scale = 1;
int s_width = 0, s_height = 0;  // 窗口大小（含阴影）
int s_page = kPageTaskbar;      // 现在显示哪一页（关了再开还停在这一页）
POINT s_pos = {};
RectF s_panel;  // 面板在窗口里的位置
float s_radius = 0;

std::vector<Item> s_items;
std::vector<RectF> s_cards;  // 每组的卡片
struct Text {
    std::wstring text;
    RectF rect;
    int style;  // 0 组标题，1 提示，2 标题，3 版本号，4 一行的名字，5 迷你任务栏现在的大小和长度（画的时候再读，拖边改了也跟着变）
};
std::vector<Text> s_texts;
struct SliderGeom {
    float x0, x1, cy;
    RectF label;
};
SliderGeom s_sliderGeom[kSliderCount];
std::vector<float> s_rowLines;  // 卡片里两行之间的分隔线：每条 x0, x1, y 三个数
RectF s_segTrack[kSegCount];    // 分段按钮的底槽
RectF s_hotkeyBox[kHotkeyCount];  // 快捷键那一行右边显示按键的框
RectF s_hotkeyModeTrack[kHotkeyCount];  // 框下面“单按 / 长按”的底槽
int s_capture = -1;               // 正在录哪个快捷键，-1 = 没在录
std::wstring s_hotkeyError[kHotkeyCount];  // 刚按的键（或刚选的单按 / 长按）为什么不行
UINT s_captureLone = 0;           // 录的时候只按了这个 Ctrl / Shift / Alt（分左右），松开时就录它
bool s_eatChar = false;           // 录快捷键用掉了这次按键，接下来的 WM_SYSCHAR 别交给系统（会响一声）

Item s_hover, s_press, s_focus;
bool s_showFocus = false;  // 用键盘切换过焦点才画焦点框
int s_drag = -1;           // 正在拖的滑块
float s_knob[kToggleCount] = {};  // 开关滑块的位置 0~1，动画用
double s_knobFrom[kToggleCount] = {}, s_knobStart[kToggleCount] = {};
double s_openedAt = 0;
BYTE s_alpha = 255;

Glass s_glass;
std::vector<DWORD> s_base;  // 玻璃，悬停等重画直接复用
bool s_baseValid = false;
std::vector<DWORD> s_content;  // 玻璃上面的内容（透明底），拖动、背景变了只重画玻璃时直接复用
bool s_contentValid = false;
bool s_winDrag = false;  // 正在按住标题栏拖窗口
POINT s_dragOffset = {};  // 鼠标相对窗口左上角的位置
HDC s_canvasDC = nullptr;
HBITMAP s_canvas = nullptr;
HGDIOBJ s_canvasOld = nullptr;
DWORD* s_bits = nullptr;
int s_canvasW = 0, s_canvasH = 0;
std::unique_ptr<FontFamily> s_fontFamily;

float Px(float dip) { return dip * s_scale; }
float ColW() { return Px(UiEnglish() ? kColWEnglish : kColW); }
// 强调色跟随系统，或者用自己选的；强调色上的字按它的深浅用黑字或白字
const Palette& Colors() {
    static Palette pal;
    bool light = s_glass.Light();
    pal = light ? kLight : kDark;
    COLORREF accent = s_v.accent;
    if (!s_v.seg[kSegAccent]) {
        Color system = SystemAccent(light);
        accent = RGB(system.GetR(), system.GetG(), system.GetB());
    }
    pal.accent = Color(255, GetRValue(accent), GetGValue(accent), GetBValue(accent));
    pal.onAccent = ColorLuma(accent) > 150 ? Color(255, 16, 22, 30) : Color(255, 255, 255, 255);
    return pal;
}

bool HotkeyEnabled(int which) { return which != kHotkeyPin || s_v.on[kKeepFloats]; }  // 固定了也要它开着才有用
bool SliderEnabled(int which) {
    if (which == kLongPressMs) return s_v.on[kLongPress];
    if (which >= kPopupHoldMs && which < kPopupHoldMs + kHotkeyCount) return HotkeyEnabled(which - kPopupHoldMs);
    return true;
}

std::wstring SliderText(int which) {
    wchar_t text[32];
    if (which == kGlassAlpha || which == kIconSize) swprintf(text, 32, L"%d%%", s_v.slider[which]);
    else if (which == kRadius || which == kIconGap) swprintf(text, 32, Tr(L"%d 像素"), s_v.slider[which]);
    else swprintf(text, 32, Tr(L"%d 毫秒"), s_v.slider[which]);
    return text;
}

// ---- 布局 ----

float MeasureWidth(const std::wstring& text, float sizePx) {
    Bitmap bmp(1, 1, PixelFormat32bppPARGB);
    Graphics g(&bmp);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    Font font(s_fontFamily.get(), sizePx, FontStyleRegular, UnitPixel);
    StringFormat format(StringFormatFlagsNoWrap);
    RectF measured;
    g.MeasureString(text.c_str(), -1, &font, PointF(0, 0), &format, &measured);
    return measured.Width;
}

// 一组：标题 + 卡片（一页只有一组时不要标题）。rows 里每个函数往卡片里加一行，返回这一行的高度
using RowFn = float (*)(float, float, float);
float AddGroup(float x, float y, const wchar_t* title, const std::vector<RowFn>& rows) {
    if (title) {
        s_texts.push_back({Tr(title), RectF(x + Px(4), y, ColW() - Px(8), Px(kGroupTitleH)), 0});
        y += Px(kGroupTitleH);
    }
    float top = y;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i > 0) s_rowLines.insert(s_rowLines.end(), {x + Px(kRowPad), x + ColW() - Px(kRowPad), y});
        y += rows[i](x, y, ColW());
    }
    s_cards.push_back(RectF(x, top, ColW(), y - top));
    return y + Px(kGroupGap);
}

float ToggleRow(int which, float x, float y, float w) {
    float h = Px(kToggleText[which].sub ? 54 : 42);
    s_items.push_back({kToggleRow, which, RectF(x, y, w, h)});
    return h;
}

float SliderRow(int which, float x, float y, float w) {
    float h = Px(56);
    s_items.push_back({kSliderRow, which, RectF(x, y, w, h)});
    SliderGeom& sg = s_sliderGeom[which];
    sg.label = RectF(x + Px(kRowPad), y + Px(8), w - 2 * Px(kRowPad), Px(20));
    sg.x0 = x + Px(kRowPad) + Px(9);
    sg.x1 = x + w - Px(kRowPad) - Px(9);
    sg.cy = y + Px(39);
    return h;
}

// 几选一：左边名字，右边一排分段按钮
float SegRow(int which, float x, float y, float w) {
    float h = Px(50), bh = Px(30);
    const SegSpec& spec = kSegSpec[which];
    std::vector<int> options;  // 对齐的“自定义”只在选着时出现（在迷你任务栏的编辑模式里拖出来的）
    for (int i = 0; i < static_cast<int>(spec.options.size()); ++i)
        if (which != kSegAlign || i != kAlignCustom || s_v.seg[which] == kAlignCustom) options.push_back(i);
    std::vector<float> widths;
    float total = 0;
    for (int option : options) {
        widths.push_back(MeasureWidth(Tr(spec.options[option]), Px(12.5f)));
        total += widths.back();
    }
    // 每项左右留白，放不下就留少一点，别压到左边的名字
    float pad = Px(3);
    float room = w - 2 * Px(kRowPad) - MeasureWidth(Tr(spec.label), Px(13.5f)) - Px(12) - 2 * pad - total;
    float extra = std::clamp(room / widths.size(), Px(10), Px(22));
    for (float& width : widths) width += extra;
    total += extra * widths.size();
    RectF track(x + w - Px(kRowPad) - total - 2 * pad, y + (h - bh) / 2, total + 2 * pad, bh);
    s_segTrack[which] = track;
    s_texts.push_back({Tr(spec.label), RectF(x + Px(kRowPad), y, track.X - x - Px(kRowPad) - Px(4), h), 4});
    float cx = track.X + pad;
    for (size_t i = 0; i < widths.size(); ++i) {
        s_items.push_back({kSegment, which * kSegStride + options[i], RectF(cx, track.Y + pad, widths[i], bh - 2 * pad)});
        cx += widths[i];
    }
    // 自定义的底色、强调色：分段按钮左边一个色块，点了换颜色
    if ((which == kSegTint || which == kSegAccent) && s_v.seg[which] == 1) {
        float d = Px(24);
        s_items.push_back({kSwatch, which == kSegTint ? 0 : 1, RectF(track.X - Px(10) - d, y + (h - d) / 2, d, d)});
    }
    return h;
}

// 时钟里还显示哪些：左边名字，右边一排小胶囊，点一下开关（开着的是强调色）
float ClockPartsRow(float x, float y, float w) {
    const int parts[] = {kClockDate, kClockWeekday, kClockSeconds};
    float h = Px(50), ch = Px(kChipH), gap = Px(6);
    float total = -gap, widths[ARRAYSIZE(parts)];
    for (size_t i = 0; i < ARRAYSIZE(parts); ++i) {
        widths[i] = MeasureWidth(Tr(kToggleText[parts[i]].label), Px(12.5f)) + Px(28);
        total += widths[i] + gap;
    }
    float cx = x + w - Px(kRowPad) - total;
    s_texts.push_back({Tr(L"同时显示"), RectF(x + Px(kRowPad), y, cx - x - Px(kRowPad) - Px(4), h), 4});
    for (size_t i = 0; i < ARRAYSIZE(parts); ++i) {
        s_items.push_back({kToggleChip, parts[i], RectF(cx, y + (h - ch) / 2, widths[i], ch)});
        cx += widths[i] + gap;
    }
    return h;
}

float SliderRow(int which, float x, float y, float w);

// 快捷键：左边名字和说明，右边一个框显示按键（设了的话框里右边有个小叉，点了清掉），框下面选单按还是长按。
// 点这一行开始录。选了长按的话下面再加一条它自己的长按时长
float HotkeyRow(int which, float x, float y, float w) {
    float h = Px(80), bw = Px(130), bh = Px(28), mh = Px(26);
    s_items.push_back({kHotkeyRow, which, RectF(x, y, w, h)});
    RectF box(x + w - Px(kRowPad) - bw, y + Px(11), bw, bh);
    s_hotkeyBox[which] = box;
    s_items.push_back({kHotkeyClear, which, RectF(box.X + box.Width - bh, box.Y, bh, bh)});
    float pad = Px(3);
    RectF track(box.X, box.Y + bh + Px(6), bw, mh);
    s_hotkeyModeTrack[which] = track;
    float ow = (bw - 2 * pad) / 2;
    for (int i = 0; i < 2; ++i)
        s_items.push_back({kHotkeyMode, which * kSegStride + i, RectF(track.X + pad + i * ow, track.Y + pad, ow, mh - 2 * pad)});
    if (s_v.hold[which]) h = Px(76) + SliderRow(kPopupHoldMs + which, x, y + Px(76), w);
    return h;
}

// 一个个小胶囊从左往右摆，排不下就换行
struct ChipFlow {
    float left, right, cx, cy;
    ChipFlow(float x, float y, float w) : left(x + Px(kRowPad)), right(x + w - Px(kRowPad)), cx(left), cy(y + Px(12)) {}
    RectF Place(float chipW) {
        if (cx + chipW > right && cx > left) {
            cx = left;
            cy += Px(kChipH) + Px(8);
        }
        RectF r(cx, cy, std::min(chipW, right - cx), Px(kChipH));
        cx += chipW + Px(8);
        return r;
    }
    float Bottom() const { return cy + Px(kChipH) + Px(8); }
};

// 名单：每个程序一个小胶囊（点一下去掉），最后是“添加程序”
float ChipsRow(float x, float y, float w) {
    ChipFlow flow(x, y, w);
    const std::vector<std::wstring>& apps = ShownApps();
    for (size_t i = 0; i < apps.size(); ++i) {
        float chipW = MeasureWidth(apps[i], Px(12.5f)) + Px(12 + 6 + 14);
        s_items.push_back({kChip, static_cast<int>(i), flow.Place(chipW)});
    }
    s_items.push_back({kAddChip, 0, flow.Place(MeasureWidth(Tr(L"＋ 添加程序"), Px(12.5f)) + Px(24))});
    float bottom = flow.Bottom();
    s_texts.push_back({Tr(L"也可以在迷你任务栏里右键运行中的程序来添加或去掉"),
                       RectF(x + Px(kRowPad), bottom, w - 2 * Px(kRowPad), Px(32)), 1});
    return bottom + Px(36) - y;
}

std::vector<int> s_presets;  // 存着的外观方案（编号）
std::wstring s_fileNote;     // 导入、导出以后的结果

std::wstring PresetName(int slot) { return TrF(L"方案 %d", slot); }

// 外观方案：每个方案一个小胶囊（点了弹菜单：换上、覆盖、删除），最后是“存下现在的外观”
float PresetsRow(float x, float y, float w) {
    ChipFlow flow(x, y, w);
    for (int slot : s_presets)
        s_items.push_back({kPreset, slot, flow.Place(MeasureWidth(PresetName(slot), Px(12.5f)) + Px(28))});
    if (static_cast<int>(s_presets.size()) < kMaxLookPresets)
        s_items.push_back({kAddPreset, 0, flow.Place(MeasureWidth(Tr(L"＋ 存下现在的外观"), Px(12.5f)) + Px(24))});
    float bottom = flow.Bottom();
    s_texts.push_back({Tr(L"方案里有材质、颜色、大小、位置和显示哪些模块"),
                       RectF(x + Px(kRowPad), bottom, w - 2 * Px(kRowPad), Px(32)), 1});
    return bottom + Px(36) - y;
}

// 设置文件：右边导入、导出两个按钮，下面一行说明（导入、导出以后换成结果）
float FileRow(float x, float y, float w) {
    float pad = Px(kRowPad), bh = Px(28);
    float importW = MeasureWidth(Tr(L"导入…"), Px(12.5f)) + Px(32), exportW = MeasureWidth(Tr(L"导出…"), Px(12.5f)) + Px(32);
    float exportX = x + w - pad - exportW, importX = exportX - Px(8) - importW;
    s_texts.push_back({Tr(L"设置文件"), RectF(x + pad, y + Px(4), importX - x - pad, Px(42)), 4});
    s_items.push_back({kFile, 0, RectF(importX, y + Px(11), importW, bh)});
    s_items.push_back({kFile, 1, RectF(exportX, y + Px(11), exportW, bh)});
    std::wstring note = s_fileNote.empty() ? Tr(L"换电脑时导出，再在新电脑上导入") : s_fileNote;
    s_texts.push_back({note, RectF(x + pad, y + Px(46), w - 2 * pad, Px(32)), 1});
    return Px(80);
}

// 迷你任务栏大小和长度：在它的编辑模式里拖边改，这里只说怎么改、现在多大
float SizeRow(float x, float y, float w) {
    float pad = Px(kRowPad);
    s_texts.push_back({Tr(L"大小"), RectF(x + pad, y + Px(8), w - 2 * pad, Px(20)), 4});
    s_texts.push_back({L"", RectF(x + pad, y + Px(8), w - 2 * pad, Px(20)), 5});
    // 说明一行放不下就换行
    std::wstring hint = Tr(L"弹出后按 F2：拖边改大小，拖模块排顺序，拖空白处挪位置");
    bool wrap = MeasureWidth(hint, Px(11.5f)) > w - 2 * pad;
    s_texts.push_back({hint, RectF(x + pad, y + Px(30), w - 2 * pad, Px(wrap ? 34 : 18)), 1});
    return Px(wrap ? 70 : 56);
}

// 每块屏幕分别设置时：说明现在改的是哪块屏幕
float ScreenRow(float x, float y, float w) {
    std::wstring text = TrF(L"现在改的是%ls，把设置窗口拖到别的屏幕上就改那块", MonitorLabel(s_screen).c_str());
    s_texts.push_back({text, RectF(x + Px(kRowPad), y + Px(9), w - 2 * Px(kRowPad), Px(32)), 1});
    return Px(44);
}

// 隐藏任务栏的屏幕：每块屏幕一个小胶囊（从左往右按屏幕的位置排），开着的是强调色，点一下切换
std::vector<std::pair<std::wstring, std::wstring>> s_screenChips;  // 每块屏幕的 MonitorKey 和名字

bool ScreenHides(const std::wstring& key) { return !InAppList(s_v.noHide, key); }

float ScreenChipsRow(float x, float y, float w) {
    s_screenChips.clear();
    for (HMONITOR m : MonitorsInOrder()) s_screenChips.push_back({MonitorKey(m), MonitorLabel(m)});
    std::wstring label = Tr(L"隐藏任务栏的屏幕");
    std::vector<float> widths;
    float total = -Px(6);
    for (const auto& chip : s_screenChips) {
        widths.push_back(MeasureWidth(chip.second, Px(12.5f)) + Px(28));
        total += widths.back() + Px(6);
    }
    float labelW = MeasureWidth(label, Px(13.5f));
    float h = Px(50), ch = Px(kChipH);
    if (labelW + Px(16) + total <= w - 2 * Px(kRowPad)) {  // 放得下就和名字排在一行
        float cx = x + w - Px(kRowPad) - total;
        s_texts.push_back({label, RectF(x + Px(kRowPad), y, cx - x - Px(kRowPad) - Px(4), h), 4});
        for (size_t i = 0; i < widths.size(); ++i) {
            s_items.push_back({kScreenChip, static_cast<int>(i), RectF(cx, y + (h - ch) / 2, widths[i], ch)});
            cx += widths[i] + Px(6);
        }
        return h;
    }
    s_texts.push_back({label, RectF(x + Px(kRowPad), y + Px(4), w - 2 * Px(kRowPad), Px(32)), 4});
    ChipFlow flow(x, y + Px(32), w);
    for (size_t i = 0; i < widths.size(); ++i) s_items.push_back({kScreenChip, static_cast<int>(i), flow.Place(widths[i])});
    return flow.Bottom() + Px(4) - y;
}

// 一页的内容，从 (x, y) 往下排，返回最后一组下面的位置
float BuildPage(int page, float x, float y) {
    switch (page) {
        case kPageTaskbar: {
            y = AddGroup(x, y, L"全屏",
                         {[](float x, float y, float w) { return ToggleRow(kAutoHide, x, y, w); },
                          [](float x, float y, float w) { return ToggleRow(kAlwaysHide, x, y, w); },
                          [](float x, float y, float w) { return ToggleRow(kFlashAlert, x, y, w); }});
            // “小窗口留在上面”关着时收起“在所有窗口上面”
            std::vector<RowFn> floats = {[](float x, float y, float w) { return ToggleRow(kKeepFloats, x, y, w); }};
            if (s_v.on[kKeepFloats]) floats.push_back([](float x, float y, float w) { return ToggleRow(kPinTopmost, x, y, w); });
            y = AddGroup(x, y, L"小窗口", floats);
            return AddGroup(x, y, AppsTitle(), {ChipsRow});
        }
        case kPageMini: {
            // 长按 Win 关着时收起长按时长
            std::vector<RowFn> popup = {[](float x, float y, float w) { return ToggleRow(kLongPress, x, y, w); }};
            if (s_v.on[kLongPress]) popup.push_back([](float x, float y, float w) { return SliderRow(kLongPressMs, x, y, w); });
            popup.push_back([](float x, float y, float w) { return ToggleRow(kEdgePopup, x, y, w); });
            y = AddGroup(x, y, L"弹出", popup);
            return AddGroup(x, y, L"显示",
                            {[](float x, float y, float w) { return ToggleRow(kPinned, x, y, w); },
                             [](float x, float y, float w) { return SegRow(kSegTray, x, y, w); }});
        }
        case kPageControls: {
            // 迷你任务栏在底部、侧边时各显示哪些：先选改哪一份
            y = AddGroup(x, y, L"显示",
                         {[](float x, float y, float w) { return SegRow(kSegSide, x, y, w); },
                          [](float x, float y, float w) { return ToggleRow(kTray, x, y, w); },
                          [](float x, float y, float w) { return ToggleRow(kLevels, x, y, w); },
                          [](float x, float y, float w) { return ToggleRow(kRadios, x, y, w); },
                          [](float x, float y, float w) { return ToggleRow(kMedia, x, y, w); },
                          [](float x, float y, float w) { return ToggleRow(kIme, x, y, w); },
                          [](float x, float y, float w) { return ToggleRow(kBattery, x, y, w); }});
            // 时钟关着时收起它的选项
            std::vector<RowFn> clock = {[](float x, float y, float w) { return ToggleRow(kClock, x, y, w); }};
            if (s_v.on[kClock]) clock.push_back(ClockPartsRow);
            return AddGroup(x, y, L"时钟", clock);
        }
        case kPageSize: {
            y = AddGroup(x, y, L"大小",
                         {SizeRow, [](float x, float y, float w) { return SliderRow(kIconSize, x, y, w); },
                          [](float x, float y, float w) { return SliderRow(kIconGap, x, y, w); },
                          [](float x, float y, float w) { return ToggleRow(kShowTitle, x, y, w); }});
            std::vector<RowFn> place;
            if (MultiScreen() || s_v.on[kPerScreen])
                place.push_back([](float x, float y, float w) { return ToggleRow(kPerScreen, x, y, w); });
            place.push_back([](float x, float y, float w) { return SegRow(kSegEdge, x, y, w); });
            if (s_v.seg[kSegEdge] == kEdgeBottom) place.push_back([](float x, float y, float w) { return SegRow(kSegAlign, x, y, w); });  // 竖排时收起对齐
            if (s_v.on[kPerScreen]) place.push_back(ScreenRow);
            return AddGroup(x, y, L"位置", place);
        }
        case kPageScreens: {
            y = AddGroup(x, y, L"任务栏",
                         {[](float x, float y, float w) { return ToggleRow(kKeepOtherBars, x, y, w); }, ScreenChipsRow});
            // 只显示这块屏幕上的窗口时用不着标出别的屏幕
            std::vector<RowFn> mini = {[](float x, float y, float w) { return SegRow(kSegMonitor, x, y, w); },
                                       [](float x, float y, float w) { return SegRow(kSegWindows, x, y, w); }};
            if (s_v.seg[kSegWindows] != kWindowsOwnScreen)
                mini.push_back([](float x, float y, float w) { return ToggleRow(kMarkScreens, x, y, w); });
            mini.push_back([](float x, float y, float w) { return ToggleRow(kBrightAll, x, y, w); });
            return AddGroup(x, y, L"迷你任务栏", mini);
        }
        case kPageLook: {
            y = AddGroup(x, y, L"界面",
                         {[](float x, float y, float w) { return SegRow(kSegStyle, x, y, w); },
                          [](float x, float y, float w) { return SegRow(kSegTheme, x, y, w); },
                          [](float x, float y, float w) { return SegRow(kSegAccent, x, y, w); },
                          [](float x, float y, float w) { return SegRow(kSegLang, x, y, w); }});
            std::vector<RowFn> mini = {[](float x, float y, float w) { return SegRow(kSegTint, x, y, w); },
                                       [](float x, float y, float w) { return SliderRow(kRadius, x, y, w); }};
            if (s_v.seg[kSegStyle] == kGlassLiquid)  // 只有液态玻璃能调透明度
                mini.push_back([](float x, float y, float w) { return SliderRow(kGlassAlpha, x, y, w); });
            return AddGroup(x, y, L"迷你任务栏", mini);
        }
        case kPagePresets:
            y = AddGroup(x, y, L"外观方案", {PresetsRow});
            return AddGroup(x, y, L"备份", {FileRow});
        case kPageHotkeys:
            return AddGroup(x, y, nullptr,
                            {[](float x, float y, float w) { return HotkeyRow(kHotkeyPopup, x, y, w); },
                             [](float x, float y, float w) { return HotkeyRow(kHotkeyPin, x, y, w); },
                             [](float x, float y, float w) { return HotkeyRow(kHotkeyMove, x, y, w); },
                             [](float x, float y, float w) { return HotkeyRow(kHotkeyNextScreen, x, y, w); }});
        default:
            return AddGroup(x, y, nullptr,
                            {[](float x, float y, float w) { return ToggleRow(kAutoStart, x, y, w); },
                             [](float x, float y, float w) { return ToggleRow(kUpdates, x, y, w); }});
    }
}

void BuildLayout() {
    s_items.clear();
    s_cards.clear();
    s_texts.clear();
    s_rowLines.clear();

    float margin = Px(kShadow);
    float navW = Px(kNavW);  // 导航按最长的名字放宽
    for (const wchar_t* name : kPageNames) navW = std::max(navW, MeasureWidth(Tr(name), Px(13.5f)) + Px(36));
    float panelW = Px(kPad) * 2 + navW + Px(kNavGap) + ColW();
    s_radius = Px(s_v.seg[kSegStyle] == kGlassWin11 ? 8.0f : 22.0f);
    float navX = margin + Px(kPad), contentX = navX + navW + Px(kNavGap);
    float top = margin + Px(kHeaderH);

    s_texts.push_back({Tr(L"TaskbarPopup 设置"), RectF(margin + Px(kPad), margin + Px(12), Px(260), Px(28)), 2});
    s_items.push_back({kClose, 0, RectF(margin + panelW - Px(12) - Px(32), margin + Px(10), Px(32), Px(32))});

    // 左边的导航
    float navStep = Px(kNavItemH) + Px(4);
    for (int i = 0; i < kPageCount; ++i)
        s_items.push_back({kNav, i, RectF(navX, top + i * navStep, navW, Px(kNavItemH))});
    float navBottom = top + kPageCount * navStep - Px(4);

    // 右边是选中的那一页。窗口高度按最高的一页算，换页时大小不变：别的页先排一遍量出高度再扔掉
    float pageBottom = top;
    for (int p = 0; p < kPageCount; ++p) {
        if (p == s_page) continue;
        size_t items = s_items.size(), cards = s_cards.size(), texts = s_texts.size(), lines = s_rowLines.size();
        pageBottom = std::max(pageBottom, BuildPage(p, contentX, top));
        s_items.erase(s_items.begin() + items, s_items.end());
        s_cards.erase(s_cards.begin() + cards, s_cards.end());
        s_texts.erase(s_texts.begin() + texts, s_texts.end());
        s_rowLines.erase(s_rowLines.begin() + lines, s_rowLines.end());
    }
    pageBottom = std::max(pageBottom, BuildPage(s_page, contentX, top));

    // “立即检查”放在“自动检查更新”那一行的开关左边
    for (const Item& it : s_items)
        if (it.kind == kToggleRow && it.index == kUpdates) {
            float w = MeasureWidth(Tr(L"立即检查"), Px(12.5f)) + Px(16);
            float x = it.rect.X + it.rect.Width - Px(kRowPad) - Px(40) - Px(10) - w;
            RectF r(x, it.rect.Y + (it.rect.Height - Px(26)) / 2, w, Px(26));
            s_items.push_back({kCheckNow, 0, r});
            break;
        }

    // 版本号在导航下面，和右边最后一组的底边对齐
    float versionH = Px(20);
    float contentBottom = std::max(navBottom + Px(16) + versionH, pageBottom - Px(kGroupGap));
    const char* version = TP_VERSION_STR;
    s_texts.push_back({std::wstring(Tr(L"版本")) + L" " + std::wstring(version, version + strlen(version)),
                       RectF(navX + Px(4), contentBottom - versionH, navW, versionH), 3});

    float panelH = contentBottom + Px(kPad) - margin;
    s_panel = RectF(margin, margin, panelW, panelH);
    s_width = static_cast<int>(std::ceil(panelW + 2 * margin));
    s_height = static_cast<int>(std::ceil(panelH + 2 * margin));
}

// 按显示器缩放算布局；放不下就整体缩小一点
void LayoutFor(HMONITOR monitor) {
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    const RECT& work = mi.rcWork;
    float base = MonitorScale(monitor);
    s_scale = base;
    BuildLayout();
    float fit = std::min(static_cast<float>(work.right - work.left) / s_width,
                         static_cast<float>(work.bottom - work.top) / s_height);
    if (fit < 1) {
        s_scale = base * std::max(0.6f, fit);
        BuildLayout();
    }
}

const Item* FindItem(const Item& key) {
    for (const Item& it : s_items)
        if (it == key) return &it;
    return nullptr;
}

bool Enabled(const Item& it) {
    if (it.kind == kSliderRow) return SliderEnabled(it.index);
    if (it.kind == kHotkeyRow) return HotkeyEnabled(it.index);
    // 小叉：设了快捷键、没在录的时候才有
    if (it.kind == kHotkeyClear) return HotkeyEnabled(it.index) && s_v.hotkey[it.index] && s_capture != it.index;
    if (it.kind == kHotkeyMode) return HotkeyEnabled(it.index / kSegStride);
    return true;
}

Item HitTest(int x, int y) {
    PointF pt(static_cast<float>(x), static_cast<float>(y));
    // “立即检查”、快捷键的小叉在一整行上面，先看它们
    auto onTop = [](const Item& it) { return it.kind == kCheckNow || it.kind == kHotkeyClear || it.kind == kHotkeyMode; };
    for (const Item& it : s_items)
        if (onTop(it) && it.rect.Contains(pt) && Enabled(it)) return it;
    for (const Item& it : s_items)
        if (!onTop(it) && it.rect.Contains(pt) && Enabled(it)) return it;
    return {};
}

// ---- 画 ----

void FillRound(Graphics& g, const RectF& r, float radius, const Color& c) {
    GraphicsPath p;
    AddRoundRect(p, r, radius);
    SolidBrush b(c);
    g.FillPath(&b, &p);
}

void StrokeRound(Graphics& g, const RectF& r, float radius, const Color& c, float width) {
    GraphicsPath p;
    AddRoundRect(p, r, radius);
    Pen pen(c, width);
    g.DrawPath(&pen, &p);
}

// 玻璃卡片：上亮下暗的一层薄片，带一圈细亮边
void DrawCard(Graphics& g, const RectF& r, float radius, const Color& fill, const Color& rim) {
    GraphicsPath path;
    AddRoundRect(path, r, radius);
    PointF top(0, r.Y - 1), bottom(0, r.Y + r.Height + 1);
    LinearGradientBrush brush(top, bottom, fill, Color(fill.GetA() / 2, fill.GetR(), fill.GetG(), fill.GetB()));
    g.FillPath(&brush, &path);
    LinearGradientBrush rimBrush(top, bottom, rim, Color(rim.GetA() / 4, rim.GetR(), rim.GetG(), rim.GetB()));
    Pen pen(&rimBrush, std::max(1.0f, Px(1)));
    g.DrawPath(&pen, &path);
}

void DrawText(Graphics& g, const std::wstring& text, float sizePx, int style, const RectF& r, const Color& c,
              StringAlignment align = StringAlignmentNear, bool wrap = false) {
    Font font(s_fontFamily.get(), sizePx, style, UnitPixel);
    StringFormat format(wrap ? 0 : StringFormatFlagsNoWrap);
    format.SetAlignment(align);
    format.SetLineAlignment(wrap ? StringAlignmentNear : StringAlignmentCenter);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    // 深色玻璃上加一层淡阴影，在花哨的背景上也看得清
    if (!s_glass.Light()) {
        RectF shadow = r;
        shadow.Offset(0, std::max(1.0f, Px(1)));
        SolidBrush sb(Colors().textShadow);
        g.DrawString(text.c_str(), -1, &font, shadow, &format, &sb);
    }
    SolidBrush brush(c);
    g.DrawString(text.c_str(), -1, &font, r, &format, &brush);
}

Color Mix(const Color& a, const Color& b, float t) {
    auto lerp = [t](BYTE x, BYTE y) { return static_cast<BYTE>(std::lround(x + (y - x) * t)); };
    return Color(lerp(a.GetA(), b.GetA()), lerp(a.GetR(), b.GetR()), lerp(a.GetG(), b.GetG()),
                 lerp(a.GetB(), b.GetB()));
}

void DrawToggleRow(Graphics& g, const Item& it) {
    const Palette& pal = Colors();
    const ToggleText& t = kToggleText[it.index];
    const RectF& r = it.rect;
    if (s_hover == it || s_press == it)
        FillRound(g, RectF(r.X + Px(4), r.Y + Px(3), r.Width - Px(8), r.Height - Px(6)), Px(9), pal.hover);
    float textW = r.Width - 2 * Px(kRowPad) - Px(50);
    if (it.index == kUpdates)
        if (const Item* check = FindItem({kCheckNow, 0})) textW = check->rect.X - r.X - Px(kRowPad) - Px(6);
    if (t.sub) {
        float x = r.X + Px(kRowPad);
        DrawText(g, Tr(t.label), Px(13.5f), FontStyleRegular, RectF(x, r.Y + Px(8), textW, Px(20)), pal.text);
        DrawText(g, Tr(t.sub), Px(11.5f), FontStyleRegular, RectF(x, r.Y + Px(29), textW, Px(17)), pal.subtle);
    } else {
        DrawText(g, Tr(t.label), Px(13.5f), FontStyleRegular, RectF(r.X + Px(kRowPad), r.Y, textW, r.Height), pal.text);
    }

    // 开关：和 Win11 一样，开着是强调色的槽、白色圆点在右边
    float k = s_knob[it.index];
    float tw = Px(40), th = Px(20);
    RectF track(r.X + r.Width - Px(kRowPad) - tw, r.Y + (r.Height - th) / 2, tw, th);
    Color fill = Mix(pal.trackOff, pal.accent, k);
    FillRound(g, track, th / 2, fill);
    if (k < 0.99f) StrokeRound(g, track, th / 2, Mix(pal.subtle, Color(0, 0, 0, 0), k), std::max(1.0f, Px(1)));
    float d = Px(14) + (s_hover == it ? Px(1) : 0);
    float cx = track.X + th / 2 + (tw - th) * k, cy = track.Y + th / 2;
    SolidBrush shadow(Color(50, 0, 0, 0));
    g.FillEllipse(&shadow, cx - d / 2, cy - d / 2 + Px(1), d, d);
    SolidBrush knob(k > 0.5f ? pal.onAccent : pal.knob);
    if (!s_glass.Light()) knob.SetColor(Mix(Color(255, 255, 255, 255), pal.onAccent, k));
    g.FillEllipse(&knob, cx - d / 2, cy - d / 2, d, d);
}

void DrawSliderRow(Graphics& g, const Item& it) {
    const Palette& pal = Colors();
    const SliderGeom& sg = s_sliderGeom[it.index];
    const SliderSpec& spec = kSliderSpec[it.index];
    bool enabled = SliderEnabled(it.index);
    Color text = enabled ? pal.text : pal.subtle;
    DrawText(g, Tr(spec.label), Px(13.5f), FontStyleRegular, sg.label, text);
    DrawText(g, SliderText(it.index), Px(12.5f), FontStyleRegular, sg.label, enabled ? pal.subtle : pal.separator,
             StringAlignmentFar);

    float h = Px(4);
    RectF track(sg.x0, sg.cy - h / 2, sg.x1 - sg.x0, h);
    FillRound(g, track, h / 2, pal.trackOff);
    float t = static_cast<float>(s_v.slider[it.index] - spec.minValue) / (spec.maxValue - spec.minValue);
    float x = sg.x0 + (sg.x1 - sg.x0) * t;
    const Color& fill = enabled ? pal.accent : pal.subtle;
    if (x > sg.x0 + 0.5f) FillRound(g, RectF(sg.x0, track.Y, x - sg.x0, h), h / 2, fill);
    // 滑块：外圈一个圆片，中间一个强调色的点，鼠标放上去时点变大
    float R = Px(9);
    SolidBrush shadow(Color(40, 0, 0, 0));
    g.FillEllipse(&shadow, x - R, sg.cy - R + Px(1), 2 * R, 2 * R);
    SolidBrush thumb(s_glass.Light() ? Color(255, 255, 255, 255) : Color(255, 58, 58, 62));
    g.FillEllipse(&thumb, x - R, sg.cy - R, 2 * R, 2 * R);
    Pen rim(s_glass.Light() ? Color(50, 0, 0, 0) : Color(80, 255, 255, 255), std::max(1.0f, Px(1)));
    g.DrawEllipse(&rim, x - R, sg.cy - R, 2 * R, 2 * R);
    float core = (s_hover == it || s_drag == it.index) && enabled ? Px(6) : Px(4.5f);
    SolidBrush dot(fill);
    g.FillEllipse(&dot, x - core, sg.cy - core, 2 * core, 2 * core);
}

void DrawSegment(Graphics& g, const Item& it) {
    const Palette& pal = Colors();
    int which = it.index / kSegStride, option = it.index % kSegStride;
    const RectF& r = it.rect;
    bool selected = s_v.seg[which] == option;
    if (option == 0) FillRound(g, s_segTrack[which], s_segTrack[which].Height / 2, pal.trackOff);
    if (selected) DrawCard(g, r, r.Height / 2, pal.buttonHot, pal.cardRim);
    else if (s_hover == it || s_press == it) FillRound(g, r, r.Height / 2, pal.hover);
    DrawText(g, Tr(kSegSpec[which].options[option]), Px(12.5f), FontStyleRegular, r, selected ? pal.text : pal.subtle,
             StringAlignmentCenter);
}

// 录快捷键时框里显示已经按着的修饰键
std::wstring HeldModifiers() {
    std::wstring text;
    if (GetKeyState(VK_CONTROL) < 0) text += L"Ctrl+";
    if (GetKeyState(VK_SHIFT) < 0) text += L"Shift+";
    if (GetKeyState(VK_MENU) < 0) text += L"Alt+";
    return text;
}

void DrawHotkeyRow(Graphics& g, const Item& it) {
    const Palette& pal = Colors();
    const ToggleText& t = kHotkeyText[it.index];
    const RectF& r = it.rect;
    const RectF& box = s_hotkeyBox[it.index];
    bool enabled = HotkeyEnabled(it.index);
    bool capturing = s_capture == it.index;
    UINT hotkey = s_v.hotkey[it.index];
    if (enabled && (s_hover == it || s_press == it))
        FillRound(g, RectF(r.X + Px(4), r.Y + Px(3), r.Width - Px(8), r.Height - Px(6)), Px(9), pal.hover);

    // 说明：刚按的键不行时说为什么；录的时候说怎么取消；设的被别的程序占了也说一声。最多两行
    std::wstring sub = Tr(t.sub);
    Color subColor = enabled ? pal.subtle : pal.separator;
    if (!s_hotkeyError[it.index].empty()) {
        sub = s_hotkeyError[it.index];
        subColor = pal.warn;
    } else if (capturing) {
        sub = Tr(L"按下按键或鼠标键，Esc 取消");
    } else if (enabled && hotkey && hotkey == s_initial.hotkey[it.index] && Hotkeys_Failed(it.index)) {
        sub = Tr(L"被别的程序占用了，换一个吧");
        subColor = pal.warn;
    }
    float x = r.X + Px(kRowPad), textW = box.X - x - Px(8);
    DrawText(g, Tr(t.label), Px(13.5f), FontStyleRegular, RectF(x, r.Y + Px(12), textW, Px(20)), enabled ? pal.text : pal.subtle);
    DrawText(g, sub, Px(11.5f), FontStyleRegular, RectF(x, r.Y + Px(35), textW, Px(38)), subColor, StringAlignmentNear,
             true);

    // 框：录的时候是强调色的边
    FillRound(g, box, Px(8), capturing ? pal.button : pal.trackOff);
    if (capturing) StrokeRound(g, box, Px(8), pal.accent, std::max(1.0f, Px(1.5f)));
    std::wstring text;
    Color color = enabled ? pal.text : pal.subtle;
    RectF textRect(box.X + Px(10), box.Y, box.Width - Px(20), box.Height);
    if (capturing) {
        std::wstring held = HeldModifiers();
        text = held.empty() ? Tr(L"按下按键…") : held + L"…";
        color = held.empty() ? pal.subtle : pal.text;
    } else if (hotkey) {
        text = HotkeyText(hotkey, true);
        textRect.Width -= box.Height - Px(10);  // 右边留给小叉
    } else {
        text = Tr(L"未设置");
        color = enabled ? pal.subtle : pal.separator;
    }
    DrawText(g, text, Px(12.5f), FontStyleRegular, textRect, color);

    if (!capturing && hotkey && enabled) {
        // 小叉：鼠标放上去时变成强调色
        bool hot = s_hover == Item{kHotkeyClear, it.index, {}} || s_press == Item{kHotkeyClear, it.index, {}};
        float cx = box.X + box.Width - box.Height / 2, cy = box.Y + box.Height / 2, a = Px(3.5f);
        if (hot) FillRound(g, RectF(cx - Px(10), cy - Px(10), Px(20), Px(20)), Px(10), pal.hover);
        Pen pen(hot ? pal.accent : pal.subtle, Px(1.4f));
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        g.DrawLine(&pen, cx - a, cy - a, cx + a, cy + a);
        g.DrawLine(&pen, cx - a, cy + a, cx + a, cy - a);
    }
}

// 框下面的“单按 / 长按”
void DrawHotkeyMode(Graphics& g, const Item& it) {
    const Palette& pal = Colors();
    int which = it.index / kSegStride, option = it.index % kSegStride;
    bool enabled = HotkeyEnabled(which);
    const RectF& r = it.rect;
    bool selected = s_v.hold[which] == (option == 1);
    if (option == 0) FillRound(g, s_hotkeyModeTrack[which], s_hotkeyModeTrack[which].Height / 2, pal.trackOff);
    if (selected && enabled) DrawCard(g, r, r.Height / 2, pal.buttonHot, pal.cardRim);
    else if (enabled && (s_hover == it || s_press == it)) FillRound(g, r, r.Height / 2, pal.hover);
    Color color = !enabled ? pal.separator : selected ? pal.text : pal.subtle;
    DrawText(g, Tr(kHotkeyModes[option]), Px(12), FontStyleRegular, r, color, StringAlignmentCenter);
}

void DrawChip(Graphics& g, const Item& it) {
    const Palette& pal = Colors();
    const RectF& r = it.rect;
    bool hot = s_hover == it || s_press == it;
    if (it.kind == kAddChip || it.kind == kAddPreset) {
        FillRound(g, r, r.Height / 2, hot ? pal.buttonHot : pal.button);
        DrawText(g, Tr(it.kind == kAddChip ? L"＋ 添加程序" : L"＋ 存下现在的外观"), Px(12.5f), FontStyleRegular, r, pal.accent,
                 StringAlignmentCenter);
        return;
    }
    if (it.kind == kPreset) {
        DrawCard(g, r, r.Height / 2, hot ? pal.buttonHot : pal.button, pal.cardRim);
        DrawText(g, PresetName(it.index), Px(12.5f), FontStyleRegular, r, pal.text, StringAlignmentCenter);
        return;
    }
    if (it.kind == kToggleChip || it.kind == kScreenChip) {
        bool screen = it.kind == kScreenChip;
        bool on = screen ? ScreenHides(s_screenChips[it.index].first) : s_v.on[it.index];
        if (on) FillRound(g, r, r.Height / 2, hot ? Mix(pal.accent, Color(255, 255, 255, 255), 0.15f) : pal.accent);
        else DrawCard(g, r, r.Height / 2, hot ? pal.buttonHot : pal.button, pal.cardRim);
        DrawText(g, screen ? s_screenChips[it.index].second : Tr(kToggleText[it.index].label), Px(12.5f), FontStyleRegular, r,
                 on ? pal.onAccent : pal.text, StringAlignmentCenter);
        return;
    }
    DrawCard(g, r, r.Height / 2, hot ? pal.buttonHot : pal.button, pal.cardRim);
    RectF textRect(r.X + Px(12), r.Y, r.Width - Px(12 + 6 + 14), r.Height);
    DrawText(g, ShownApps()[it.index], Px(12.5f), FontStyleRegular, textRect, pal.text);
    // 右边的小叉：鼠标放上去时变成强调色
    float cx = r.X + r.Width - Px(13), cy = r.Y + r.Height / 2, a = Px(3.5f);
    Pen pen(hot ? pal.accent : pal.subtle, Px(1.4f));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, cx - a, cy - a, cx + a, cy + a);
    g.DrawLine(&pen, cx - a, cy + a, cx + a, cy - a);
}

// 自定义颜色的色块：圆的，鼠标放上去时外面一圈浅色
void DrawSwatch(Graphics& g, const Item& it) {
    const Palette& pal = Colors();
    const RectF& r = it.rect;
    COLORREF c = it.index == 0 ? s_v.tint : s_v.accent;
    if (s_hover == it || s_press == it) {
        RectF ring = r;
        ring.Inflate(Px(4), Px(4));
        FillRound(g, ring, ring.Height / 2, pal.hover);
    }
    FillRound(g, r, r.Height / 2, Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
    StrokeRound(g, r, r.Height / 2, Color(80, pal.text.GetR(), pal.text.GetG(), pal.text.GetB()), std::max(1.0f, Px(1)));
}

// 左边导航的一项：选中的是一片浅色胶囊，左边一条强调色的短竖线（和 Win11 的设置一样）
void DrawNav(Graphics& g, const Item& it) {
    const Palette& pal = Colors();
    const RectF& r = it.rect;
    bool selected = it.index == s_page;
    if (selected) {
        DrawCard(g, r, Px(8), pal.buttonHot, pal.cardRim);
        FillRound(g, RectF(r.X + Px(5), r.Y + (r.Height - Px(16)) / 2, Px(3), Px(16)), Px(1.5f), pal.accent);
    } else if (s_hover == it || s_press == it) {
        FillRound(g, r, Px(8), pal.hover);
    }
    DrawText(g, Tr(kPageNames[it.index]), Px(13.5f), FontStyleRegular, RectF(r.X + Px(16), r.Y, r.Width - Px(20), r.Height),
             selected ? pal.text : pal.subtle);
}

void DrawButton(Graphics& g, const Item& it) {
    const Palette& pal = Colors();
    const RectF& r = it.rect;
    bool hot = s_hover == it || s_press == it;
    if (it.kind == kClose) {
        if (hot) FillRound(g, r, Px(8), Color(230, 196, 43, 28));
        float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2, a = Px(5);
        Pen pen(hot ? Color(255, 255, 255, 255) : pal.text, Px(1.4f));
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        g.DrawLine(&pen, cx - a, cy - a, cx + a, cy + a);
        g.DrawLine(&pen, cx - a, cy + a, cx + a, cy - a);
        return;
    }
    if (it.kind == kCheckNow) {
        if (hot) FillRound(g, r, r.Height / 2, pal.button);
        DrawText(g, Tr(L"立即检查"), Px(12.5f), FontStyleRegular, r, pal.accent, StringAlignmentCenter);
        return;
    }
    // 导入、导出
    DrawCard(g, r, r.Height / 2, hot ? pal.buttonHot : pal.button, pal.cardRim);
    DrawText(g, Tr(it.index ? L"导出…" : L"导入…"), Px(12.5f), FontStyleRegular, r, pal.text, StringAlignmentCenter);
}

void DrawContent(Graphics& g) {
    const Palette& pal = Colors();
    for (const RectF& card : s_cards) DrawCard(g, card, Px(14), pal.card, pal.cardRim);
    Pen line(pal.separator, 1.0f);
    for (size_t i = 0; i + 2 < s_rowLines.size(); i += 3)
        g.DrawLine(&line, s_rowLines[i], s_rowLines[i + 2], s_rowLines[i + 1], s_rowLines[i + 2]);
    for (const Text& t : s_texts) {
        switch (t.style) {
            case 0: DrawText(g, t.text, Px(12), FontStyleBold, t.rect, pal.subtle); break;
            case 1:
                DrawText(g, t.text, Px(11.5f), FontStyleRegular, t.rect, pal.subtle, StringAlignmentNear, true);
                break;
            case 2: DrawText(g, t.text, Px(17), FontStyleBold, t.rect, pal.text); break;
            case 4: DrawText(g, t.text, Px(13.5f), FontStyleRegular, t.rect, pal.text); break;
            case 5: {
                std::wstring size = g_settings.popupLength > 0
                                        ? TrF(L"%d%%，长度 %d%%", g_settings.popupScale, (g_settings.popupLength + 5) / 10)
                                        : TrF(L"%d%%", g_settings.popupScale);
                DrawText(g, size, Px(12.5f), FontStyleRegular, t.rect, pal.subtle, StringAlignmentFar);
                break;
            }
            default: DrawText(g, t.text, Px(11.5f), FontStyleRegular, t.rect, pal.subtle); break;
        }
    }
    for (const Item& it : s_items) {
        switch (it.kind) {
            case kToggleRow: DrawToggleRow(g, it); break;
            case kSliderRow: DrawSliderRow(g, it); break;
            case kSegment: DrawSegment(g, it); break;
            case kHotkeyRow: DrawHotkeyRow(g, it); break;
            case kHotkeyClear: break;  // 和那一行一起画
            case kHotkeyMode: DrawHotkeyMode(g, it); break;
            case kChip:
            case kAddChip:
            case kPreset:
            case kAddPreset:
            case kToggleChip:
            case kScreenChip: DrawChip(g, it); break;
            case kSwatch: DrawSwatch(g, it); break;
            case kNav: DrawNav(g, it); break;
            default: DrawButton(g, it); break;
        }
    }
    if (s_showFocus)
        if (const Item* f = FindItem(s_focus)) {
            RectF r = f->rect;
            r.Inflate(Px(2), Px(2));
            float radius = f->kind == kToggleRow || f->kind == kSliderRow || f->kind == kHotkeyRow ? Px(10)
                           : f->kind == kNav                                                   ? Px(9)
                                                                                               : r.Height / 2;
            StrokeRound(g, r, radius, pal.accent, Px(2));
        }
}

void ReleaseCanvas() {
    if (s_canvasDC) {
        SelectObject(s_canvasDC, s_canvasOld);
        DeleteDC(s_canvasDC);
    }
    if (s_canvas) DeleteObject(s_canvas);
    s_canvasDC = nullptr;
    s_canvas = nullptr;
    s_canvasOld = nullptr;
    s_bits = nullptr;
    s_canvasW = s_canvasH = 0;
}

bool EnsureCanvas() {
    if (s_canvas && s_canvasW == s_width && s_canvasH == s_height) return true;
    ReleaseCanvas();
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = s_width;
    bi.bmiHeader.biHeight = -s_height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    s_canvas = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!s_canvas) return false;
    s_canvasDC = CreateCompatibleDC(nullptr);
    s_canvasOld = SelectObject(s_canvasDC, s_canvas);
    s_bits = static_cast<DWORD*>(bits);
    s_canvasW = s_width;
    s_canvasH = s_height;
    return true;
}

// 先画玻璃（不变时用缓存），再叠上内容（GDI+ 画在透明底上，没变时用缓存），最后带逐像素透明度贴到屏幕。
// 窗口位置也在这里一起更新，拖动时位置和画面同一帧变。content：内容变了没有（只是玻璃变了时传 false）
void Render(bool content = true) {
    if (!s_hwnd || s_width <= 0 || s_height <= 0 || !EnsureCanvas()) return;
    size_t count = static_cast<size_t>(s_width) * s_height;
    if (s_baseValid && s_base.size() == count) {
        memcpy(s_bits, s_base.data(), count * sizeof(DWORD));
    } else {
        s_glass.Prepare(s_width, s_height, s_panel, s_radius);
        s_glass.Render(s_bits, s_pos, 0);
        s_base.assign(s_bits, s_bits + count);
        s_baseValid = true;
    }
    if (content || !s_contentValid || s_content.size() != count) {
        s_content.assign(count, 0);
        Bitmap canvas(s_width, s_height, s_width * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(s_content.data()));
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        DrawContent(g);
        s_contentValid = true;
    }
    // 预乘颜色的“盖在上面”：结果 = 内容 + 玻璃 × (1 − 内容的不透明度)
    for (size_t i = 0; i < count; ++i) {
        DWORD c = s_content[i];
        DWORD a = c >> 24;
        if (a == 0) continue;
        if (a == 255) {
            s_bits[i] = c;
            continue;
        }
        DWORD d = s_bits[i], k = 255 - a;
        DWORD rb = (d & 0x00FF00FF) * k, ag = ((d >> 8) & 0x00FF00FF) * k;
        rb = ((rb + 0x00800080 + ((rb >> 8) & 0x00FF00FF)) >> 8) & 0x00FF00FF;
        ag = (ag + 0x00800080 + ((ag >> 8) & 0x00FF00FF)) & 0xFF00FF00;
        s_bits[i] = c + (rb | ag);
    }
    SIZE size = {s_width, s_height};
    POINT src = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, s_alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(s_hwnd, nullptr, &s_pos, &size, s_canvasDC, &src, 0, &blend, ULW_ALPHA);
}

// ---- 玻璃的背景 ----
//
// 截窗口后面的屏幕时让本窗口不进截图（Win10 2004 起支持），屏幕上照常显示。打开排除要等合成两帧才生效，
// 所以背景在变、正在拖动时一直开着，背景一段时间不动就关掉，之后隔一会儿开一下截一张看看。
// 不支持的系统上截到的会是自己，一叠加就越来越花，这时只用打开窗口那一刻截的背景

constexpr double kExcludeSettleMs = 50;  // 打开排除以后等这么久再截
constexpr int kStillTicks = 30;          // 连续这么多次没变就算安静了

int s_excludeWorks = 0;     // 0 = 还不知道，1 = 管用，-1 = 不管用
bool s_excluded = false;    // 现在截图时排除着本窗口
double s_excludedAt = 0;    // 什么时候打开的排除
bool s_moving = false;      // 正在拖动窗口
int s_still = 0;            // 背景连续几次没变
UINT s_shotSeq = 0;         // 背景换过一次就加一，截到一半的旧结果不要
HANDLE s_shotThread = nullptr;  // 后台正在截

struct ShotJob {
    HWND hwnd;
    UINT seq;
    RECT window;
    float scale;
    int style;
    bool compare;    // 和 prev 比较，一样就不处理
    GlassShot prev;  // 只有 area 和 raw
    GlassShot shot;
    bool changed = false;
};

bool SetExclude(bool on) {
    if (on == s_excluded) return true;
    if (!on) {
        SetWindowDisplayAffinity(s_hwnd, WDA_NONE);
        s_excluded = false;
        return true;
    }
    if (s_excludeWorks < 0) return false;
    // Win10 2004（19041）以前没有这个功能：调用照样成功，却当成 WDA_MONITOR，截到的本窗口是一块黑的，
    // 后面的核对也看不出来，玻璃背景就成了黑的
    if (DWORD build = WindowsBuild(); build && build < 19041) {
        s_excludeWorks = -1;
        Log(L"设置窗口：系统版本 %lu 不支持截图时排除本窗口，玻璃背景不再实时刷新", build);
        return false;
    }
    if (!SetWindowDisplayAffinity(s_hwnd, WDA_EXCLUDEFROMCAPTURE)) {
        s_excludeWorks = -1;
        Log(L"设置窗口：系统不支持截图时排除本窗口（错误 %lu），玻璃背景不再实时刷新", GetLastError());
        return false;
    }
    s_excluded = true;
    s_excludedAt = NowMs();
    return true;
}

double ExcludeWait() { return std::max(0.0, kExcludeSettleMs - (NowMs() - s_excludedAt)); }

// 窗口显示着、第一次排除后截图时核对一下：面板中间一小块截到的和刚画上去的一模一样，说明排除不管用
bool ExcludeWorks() {
    if (s_excludeWorks) return s_excludeWorks > 0;
    if (!s_bits || s_alpha != 255 || s_canvasW != s_width || s_canvasH != s_height) return true;  // 还判断不了，先当管用
    int size = static_cast<int>(Px(40));
    int x0 = static_cast<int>(s_panel.X + s_panel.Width / 2) - size / 2;
    int y0 = static_cast<int>(s_panel.Y + s_panel.Height / 2) - size / 2;
    RECT r = {s_pos.x + x0, s_pos.y + y0, s_pos.x + x0 + size, s_pos.y + y0 + size};
    std::vector<DWORD> shot;
    if (!CaptureScreen(r, shot)) return true;
    int same = 0;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            if ((shot[static_cast<size_t>(y) * size + x] & 0xFFFFFF) ==
                (s_bits[static_cast<size_t>(y0 + y) * s_width + x0 + x] & 0xFFFFFF))
                ++same;
    s_excludeWorks = same * 10 >= size * size * 9 ? -1 : 1;
    if (s_excludeWorks < 0) Log(L"设置窗口：截图时排除本窗口不管用，玻璃背景不再实时刷新");
    return s_excludeWorks > 0;
}

// 马上截下窗口后面的屏幕内容做玻璃的背景（打开窗口、换布局时用，会等排除生效）
void CaptureBehind() {
    ++s_shotSeq;  // 后台截到一半的作废
    bool visible = IsWindowVisible(s_hwnd) && !IsIconic(s_hwnd);
    if (visible) {
        if (!SetExclude(true)) return;
        if (ExcludeWait() > 0) {
            DwmFlush();
            DwmFlush();
            s_excludedAt -= kExcludeSettleMs;
        }
        if (!ExcludeWorks()) {
            SetExclude(false);
            return;
        }
    }
    RECT window = {s_pos.x, s_pos.y, s_pos.x + s_width, s_pos.y + s_height};
    s_glass.Capture(window, s_scale);
    s_baseValid = false;
    s_still = 0;
}

DWORD WINAPI ShotThread(void* param) {
    auto* job = static_cast<ShotJob*>(param);
    job->changed = Glass::Shoot(job->window, job->scale, job->style, job->shot, job->compare ? &job->prev : nullptr);
    if (!PostMessageW(job->hwnd, WM_APP_SHOT, 0, reinterpret_cast<LPARAM>(job))) delete job;
    return 0;
}

// 后台截一张：拖动时截窗口四周一大片（挪一段都在范围里），平时只截窗口这么大，和上一张比较
bool StartShot() {
    auto job = std::make_unique<ShotJob>();
    job->hwnd = s_hwnd;
    job->seq = s_shotSeq;
    LONG extra = s_moving ? static_cast<LONG>(std::lround(Px(320))) : 0;
    job->window = {s_pos.x - extra, s_pos.y - extra, s_pos.x + s_width + extra, s_pos.y + s_height + extra};
    job->scale = s_scale;
    job->style = s_glass.Style();
    job->compare = !s_moving;
    if (job->compare) {
        job->prev.area = s_glass.Area();
        job->prev.raw = s_glass.Raw();
    }
    HANDLE thread = CreateThread(nullptr, 0, ShotThread, job.get(), 0, nullptr);
    if (!thread) return false;
    job.release();
    s_shotThread = thread;
    return true;
}

// 等后台截完（关窗口时），没处理的结果扔掉。截图卡住时最多等 1 秒：线程截完发消息时窗口已经没了，
// 发不出去就自己释放
void StopShot() {
    if (!s_shotThread) return;
    if (WaitForSingleObject(s_shotThread, 1000) == WAIT_TIMEOUT) Log(L"设置窗口：后台截图没有及时结束");
    CloseHandle(s_shotThread);
    s_shotThread = nullptr;
    MSG msg;
    while (PeekMessageW(&msg, s_hwnd, WM_APP_SHOT, WM_APP_SHOT, PM_REMOVE)) delete reinterpret_cast<ShotJob*>(msg.lParam);
}

// 该截下一张了：先打开排除、等它生效，再交给后台
void LiveTick() {
    KillTimer(s_hwnd, kTimerLive);
    if (s_shotThread || s_excludeWorks < 0) return;  // 截完会再来；不支持就不再刷新
    double sinceOpen = NowMs() - s_openedAt;
    if (sinceOpen < kFadeMs) {  // 淡入完了再开始
        SetTimer(s_hwnd, kTimerLive, static_cast<UINT>(kFadeMs - sinceOpen) + 20, nullptr);
        return;
    }
    if (!IsWindowVisible(s_hwnd) || IsIconic(s_hwnd)) {
        SetExclude(false);
        SetTimer(s_hwnd, kTimerLive, 500, nullptr);
        return;
    }
    if (!SetExclude(true)) return;
    if (double wait = ExcludeWait(); wait > 0) {
        SetTimer(s_hwnd, kTimerLive, static_cast<UINT>(std::ceil(wait)), nullptr);
        return;
    }
    if (!ExcludeWorks()) {
        SetExclude(false);
        return;
    }
    if (!StartShot()) SetTimer(s_hwnd, kTimerLive, 500, nullptr);
}

// 后台截完了：变了就换上重画。拖动中接着截；背景在变就尽快再截（约每秒 60 次）；
// 一直没变就关掉排除，隔一会儿再看
void OnShot(ShotJob* raw) {
    std::unique_ptr<ShotJob> job(raw);
    if (s_shotThread) {
        CloseHandle(s_shotThread);
        s_shotThread = nullptr;
    }
    if (job->seq == s_shotSeq && job->style == s_glass.Style()) {
        if (job->changed) {
            s_glass.Adopt(std::move(job->shot), job->scale);
            s_baseValid = false;
            Render(false);
            s_still = 0;
        } else {
            ++s_still;
        }
    }
    if (s_moving) {
        LiveTick();
        return;
    }
    UINT next;
    if (s_still == 0) {
        next = 10;
    } else if (s_still < kStillTicks) {
        next = 30;
    } else {
        SetExclude(false);
        next = GetForegroundWindow() == s_hwnd ? 250 : 400;
    }
    SetTimer(s_hwnd, kTimerLive, next, nullptr);
}

// 拖动中：按新位置去背景里取色（后台截不过来时边上先用截图边缘的颜色顶着）；
// 不支持实时刷新时玻璃保持原样，跟着窗口走
void FollowMove() {
    if (s_excludeWorks < 0) {
        Render(false);
        return;
    }
    s_baseValid = false;
    Render(false);
    if (!s_shotThread) LiveTick();
}

// 拖完了：接着按平常的节奏刷新
void FollowScreen();

void EndMove() {
    s_moving = false;
    s_still = 0;
    if (s_excludeWorks >= 0) LiveTick();
    FollowScreen();
}

// 材质、颜色选项变了（或者系统换了深浅色）：换玻璃，各种材质模糊得不一样，要重新截
void ApplyLook() {
    s_glass.SetLight(ThemeIsLight(s_v.seg[kSegTheme], false));
    if (s_v.seg[kSegStyle] != s_glass.Style()) {
        s_glass.SetStyle(s_v.seg[kSegStyle]);
        if (s_hwnd && IsWindowVisible(s_hwnd)) CaptureBehind();  // 还没显示时打开窗口那里会截
    }
    s_baseValid = false;
}

// 布局变了（名单增删、换了显示器）：上边缘不动，大小跟着变，背景重新截
void Relayout(bool keepTop) {
    int oldW = s_width, oldH = s_height;
    POINT center = {s_pos.x + s_width / 2, s_pos.y + s_height / 2};
    LayoutFor(MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTONEAREST));  // 变高了也要缩到屏幕放得下
    if (s_width == oldW && s_height == oldH) {
        Render();
        return;
    }
    s_pos.x = center.x - s_width / 2;
    if (!keepTop) s_pos.y = center.y - s_height / 2;
    // 别跑出屏幕下边
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    if (s_pos.y + s_height > mi.rcWork.bottom) s_pos.y = std::max<LONG>(mi.rcWork.top, mi.rcWork.bottom - s_height);
    SetWindowPos(s_hwnd, nullptr, s_pos.x, s_pos.y, s_width, s_height, SWP_NOZORDER | SWP_NOACTIVATE);
    CaptureBehind();
    Render();
}

// 设置窗口挪到了另一块屏幕上：每块屏幕分别设置时，位置和对齐换成那块屏幕的
void FollowScreen() {
    HMONITOR monitor = MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTONEAREST);
    if (monitor == s_screen) return;
    s_screen = monitor;
    s_screenKey = MonitorKey(monitor);
    if (!s_v.on[kPerScreen]) return;
    ShowPlace();
    Relayout(true);
}

// ---- 动画 ----

bool Animating(double now) {
    if (now - s_openedAt < kFadeMs) return true;
    for (int i = 0; i < kToggleCount; ++i)
        if (s_knob[i] != (s_v.on[i] ? 1.0f : 0.0f)) return true;
    return false;
}

bool s_framePosted = false;

// 动画的一帧。只是淡入时内容不用重画，只改整体透明度；放完之前等屏幕刷新再画下一帧，不跟着 15 毫秒的定时器走（不匀）
void StepAnimation() {
    s_framePosted = false;
    if (!s_hwnd) return;
    double now = NowMs();
    bool on = AnimationsOn();
    double fade = on ? std::min(1.0, (now - s_openedAt) / kFadeMs) : 1.0;
    s_alpha = static_cast<BYTE>(std::lround(255 * fade));
    bool knobs = false;
    for (int i = 0; i < kToggleCount; ++i) {
        float target = s_v.on[i] ? 1.0f : 0.0f;
        if (s_knob[i] == target) continue;
        knobs = true;
        double t = on ? std::min(1.0, (now - s_knobStart[i]) / kKnobMs) : 1.0;
        double e = 1 - (1 - t) * (1 - t) * (1 - t);
        s_knob[i] = static_cast<float>(s_knobFrom[i] + (target - s_knobFrom[i]) * e);
        if (t >= 1) s_knob[i] = target;
    }
    Render(knobs);
    if (on && Animating(now)) {
        WaitForVBlank();
        s_framePosted = true;
        PostMessageW(s_hwnd, WM_APP_FRAME, 0, 0);
    }
}

void StartAnimation() {
    if (s_framePosted || !s_hwnd) return;
    s_framePosted = true;
    PostMessageW(s_hwnd, WM_APP_FRAME, 0, 0);
}

// ---- 操作 ----

void PreviewPopup(bool hold);

// 开关的值不是点着换的（换了底部 / 侧边、在别处改了设置）：滑钮从现在的位置滑过去
void AnimateKnobs() {
    for (int i = 0; i < kToggleCount; ++i) {
        float target = s_v.on[i] ? 1.0f : 0.0f;
        if (s_knob[i] != target) {
            s_knobFrom[i] = s_knob[i];
            s_knobStart[i] = NowMs();
            StartAnimation();
        }
    }
}

void SetToggle(int which, bool on) {
    if (s_v.on[which] == on) return;
    s_v.on[which] = on;
    if (const SideToggle* t = FindSideToggle(which)) s_v.sides[s_v.seg[kSegSide]].*t->item = on;
    s_knobFrom[which] = s_knob[which];
    s_knobStart[which] = NowMs();
    StartAnimation();
    // 跟着收起 / 放出来的行；“最大化或全屏时隐藏任务栏”换的是下面那个名单
    if (which == kAutoHide || which == kKeepFloats || which == kLongPress || which == kClock) Relayout(true);
    // 显示哪些、时钟显示什么：弹出来看看
    if (which == kShowTitle || FindSideToggle(which) || which == kClockSeconds || which == kClockWeekday ||
        which == kClockDate)
        PreviewPopup(false);
    if (which == kPerScreen) {  // 位置和对齐换成这块屏幕自己的（或者共用的）
        ShowPlace();
        Relayout(true);
        PreviewPopup(false);
    }
}

void EndCapture();

bool PreviewsPopup(int which);
void PreviewPopup(bool hold);

// 换一页（窗口大小不变）。焦点跟到导航上新的这一项
void SetPage(int page) {
    page = (page % kPageCount + kPageCount) % kPageCount;
    if (page == s_page) return;
    EndCapture();
    if (PreviewsPopup(s_drag)) PreviewPopup(false);  // 拖着滑块换页：预览过一会儿照常收起
    s_page = page;
    s_hover = s_press = {};
    s_drag = -1;
    s_focus = {kNav, page, {}};
    Relayout(true);
}

// 调这个滑块时弹出迷你任务栏预览
bool PreviewsPopup(int which) { return which == kRadius || which == kGlassAlpha || which == kIconSize || which == kIconGap; }

// 窗口里现在选的外观
PopupLook DraftLook() {
    PopupLook look;
    look.style = s_v.seg[kSegStyle];
    look.customTint = s_v.seg[kSegTint] == 1;
    look.tint = s_v.tint;
    look.light = look.customTint ? ColorLuma(s_v.tint) > 150 : ThemeIsLight(s_v.seg[kSegTheme], true);
    look.transparency = s_v.slider[kGlassAlpha];
    look.radius = s_v.slider[kRadius];
    look.iconSize = s_v.slider[kIconSize];
    look.iconGap = s_v.slider[kIconGap];
    look.showTitle = s_v.on[kShowTitle];
    look.customAccent = s_v.seg[kSegAccent] == 1;
    look.accent = s_v.accent;
    for (int side = 0; side < 2; ++side) {
        look.modules[side] = g_settings.modules[side];  // 顺序在编辑模式里排，窗口里只改显示哪些
        for (const SideToggle& t : kSideToggles) look.modules[side].*t.item = s_v.sides[side].*t.item;
    }
    look.clockSeconds = s_v.on[kClockSeconds];
    look.clockWeekday = s_v.on[kClockWeekday];
    look.clockDate = s_v.on[kClockDate];
    look.trayStyle = s_v.seg[kSegTray];
    return look;
}

// 预览弹在哪条边：在“状态和控制”页看的是“迷你任务栏在”选的那种（选了侧边、位置却在底部时弹在左边）
int PreviewEdge() {
    int edge = s_v.seg[kSegEdge];
    if (s_page != kPageControls || ModuleSideOf(edge) == s_v.seg[kSegSide]) return edge;
    return s_v.seg[kSegSide] == kSideBottom ? kEdgeBottom : kEdgeLeft;
}

// 按窗口里现在选的位置、对齐、外观和显示哪些弹出迷你任务栏预览（大小是现在的）。hold：还按着滑块，先不收起
void PreviewPopup(bool hold) {
    Popup_PreviewLook(DraftLook());
    Popup_Preview(g_settings.popupScale, PreviewEdge(), s_v.seg[kSegAlign], hold);
}

void SetSlider(int which, int value) {
    const SliderSpec& spec = kSliderSpec[which];
    value = spec.minValue + (value - spec.minValue + spec.step / 2) / spec.step * spec.step;
    s_v.slider[which] = std::clamp(value, spec.minValue, spec.maxValue);
    if (PreviewsPopup(which)) PreviewPopup(s_drag == which);  // 弹出迷你任务栏看看大小、透明度
}

void SetSliderFromX(int which, int x) {
    const SliderGeom& sg = s_sliderGeom[which];
    const SliderSpec& spec = kSliderSpec[which];
    float t = std::clamp((x - sg.x0) / (sg.x1 - sg.x0), 0.0f, 1.0f);
    SetSlider(which, spec.minValue + static_cast<int>(std::lround(t * (spec.maxValue - spec.minValue))));
}

void NudgeSlider(int which, int steps) {
    SetSlider(which, s_v.slider[which] + steps * kSliderSpec[which].step);
}

// ---- 录快捷键 ----
// 录的时候先注销本程序已经设的快捷键（不然按下它们会被系统拿走，这里收不到）

void StartCapture(int which) {
    s_capture = which;
    s_hotkeyError[which].clear();
    s_captureLone = 0;
    s_focus = {kHotkeyRow, which, {}};
    Hotkeys_Suspend(true);
}

void EndCapture() {
    if (s_capture < 0) return;
    s_hotkeyError[s_capture].clear();
    s_capture = -1;
    s_captureLone = 0;
    Hotkeys_Suspend(false);
}

void SetHotkey(int which, UINT hotkey) {
    s_v.hotkey[which] = hotkey;
    s_hotkeyError[which].clear();
    // 和小叉一起消失的悬停状态清掉
    if (s_hover.kind == kHotkeyClear) s_hover = {};
}

// which 设成 hotkey、按 hold（单按 / 长按）触发行不行，不行的话为什么
const wchar_t* HotkeyRefusal(int which, UINT hotkey, bool hold) {
    if (const wchar_t* problem = HotkeyProblem(hotkey, hold)) return problem;
    for (int other = 0; other < kHotkeyCount; ++other)
        if (other != which && hotkey == s_v.hotkey[other] && hold == s_v.hold[other]) return Tr(L"和另一个快捷键重复了");
    return nullptr;
}

// 录到了一个键：能用就设上、不录了，不能用就说为什么、接着录
void TryHotkey(UINT hotkey) {
    int which = s_capture;
    bool hold = s_v.hold[which];
    const wchar_t* refusal = HotkeyRefusal(which, hotkey, hold);
    UINT vk = HotkeyVk(hotkey);
    // 单按的键盘按键用 RegisterHotKey，先试试是不是被别的程序占了（鼠标键、长按这些试不出来）
    bool viaRegister = !hold && !IsMouseVk(vk) && !IsModifierVk(vk) && vk != VK_CAPITAL && vk != VK_NUMLOCK && vk != VK_SCROLL;
    if (!refusal && viaRegister) {
        if (RegisterHotKey(s_hwnd, 0xBFFF, HotkeyMods(hotkey) | MOD_NOREPEAT, vk)) UnregisterHotKey(s_hwnd, 0xBFFF);
        else refusal = Tr(L"被别的程序占用了，换一个吧");
    }
    if (refusal) {
        s_hotkeyError[which] = refusal;
        return;
    }
    SetHotkey(which, hotkey);
    EndCapture();
    if (which == kHotkeyPopup) SetToggle(kLongPress, false);  // 有了弹出迷你任务栏的快捷键，长按 Win 先关掉（想要可以再打开）
}

// 录的时候现在按着的 Ctrl / Shift / Alt；except 这个（分左右的）键不算
UINT CaptureMods(UINT except = 0) {
    auto held = [except](int vk) { return static_cast<UINT>(vk) != except && GetKeyState(vk) < 0; };
    return (held(VK_LCONTROL) || held(VK_RCONTROL) ? MOD_CONTROL : 0) | (held(VK_LSHIFT) || held(VK_RSHIFT) ? MOD_SHIFT : 0) |
           (held(VK_LMENU) || held(VK_RMENU) ? MOD_ALT : 0);
}

// 键盘消息里的 Shift / Ctrl / Alt 分出左右
UINT SpecificModifier(UINT vk, LPARAM lParam) {
    bool extended = (lParam & (1 << 24)) != 0;
    switch (vk) {
        case VK_SHIFT: {
            UINT scan = (lParam >> 16) & 0xFF;
            UINT specific = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
            return specific == VK_RSHIFT || specific == VK_LSHIFT ? specific : (scan == 0x36 ? VK_RSHIFT : VK_LSHIFT);
        }
        case VK_CONTROL: return extended ? VK_RCONTROL : VK_LCONTROL;
        case VK_MENU: return extended ? VK_RMENU : VK_LMENU;
    }
    return 0;
}

// 录的时候按了键（WM_KEYDOWN / WM_SYSKEYDOWN）
void CaptureKey(UINT vk, LPARAM lParam) {
    if (vk == VK_PROCESSKEY || vk == VK_PACKET || vk == VK_LWIN || vk == VK_RWIN) return;  // 输入法、模拟输入的字符；Win 留给长按
    if (UINT specific = SpecificModifier(vk, lParam)) {
        // 只按了修饰键：框里跟着显示，等主键；就这一个修饰键按下又松开的话录它自己
        if (!(lParam & (1 << 30))) s_captureLone = CaptureMods(specific) ? 0 : specific;
        return;
    }
    s_captureLone = 0;
    s_eatChar = true;
    UINT mods = CaptureMods();
    if (!mods && vk == VK_ESCAPE) {
        EndCapture();
        return;
    }
    if (!mods && (vk == VK_BACK || vk == VK_DELETE)) {
        SetHotkey(s_capture, 0);
        EndCapture();
        return;
    }
    TryHotkey(MakeHotkey(mods, vk));
}

// 录的时候松开了键：刚才只按了一个 Ctrl / Shift / Alt 就录它
void CaptureKeyUp(UINT vk, LPARAM lParam) {
    UINT specific = SpecificModifier(vk, lParam);
    if (specific && specific == s_captureLone) TryHotkey(MakeHotkey(0, specific));
    s_captureLone = 0;
}

// 录的时候在窗口上按了鼠标键
void CaptureMouse(UINT vk) {
    s_captureLone = 0;
    TryHotkey(MakeHotkey(CaptureMods(), vk));
}

// 选单按还是长按：设着的键这样触发不行的话不换，说为什么
void SetHotkeyMode(int which, bool hold) {
    if (s_v.hold[which] == hold) return;
    if (s_v.hotkey[which])
        if (const wchar_t* refusal = HotkeyRefusal(which, s_v.hotkey[which], hold)) {
            s_hotkeyError[which] = refusal;
            return;
        }
    s_v.hold[which] = hold;
    s_hotkeyError[which].clear();
    Relayout(true);  // 长按时长跟着放出来 / 收起
}

void AddExclude(const std::wstring& path) {
    std::wstring name = NormalizeExeName(path);
    std::vector<std::wstring>& apps = ShownApps();
    if (name.empty() || std::find(apps.begin(), apps.end(), name) != apps.end()) return;
    apps.push_back(name);
    Relayout(true);
}

void RemoveExclude(int index) {
    std::vector<std::wstring>& apps = ShownApps();
    if (index < 0 || index >= static_cast<int>(apps.size())) return;
    apps.erase(apps.begin() + index);
    s_hover = s_press = {};
    // 焦点在被去掉的那个上：移到同一位置的下一个（没有了就落到“添加程序”上）
    if (s_focus.kind == kChip && s_focus.index >= static_cast<int>(apps.size()))
        s_focus = apps.empty() ? Item{kAddChip, 0, {}} : Item{kChip, static_cast<int>(apps.size()) - 1, {}};
    Relayout(true);
}

// 选一个 exe 文件
std::wstring BrowseExe() {
    std::wstring result;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return result;
    COMDLG_FILTERSPEC filter[] = {{Tr(L"程序 (*.exe)"), L"*.exe"}};
    dialog->SetFileTypes(1, filter);
    dialog->SetTitle(Tr(s_v.on[kAutoHide] ? L"选择最大化时不隐藏任务栏的程序" : L"选择最大化时隐藏任务栏的程序"));
    if (SUCCEEDED(dialog->Show(s_hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

// 正开着窗口的程序（不在名单里的），给“添加程序”菜单用
std::vector<std::wstring> RunningExeNames() {
    struct Ctx {
        std::vector<std::wstring> names;
    } ctx;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            if (!IsWindowVisible(h) || IsCloaked(h) || GetWindow(h, GW_OWNER) || IsOwnProcess(h)) return TRUE;
            if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
            if (GetWindowTextLengthW(h) == 0) return TRUE;
            std::wstring name = WindowExeName(h);
            if (name.empty() || name == L"explorer.exe" || name == L"applicationframehost.exe") return TRUE;
            if (std::find(c->names.begin(), c->names.end(), name) == c->names.end()) c->names.push_back(name);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    std::vector<std::wstring> list;
    const std::vector<std::wstring>& apps = ShownApps();
    for (const std::wstring& name : ctx.names)
        if (std::find(apps.begin(), apps.end(), name) == apps.end()) list.push_back(name);
    std::sort(list.begin(), list.end());
    if (list.size() > 20) list.resize(20);
    return list;
}

void ShowAddMenu(const RectF& anchor) {
    std::vector<std::wstring> running = RunningExeNames();
    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < running.size(); ++i) {
        std::wstring text = running[i];
        for (size_t p = 0; (p = text.find(L'&', p)) != std::wstring::npos; p += 2) text.insert(p, 1, L'&');
        AppendMenuW(menu, MF_STRING, 100 + i, text.c_str());
    }
    if (running.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, Tr(L"没有其它开着窗口的程序"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1, Tr(L"浏览..."));
    POINT at = {static_cast<LONG>(anchor.X), static_cast<LONG>(anchor.Y + anchor.Height + Px(4))};
    ClientToScreen(s_hwnd, &at);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, at.x, at.y, 0, s_hwnd, nullptr);
    DestroyMenu(menu);
    if (!s_hwnd) return;
    if (cmd == 1) {
        std::wstring path = BrowseExe();
        if (!path.empty() && s_hwnd) AddExclude(path);
    } else if (cmd >= 100 && cmd < 100 + running.size()) {
        AddExclude(running[cmd - 100]);
    }
}

// 自定义底色（accent = false）、强调色：系统的选颜色窗口。返回选了没有
bool PickColor(bool accent) {
    static COLORREF custom[16];
    static bool inited = false;
    if (!inited) {
        std::fill(std::begin(custom), std::end(custom), RGB(255, 255, 255));
        inited = true;
    }
    COLORREF& color = accent ? s_v.accent : s_v.tint;
    CHOOSECOLORW cc = {};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = s_hwnd;
    cc.rgbResult = color;
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc) || !s_hwnd) return false;
    color = cc.rgbResult;
    ApplyLook();
    return true;
}

Values ValuesFromSettings() {
    const Settings& st = g_settings;
    Values v;
    v.on[kAutoHide] = st.autoHideOnFullscreen;
    v.on[kAlwaysHide] = st.alwaysHideTaskbar;
    v.on[kKeepFloats] = st.keepFloatsOnTop;
    v.on[kPinTopmost] = st.pinTopmost;
    v.on[kLongPress] = st.longPressPopup;
    v.on[kPinned] = st.showPinnedApps;
    v.on[kFlashAlert] = st.flashAlert;
    v.on[kEdgePopup] = st.edgePopup;
    v.sides[kSideBottom] = st.modules[kSideBottom];
    v.sides[kSideEdge] = st.modules[kSideEdge];
    v.seg[kSegSide] = ModuleSideOf(st.popupEdge);
    for (const SideToggle& t : kSideToggles) v.on[t.toggle] = v.sides[v.seg[kSegSide]].*t.item;
    v.on[kClockSeconds] = st.clockSeconds;
    v.on[kClockWeekday] = st.clockWeekday;
    v.on[kClockDate] = st.clockDate;
    v.on[kShowTitle] = st.showTitle;
    v.on[kAutoStart] = IsAutoStartEnabled();
    v.on[kUpdates] = st.checkUpdates;
    v.on[kPerScreen] = st.popupPerScreen;
    v.on[kKeepOtherBars] = st.keepOtherTaskbars;
    v.on[kMarkScreens] = st.markScreens;
    v.on[kBrightAll] = st.brightnessAllScreens;
    v.seg[kSegWindows] = st.popupWindows;
    v.slider[kLongPressMs] = st.longPressMs;
    v.slider[kRadius] = st.popupRadius;
    v.slider[kGlassAlpha] = st.glassTransparency;
    v.slider[kIconSize] = st.iconSize;
    v.slider[kIconGap] = st.iconGap;
    v.seg[kSegStyle] = st.glassStyle;
    v.seg[kSegTheme] = st.theme;
    v.seg[kSegTray] = st.trayStyle;
    v.seg[kSegMonitor] = st.popupMonitor;
    v.seg[kSegLang] = st.language;
    v.seg[kSegTint] = st.customTint ? 1 : 0;
    v.seg[kSegAccent] = st.customAccent ? 1 : 0;
    v.tint = st.tintColor;
    v.accent = st.accentColor;
    v.edge = st.popupEdge;
    v.align = st.popupAlign;
    v.offset = st.popupOffset;
    v.screens = st.screens;
    for (int i = 0; i < kHotkeyCount; ++i) {
        v.hotkey[i] = st.hotkey[i];
        v.hold[i] = st.hotkeyHold[i];
        v.slider[kPopupHoldMs + i] = st.hotkeyHoldMs[i];
    }
    v.exclude = st.excludeApps;
    v.include = st.includeApps;
    v.noHide = st.noHideScreens;
    return v;
}

// 窗口里的值改成 now：没动过的项（或者 force 的）换成新的
template <typename T>
void Take(T& now, T& before, const T& fresh, bool force) {
    if (force || now == before) now = fresh;
    before = fresh;
}

// 设置在别处变了，窗口里的值跟着变。kMerge：只换没动过的；kForceLook：换上了外观方案，外观的都换；kForceAll：导入了设置，全换
enum ReloadMode { kMerge, kForceLook, kForceAll };
void Reload(ReloadMode mode) {
    Values fresh = ValuesFromSettings();
    bool all = mode == kForceAll, look = mode != kMerge;
    auto in = [](int i, std::initializer_list<int> list) { return std::find(list.begin(), list.end(), i) != list.end(); };
    for (int i = 0; i < kToggleCount; ++i)
        if (!FindSideToggle(i))
            Take(s_v.on[i], s_initial.on[i], fresh.on[i],
                 all || (look && in(i, {kClockSeconds, kClockWeekday, kClockDate, kShowTitle, kPerScreen})));
    for (int side = 0; side < 2; ++side)  // 底部、侧边各一份的：外观方案里也有
        for (const SideToggle& t : kSideToggles)
            Take(s_v.sides[side].*t.item, s_initial.sides[side].*t.item, fresh.sides[side].*t.item, look);
    ShowSide(s_v.seg[kSegSide]);
    AnimateKnobs();
    for (int i = 0; i < kSliderCount; ++i)
        Take(s_v.slider[i], s_initial.slider[i], fresh.slider[i], all || (look && in(i, {kRadius, kGlassAlpha, kIconSize, kIconGap})));
    for (int i = 0; i < kSegCount; ++i)
        if (i != kSegEdge && i != kSegAlign && i != kSegSide)  // 这几项是显示用的，见 ShowPlace、ShowSide
            Take(s_v.seg[i], s_initial.seg[i], fresh.seg[i],
                 all || (look && in(i, {kSegStyle, kSegTheme, kSegTray, kSegTint, kSegAccent})));
    Take(s_v.tint, s_initial.tint, fresh.tint, look);
    Take(s_v.accent, s_initial.accent, fresh.accent, look);
    Take(s_v.edge, s_initial.edge, fresh.edge, look);
    Take(s_v.align, s_initial.align, fresh.align, look);
    Take(s_v.offset, s_initial.offset, fresh.offset, look);
    Take(s_v.screens, s_initial.screens, fresh.screens, look);
    for (int i = 0; i < kHotkeyCount; ++i) {
        Take(s_v.hotkey[i], s_initial.hotkey[i], fresh.hotkey[i], all);
        Take(s_v.hold[i], s_initial.hold[i], fresh.hold[i], all);
    }
    Take(s_v.exclude, s_initial.exclude, fresh.exclude, all);
    Take(s_v.include, s_initial.include, fresh.include, all);
    Take(s_v.noHide, s_initial.noHide, fresh.noHide, all);
    if (all) {
        EndCapture();
        for (auto& error : s_hotkeyError) error.clear();
        SetUiEnglish(LanguageIsEnglish(s_v.seg[kSegLang]));
        SetWindowTextW(s_hwnd, Tr(L"TaskbarPopup 设置"));
    }
    s_presets = LookPresets();
    s_hover = s_press = {};
    ShowPlace();
    ApplyLook();
    Relayout(true);
    if (!FindItem(s_focus)) s_focus = {};
}

void Apply();

// 把窗口里改的存下、生效（存外观方案、导出之前也先存，存的是设置文件里的）
void Commit() {
    Apply();
    s_initial = s_v;
}

// 窗口里有没有改了还没存的（只看存进设置的，位置、对齐的分段按钮和“迷你任务栏在”是显示用的）
bool Dirty() {
    const Values &a = s_v, &b = s_initial;
    for (int i = 0; i < kToggleCount; ++i)
        if (!FindSideToggle(i) && a.on[i] != b.on[i]) return true;
    for (int side = 0; side < 2; ++side)
        for (const SideToggle& t : kSideToggles)
            if (a.sides[side].*t.item != b.sides[side].*t.item) return true;
    if (!std::equal(a.slider, a.slider + kSliderCount, b.slider)) return true;
    for (int i = 0; i < kSegCount; ++i)
        if (i != kSegEdge && i != kSegAlign && i != kSegSide && a.seg[i] != b.seg[i]) return true;
    return a.edge != b.edge || a.align != b.align || a.offset != b.offset || !(a.screens == b.screens) ||
           a.tint != b.tint || a.accent != b.accent || !std::equal(a.hotkey, a.hotkey + kHotkeyCount, b.hotkey) ||
           !std::equal(a.hold, a.hold + kHotkeyCount, b.hold) || a.exclude != b.exclude || a.include != b.include ||
           a.noHide != b.noHide;
}

// 改了什么过一小会儿就存下、生效；拖着滑块时等松开
void ScheduleSave() {
    if (s_hwnd && s_drag < 0 && Dirty()) SetTimer(s_hwnd, kTimerSave, kSaveDelayMs, nullptr);
}

void SaveNow() {
    if (s_hwnd) KillTimer(s_hwnd, kTimerSave);
    if (Dirty()) Commit();
}

// 把现在的外观存成第 slot 个方案（0 = 新的一个）
void SavePreset(int slot) {
    Commit();
    int saved = SaveLookPreset(slot);
    s_presets = LookPresets();
    if (saved) s_focus = {kPreset, saved, {}};
    s_hover = s_press = {};
    Relayout(true);
}

void ShowPresetMenu(const Item& it) {
    enum { kCmdUse = 1, kCmdOverwrite, kCmdDelete };
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCmdUse, Tr(L"换上"));
    AppendMenuW(menu, MF_STRING, kCmdOverwrite, Tr(L"用现在的外观覆盖"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdDelete, Tr(L"删除"));
    POINT at = {static_cast<LONG>(it.rect.X), static_cast<LONG>(it.rect.Y + it.rect.Height + Px(4))};
    ClientToScreen(s_hwnd, &at);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, at.x, at.y, 0, s_hwnd, nullptr);
    DestroyMenu(menu);
    if (!s_hwnd) return;
    int slot = it.index;
    if (cmd == kCmdUse) {
        ApplyLookPreset(slot);
        Reload(kForceLook);
        PreviewPopup(false);  // 弹出来看看换上的样子
    } else if (cmd == kCmdOverwrite) {
        SavePreset(slot);
    } else if (cmd == kCmdDelete) {
        DeleteLookPreset(slot);
        s_presets = LookPresets();
        s_hover = s_press = {};
        if (s_focus.kind == kPreset && s_focus.index == slot) s_focus = {kAddPreset, 0, {}};
        Relayout(true);
    } else {
        Render();
    }
}

// 选设置文件：save 是导出（另存为），否则是导入（打开）
std::wstring BrowseSettingsFile(bool save) {
    std::wstring result;
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))))
        return result;
    COMDLG_FILTERSPEC filter[] = {{Tr(L"设置文件 (*.ini)"), L"*.ini"}};
    dialog->SetFileTypes(1, filter);
    dialog->SetDefaultExtension(L"ini");
    dialog->SetTitle(Tr(save ? L"导出设置" : L"导入设置"));
    if (save) dialog->SetFileName(L"TaskbarPopup.ini");
    if (SUCCEEDED(dialog->Show(s_hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

void ExportFile() {
    std::wstring path = BrowseSettingsFile(true);
    if (path.empty() || !s_hwnd) return;
    Commit();
    s_fileNote = ExportSettings(path) ? TrF(L"已导出到 %ls", FileNameOf(path).c_str()) : Tr(L"导出失败");
    Relayout(true);
}

void ImportFile() {
    std::wstring path = BrowseSettingsFile(false);
    if (path.empty() || !s_hwnd) return;
    if (!ImportSettings(path)) {
        s_fileNote = Tr(L"导入失败：不是 TaskbarPopup 的设置文件");
        Relayout(true);
        return;
    }
    s_fileNote = TrF(L"已导入 %ls", FileNameOf(path).c_str());
    Reload(kForceAll);
}

void Apply() {
    // 只改窗口里动过的项，其余的保持现在的值（窗口开着的时候可能在别处改过）
    auto changed = [](int t) { return s_v.on[t] != s_initial.on[t]; };
    if (changed(kAutoHide)) g_settings.autoHideOnFullscreen = s_v.on[kAutoHide];
    if (changed(kAlwaysHide)) g_settings.alwaysHideTaskbar = s_v.on[kAlwaysHide];
    if (changed(kKeepFloats)) g_settings.keepFloatsOnTop = s_v.on[kKeepFloats];
    if (changed(kPinTopmost)) g_settings.pinTopmost = s_v.on[kPinTopmost];
    if (changed(kLongPress)) g_settings.longPressPopup = s_v.on[kLongPress];
    if (changed(kPinned)) g_settings.showPinnedApps = s_v.on[kPinned];
    if (changed(kFlashAlert)) g_settings.flashAlert = s_v.on[kFlashAlert];
    if (changed(kEdgePopup)) g_settings.edgePopup = s_v.on[kEdgePopup];
    for (int side = 0; side < 2; ++side)
        for (const SideToggle& t : kSideToggles)
            if (s_v.sides[side].*t.item != s_initial.sides[side].*t.item)
                g_settings.modules[side].*t.item = s_v.sides[side].*t.item;
    if (changed(kClockSeconds)) g_settings.clockSeconds = s_v.on[kClockSeconds];
    if (changed(kClockWeekday)) g_settings.clockWeekday = s_v.on[kClockWeekday];
    if (changed(kClockDate)) g_settings.clockDate = s_v.on[kClockDate];
    if (changed(kShowTitle)) g_settings.showTitle = s_v.on[kShowTitle];
    if (changed(kUpdates)) g_settings.checkUpdates = s_v.on[kUpdates];
    if (s_v.slider[kLongPressMs] != s_initial.slider[kLongPressMs]) g_settings.longPressMs = s_v.slider[kLongPressMs];
    if (s_v.slider[kRadius] != s_initial.slider[kRadius]) g_settings.popupRadius = s_v.slider[kRadius];
    if (s_v.slider[kGlassAlpha] != s_initial.slider[kGlassAlpha]) g_settings.glassTransparency = s_v.slider[kGlassAlpha];
    if (s_v.slider[kIconSize] != s_initial.slider[kIconSize]) g_settings.iconSize = s_v.slider[kIconSize];
    if (s_v.slider[kIconGap] != s_initial.slider[kIconGap]) g_settings.iconGap = s_v.slider[kIconGap];
    if (s_v.seg[kSegTint] != s_initial.seg[kSegTint]) g_settings.customTint = s_v.seg[kSegTint] == 1;
    if (s_v.seg[kSegAccent] != s_initial.seg[kSegAccent]) g_settings.customAccent = s_v.seg[kSegAccent] == 1;
    if (s_v.tint != s_initial.tint) g_settings.tintColor = s_v.tint;
    if (s_v.accent != s_initial.accent) g_settings.accentColor = s_v.accent;
    for (int i = 0; i < kHotkeyCount; ++i)
        if (s_v.slider[kPopupHoldMs + i] != s_initial.slider[kPopupHoldMs + i])
            g_settings.hotkeyHoldMs[i] = s_v.slider[kPopupHoldMs + i];
    if (s_v.seg[kSegStyle] != s_initial.seg[kSegStyle]) g_settings.glassStyle = s_v.seg[kSegStyle];
    if (s_v.seg[kSegTheme] != s_initial.seg[kSegTheme]) g_settings.theme = s_v.seg[kSegTheme];
    if (s_v.seg[kSegTray] != s_initial.seg[kSegTray]) g_settings.trayStyle = s_v.seg[kSegTray];
    if (s_v.align != s_initial.align) g_settings.popupAlign = s_v.align;
    if (s_v.offset != s_initial.offset) g_settings.popupOffset = s_v.offset;
    if (s_v.edge != s_initial.edge) g_settings.popupEdge = s_v.edge;
    if (s_v.seg[kSegMonitor] != s_initial.seg[kSegMonitor]) g_settings.popupMonitor = s_v.seg[kSegMonitor];
    if (changed(kPerScreen)) g_settings.popupPerScreen = s_v.on[kPerScreen];
    if (changed(kKeepOtherBars)) g_settings.keepOtherTaskbars = s_v.on[kKeepOtherBars];
    if (changed(kMarkScreens)) g_settings.markScreens = s_v.on[kMarkScreens];
    if (changed(kBrightAll)) g_settings.brightnessAllScreens = s_v.on[kBrightAll];
    if (s_v.seg[kSegWindows] != s_initial.seg[kSegWindows]) g_settings.popupWindows = s_v.seg[kSegWindows];
    for (const ScreenPlace& p : s_v.screens) {  // 只改动过的屏幕
        ScreenPlace* before = FindScreenPlace(s_initial.screens, p.key);
        if (before && *before == p) continue;
        if (ScreenPlace* now = FindScreenPlace(g_settings.screens, p.key)) *now = p;
        else g_settings.screens.push_back(p);
    }
    if (s_v.seg[kSegLang] != s_initial.seg[kSegLang]) g_settings.language = s_v.seg[kSegLang];
    for (int i = 0; i < kHotkeyCount; ++i) {
        if (s_v.hotkey[i] != s_initial.hotkey[i]) g_settings.hotkey[i] = s_v.hotkey[i];
        if (s_v.hold[i] != s_initial.hold[i]) g_settings.hotkeyHold[i] = s_v.hold[i];
    }

    // 名单按增删合并到现在的名单上，不整个替换
    auto contains = [](const std::vector<std::wstring>& list, const std::wstring& name) {
        return std::find(list.begin(), list.end(), name) != list.end();
    };
    auto merge = [&](const std::vector<std::wstring>& before, const std::vector<std::wstring>& after,
                     std::vector<std::wstring>& current) {
        for (const std::wstring& name : before)
            if (!contains(after, name)) current.erase(std::remove(current.begin(), current.end(), name), current.end());
        for (const std::wstring& name : after)
            if (!contains(before, name) && !contains(current, name)) current.push_back(name);
    };
    merge(s_initial.exclude, s_v.exclude, g_settings.excludeApps);
    merge(s_initial.include, s_v.include, g_settings.includeApps);
    merge(s_initial.noHide, s_v.noHide, g_settings.noHideScreens);

    SaveSettings();
    if (changed(kAutoStart)) SetAutoStart(s_v.on[kAutoStart]);
    ApplySettings();
}

void Activate(const Item& it) {
    // 快捷键那一行下面的提示（为什么不行）点了别处就去掉
    for (int i = 0; i < kHotkeyCount; ++i) {
        bool sameRow = ((it.kind == kHotkeyRow || it.kind == kHotkeyClear) && it.index == i) ||
                       (it.kind == kHotkeyMode && it.index / kSegStride == i);
        if (!sameRow && s_capture != i) s_hotkeyError[i].clear();
    }
    switch (it.kind) {
        case kToggleRow:
        case kToggleChip: SetToggle(it.index, !s_v.on[it.index]); break;
        case kSegment: {
            int group = it.index / kSegStride, old = s_v.seg[group];
            s_v.seg[group] = it.index % kSegStride;
            // 换材质时圆角还是原来那种的默认值：跟着换成新材质的（Win11 原版是 8）
            if (group == kSegStyle && s_v.slider[kRadius] == DefaultPopupRadius(old))
                s_v.slider[kRadius] = DefaultPopupRadius(s_v.seg[group]);
            // 点了自定义颜色：选一个；原来跟随着、又没选的话还是跟随
            if ((group == kSegTint || group == kSegAccent) && s_v.seg[group] == 1 && !PickColor(group == kSegAccent) && old == 0)
                s_v.seg[group] = 0;
            if (it.index / kSegStride == kSegEdge || it.index / kSegStride == kSegAlign) StorePlace();
            // 换了底部、侧边（或者把位置换到了另一种）：开关换成那一份
            if (group == kSegSide || (group == kSegEdge && ModuleSideOf(s_v.seg[kSegEdge]) != s_v.seg[kSegSide])) {
                ShowSide(group == kSegSide ? s_v.seg[kSegSide] : ModuleSideOf(s_v.seg[kSegEdge]));
                AnimateKnobs();
            }
            ApplyLook();  // 马上换上
            if (it.index / kSegStride == kSegLang) {  // 马上换成选的语言
                SetUiEnglish(LanguageIsEnglish(s_v.seg[kSegLang]));
                SetWindowTextW(s_hwnd, Tr(L"TaskbarPopup 设置"));
            }
            // 竖排时收起对齐那一行；Win11 原版时收起透明度；换语言字的宽度变了；自定义颜色时多一个色块
            if (group == kSegEdge || group == kSegStyle || group == kSegLang || group == kSegTint || group == kSegAccent ||
                group == kSegAlign || group == kSegSide || group == kSegWindows)
                Relayout(true);
            if (group == kSegEdge || group == kSegAlign || group == kSegStyle || group == kSegTheme || group == kSegTint ||
                group == kSegAccent || group == kSegSide || group == kSegTray)
                PreviewPopup(false);  // 弹在选的位置、换上选的样子看看
            break;
        }
        case kHotkeyRow:
            if (s_capture == it.index) EndCapture();  // 再点一下不录了
            else StartCapture(it.index);
            break;
        case kHotkeyClear: SetHotkey(it.index, 0); break;
        case kHotkeyMode: SetHotkeyMode(it.index / kSegStride, it.index % kSegStride == 1); break;
        case kCheckNow: Update_CheckNow(true); break;
        case kSwatch:
            if (PickColor(it.index == 1)) PreviewPopup(false);
            break;
        case kPreset: ShowPresetMenu(it); return;
        case kAddPreset: SavePreset(0); return;
        case kFile:
            if (it.index == 0) ImportFile();
            else ExportFile();
            return;
        case kNav: SetPage(it.index); return;
        case kChip: RemoveExclude(it.index); return;
        case kScreenChip: {
            const std::wstring& key = s_screenChips[it.index].first;
            if (key.empty()) break;  // 认不出这块屏幕
            if (ScreenHides(key)) s_v.noHide.push_back(key);
            else s_v.noHide.erase(std::remove(s_v.noHide.begin(), s_v.noHide.end(), key), s_v.noHide.end());
            break;
        }
        case kAddChip: ShowAddMenu(it.rect); return;
        case kClose: DestroyWindow(s_hwnd); return;
        default: break;
    }
    Render();
}

// 能用 Tab 走到的项，按显示顺序
std::vector<Item> FocusOrder() {
    std::vector<Item> list;
    for (const Item& it : s_items)
        if (it.kind != kClose && it.kind != kHotkeyClear && Enabled(it)) list.push_back(it);
    // “立即检查”排在“自动检查更新”后面
    auto check = std::find_if(list.begin(), list.end(), [](const Item& i) { return i.kind == kCheckNow; });
    auto isUpdates = [](const Item& i) { return i.kind == kToggleRow && i.index == kUpdates; };
    if (check != list.end() && std::find_if(list.begin(), list.end(), isUpdates) != list.end()) {
        Item c = *check;
        list.erase(check);
        list.insert(std::find_if(list.begin(), list.end(), isUpdates) + 1, c);
    }
    return list;
}

void MoveFocus(int delta) {
    std::vector<Item> order = FocusOrder();
    if (order.empty()) return;
    auto it = std::find(order.begin(), order.end(), s_focus);
    int i = it == order.end() ? (delta > 0 ? -1 : 0) : static_cast<int>(it - order.begin());
    int n = static_cast<int>(order.size());
    s_focus = order[((i + delta) % n + n) % n];
    s_showFocus = true;
    Render();
}

void OnKey(WPARAM vk) {
    bool shift = GetKeyState(VK_SHIFT) < 0;
    const Item* f = FindItem(s_focus);
    switch (vk) {
        case VK_ESCAPE: DestroyWindow(s_hwnd); return;
        case VK_TAB:
            if (GetKeyState(VK_CONTROL) < 0) SetPage(s_page + (shift ? -1 : 1));  // Ctrl+Tab 换页
            else MoveFocus(shift ? -1 : 1);
            return;
        case VK_UP: MoveFocus(-1); return;
        case VK_DOWN: MoveFocus(1); return;
        case VK_LEFT:
        case VK_RIGHT:
            if (f && f->kind == kSliderRow) {
                NudgeSlider(f->index, vk == VK_RIGHT ? 1 : -1);
                s_showFocus = true;
                Render();
            } else {
                MoveFocus(vk == VK_RIGHT ? 1 : -1);
            }
            return;
        case VK_DELETE:
        case VK_BACK:
            if (f && f->kind == kChip) RemoveExclude(f->index);
            if (f && f->kind == kHotkeyRow && s_v.hotkey[f->index]) {
                SetHotkey(f->index, 0);
                Render();
            }
            return;
        case VK_SPACE:
            if (f && f->kind != kSliderRow) Activate(*f);
            return;
        case VK_RETURN:
            // 焦点在按钮、开关这类上时按它；否则关掉窗口（改的都已经存了）
            if (f && f->kind != kSliderRow) Activate(*f);
            else DestroyWindow(s_hwnd);
            return;
    }
}

void SetHover(const Item& it) {
    if (it == s_hover) return;
    s_hover = it;
    Render();
}

void ReleaseAll() {
    EndCapture();
    Popup_EndPreview();
    KillTimer(s_hwnd, kTimerLive);
    s_framePosted = false;  // 排着的下一帧窗口关了就收不到了
    StopShot();
    s_excluded = false;
    s_moving = false;
    s_winDrag = false;
    s_content.clear();
    s_content.shrink_to_fit();
    s_contentValid = false;
    ReleaseCanvas();
    s_base.clear();
    s_base.shrink_to_fit();
    s_baseValid = false;
    s_glass = Glass();
    s_items.clear();
    s_texts.clear();
    s_cards.clear();
    s_fontFamily.reset();
}

LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        // 标题栏拖动窗口自己做（不用系统的 HTCAPTION）：新位置和按新位置画好的玻璃用同一次
        // UpdateLayeredWindow 贴上去，不会先挪窗口、下一帧玻璃才跟上
        case WM_ENTERSIZEMOVE:  // Alt+空格菜单里的“移动”还是走系统的
            s_moving = true;
            s_still = 0;
            if (s_excludeWorks >= 0) LiveTick();
            return 0;

        case WM_EXITSIZEMOVE:
            EndMove();
            return 0;

        case WM_MOVE: {
            RECT r;
            GetWindowRect(hwnd, &r);
            POINT pos = {r.left, r.top};
            if (pos.x == s_pos.x && pos.y == s_pos.y) return 0;
            s_pos = pos;
            if (s_moving && !s_winDrag) FollowMove();
            return 0;
        }

        case WM_SETTINGCHANGE:
            // 系统换了深浅色
            if (lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0 &&
                s_v.seg[kSegTheme] == kThemeSystem) {
                ApplyLook();
                Render();
            }
            break;

        case WM_ACTIVATE:
            // 每次回到前台都把键盘焦点放回来（DefWindowProc 本来会做），不然按键会变成系统键、按一下响一声
            if (LOWORD(wParam) != WA_INACTIVE && !HIWORD(wParam)) SetFocus(hwnd);
            if (LOWORD(wParam) == WA_INACTIVE && s_capture >= 0) {  // 切走了就不录了
                EndCapture();
                Render();
            }
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wParam == VK_F4 && msg == WM_SYSKEYDOWN) break;  // Alt+F4 照常关闭
            s_eatChar = false;
            if (s_capture >= 0) {
                CaptureKey(static_cast<UINT>(wParam), lParam);
                Render();
                return 0;
            }
            // Alt+字母等别当成 Enter/Esc；也不交给 DefWindowProc，不然单按 Alt / F10 松开后进了看不见的系统菜单模式。
            // Alt+空格照常弹窗口菜单（靠后面的 WM_SYSCHAR）
            if (msg == WM_SYSKEYDOWN) return 0;
            OnKey(wParam);
            return 0;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (s_capture < 0) break;
            CaptureKeyUp(static_cast<UINT>(wParam), lParam);
            Render();  // 松开了修饰键，框里跟着变
            return 0;

        // 录快捷键时在窗口上按的鼠标键（左键要配修饰键，单按左键是点别处、不录了）
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
            if (s_capture >= 0) {
                UINT vk = msg == WM_RBUTTONDOWN ? VK_RBUTTON
                          : msg == WM_MBUTTONDOWN ? VK_MBUTTON
                          : GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
                CaptureMouse(vk);
                Render();
            }
            return msg == WM_XBUTTONDOWN ? TRUE : 0;

        case WM_XBUTTONUP:
            return TRUE;  // 不让 DefWindowProc 再发“后退 / 前进”命令

        case WM_SYSCHAR:
            if (!s_eatChar) break;
            s_eatChar = false;
            return 0;

        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            if (s_winDrag) {
                POINT pt;
                GetCursorPos(&pt);
                POINT pos = {pt.x - s_dragOffset.x, pt.y - s_dragOffset.y};
                if (pos.x != s_pos.x || pos.y != s_pos.y) {
                    s_pos = pos;
                    FollowMove();
                }
                return 0;
            }
            if (s_drag >= 0) {
                int before = s_v.slider[s_drag];
                SetSliderFromX(s_drag, x);
                if (s_v.slider[s_drag] != before) Render();
                return 0;
            }
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            SetHover(HitTest(x, y));
            return 0;
        }

        case WM_MOUSELEAVE:
            SetHover({});
            return 0;

        case WM_LBUTTONDOWN: {
            if (s_capture >= 0 && CaptureMods()) {  // 录的时候按着修饰键点左键：录 Ctrl + 左键这类
                CaptureMouse(VK_LBUTTON);
                Render();
                return 0;
            }
            Item it = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (s_capture >= 0 && !(it.kind == kHotkeyRow && it.index == s_capture)) EndCapture();  // 点了别处就不录了
            if (it.kind == kNoItem &&
                RectF(s_panel.X, s_panel.Y, s_panel.Width, Px(kHeaderH))
                    .Contains(PointF(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam))))) {
                POINT pt;
                GetCursorPos(&pt);
                s_dragOffset = {pt.x - s_pos.x, pt.y - s_pos.y};
                s_winDrag = true;
                s_moving = true;
                s_still = 0;
                SetCapture(hwnd);
                if (s_excludeWorks >= 0) LiveTick();  // 后台开始不停地截窗口四周一大片
                return 0;
            }
            s_press = it;
            s_showFocus = false;
            if (it.kind != kNoItem) {
                s_focus = it;
                SetCapture(hwnd);  // 按下后拖到窗口外松开也能收到 WM_LBUTTONUP
            }
            // 滑块只有点在槽附近才跳过去，点标题和数值只是选中它
            if (it.kind == kSliderRow && std::fabs(GET_Y_LPARAM(lParam) - s_sliderGeom[it.index].cy) <= Px(12)) {
                s_drag = it.index;
                SetSliderFromX(s_drag, GET_X_LPARAM(lParam));
            }
            Render();
            return 0;
        }

        case WM_LBUTTONUP: {
            if (s_winDrag) {
                s_winDrag = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                EndMove();
                return 0;
            }
            Item it = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            Item pressed = s_press;
            bool dragged = s_drag >= 0;
            int draggedSlider = s_drag;
            s_press = {};
            s_drag = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (PreviewsPopup(draggedSlider)) PreviewPopup(false);  // 松开了：预览过一会儿收起
            if (dragged || pressed.kind == kSliderRow) {
                Render();
                return 0;
            }
            if (pressed.kind != kNoItem && it == pressed) Activate(it);
            else Render();
            return 0;
        }

        case WM_CAPTURECHANGED:
            if (s_winDrag && reinterpret_cast<HWND>(lParam) != hwnd) {
                s_winDrag = false;
                EndMove();
                return 0;
            }
            if ((s_drag >= 0 || s_press.kind != kNoItem) && reinterpret_cast<HWND>(lParam) != hwnd) {
                if (PreviewsPopup(s_drag)) PreviewPopup(false);
                s_drag = -1;
                s_press = {};
                Render();
            }
            return 0;

        case WM_MOUSEWHEEL: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            Item it = HitTest(pt.x, pt.y);
            // 触控板、高精度滚轮一次只给零点几格，攒够一格再动
            static int rest = 0;
            rest += GET_WHEEL_DELTA_WPARAM(wParam);
            int steps = rest / WHEEL_DELTA;
            rest -= steps * WHEEL_DELTA;
            if (it.kind == kSliderRow && steps) {
                NudgeSlider(it.index, steps);
                Render();
            }
            return 0;
        }

        case WM_APP_FRAME:
            StepAnimation();
            return 0;

        case WM_TIMER:
            if (wParam == kTimerLive) LiveTick();
            else if (wParam == kTimerSave && s_drag < 0) SaveNow();
            return 0;

        case WM_APP_SHOT:
            OnShot(reinterpret_cast<ShotJob*>(lParam));
            return 0;

        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            s_pos = {suggested->left, suggested->top};
            LayoutFor(MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST));
            SetWindowPos(hwnd, nullptr, s_pos.x, s_pos.y, s_width, s_height, SWP_NOZORDER | SWP_NOACTIVATE);
            CaptureBehind();
            Render();
            return 0;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) DestroyWindow(hwnd);
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            s_drag = -1;
            SaveNow();  // 还没到时候存的（比如拖着滑块关了窗口）
            SetUiEnglish(LanguageIsEnglish(g_settings.language));
            ReleaseAll();
            s_hwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 点了、按了键以后看看改了什么，改了就存
LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    LRESULT result = HandleMessage(hwnd, msg, wParam, lParam);
    bool input = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
                 msg == WM_COMMAND || msg == WM_CAPTURECHANGED;
    if (input) ScheduleSave();
    return result;
}

}  // namespace

HWND SettingsDialog_Hwnd() { return s_hwnd; }

void SettingsDialog_SettingsChanged() {
    if (s_hwnd) Reload(kMerge);
}

// 插拔了显示器、改了分辨率或缩放：系统可能把窗口挪到了别的屏幕上，按它现在所在的屏幕重新排，别跑出屏幕
void SettingsDialog_DisplayChanged() {
    if (!s_hwnd) return;
    RECT r;
    if (GetWindowRect(s_hwnd, &r)) s_pos = {r.left, r.top};
    HMONITOR monitor = MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTONEAREST);
    s_screen = monitor;
    s_screenKey = MonitorKey(monitor);
    ShowPlace();
    s_hover = s_press = {};
    Relayout(true);
    if (!FindItem(s_focus)) s_focus = {};  // 比如拔掉的那块屏幕的小胶囊
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    const RECT& work = mi.rcWork;
    POINT pos = {std::clamp<LONG>(s_pos.x, work.left, std::max(work.left, work.right - s_width)),
                 std::clamp<LONG>(s_pos.y, work.top, std::max(work.top, work.bottom - s_height))};
    if (pos.x != s_pos.x || pos.y != s_pos.y) {
        s_pos = pos;
        SetWindowPos(s_hwnd, nullptr, s_pos.x, s_pos.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        CaptureBehind();
        Render();
    }
}

void SettingsDialog_Show() {
    if (s_hwnd) {
        ShowWindow(s_hwnd, SW_RESTORE);
        ForceForeground(s_hwnd);
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = g_instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        wc.hIcon = reinterpret_cast<HICON>(SendMessageW(g_mainWnd, WM_GETICON, ICON_BIG, 0));
        wc.hIconSm = reinterpret_cast<HICON>(SendMessageW(g_mainWnd, WM_GETICON, ICON_SMALL, 0));
        RegisterClassExW(&wc);
        registered = true;
    }

    for (const wchar_t* name : {L"Microsoft YaHei UI", L"Segoe UI"}) {
        s_fontFamily = std::make_unique<FontFamily>(name);
        if (s_fontFamily->GetLastStatus() == Ok && s_fontFamily->IsAvailable()) break;
        s_fontFamily.reset();
    }
    if (!s_fontFamily) s_fontFamily.reset(FontFamily::GenericSansSerif()->Clone());

    s_initial = ValuesFromSettings();
    for (auto& error : s_hotkeyError) error.clear();
    s_presets = LookPresets();
    s_fileNote.clear();
    s_v = s_initial;
    s_hover = s_press = s_focus = {};
    s_showFocus = false;
    s_drag = -1;
    s_still = 0;
    s_capture = -1;
    s_captureLone = 0;

    // 居中到鼠标所在的显示器
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    s_screen = mon;
    s_screenKey = MonitorKey(mon);
    ShowPlace();
    ShowSide(ModuleSideOf(s_v.seg[kSegEdge]));  // 先显示现在弹的那条边的
    for (int i = 0; i < kToggleCount; ++i) s_knob[i] = s_v.on[i] ? 1.0f : 0.0f;
    LayoutFor(mon);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const RECT& work = mi.rcWork;
    s_pos = {work.left + (work.right - work.left - s_width) / 2, work.top + (work.bottom - work.top - s_height) / 2};

    s_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_APPWINDOW, kClassName, Tr(L"TaskbarPopup 设置"),
                             WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, s_pos.x, s_pos.y, s_width, s_height, nullptr, nullptr,
                             g_instance, nullptr);
    if (!s_hwnd) {
        s_fontFamily.reset();
        return;
    }
    // 淡入自己画；系统的窗口动画会和它叠在一起
    BOOL disable = TRUE;
    DwmSetWindowAttribute(s_hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));

    ApplyLook();
    CaptureBehind();  // 还没显示：直接截
    s_openedAt = NowMs();
    s_alpha = 0;
    Render();
    ShowWindow(s_hwnd, SW_SHOW);
    ForceForeground(s_hwnd);
    SetFocus(s_hwnd);
    StartAnimation();
    SetTimer(s_hwnd, kTimerLive, static_cast<UINT>(kFadeMs) + 100, nullptr);  // 淡入完了再开始跟着背景刷新
}

}  // namespace app
