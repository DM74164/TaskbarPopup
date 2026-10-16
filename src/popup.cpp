// 迷你任务栏：液态玻璃质感的分层窗口，从屏幕底部中央弹上来。
// 先按任务栏上的顺序列出固定的应用（已经打开的换成它的窗口），再列出其余打开的窗口。
// 单击切换窗口（再点当前窗口则最小化）或启动应用，Shift+单击再开一个，中键关闭窗口，
// 右键弹出和系统任务栏差不多的菜单（开始按钮上是 Win+X 菜单），方向键 + 回车选择，Esc 或点到别处收起。直接打字筛选窗口和应用（也搜开始菜单里的所有应用）。
// 鼠标停在窗口图标上时在上方显示它的实时缩略图。
// 下面一行是音量和亮度调节条：拖动或在上面滚滚轮调节，点喇叭静音（可以在设置里关掉）。
// 这一行最右边是 Wi-Fi 和蓝牙开关：点一下开关，右键弹出系统自己的网络列表、蓝牙设备列表（也可以在设置里关掉）。
// 时钟左边是系统托盘“隐藏的图标”里的程序（可以在设置里关掉、选两行小图标还是一行大图标），左键右键和在托盘里点一样，
// 右键时迷你任务栏留着。两行小图标是 2×2 一页，鼠标停在上面按左右方向键或者滚滚轮翻页；一行大图标放不下时同样滚着看。
// 任务栏藏着时短按 Win：迷你任务栏只留一行格子贴着屏幕底边弹上来，弹完再打开开始菜单，开始菜单关了一起收起。
#include "common.h"

#include <climits>
#include <cmath>
#include <cwchar>

using namespace Gdiplus;

namespace app {
namespace {

constexpr wchar_t kClassName[] = L"TaskbarPopupMini";
constexpr UINT_PTR kTimerClock = 2;
constexpr UINT_PTR kTimerThumb = 3;  // 缩略图延迟出现 / 消失
constexpr UINT kThumbShowMs = 400;
constexpr UINT kThumbHideMs = 120;   // 鼠标在两个图标之间的缝里时别闪
constexpr size_t kMaxAppMatches = 12;
constexpr UINT WM_APP_FRAME = WM_APP + 20;  // 动画的下一帧
constexpr UINT_PTR kTimerRefocus = 4;  // 刚弹出就失去了前台：稍等一下再抢回来
constexpr UINT kRefocusDelayMs = 100;
constexpr double kRefocusMs = 500;     // 弹出后多久之内失去前台算“刚弹出”
constexpr double kShowMs = 320.0 * TP_ANIM_SCALE;
constexpr double kStartShowMs = 180.0 * TP_ANIM_SCALE;  // 任务栏藏着时短按 Win：弹得快一点，
constexpr double kStartAt = 0.7;                        // 到位（这时已经冲过头一点）就打开开始菜单
constexpr double kHideMs = 170.0 * TP_ANIM_SCALE;
constexpr double kPillMs = 160.0 * TP_ANIM_SCALE;
constexpr UINT_PTR kTimerPreview = 5;  // 预览：松开滑块后过一会儿收起
constexpr UINT kPreviewHideMs = 1200;
constexpr UINT_PTR kTimerTray = 6;     // 每隔一会儿在后台重新读一遍托盘图标，弹出时直接用
constexpr UINT kTrayRefreshMs = 15000;
constexpr UINT_PTR kTimerStart = 7;    // 开始菜单开着时：看它关了没有
constexpr UINT kStartWatchMs = 100;
constexpr DWORD kStartWaitMs = 2000;   // 补发 Win 以后等开始菜单出来的时间
constexpr int kMaxTrayRow = 8;         // 一行大图标时托盘图标最多同时显示几个，再多用滚轮或左右方向键滚着看
constexpr double kFlipMs = 180.0 * TP_ANIM_SCALE;  // 托盘图标翻页
constexpr UINT_PTR kTimerTrayMenu = 8;  // 右键了托盘图标：看系统托盘弹出的菜单关了没有
constexpr UINT kTrayWatchMs = 100;
constexpr DWORD kTrayIdleMs = 400;      // 点完以后等程序的菜单出来的时间
constexpr DWORD kTrayBusyMaxMs = 5000;  // 在系统托盘里找、点最多等这么久

enum class Anim { None, Showing, Hiding };

// 玻璃上的配色：深色玻璃配白字，浅色玻璃配深色字
struct Palette {
    Color text, subtle, separator, pillTop, pillBottom, pillRim, running, accent, textShadow;
    Color track, thumb, thumbRim;  // 调节条的槽、滑块、滑块描边
};

const Palette kDark = {Color(240, 255, 255, 255), Color(165, 255, 255, 255), Color(55, 255, 255, 255),
                       Color(62, 255, 255, 255),  Color(22, 255, 255, 255),  Color(120, 255, 255, 255),
                       Color(190, 255, 255, 255), Color(255, 96, 205, 255),  Color(90, 0, 0, 0),
                       Color(80, 255, 255, 255),  Color(255, 58, 58, 62),    Color(80, 255, 255, 255)};
const Palette kLight = {Color(235, 18, 18, 22), Color(165, 18, 18, 22),    Color(45, 0, 0, 0),
                        Color(34, 0, 0, 0),     Color(16, 0, 0, 0),        Color(230, 255, 255, 255),
                        Color(150, 20, 20, 24), Color(255, 0, 95, 184),    Color(120, 255, 255, 255),
                        Color(55, 0, 0, 0),     Color(255, 255, 255, 255), Color(50, 0, 0, 0)};

// 一个格子：打开的窗口，或者固定了但还没运行的应用
struct Tile {
    std::wstring title;
    std::shared_ptr<Bitmap> icon;
    HWND hwnd = nullptr;  // 没运行的固定应用为 nullptr
    bool active = false;
    // 属于某个固定应用时才有
    std::wstring launch;
    std::wstring pinName;
    std::shared_ptr<Bitmap> pinIcon;
    std::wstring search;  // 打字筛选时比较的小写文字：标题、应用名、程序文件名
};

// 以下尺寸都是物理像素，按目标显示器的缩放比例计算
struct Layout {
    float margin, pad, tile, gap, titleH, sep, clockW, emptyW, radius, rowGap, sliderH, minPanelW;
    float panelW, panelH, itemsX, itemsW, tilesX, clockX, tileY, sliderY;
    float trayX, trayW, cellW, cellH;  // 托盘：堆叠时 2×2 一组的小格子，横排时一行和窗口格子一样高
    float bottomGap;                   // 面板底边离屏幕底边（或者露着的任务栏）多远
    float panelX, panelY;              // 面板在窗口里的位置
    float radioW, radioH, radioGap;    // Wi-Fi 和蓝牙按钮的大小、间隔
    // 竖排（弹在屏幕左右两边）时：一列格子的左边、各段的顶边。这时 itemsW、trayW 是竖着的长度，
    // 窗口在面板里侧多留 bubbleW 宽，放显示标题的小牌子
    float colX, startY, itemsY, tilesY, trayY, clockY, clockH, bubbleW;
};

// 调节条：0 是音量，1 是亮度
struct Slider {
    RectF icon;      // 图标（喇叭可以点）：横排在左边，竖排在下边
    float a0, a1;    // 槽的两端：a0 是 0%，a1 是 100%（横排是 x，竖排是 y，下边是 0%）
    float c;         // 槽的中线（横排是 y，竖排是 x）
};

// 鼠标下的调节条部件
enum Control { kNone, kVolumeIcon, kVolumeTrack, kBrightIcon, kBrightTrack };

int SliderOf(int control) { return control == kVolumeIcon || control == kVolumeTrack ? 0 : 1; }

HWND s_hwnd = nullptr;
std::vector<Tile> s_items;     // 现在显示的格子（打字筛选后的）
std::vector<Tile> s_allItems;  // 弹出时列出的全部格子
std::wstring s_filter;         // 打的字
float s_lockedItemsW = 0;      // 筛选期间格子区宽度不变（面板不跟着变窄变宽）；0 = 没锁
bool s_keyNav = false;         // 最近是用键盘选的（选中项也显示缩略图）
bool s_menuOpen = false;
POINT s_lastMouse = {INT_MIN, INT_MIN};  // 上一次 WM_MOUSEMOVE 的位置：窗口出现、移动时系统会补发一条位置没变的
HWND s_thumbWant = nullptr;    // 想显示缩略图的窗口
HWND s_thumbShown = nullptr;   // 正显示着缩略图的窗口
int s_thumbIdx = -1;
HWND s_prevForeground = nullptr;
RECT s_bounds = {};  // 弹窗摆放的范围：任务栏露着时是工作区，藏起来时是整块屏幕
float s_scale = 1.0f;
Layout s_L = {};
int s_width = 0, s_height = 0;
POINT s_pos = {};   // 窗口在屏幕上的位置
int s_first = 0;    // 横向滚动：第一个可见的格子序号
int s_visible = 0;  // 一屏能放下几个
int s_hover = -1;   // 鼠标下的格子：-1 无，0 开始按钮，i+1 第 i 个格子
int s_sel = 0;      // 选中的格子（键盘焦点），编号同上
bool s_open = false;
bool s_preview = false;      // 设置窗口里调大小时的预览：不抢前台、鼠标点不到，过一会儿自己收起
int s_previewPercent = 100;  // 预览用的大小，百分比
int s_previewAlign = kAlignSystem;  // 预览用的对齐，见 PopupAlign
int s_previewEdge = kEdgeBottom;    // 预览用的边，见 PopupEdge
bool s_previewFrosted = false;      // 预览用的玻璃，见 Popup_PreviewGlass
bool s_previewLight = false;
int s_previewTransparency = kDefaultGlassTransparency;
int s_edge = kEdgeBottom;           // 这次弹在哪条边（弹出时按设置定下来）
double s_shownAt = 0;      // 这次弹出的时间
bool s_refocused = false;  // 这次弹出后已经抢回过一次前台
Anim s_anim = Anim::None;
double s_animStart = 0;
float s_lastSlide = 0;  // 最近一帧的位置和透明度：弹出到一半就收起时从这里接着走
BYTE s_lastAlpha = 255;
float s_hideFromSlide = 0;
BYTE s_hideFromAlpha = 255;
bool s_framePosted = false;
std::unique_ptr<FontFamily> s_fontFamily;

// 音量和亮度
bool s_levels = true;  // 这次弹出显不显示调节条（弹出时按设置定下来）
Slider s_sliders[2];
HMONITOR s_monitor = nullptr;  // 弹窗所在的屏幕，亮度调的是它
bool s_hasVolume = false;      // 有没有可用的播放设备
float s_volume = 0;
bool s_muted = false;
int s_brightness = -1;  // 0~100；-1 = 这块屏不支持；-2 = 还在读
std::vector<std::pair<HMONITOR, int>> s_knownBrightness;  // 读到过的亮度，下次弹出时先显示它
bool s_brightnessEdited = false;  // 这次弹出后调过亮度：之后才到的查询结果是旧值，不用
int s_hoverControl = kNone;
int s_drag = kNone;  // 正在拖的槽
int s_wheelRest = 0;  // 触控板、高精度滚轮一次只给零点几格，攒够一格再动

// 托盘里隐藏的图标（弹出时按设置定下来）
std::vector<TrayApp> s_tray;
int s_trayFirst = 0;           // 一行大图标放不下时：第一个显示的是第几个
int s_trayStyle = kTrayStacked;
int s_hoverTray = -1;          // 鼠标下的托盘格子
bool s_overTray = false;       // 鼠标在托盘图标那块上（堆叠时没放图标的空格也算）
int s_trayPage = 0;            // 堆叠时显示第几页（一页 2×2 四个）
int s_flipFrom = -1;           // 翻页动画：从哪一页滑走，-1 = 没在翻
int s_flipDir = 1;
double s_flipStart = 0;
bool s_trayMode = false;       // 右键了托盘图标：系统托盘里程序自己的菜单开着（或者正在弹），迷你任务栏不收起
DWORD s_trayModeAt = 0;
DWORD s_trayIdleAt = 0;        // 点完了、看不到菜单的时间；0 = 还在点或者菜单开着
int s_trayHot = -1;            // 右键的那一格，菜单开着时一直亮着
bool s_trayRefocused = false;  // 菜单关了以后已经试过把前台拿回来

// Wi-Fi 和蓝牙开关（弹出时按设置定下来；这台电脑上没有的不显示）
int s_radioCount = 0;              // 这次显示几个按钮
int s_radioIds[kRadioCount] = {};  // 第 k 个按钮是哪个开关（RadioId）
RectF s_radioRects[kRadioCount];
RadioInfo s_radio[kRadioCount];    // 最近读到的样子
int s_hoverRadio = -1;             // 鼠标下的按钮（第几个）
int s_radioFailed = 0;             // 切换失败的开关（1 << RadioId）：鼠标离开那个按钮之前标题一直说失败了

// 任务栏藏着时短按 Win：迷你任务栏弹完再打开开始菜单，开始菜单开着时它一直在
bool s_compact = false;       // 这次弹出只有一行格子、贴着屏幕底边，给开始菜单让地方
bool s_startPending = false;  // 弹出动画放完就打开开始菜单
bool s_startMode = false;     // 开始菜单打开了（或者正在打开），迷你任务栏跟着它收起
bool s_startSeen = false;     // 开始菜单到过前台
DWORD s_startAt = 0;          // 补发 Win 的时间
double s_startClosedAt = -1e9;  // 开始菜单开着时点到迷你任务栏上（开始菜单自己关了）的时间

// 玻璃和画布
Glass s_glass;
std::vector<DWORD> s_base;  // 静止时的阴影 + 玻璃，悬停、时钟等重绘直接复用
bool s_baseValid = false;
HDC s_canvasDC = nullptr;
HBITMAP s_canvas = nullptr;
HGDIOBJ s_canvasOld = nullptr;
DWORD* s_bits = nullptr;
int s_canvasW = 0, s_canvasH = 0;

// 高亮块：从一个格子滑到另一个格子
int s_pillIndex = -1;
float s_pillX = 0, s_pillFrom = 0, s_pillTo = 0;
double s_pillStart = 0;

float Px(float dip) { return dip * s_scale; }

// 显示器的缩放比例再乘上设置里的迷你任务栏大小
float PopupScale(HMONITOR monitor) {
    return MonitorScale(monitor) * (s_preview ? s_previewPercent : g_settings.popupScale) / 100.0f;
}

const Palette& Colors() { return s_glass.Light() ? kLight : kDark; }

// 弹在屏幕左右两边：竖着排，从侧边滑出来
bool Vertical() { return s_edge != kEdgeBottom; }

void ComputeLayout() {
    Layout& L = s_L;
    bool vertical = Vertical();
    L.margin = Px(24);  // 给阴影留位置，也是面板离屏幕边的距离
    L.pad = s_compact ? Px(4) : Px(8);
    L.tile = Px(48);
    L.gap = Px(4);
    L.titleH = s_compact || vertical ? 0 : Px(24);  // 竖排时标题显示在面板旁边的小牌子上
    L.sep = Px(9);
    L.clockW = Px(72);
    L.clockH = Px(40);
    L.emptyW = vertical ? Px(96) : Px(130);
    L.radius = Px(22);
    // Wi-Fi 和蓝牙按钮：横排时在调节条那一行的最右边（没有调节条时这一行只有它们，居中），竖排时在最下面一个个往下排
    L.radioW = Px(40);
    L.radioH = Px(28);
    L.radioGap = Px(6);
    float radiosW = s_radioCount > 0 ? s_radioCount * L.radioW + (s_radioCount - 1) * L.radioGap : 0;
    bool row = s_levels || (s_radioCount > 0 && !vertical);
    L.rowGap = row ? (vertical ? Px(4) : Px(2)) : 0;
    L.sliderH = row ? (vertical ? Px(128) : Px(34)) : 0;
    L.minPanelW = s_levels ? Px(380) : Px(260);  // 两根调节条要有地方拖；没有调节条时标题也要放得下
    if (s_levels && radiosW > 0) L.minPanelW += radiosW + Px(12);
    // 开始菜单模式：面板差不多贴着屏幕底边，和系统任务栏一样高，开始菜单才不会盖住它
    L.bottomGap = s_compact ? Px(4) : L.margin;
    L.bubbleW = vertical ? Px(280) : 0;

    // 托盘图标：一行大图标时最多同时显示 kMaxTrayRow 个，多的滚着看；堆叠时 2×2 一页，和窗口格子一样大，多的翻页看。
    // 竖排时“一行大图标”样式的格子竖着一个个往下排
    int trayCount = static_cast<int>(s_tray.size());
    if (s_trayStyle == kTrayRow) {
        L.cellW = vertical ? L.tile : Px(40);
        L.cellH = vertical ? Px(40) : L.tile;
        L.trayW = std::min(trayCount, kMaxTrayRow) * (vertical ? L.cellH : L.cellW);
    } else {
        L.cellW = L.cellH = L.tile / 2;
        L.trayW = trayCount > 0 ? L.tile : 0;
    }
    float traySep = trayCount > 0 ? L.sep : 0;
    int n = static_cast<int>(s_items.size());

    if (vertical) {
        // 从上到下：开始按钮、格子、托盘图标、时钟、两根竖的调节条、Wi-Fi 和蓝牙按钮
        float radiosH = s_radioCount > 0 ? Px(8) + s_radioCount * L.radioH + (s_radioCount - 1) * L.radioGap : 0;
        float others = 2 * L.pad + L.tile + 2 * L.sep + traySep + L.trayW + L.clockH + L.rowGap + L.sliderH + radiosH;
        float avail = (s_bounds.bottom - s_bounds.top) * 0.9f - 2 * L.margin - others;
        s_visible = std::min(n, std::max(1, static_cast<int>((avail + L.gap) / (L.tile + L.gap))));
        if (s_lockedItemsW > 0)
            s_visible = std::min(n, std::max(1, static_cast<int>((s_lockedItemsW + L.gap) / (L.tile + L.gap))));
        float tilesH = n == 0 ? L.emptyW : s_visible * (L.tile + L.gap) - L.gap;
        L.itemsW = s_lockedItemsW > 0 ? s_lockedItemsW : tilesH;
        L.panelW = 2 * L.pad + L.tile;
        L.panelH = others + L.itemsW;
        L.panelX = s_edge == kEdgeLeft ? L.margin : L.bubbleW + L.margin;
        L.panelY = L.margin;
        L.colX = L.panelX + L.pad;
        L.startY = L.panelY + L.pad;
        L.itemsY = L.startY + L.tile + L.sep;
        L.tilesY = L.itemsY + (L.itemsW - tilesH) / 2;
        L.trayY = L.itemsY + L.itemsW + L.sep;
        L.clockY = L.trayY + L.trayW + traySep;
        L.sliderY = L.clockY + L.clockH + L.rowGap;
        // 音量在左半边，亮度在右半边，图标在下面，往上拖变大
        float icon = Px(24), half = L.tile / 2, iconY = L.sliderY + L.sliderH - icon;
        auto make = [&](float cx) {
            return Slider{RectF(cx - icon / 2, iconY, icon, icon), iconY - Px(10), L.sliderY + Px(10), cx};
        };
        s_sliders[0] = make(L.colX + half / 2);
        s_sliders[1] = make(L.colX + half + half / 2);
        for (int k = 0; k < s_radioCount; ++k)
            s_radioRects[k] = RectF(L.colX + (L.tile - L.radioW) / 2, L.sliderY + L.sliderH + Px(8) + k * (L.radioH + L.radioGap),
                                    L.radioW, L.radioH);
        s_width = static_cast<int>(std::ceil(L.panelW + 2 * L.margin + L.bubbleW));
        s_height = static_cast<int>(std::ceil(L.panelH + 2 * L.margin));
        return;
    }

    float fixed = 2 * L.margin + 2 * L.pad + L.tile + 2 * L.sep + traySep + L.trayW + L.clockW;
    float avail = (s_bounds.right - s_bounds.left) * 0.85f - fixed;
    int maxFit = std::max(1, static_cast<int>((avail + L.gap) / (L.tile + L.gap)));
    s_visible = std::min(n, maxFit);

    // 格子少的时候面板按最小宽度来，格子在中间那段里居中
    float others = 2 * L.pad + L.tile + 2 * L.sep + traySep + L.trayW + L.clockW;
    if (s_lockedItemsW > 0) {
        int fit = std::max(1, static_cast<int>((s_lockedItemsW + L.gap) / (L.tile + L.gap)));
        s_visible = std::min(n, fit);
    }
    float tilesW = n == 0 ? L.emptyW : s_visible * (L.tile + L.gap) - L.gap;
    L.itemsW = s_lockedItemsW > 0 ? s_lockedItemsW : std::max(tilesW, L.minPanelW - others);
    L.panelW = others + L.itemsW;
    L.panelH = 2 * L.pad + L.titleH + L.tile + L.rowGap + L.sliderH;
    L.panelX = L.panelY = L.margin;
    L.itemsX = L.margin + L.pad + L.tile + L.sep;
    L.tilesX = L.itemsX + (L.itemsW - tilesW) / 2;
    L.trayX = L.itemsX + L.itemsW + L.sep;
    L.clockX = L.trayX + L.trayW + traySep;
    L.tileY = L.margin + L.pad + L.titleH;
    L.sliderY = L.tileY + L.tile + L.rowGap;

    // 音量在左半边，亮度在右半边，Wi-Fi 和蓝牙按钮在最右边
    float left = L.margin + L.pad + Px(2), right = L.margin + L.panelW - L.pad - Px(2);
    float cy = L.sliderY + L.sliderH / 2, icon = Px(30);
    float radioX = s_levels ? right - radiosW : L.margin + (L.panelW - radiosW) / 2;
    for (int k = 0; k < s_radioCount; ++k)
        s_radioRects[k] = RectF(radioX + k * (L.radioW + L.radioGap), cy - L.radioH / 2, L.radioW, L.radioH);
    if (radiosW > 0) right -= radiosW + Px(12);
    float mid = (left + right) / 2;
    auto make = [&](float x0, float x1) {
        return Slider{RectF(x0, cy - icon / 2, icon, icon), x0 + icon + Px(10), x1 - Px(10), cy};
    };
    s_sliders[0] = make(left, mid - Px(8));
    s_sliders[1] = make(mid + Px(8), right);

    s_width = static_cast<int>(std::ceil(L.panelW + 2 * L.margin));
    s_height = static_cast<int>(std::ceil(L.panelH + L.margin + L.bottomGap));
}

bool IsWindows11() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    OSVERSIONINFOW ver = {sizeof(ver)};
    return !(rtlGetVersion && rtlGetVersion(&ver) == 0 && ver.dwBuildNumber < 22000);
}

// 系统任务栏的图标是不是居中：Win11 看设置里的“任务栏对齐方式”（没改过是居中），Win10 一直靠左
bool SystemTaskbarCentered() {
    if (!IsWindows11()) return false;
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"TaskbarAl",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return true;
    return value != 0;
}

// 贴着底边（屏幕底边，或者露着的任务栏的上沿），按设置居中、靠左或靠右，靠边时面板离屏幕侧边和离底边一样远
// （窗口四周留的阴影位置）。竖排时贴着屏幕左边或右边，上下居中
void UpdatePlacement() {
    if (Vertical()) {
        int x = s_edge == kEdgeLeft ? s_bounds.left : s_bounds.right - s_width;
        int y = std::max<int>(s_bounds.top, s_bounds.top + (s_bounds.bottom - s_bounds.top - s_height) / 2);
        s_pos = {x, y};
        s_baseValid = false;
        return;
    }
    int align = s_preview ? s_previewAlign : g_settings.popupAlign;
    if (align == kAlignSystem) align = SystemTaskbarCentered() ? kAlignCenter : kAlignLeft;
    int x = s_bounds.left + (s_bounds.right - s_bounds.left - s_width) / 2;
    if (align == kAlignLeft) x = s_bounds.left;
    else if (align == kAlignRight) x = s_bounds.right - s_width;
    s_pos = {x, s_bounds.bottom - s_height};
    s_baseValid = false;
}

float SlideDistance() { return Vertical() ? s_L.panelW + s_L.margin + Px(8) : s_L.panelH + s_L.bottomGap + Px(8); }

int ItemCount() { return static_cast<int>(s_items.size()); }

bool IsTileVisible(int index) {
    if (index == 0) return true;
    int i = index - 1;
    return i >= s_first && i < s_first + s_visible;
}

RectF TileRect(int index) {
    const Layout& L = s_L;
    int slot = index - 1 - s_first;
    if (Vertical()) return RectF(L.colX, index == 0 ? L.startY : L.tilesY + slot * (L.tile + L.gap), L.tile, L.tile);
    if (index == 0) return RectF(L.margin + L.pad, L.tileY, L.tile, L.tile);
    return RectF(L.tilesX + slot * (L.tile + L.gap), L.tileY, L.tile, L.tile);
}

// 沿着格子排列方向的坐标：横排是 x，竖排是 y
float Along(const RectF& r) { return Vertical() ? r.Y : r.X; }

int HitTest(int x, int y) {
    for (int idx = 0; idx <= ItemCount(); ++idx) {
        if (!IsTileVisible(idx)) continue;
        RectF r = TileRect(idx);
        if (r.Contains(static_cast<REAL>(x), static_cast<REAL>(y))) return idx;
    }
    return -1;
}

int ControlAt(int x, int y) {
    if (!s_levels) return kNone;
    REAL fx = static_cast<REAL>(x), fy = static_cast<REAL>(y);
    for (int i = 0; i < 2; ++i) {
        const Slider& sl = s_sliders[i];
        if (sl.icon.Contains(fx, fy)) return i == 0 ? kVolumeIcon : kBrightIcon;
        RectF track = Vertical() ? RectF(sl.c - Px(12), sl.a1 - Px(10), Px(24), sl.a0 - sl.a1 + Px(20))
                                 : RectF(sl.a0 - Px(10), sl.c - Px(15), sl.a1 - sl.a0 + Px(20), Px(30));
        if (track.Contains(fx, fy)) return i == 0 ? kVolumeTrack : kBrightTrack;
    }
    return kNone;
}

// 鼠标下的 Wi-Fi / 蓝牙按钮（第几个），没有为 -1
int RadioAt(int x, int y) {
    for (int k = 0; k < s_radioCount; ++k)
        if (s_radioRects[k].Contains(static_cast<REAL>(x), static_cast<REAL>(y))) return k;
    return -1;
}

// ---- 托盘图标 ----

int TrayCells() { return static_cast<int>(s_tray.size()); }

// 一行大图标时同时显示几个
int TrayRowVisible() { return std::min(TrayCells(), kMaxTrayRow); }

int TrayPages() { return s_trayStyle == kTrayRow ? 1 : (static_cast<int>(s_tray.size()) + 3) / 4; }

// 托盘图标多到要翻页（堆叠）或者滚动（一行大图标）
bool TrayScrolls() { return s_trayStyle == kTrayRow ? TrayCells() > kMaxTrayRow : TrayPages() > 1; }

// 托盘图标那一段的左上角
PointF TrayOrigin() { return Vertical() ? PointF(s_L.colX, s_L.trayY) : PointF(s_L.trayX, s_L.tileY); }

// 堆叠时的那一页（2×2，和窗口格子一样大）
RectF TrayGroupRect() { return RectF(TrayOrigin(), SizeF(s_L.tile, s_L.tile)); }

// 第 i 格。堆叠时第 i / 4 页，每页先上后下、从左往右；一行大图标时从 s_trayFirst 个开始排，竖排时一格格往下排
RectF TrayCellRect(int i) {
    const Layout& L = s_L;
    PointF o = TrayOrigin();
    if (s_trayStyle == kTrayRow) {
        int k = i - s_trayFirst;
        return Vertical() ? RectF(o.X, o.Y + k * L.cellH, L.cellW, L.cellH) : RectF(o.X + k * L.cellW, o.Y, L.cellW, L.cellH);
    }
    int k = i % 4;
    return RectF(o.X + (k % 2) * L.cellW, o.Y + (k / 2) * L.cellH, L.cellW, L.cellH);
}

bool TrayOnPage(int i) {
    if (s_trayStyle == kTrayRow) return i >= s_trayFirst && i < s_trayFirst + TrayRowVisible();
    return i / 4 == s_trayPage;
}

int TrayAt(int x, int y) {
    REAL fx = static_cast<REAL>(x), fy = static_cast<REAL>(y);
    for (int i = 0; i < TrayCells(); ++i)
        if (TrayOnPage(i) && TrayCellRect(i).Contains(fx, fy)) return i;
    return -1;
}

bool OverTray(int x, int y) {
    if (s_L.trayW <= 0) return false;
    PointF o = TrayOrigin();
    RectF area = Vertical() ? RectF(o.X, o.Y, s_L.tile, s_L.trayW) : RectF(o.X, o.Y, s_L.trayW, s_L.tile);
    return area.Contains(static_cast<REAL>(x), static_cast<REAL>(y));
}

// 鼠标所指的托盘格子的标题；堆叠又不止一页时带上第几页
std::wstring TrayTitle(int i) {
    std::wstring name = i >= 0 && i < TrayCells() ? s_tray[i].name : L"";
    if (TrayPages() <= 1) return name;
    if (name.empty()) return TrF(L"第 %d/%d 页", s_trayPage + 1, TrayPages());
    return TrF(L"%ls（第 %d/%d 页）", name.c_str(), s_trayPage + 1, TrayPages());
}

double TrayFlipT() { return s_flipFrom < 0 ? 1.0 : std::min(1.0, (NowMs() - s_flipStart) / kFlipMs); }
bool TrayFlipping() { return TrayFlipT() < 1.0; }

void EnsureVisible(int index) {
    if (index <= 0) return;
    int i = index - 1;
    if (i < s_first) s_first = i;
    else if (i >= s_first + s_visible) s_first = i - s_visible + 1;
}

// ---- 高亮块 ----

// 跟着鼠标所指 / 键盘选中的格子走；刚出现时直接到位，之后在格子之间滑动
void SyncPill() {
    int shown = s_hover >= 0 ? s_hover : s_sel;
    if (shown < 0 || shown > ItemCount() || !IsTileVisible(shown)) {
        s_pillIndex = -1;
        return;
    }
    float x = Along(TileRect(shown));
    if (s_pillIndex < 0) {
        s_pillX = s_pillFrom = s_pillTo = x;
    } else if (x != s_pillTo) {
        s_pillFrom = s_pillX;
        s_pillTo = x;
        s_pillStart = NowMs();
    }
    s_pillIndex = shown;
}

bool PillMoving() { return s_pillIndex >= 0 && s_pillX != s_pillTo; }

void AdvancePill(double now) {
    if (!PillMoving()) return;
    double t = std::min(1.0, (now - s_pillStart) / kPillMs);
    double e = 1 - std::pow(1 - t, 3.0);
    s_pillX = t >= 1 ? s_pillTo : static_cast<float>(s_pillFrom + (s_pillTo - s_pillFrom) * e);
}

// ---- 绘制 ----

void FillRound(Graphics& g, const RectF& r, float radius, const Color& c) {
    GraphicsPath p;
    AddRoundRect(p, r, radius);
    SolidBrush b(c);
    g.FillPath(&b, &p);
}

// 带一层淡阴影的文字，在花哨的背景上也看得清
void DrawLabel(Graphics& g, const wchar_t* text, const Font& font, const RectF& rect, const StringFormat& format,
               const Color& color) {
    RectF shadowRect = rect;
    shadowRect.Offset(0, std::max(1.0f, Px(1)));
    SolidBrush shadow(Colors().textShadow);
    g.DrawString(text, -1, &font, shadowRect, &format, &shadow);
    SolidBrush brush(color);
    g.DrawString(text, -1, &font, rect, &format, &brush);
}

// 高亮块：一颗小玻璃珠，上亮下暗，带一圈细亮边
void DrawPill(Graphics& g) {
    if (s_pillIndex < 0) return;
    const Palette& pal = Colors();
    RectF r = Vertical() ? RectF(s_L.colX, s_pillX, s_L.tile, s_L.tile) : RectF(s_pillX, s_L.tileY, s_L.tile, s_L.tile);
    r.Inflate(-0.5f, -0.5f);
    GraphicsPath path;
    AddRoundRect(path, r, Px(12));
    PointF top(0, r.Y - 1), bottom(0, r.Y + r.Height + 1);
    LinearGradientBrush fill(top, bottom, pal.pillTop, pal.pillBottom);
    g.FillPath(&fill, &path);
    LinearGradientBrush rimBrush(top, bottom, pal.pillRim, Color(pal.pillRim.GetA() / 5, 255, 255, 255));
    Pen rim(&rimBrush, std::max(1.0f, Px(1)));
    g.DrawPath(&rim, &path);
}

void DrawTile(Graphics& g, int idx) {
    const Palette& pal = Colors();
    RectF r = TileRect(idx);
    float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
    if (idx == 0) {
        // Windows 标志
        float sq = Px(8), gap = Px(2);
        float x0 = cx - sq - gap / 2, y0 = cy - sq - gap / 2;
        SolidBrush b(pal.accent);
        g.FillRectangle(&b, x0, y0, sq, sq);
        g.FillRectangle(&b, x0 + sq + gap, y0, sq, sq);
        g.FillRectangle(&b, x0, y0 + sq + gap, sq, sq);
        g.FillRectangle(&b, x0 + sq + gap, y0 + sq + gap, sq, sq);
        return;
    }

    const Tile& item = s_items[idx - 1];
    float is = Px(24);
    RectF iconRect(cx - is / 2, cy - is / 2, is, is);
    if (item.icon) g.DrawImage(item.icon.get(), iconRect);
    else FillRound(g, iconRect, Px(4), pal.separator);

    // 和 Win11 任务栏一样：当前窗口是一条长的强调色短线，其它打开的窗口是短点，没运行的固定应用没有。
    // 竖排时竖着画在靠屏幕边的那一侧
    float len = item.active ? Px(16) : Px(6);
    RectF bar(cx - len / 2, r.Y + r.Height - Px(6), len, Px(3));
    if (Vertical()) bar = RectF(s_edge == kEdgeLeft ? r.X + Px(3) : r.X + r.Width - Px(6), cy - len / 2, Px(3), len);
    if (item.active) FillRound(g, bar, Px(1.5f), pal.accent);
    else if (item.hwnd) FillRound(g, bar, Px(1.5f), pal.running);
}

// ---- 音量和亮度 ----

bool SliderEnabled(int which) { return which == 0 ? s_hasVolume : s_brightness >= 0; }

float SliderValue(int which) {
    if (which == 0) return s_hasVolume ? s_volume : 0;
    return s_brightness >= 0 ? s_brightness / 100.0f : 0;
}

std::wstring SliderText(int which) {
    if (which == 0) {
        if (!s_hasVolume) return Tr(L"没有可用的播放设备");
        if (s_muted) return Tr(L"已静音（点喇叭取消）");
        return TrF(L"音量 %d%%", static_cast<int>(std::lround(s_volume * 100)));
    }
    if (s_brightness == -2) return Tr(L"正在读取亮度…");
    if (s_brightness < 0) return Tr(L"这块屏幕不支持调节亮度");
    return TrF(L"亮度 %d%%", s_brightness);
}

void RefreshVolume() { s_hasVolume = Volume_Get(s_volume, s_muted); }

void RememberBrightness(HMONITOR monitor, int value) {
    for (auto& known : s_knownBrightness)
        if (known.first == monitor) {
            known.second = value;
            return;
        }
    s_knownBrightness.push_back({monitor, value});
}

void Redraw();

void SetSlider(int which, float value) {
    value = std::max(0.0f, std::min(value, 1.0f));
    if (which == 0) {
        if (!s_hasVolume) return;
        s_volume = value;
        s_muted = false;
        Volume_Set(value);
    } else {
        if (s_brightness < 0) return;
        int percent = static_cast<int>(std::lround(value * 100));
        if (percent == s_brightness) return;
        s_brightness = percent;
        s_brightnessEdited = true;
        RememberBrightness(s_monitor, percent);
        Brightness_Set(s_monitor, percent);
    }
    Redraw();
}

// 按鼠标位置（客户区坐标）设调节条
void SetSliderAt(int which, int x, int y) {
    const Slider& sl = s_sliders[which];
    float span = sl.a1 - sl.a0;
    if (std::fabs(span) < 1) return;
    SetSlider(which, ((Vertical() ? y : x) - sl.a0) / span);
}

// 滚轮一格 2%，和系统音量一样
void NudgeSlider(int which, int steps) {
    float v = SliderValue(which) * 50 + steps;
    SetSlider(which, std::round(v) / 50);
}

void RoundCaps(Pen& pen) {
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
}

// 喇叭：音量越大声波越多，静音时画一个叉
void DrawSpeaker(Graphics& g, const RectF& box, const Color& c) {
    float u = Px(1);
    float cx = box.X + box.Width / 2 - 2.25f * u, cy = box.Y + box.Height / 2;
    PointF body[6] = {PointF(cx - 6 * u, cy - 2.5f * u), PointF(cx - 2.5f * u, cy - 2.5f * u),
                      PointF(cx + 1.5f * u, cy - 6.5f * u), PointF(cx + 1.5f * u, cy + 6.5f * u),
                      PointF(cx - 2.5f * u, cy + 2.5f * u), PointF(cx - 6 * u, cy + 2.5f * u)};
    SolidBrush brush(c);
    g.FillPolygon(&brush, body, 6);
    Pen pen(c, 1.5f * u);
    RoundCaps(pen);
    if (!s_hasVolume || s_muted) {
        float x = cx + 5 * u, d = 2.5f * u;
        g.DrawLine(&pen, x, cy - d, x + 2 * d, cy + d);
        g.DrawLine(&pen, x, cy + d, x + 2 * d, cy - d);
        return;
    }
    int waves = s_volume <= 0.005f ? 0 : s_volume < 0.34f ? 1 : s_volume < 0.67f ? 2 : 3;
    for (int i = 0; i < waves; ++i) {
        float r = (4.5f + 3.2f * i) * u, ax = cx + 0.5f * u;
        g.DrawArc(&pen, ax - r, cy - r, 2 * r, 2 * r, -45.0f, 90.0f);
    }
}

// 太阳：一个圆圈加八道光
void DrawSun(Graphics& g, const RectF& box, const Color& c) {
    float u = Px(1);
    float cx = box.X + box.Width / 2, cy = box.Y + box.Height / 2, r = 3.5f * u;
    Pen pen(c, 1.5f * u);
    RoundCaps(pen);
    g.DrawEllipse(&pen, cx - r, cy - r, 2 * r, 2 * r);
    for (int k = 0; k < 8; ++k) {
        float a = k * 3.14159265f / 4, dx = std::cos(a), dy = std::sin(a);
        g.DrawLine(&pen, cx + dx * 6 * u, cy + dy * 6 * u, cx + dx * 8.2f * u, cy + dy * 8.2f * u);
    }
}

void DrawSlider(Graphics& g, int which) {
    const Palette& pal = Colors();
    const Slider& sl = s_sliders[which];
    bool enabled = SliderEnabled(which);
    bool hot = s_drag != kNone ? SliderOf(s_drag) == which : (s_hoverControl != kNone && SliderOf(s_hoverControl) == which);

    // 喇叭能点，鼠标放上去时垫一块高亮
    if (which == 0 && s_hoverControl == kVolumeIcon && s_hasVolume) FillRound(g, sl.icon, Px(8), pal.pillTop);
    Color iconColor = enabled ? pal.text : pal.subtle;
    if (which == 0) DrawSpeaker(g, sl.icon, iconColor);
    else DrawSun(g, sl.icon, iconColor);

    // 横排时槽横着，竖排时竖着（下边是 0）
    bool vertical = Vertical();
    float h = Px(4);
    auto span = [&](float from, float to) {  // 槽上从 from 到 to 的一段
        float lo = std::min(from, to), len = std::fabs(to - from);
        return vertical ? RectF(sl.c - h / 2, lo, h, len) : RectF(lo, sl.c - h / 2, len, h);
    };
    FillRound(g, span(sl.a0, sl.a1), h / 2, pal.track);
    if (!enabled) return;

    float pos = sl.a0 + (sl.a1 - sl.a0) * SliderValue(which);
    bool dim = which == 0 && s_muted;
    const Color& fill = dim ? pal.subtle : pal.accent;
    if (std::fabs(pos - sl.a0) > 0.5f) FillRound(g, span(sl.a0, pos), h / 2, fill);

    // 滑块：和 Win11 一样，外圈一个圆片，中间一个强调色的点，鼠标放上去时点变大
    float x = vertical ? sl.c : pos, y = vertical ? pos : sl.c;
    float R = Px(9);
    SolidBrush shadow(Color(40, 0, 0, 0));
    g.FillEllipse(&shadow, x - R, y - R + Px(1), 2 * R, 2 * R);
    SolidBrush thumb(pal.thumb);
    g.FillEllipse(&thumb, x - R, y - R, 2 * R, 2 * R);
    Pen rim(pal.thumbRim, std::max(1.0f, Px(1)));
    g.DrawEllipse(&rim, x - R, y - R, 2 * R, 2 * R);
    float core = hot ? Px(6) : Px(4.5f);
    SolidBrush dot(fill);
    g.FillEllipse(&dot, x - core, y - core, 2 * core, 2 * core);
}

// ---- Wi-Fi 和蓝牙 ----

std::wstring RadioText(int radio) {
    const RadioInfo& r = s_radio[radio];
    bool wifi = radio == kRadioWifi;
    if (s_radioFailed & (1 << radio)) return Tr(wifi ? L"Wi-Fi 切换失败" : L"蓝牙切换失败");
    switch (r.status) {
        case kRadioOn:
            if (r.switching) return Tr(wifi ? L"正在打开 Wi-Fi…" : L"正在打开蓝牙…");
            if (wifi && !r.network.empty()) return TrF(L"Wi-Fi：%ls", r.network.c_str());
            return Tr(wifi ? L"Wi-Fi 已打开" : L"蓝牙已打开");
        case kRadioOff:
            if (r.switching) return Tr(wifi ? L"正在关闭 Wi-Fi…" : L"正在关闭蓝牙…");
            return Tr(wifi ? L"Wi-Fi 已关闭" : L"蓝牙已关闭");
        case kRadioMissing:
            return Tr(wifi ? L"没有 Wi-Fi" : L"没有蓝牙");
        case kRadioUnavailable:
            return Tr(wifi ? L"Wi-Fi 不可用" : L"蓝牙不可用");
        default:
            return wifi ? L"Wi-Fi" : Tr(L"蓝牙");
    }
}

bool RadioUsable(int radio) { return s_radio[radio].status == kRadioOn || s_radio[radio].status == kRadioOff; }

// Wi-Fi：三道弧加一个点
void DrawWifi(Graphics& g, const RectF& box, const Color& c) {
    float u = Px(1);
    float cx = box.X + box.Width / 2, by = box.Y + box.Height / 2 + 5.5f * u;
    SolidBrush brush(c);
    float dot = 1.4f * u;
    g.FillEllipse(&brush, cx - dot, by - dot, 2 * dot, 2 * dot);
    Pen pen(c, 1.6f * u);
    RoundCaps(pen);
    for (int i = 0; i < 3; ++i) {
        float r = (4.5f + 3.6f * i) * u;
        g.DrawArc(&pen, cx - r, by - r, 2 * r, 2 * r, -135.0f, 90.0f);
    }
}

// 蓝牙的符号：一竖加两个折角
void DrawBluetooth(Graphics& g, const RectF& box, const Color& c) {
    float u = Px(1);
    float cx = box.X + box.Width / 2 + 0.3f * u, cy = box.Y + box.Height / 2;
    PointF pts[6] = {PointF(cx - 4 * u, cy - 3.5f * u), PointF(cx + 3.5f * u, cy + 3.5f * u), PointF(cx, cy + 7 * u),
                     PointF(cx, cy - 7 * u),            PointF(cx + 3.5f * u, cy - 3.5f * u), PointF(cx - 4 * u, cy + 3.5f * u)};
    Pen pen(c, 1.5f * u);
    RoundCaps(pen);
    g.DrawLines(&pen, pts, 6);
}

// 和系统快速设置里的开关一样：开着是强调色的底，关着是淡淡的底，读不到、不可用时只画暗的图标；正在切换时颜色淡一点
void DrawRadios(Graphics& g) {
    const Palette& pal = Colors();
    for (int k = 0; k < s_radioCount; ++k) {
        int id = s_radioIds[k];
        const RadioInfo& r = s_radio[id];
        const RectF& box = s_radioRects[k];
        bool on = r.status == kRadioOn, usable = RadioUsable(id);
        float radius = box.Height / 2;
        if (on) {
            const Color& a = pal.accent;
            FillRound(g, box, radius, r.switching ? Color(140, a.GetR(), a.GetG(), a.GetB()) : a);
        } else if (usable) {
            FillRound(g, box, radius, pal.pillTop);
        }
        if (k == s_hoverRadio && usable) FillRound(g, box, radius, on ? Color(45, 255, 255, 255) : pal.pillBottom);
        Color glyph = pal.subtle;
        if (on) glyph = s_glass.Light() ? Color(255, 255, 255, 255) : Color(235, 0, 0, 0);
        else if (usable && !r.switching) glyph = pal.text;
        if (id == kRadioWifi) DrawWifi(g, box, glyph);
        else DrawBluetooth(g, box, glyph);
    }
}

// 托盘图标。一行大图标放不下时两头画个小三角，表示那边还有（滚轮滚着看）；
// 堆叠时只画当前这一页，垫一块淡淡的底，不止一页时底边画几个小点表示第几页，翻页时新一页从旁边滑进来
void DrawTray(Graphics& g) {
    int cells = TrayCells();
    if (cells == 0) return;
    const Palette& pal = Colors();
    bool row = s_trayStyle == kTrayRow;
    int hot = s_hoverTray >= 0 ? s_hoverTray : s_trayHot;
    if (!row) {
        RectF back = TrayGroupRect();
        back.Inflate(-0.5f, -0.5f);
        FillRound(g, back, Px(12), pal.pillBottom);
    }
    if (hot >= 0 && hot < cells && TrayOnPage(hot) && !TrayFlipping()) {
        RectF r = TrayCellRect(hot);
        r.Inflate(-Px(1), -Px(1));
        FillRound(g, r, row ? Px(10) : Px(6), pal.pillTop);
    }
    float icon = row ? Px(24) : Px(16);
    auto drawIcon = [&](int i, float dx) {
        RectF r = TrayCellRect(i);
        RectF iconRect(r.X + dx + (r.Width - icon) / 2, r.Y + (r.Height - icon) / 2, icon, icon);
        if (s_tray[i].icon) g.DrawImage(s_tray[i].icon.get(), iconRect);
        else FillRound(g, iconRect, Px(3), pal.separator);
    };
    if (row) {
        int shown = TrayRowVisible();
        for (int i = s_trayFirst; i < s_trayFirst + shown; ++i) drawIcon(i, 0);
        // 小三角画在两头那一格图标外面的空白里
        SolidBrush hint(pal.subtle);
        float tri = Px(3.5f);
        PointF o = TrayOrigin();
        if (Vertical()) {
            float cx = o.X + s_L.cellW / 2, y0 = o.Y + Px(3), y1 = o.Y + s_L.trayW - Px(3);
            PointF up[3] = {PointF(cx, y0), PointF(cx - tri, y0 + tri), PointF(cx + tri, y0 + tri)};
            PointF down[3] = {PointF(cx, y1), PointF(cx - tri, y1 - tri), PointF(cx + tri, y1 - tri)};
            if (s_trayFirst > 0) g.FillPolygon(&hint, up, 3);
            if (s_trayFirst + shown < cells) g.FillPolygon(&hint, down, 3);
        } else {
            float cy = o.Y + s_L.cellH / 2, x0 = o.X + Px(3), x1 = o.X + s_L.trayW - Px(3);
            PointF left[3] = {PointF(x0, cy), PointF(x0 + tri, cy - tri), PointF(x0 + tri, cy + tri)};
            PointF right[3] = {PointF(x1, cy), PointF(x1 - tri, cy - tri), PointF(x1 - tri, cy + tri)};
            if (s_trayFirst > 0) g.FillPolygon(&hint, left, 3);
            if (s_trayFirst + shown < cells) g.FillPolygon(&hint, right, 3);
        }
        return;
    }

    RectF group = TrayGroupRect();
    auto drawPage = [&](int page, float dx) {
        for (int i = page * 4; i < std::min(cells, page * 4 + 4); ++i) drawIcon(i, dx);
    };
    double t = TrayFlipT();
    if (t < 1.0) {
        double e = 1 - (1 - t) * (1 - t) * (1 - t);
        float offset = static_cast<float>(s_flipDir * (1 - e) * group.Width);
        g.SetClip(group);
        drawPage(s_flipFrom, offset - s_flipDir * group.Width);
        drawPage(s_trayPage, offset);
        g.ResetClip();
    } else {
        drawPage(s_trayPage, 0);
    }

    int pages = TrayPages();
    if (pages > 1) {
        float r = Px(1.4f), y = group.Y + group.Height - Px(2.6f);
        float step = std::min(Px(6), (group.Width - Px(14)) / (pages - 1));
        float x0 = group.X + (group.Width - step * (pages - 1)) / 2;
        SolidBrush on(pal.text), off(pal.subtle);
        for (int p = 0; p < pages; ++p) {
            float x = x0 + p * step, rr = p == s_trayPage ? r * 1.3f : r;
            g.FillEllipse(p == s_trayPage ? &on : &off, x - rr, y - rr, 2 * rr, 2 * rr);
        }
    }
}

// 打字筛选时的搜索框：放大镜加打的字，太长时显示后面的部分
void DrawSearchBox(Graphics& g, const RectF& box, const Font& font) {
    const Palette& pal = Colors();
    FillRound(g, box, box.Height / 2, pal.pillTop);
    Pen pen(pal.text, Px(1.3f));
    float r = Px(3.5f), cx = box.X + Px(10), cy = box.Y + box.Height / 2 - Px(0.5f), d = r * 0.7f;
    g.DrawEllipse(&pen, cx - r, cy - r, 2 * r, 2 * r);
    g.DrawLine(&pen, cx + d, cy + d, cx + d + Px(2.5f), cy + d + Px(2.5f));
    StringFormat left(StringFormatFlagsNoWrap);
    left.SetLineAlignment(StringAlignmentCenter);
    RectF measured;
    g.MeasureString(s_filter.c_str(), -1, &font, PointF(0, 0), &left, &measured);
    RectF textRect(box.X + Px(18), box.Y, box.Width - Px(22), box.Height);
    left.SetAlignment(measured.Width > textRect.Width ? StringAlignmentFar : StringAlignmentNear);
    DrawLabel(g, s_filter.c_str(), font, textRect, left, pal.text);
}

// 搜索框的宽度：放得下打的字，最多 maxW
float SearchBoxWidth(Graphics& g, const Font& font, float maxW) {
    StringFormat left(StringFormatFlagsNoWrap);
    RectF measured;
    g.MeasureString(s_filter.c_str(), -1, &font, PointF(0, 0), &left, &measured);
    return std::min(maxW, measured.Width + Px(30));
}

// 竖排时显示标题的小牌子：贴着面板里侧，上下对着 anchorY（鼠标所指的格子、调节条等的中线）。
// 实心底，放在什么背景上都看得清；打字筛选时前面是搜索框
void DrawBubble(Graphics& g, float anchorY, const std::wstring& title, const Font& font) {
    const Layout& L = s_L;
    const Palette& pal = Colors();
    bool filtering = !s_filter.empty();
    if (title.empty() && !filtering) return;
    StringFormat oneLine(StringFormatFlagsNoWrap);
    oneLine.SetLineAlignment(StringAlignmentCenter);
    oneLine.SetTrimming(StringTrimmingEllipsisCharacter);
    float maxW = L.bubbleW - Px(12), padX = Px(10), h = Px(30);
    float searchW = filtering ? SearchBoxWidth(g, font, maxW * 0.5f) : 0;
    float titleW = 0;
    if (!title.empty()) {
        RectF measured;
        g.MeasureString(title.c_str(), -1, &font, PointF(0, 0), &oneLine, &measured);
        titleW = measured.Width + (filtering ? Px(6) : 0);
    }
    float w = std::min(maxW, 2 * padX + searchW + titleW);
    float x = s_edge == kEdgeLeft ? L.panelX + L.panelW + Px(8) : L.panelX - Px(8) - w;
    float y = std::clamp(anchorY - h / 2, L.panelY, L.panelY + L.panelH - h);
    RectF box(x, y, w, h);
    RectF shadow = box;
    shadow.Offset(0, Px(1.5f));
    FillRound(g, shadow, h / 2, Color(50, 0, 0, 0));
    bool light = s_glass.Light();
    FillRound(g, box, h / 2, light ? Color(245, 250, 250, 252) : Color(240, 40, 40, 44));
    GraphicsPath path;
    AddRoundRect(path, box, h / 2);
    Pen rim(light ? Color(36, 0, 0, 0) : Color(46, 255, 255, 255), std::max(1.0f, Px(1)));
    g.DrawPath(&rim, &path);
    float textX = box.X + padX;
    if (filtering) {
        DrawSearchBox(g, RectF(textX - Px(4), box.Y + Px(4), searchW, h - Px(8)), font);
        textX += searchW + Px(2);
    }
    if (!title.empty()) {
        SolidBrush brush(pal.text);
        g.DrawString(title.c_str(), -1, &font, RectF(textX, box.Y, box.X + box.Width - padX - textX, h), &oneLine, &brush);
    }
}

void DrawContent(Graphics& g) {
    const Layout& L = s_L;
    const Palette& pal = Colors();
    bool vertical = Vertical();

    DrawPill(g);

    Font titleFont(s_fontFamily.get(), Px(12), FontStyleRegular, UnitPixel);
    Font smallFont(s_fontFamily.get(), Px(11.5f), FontStyleRegular, UnitPixel);
    StringFormat center;
    center.SetAlignment(StringAlignmentCenter);
    center.SetLineAlignment(StringAlignmentCenter);
    center.SetTrimming(StringTrimmingEllipsisCharacter);

    // 顶部一行显示鼠标所指或选中项的标题；在调节条上时显示音量 / 亮度
    std::wstring title;
    int shown = s_hover >= 0 ? s_hover : s_sel;
    if (s_drag != kNone) title = SliderText(SliderOf(s_drag));
    else if (s_hoverControl != kNone) title = SliderText(SliderOf(s_hoverControl));
    else if (s_hoverRadio >= 0) title = RadioText(s_radioIds[s_hoverRadio]);
    else if (s_hoverTray >= 0 || s_overTray) title = TrayTitle(s_hoverTray);
    else if (shown == 0) title = Tr(L"开始");
    else if (shown > 0 && shown <= ItemCount()) title = s_items[shown - 1].title;
    StringFormat oneLine(StringFormatFlagsNoWrap);
    oneLine.SetAlignment(StringAlignmentCenter);
    oneLine.SetLineAlignment(StringAlignmentCenter);
    oneLine.SetTrimming(StringTrimmingEllipsisCharacter);
    RectF titleRect(L.margin + L.pad + Px(10), L.margin + L.pad, L.panelW - 2 * L.pad - Px(20), L.titleH);
    if (vertical) {
        // 竖排：标题在面板旁边的小牌子上，对着它说的那一项。打字筛选时只在指着或选了一项时显示名字；
        // 开始菜单模式只在鼠标指着时显示
        bool pointing = s_hover >= 0 || s_hoverTray >= 0 || s_overTray || s_hoverControl != kNone || s_drag != kNone ||
                        s_hoverRadio >= 0;
        float anchorY = L.itemsY + L.tile / 2;
        if (s_drag != kNone || s_hoverControl != kNone) {
            const Slider& sl = s_sliders[SliderOf(s_drag != kNone ? s_drag : s_hoverControl)];
            anchorY = (sl.a0 + sl.a1) / 2;
        } else if (s_hoverRadio >= 0) {
            const RectF& r = s_radioRects[s_hoverRadio];
            anchorY = r.Y + r.Height / 2;
        } else if (s_hoverTray >= 0) {
            RectF r = TrayCellRect(s_hoverTray);
            anchorY = r.Y + r.Height / 2;
        } else if (s_overTray) {
            anchorY = L.trayY + L.trayW / 2;
        } else if (shown >= 0 && shown <= ItemCount() && IsTileVisible(shown)) {
            RectF r = TileRect(shown);
            anchorY = r.Y + r.Height / 2;
        }
        if (!s_filter.empty() && !(s_hover >= 0 || s_sel > 0)) title.clear();
        if (!s_compact || pointing) DrawBubble(g, anchorY, title, titleFont);
    } else if (L.titleH <= 0) {
        // 开始菜单模式没有标题这一行
    } else if (!s_filter.empty()) {
        // 打字筛选：左边一个搜索框显示打的字，右边是选中项的名字
        float boxW = SearchBoxWidth(g, titleFont, titleRect.Width * 0.5f);
        RectF box(titleRect.X - Px(6), titleRect.Y + Px(2), boxW, titleRect.Height - Px(4));
        DrawSearchBox(g, box, titleFont);
        float restX = box.X + box.Width + Px(6);
        if (!title.empty() && (s_hover >= 0 || s_sel > 0))
            DrawLabel(g, title.c_str(), titleFont,
                      RectF(restX, titleRect.Y, titleRect.X + titleRect.Width - restX, titleRect.Height), oneLine,
                      pal.text);
    } else if (!title.empty()) {
        DrawLabel(g, title.c_str(), titleFont, titleRect, oneLine, pal.text);
    }

    for (int idx = 0; idx <= ItemCount(); ++idx)
        if (IsTileVisible(idx)) DrawTile(g, idx);

    // 窗口多到放不下时，两头的小三角提示还能滚动
    SolidBrush hint(pal.subtle);
    float tri = Px(4);
    bool before = s_first > 0, after = s_first + s_visible < ItemCount();
    if (vertical) {
        float midX = L.colX + L.tile / 2, y0 = L.itemsY - Px(2), y1 = L.itemsY + L.itemsW + Px(2);
        PointF up[3] = {PointF(midX, y0 - tri), PointF(midX - tri, y0), PointF(midX + tri, y0)};
        PointF down[3] = {PointF(midX, y1 + tri), PointF(midX - tri, y1), PointF(midX + tri, y1)};
        if (before) g.FillPolygon(&hint, up, 3);
        if (after) g.FillPolygon(&hint, down, 3);
    } else {
        float midY = L.tileY + L.tile / 2, x0 = L.itemsX - Px(2), x1 = L.itemsX + L.itemsW + Px(2);
        PointF left[3] = {PointF(x0 - tri, midY), PointF(x0, midY - tri), PointF(x0, midY + tri)};
        PointF right[3] = {PointF(x1 + tri, midY), PointF(x1, midY - tri), PointF(x1, midY + tri)};
        if (before) g.FillPolygon(&hint, left, 3);
        if (after) g.FillPolygon(&hint, right, 3);
    }

    // 分隔线：横排是竖线，竖排是横线
    Pen sepPen(pal.separator, 1.0f);
    auto separator = [&](float at) {
        if (vertical) g.DrawLine(&sepPen, L.colX + Px(10), at - L.sep / 2, L.colX + L.tile - Px(10), at - L.sep / 2);
        else g.DrawLine(&sepPen, at - L.sep / 2, L.tileY + Px(10), at - L.sep / 2, L.tileY + L.tile - Px(10));
    };
    separator(vertical ? L.itemsY : L.itemsX);
    if (L.trayW > 0) separator(vertical ? L.trayY : L.trayX);
    separator(vertical ? L.clockY : L.clockX);
    DrawTray(g);

    if (s_items.empty()) {
        const wchar_t* empty = Tr(s_filter.empty() ? L"没有打开的窗口" : L"没有匹配的窗口或应用");
        if (vertical) DrawLabel(g, empty, smallFont, RectF(L.panelX + Px(4), L.itemsY, L.panelW - Px(8), L.itemsW), center, pal.subtle);
        else DrawLabel(g, empty, titleFont, RectF(L.itemsX, L.tileY, L.itemsW, L.tile), center, pal.subtle);
    }

    // 时钟：竖排时面板窄，日期只写月和日
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t clock[64];
    if (vertical) swprintf(clock, 64, L"%02d:%02d\n%d/%d", st.wHour, st.wMinute, st.wMonth, st.wDay);
    else swprintf(clock, 64, L"%02d:%02d\n%d/%d/%d", st.wHour, st.wMinute, st.wYear, st.wMonth, st.wDay);
    RectF clockRect = vertical ? RectF(L.panelX, L.clockY, L.panelW, L.clockH) : RectF(L.clockX, L.tileY, L.clockW, L.tile);
    DrawLabel(g, clock, smallFont, clockRect, center, pal.text);

    if (s_levels) {
        DrawSlider(g, 0);
        DrawSlider(g, 1);
    }
    DrawRadios(g);
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

// 先画玻璃（静止时用缓存），再用 GDI+ 画上面的内容，最后用 UpdateLayeredWindow 带逐像素透明度贴到屏幕。
// slide：面板往下挪的距离（弹出 / 收起动画），按整像素挪，玻璃和上面的内容才能严丝合缝
// ---- 缩略图 ----

void HideThumb() {
    KillTimer(s_hwnd, kTimerThumb);
    Thumb_Hide();
    s_thumbWant = s_thumbShown = nullptr;
    s_thumbIdx = -1;
}

// 鼠标所指的格子（用键盘选时是选中的格子）是打开的窗口时显示它的缩略图；弹出、收起动画期间和右键菜单开着时不显示
int ThumbIndex() {
    if (!s_open || s_anim != Anim::None || s_drag != kNone || s_menuOpen || s_startMode) return -1;  // 开始菜单会盖住缩略图
    int idx = s_hover >= 0 ? s_hover : s_keyNav ? s_sel : -1;
    if (idx <= 0 || idx > ItemCount() || !IsTileVisible(idx) || !IsWindow(s_items[idx - 1].hwnd)) return -1;
    return idx;
}

void ShowThumbNow() {
    KillTimer(s_hwnd, kTimerThumb);
    int idx = ThumbIndex();
    if (idx < 0) {
        HideThumb();
        return;
    }
    // 横排时在格子上方；竖排时在面板里侧、标题小牌子下面
    RectF r = TileRect(idx);
    const Layout& L = s_L;
    POINT at = {s_pos.x + static_cast<int>(r.X + r.Width / 2), s_pos.y + static_cast<int>(L.margin - Px(8))};
    int place = kThumbAbove;
    if (Vertical()) {
        bool left = s_edge == kEdgeLeft;
        at = {s_pos.x + static_cast<int>(left ? L.panelX + L.panelW + Px(8) : L.panelX - Px(8)),
              s_pos.y + static_cast<int>(r.Y + r.Height / 2 + Px(21))};
        place = left ? kThumbRight : kThumbLeft;
    }
    HWND h = s_items[idx - 1].hwnd;
    s_thumbShown = Thumb_Show(h, at, place, s_bounds, s_scale, s_glass.Light()) ? h : nullptr;
    s_thumbWant = h;
    s_thumbIdx = idx;
}

// 每次重画后调用：想显示的缩略图变了就安排出现、切换或消失
void SyncThumb() {
    if (!s_open) {
        if (s_thumbWant || s_thumbShown) HideThumb();
        return;
    }
    int idx = ThumbIndex();
    HWND want = idx > 0 ? s_items[idx - 1].hwnd : nullptr;
    if (want == s_thumbWant && idx == s_thumbIdx) return;
    s_thumbWant = want;
    s_thumbIdx = idx;
    if (want && s_thumbShown) ShowThumbNow();  // 已经在显示：直接换成这个窗口
    else if (want || s_thumbShown) SetTimer(s_hwnd, kTimerThumb, want ? kThumbShowMs : kThumbHideMs, nullptr);
    else KillTimer(s_hwnd, kTimerThumb);
}

void Render(float slide, BYTE alpha) {
    if (!s_hwnd || s_width <= 0 || s_height <= 0 || !EnsureCanvas()) return;

    // 横排时往下滑出去，竖排时往屏幕边那一侧滑出去
    int moved = static_cast<int>(std::lround(slide)), dx = 0, dy = moved;
    if (Vertical()) {
        dy = 0;
        dx = s_edge == kEdgeLeft ? -moved : moved;
    }
    size_t count = static_cast<size_t>(s_width) * s_height;
    if (moved == 0 && s_baseValid && s_base.size() == count) {
        memcpy(s_bits, s_base.data(), count * sizeof(DWORD));
    } else {
        s_glass.Prepare(s_width, s_height, RectF(s_L.panelX, s_L.panelY, s_L.panelW, s_L.panelH), s_L.radius);
        s_glass.Render(s_bits, s_pos, dy, dx);
        if (moved == 0) {
            s_base.assign(s_bits, s_bits + count);
            s_baseValid = true;
        }
    }

    {
        Bitmap canvas(s_width, s_height, s_width * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(s_bits));
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.TranslateTransform(static_cast<REAL>(dx), static_cast<REAL>(dy));
        DrawContent(g);
    }

    SIZE size = {s_width, s_height};
    POINT src = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(s_hwnd, nullptr, &s_pos, &size, s_canvasDC, &src, 0, &blend, ULW_ALPHA);
    SyncThumb();
}

void RequestFrame() {
    if (s_framePosted || !s_hwnd) return;
    s_framePosted = true;
    PostMessageW(s_hwnd, WM_APP_FRAME, 0, 0);
}

// 先冲过头一点再回弹，像液体一样有点弹性
double EaseOutBack(double t) {
    const double c1 = 1.1, c3 = c1 + 1;
    double u = t - 1;
    return 1 + c3 * u * u * u + c1 * u * u;
}

bool MouseButtonDown() {
    for (int vk : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2})
        if (GetAsyncKeyState(vk) & 0x8000) return true;
    return false;
}

void EndStartMode() {
    s_startPending = s_startMode = false;
    KillTimer(s_hwnd, kTimerStart);
}

void EndTrayMode() {
    s_trayMode = false;
    s_trayHot = -1;
    KillTimer(s_hwnd, kTimerTrayMenu);
}

void HideNow() {
    HideThumb();
    EndStartMode();
    EndTrayMode();
    s_open = false;
    s_preview = false;
    KillTimer(s_hwnd, kTimerPreview);
    if (g_settings.trayIcons) Tray_Refresh();  // 下次弹出时用新的
    s_anim = Anim::None;
    s_drag = kNone;
    if (GetCapture() == s_hwnd) ReleaseCapture();
    KillTimer(s_hwnd, kTimerClock);
    ShowWindow(s_hwnd, SW_HIDE);
}

void BeginStart();

// 动画的一帧：按时间算出位置画出来，还没播完就等下一次屏幕刷新再画下一帧
void Frame() {
    double now = NowMs();
    float slide = 0;
    BYTE alpha = 255;
    bool more = false, start = false;
    if (s_anim == Anim::Showing) {
        double t = std::min(1.0, (now - s_animStart) / (s_compact ? kStartShowMs : kShowMs));
        slide = static_cast<float>((1 - EaseOutBack(t)) * SlideDistance());
        alpha = static_cast<BYTE>(std::lround(255 * std::min(1.0, t * 3)));
        start = s_startPending && t >= kStartAt;  // 第二段：画完这一帧打开开始菜单
        if (t >= 1) s_anim = Anim::None;
        else more = true;
    } else if (s_anim == Anim::Hiding) {
        double t = std::min(1.0, (now - s_animStart) / kHideMs);
        if (t >= 1) {
            HideNow();
            return;
        }
        slide = static_cast<float>(s_hideFromSlide + t * t * t * (SlideDistance() - s_hideFromSlide));
        alpha = static_cast<BYTE>(std::lround(s_hideFromAlpha * (1 - t)));
        more = true;
    } else if (!s_open) {
        return;
    }
    SyncPill();
    AdvancePill(now);
    more = more || PillMoving() || TrayFlipping();
    s_lastSlide = slide;
    s_lastAlpha = alpha;
    Render(slide, alpha);
    if (start) BeginStart();
    if (more) {
        WaitForVBlank();
        RequestFrame();
    }
}

void Redraw() {
    if (!s_open) return;
    SyncPill();
    if (s_anim != Anim::None || PillMoving() || TrayFlipping()) RequestFrame();  // 交给动画帧去画
    else Render(0, 255);
}

// 托盘图标翻页（堆叠、不止一页时），转一圈回到头；一行大图标放不下时滚动 delta 个，到头就停
void FlipTray(int delta) {
    if (s_trayStyle == kTrayRow) {
        int first = std::clamp(s_trayFirst + delta, 0, TrayCells() - TrayRowVisible());
        if (first == s_trayFirst) return;
        s_trayFirst = first;
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(s_hwnd, &pt);
        s_hoverTray = TrayAt(pt.x, pt.y);
        Redraw();
        return;
    }
    int pages = TrayPages();
    if (pages <= 1 || delta == 0) return;
    s_flipFrom = s_trayPage;
    s_flipDir = delta > 0 ? 1 : -1;
    s_flipStart = NowMs();
    s_trayPage = ((s_trayPage + delta) % pages + pages) % pages;
    POINT pt;  // 鼠标下换成了新一页的图标
    GetCursorPos(&pt);
    ScreenToClient(s_hwnd, &pt);
    s_hoverTray = TrayAt(pt.x, pt.y);
    Redraw();
}

// 鼠标停在托盘图标上时，左右方向键翻页（一行大图标时滚动）
bool FlipTrayUnderMouse(int delta) {
    if (!TrayScrolls()) return false;
    POINT pt;
    GetCursorPos(&pt);
    if (WindowFromPoint(pt) != s_hwnd) return false;
    ScreenToClient(s_hwnd, &pt);
    if (!OverTray(pt.x, pt.y)) return false;
    FlipTray(delta);
    return true;
}

void MoveSelection(int delta) {
    int count = ItemCount() + 1;
    s_sel = ((s_sel + delta) % count + count) % count;
    s_hover = -1;
    s_hoverControl = kNone;  // 鼠标停在调节条上时，标题也要换成选中的窗口
    s_hoverRadio = -1;
    s_hoverTray = -1;
    s_overTray = false;
    s_keyNav = true;
    EnsureVisible(s_sel);
    Redraw();
}

// 默认选中当前窗口，没有的话选第一个打开的窗口，再没有就选第一个格子
int DefaultSelection() {
    for (int i = 0; i < ItemCount(); ++i)
        if (s_items[i].active) return i + 1;
    for (int i = 0; i < ItemCount(); ++i)
        if (s_items[i].hwnd) return i + 1;
    return s_items.empty() ? 0 : 1;
}

// 打了字：只留下标题、应用名或程序名里有这些字的格子，后面再补上开始菜单里匹配的应用。
// 面板宽度保持不变（玻璃背景是弹出时按这个大小截的），放不下就左右滚动
void ApplyFilter() {
    if (s_lockedItemsW <= 0) s_lockedItemsW = s_L.itemsW;
    std::wstring f = ToLower(s_filter);
    if (f.empty()) {
        s_items = s_allItems;
    } else {
        s_items.clear();
        for (const Tile& t : s_allItems)
            if (t.search.find(f) != std::wstring::npos) s_items.push_back(t);
        int iconPx = static_cast<int>(std::lround(Px(32)));
        for (const InstalledApp& a : Apps_Match(f, kMaxAppMatches)) {
            std::wstring name = ToLower(a.name);
            bool shown = std::any_of(s_items.begin(), s_items.end(), [&](const Tile& t) {
                return t.launch == a.launch || ToLower(t.pinName) == name || ToLower(t.title) == name;
            });
            if (shown) continue;
            auto icon = ShellItemIcon(a.launch, iconPx);
            s_items.push_back({a.name, icon, nullptr, false, a.launch, a.name, icon, a.search});
        }
    }
    ComputeLayout();
    s_first = 0;
    s_hover = -1;
    s_hoverControl = kNone;
    s_hoverRadio = -1;
    s_hoverTray = -1;
    s_pillIndex = -1;
    s_sel = f.empty() ? DefaultSelection() : (s_items.empty() ? -1 : 1);  // 没有匹配的就什么都不选
    s_keyNav = !f.empty();
    s_thumbIdx = -1;  // 格子重新排过：缩略图按新位置摆
    EnsureVisible(s_sel);
    Redraw();
}

void Launch(const std::wstring& target) {
    LaunchApp(target);  // 趁本窗口还在前台时启动，新程序才能顺利拿到前台
    HideNow();
}

// 点了 Wi-Fi / 蓝牙按钮：切过去，按钮马上显示成要切成的样子（颜色淡一点），切完了后台线程再通知
void ToggleRadio(int k) {
    if (k < 0 || k >= s_radioCount) return;
    int id = s_radioIds[k];
    if (!RadioUsable(id)) return;
    s_radioFailed &= ~(1 << id);
    Radios_Set(id, s_radio[id].status != kRadioOn, s_hwnd);
    s_radio[id] = Radios_Get(id);
    Redraw();
}

// 右键 Wi-Fi / 蓝牙按钮：弹出系统自己的快捷列表（Wi-Fi：网络列表，Win10、Win11 都有；蓝牙：Win11 快速设置里的设备列表），
// 打不开时（没有程序能打开这个链接，或者 Win10 的蓝牙）退回系统设置里对应的页面
void OpenRadioPanel(int k) {
    if (k < 0 || k >= s_radioCount) return;
    bool wifi = s_radioIds[k] == kRadioWifi;
    const wchar_t* panel = wifi ? L"ms-availablenetworks:" : IsWindows11() ? L"ms-actioncenter:controlcenter/bluetooth" : nullptr;
    if (panel) {
        AllowSetForegroundWindow(ASFW_ANY);
        SHELLEXECUTEINFOW sei = {sizeof(sei)};
        sei.fMask = SEE_MASK_FLAG_NO_UI;  // 打不开时不弹“选择应用”，改去设置
        sei.lpFile = panel;
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) {
            Log(L"右键%ls按钮：打开 %ls", wifi ? L" Wi-Fi " : L"蓝牙", panel);
            HideNow();
            return;
        }
        Log(L"右键%ls按钮：打开 %ls 失败（%lu），改开系统设置", wifi ? L" Wi-Fi " : L"蓝牙", panel, GetLastError());
    }
    Launch(wifi ? L"ms-settings:network-wifi" : L"ms-settings:bluetooth");
}

void ActivateIndex(int idx, bool newInstance = false) {
    if (idx == 0) {
        // 开始菜单开着时点的开始按钮：和系统的一样是关掉它（它已经因为点到这里关了），不再打开
        bool closing = NowMs() - s_startClosedAt < 500;
        HideNow();
        if (closing) return;
        Fullscreen_QuietStartMenu();
        SendStartMenu();
        return;
    }
    if (idx < 0 || idx > ItemCount()) return;
    Tile item = s_items[idx - 1];
    if (!item.launch.empty() && (newInstance || !item.hwnd)) {
        Launch(item.launch);
        return;
    }
    HWND h = item.hwnd;
    if (!IsWindow(h)) {
        HideNow();
        return;
    }
    // 和系统任务栏一样：点当前窗口就最小化
    if (item.active && !IsIconic(h)) {
        HideNow();
        ShowWindowAsync(h, SW_MINIMIZE);  // 异步：那个程序卡住时本程序不跟着卡
        return;
    }
    s_open = false;  // 切换焦点会触发失活，这里不要再播收起动画
    if (IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);
    ForceForeground(h);  // 先激活目标窗口，趁本窗口还在前台有权限切换
    HideNow();
}

void CloseItem(int idx) {
    if (idx <= 0 || idx > ItemCount()) return;
    Tile& item = s_items[idx - 1];
    if (!item.hwnd) {
        Launch(item.launch);  // 没运行的固定应用：中键和任务栏一样是启动
        return;
    }
    HWND h = item.hwnd;
    std::wstring launch = item.launch;
    if (!PostMessageW(h, WM_CLOSE, 0, 0)) {
        DWORD error = GetLastError();
        // 以管理员身份运行的程序（比如任务管理器），普通权限发不进去：窗口还在，格子也留着。
        // 窗口已经没了的话照常去掉格子
        if (IsWindow(h)) {
            Log(L"关闭窗口失败（%lu）：%ls", error, item.title.c_str());
            MessageBeep(MB_ICONWARNING);
            return;
        }
    }

    // 固定应用关掉最后一个窗口后，格子变回启动按钮；其余情况直接去掉。
    // 按全部格子判断，筛选后显示的格子和全部格子一起改
    bool lastOfPin = !launch.empty() && std::none_of(s_allItems.begin(), s_allItems.end(), [&](const Tile& t) {
                         return t.hwnd && t.hwnd != h && t.launch == launch;
                     });
    auto closeIn = [&](std::vector<Tile>& list) {
        auto it = std::find_if(list.begin(), list.end(), [&](const Tile& t) { return t.hwnd == h; });
        if (it == list.end()) return;
        if (!lastOfPin) {
            list.erase(it);
            return;
        }
        it->hwnd = nullptr;
        it->active = false;
        it->title = it->pinName;
        it->icon = it->pinIcon ? it->pinIcon : it->icon;
    };
    closeIn(s_allItems);
    closeIn(s_items);
    if (lastOfPin) {
        Redraw();
        return;
    }
    ComputeLayout();
    UpdatePlacement();  // 面板变窄了，重新居中（玻璃背景也跟着重算）
    s_first = std::max(0, std::min(s_first, ItemCount() - s_visible));
    s_sel = std::min(s_sel, ItemCount());
    s_hover = -1;
    s_hoverTray = -1;  // 托盘格子跟着挪了
    s_pillIndex = -1;
    s_thumbIdx = -1;  // 格子重新排过：缩略图按新位置摆
    Redraw();
}

// 右键菜单的命令
enum MenuCmd : UINT {
    kCmdClose = 1,
    kCmdCloseAll,
    kCmdExclude,
    kCmdLaunch,  // 再开一个（运行中）/ 打开（没运行）
    kCmdRunAs,
    kCmdLocation,
    kCmdRestore,
    kCmdMinimize,
    kCmdMaximize,
    kCmdRecent = 100,  // 最近项：kCmdRecent + 序号
};
constexpr size_t kMaxRecent = 10;
constexpr size_t kMaxMenuText = 40;

// 菜单文字：太长的截断加省略号；& 在菜单里是快捷键标记，要写成 &&
std::wstring MenuText(std::wstring text) {
    if (text.size() > kMaxMenuText) {
        size_t cut = kMaxMenuText - 1;
        if (IS_HIGH_SURROGATE(text[cut - 1])) --cut;  // 别把一个字劈成两半
        text = text.substr(0, cut) + L"…";
    }
    std::wstring out;
    for (wchar_t c : text) {
        out += c;
        if (c == L'&') out += L'&';
    }
    return out;
}

// 还原 / 最小化 / 最大化：和系统的窗口菜单一样发 WM_SYSCOMMAND，自己画标题栏的程序也能照自己的方式处理。
// 发不进去（权限比本程序高）时退回 ShowWindowAsync。还原、最大化后和点格子一样切到这个窗口
void WindowCommand(HWND h, WPARAM command) {
    if (!IsWindow(h)) {
        HideNow();
        return;
    }
    if (command == SC_MINIMIZE) {
        HideNow();
        if (!PostMessageW(h, WM_SYSCOMMAND, command, 0)) ShowWindowAsync(h, SW_MINIMIZE);
        return;
    }
    s_open = false;  // 切换焦点会触发失活，这里不要再播收起动画
    if (!PostMessageW(h, WM_SYSCOMMAND, command, 0))
        ShowWindowAsync(h, command == SC_MAXIMIZE ? SW_SHOWMAXIMIZED : SW_RESTORE);
    ForceForeground(h);
    HideNow();
}

// 开始按钮：和右键系统的开始按钮一样，打开 Win+X 菜单
void ShowStartContextMenu() {
    HideNow();
    Fullscreen_QuietStartMenu();  // 藏着的系统任务栏别因为它弹出来
    SendWinX();
}

// 右键菜单从哪弹：横排在格子上方（菜单底边对着它）；竖排在面板里侧，菜单顶边和格子顶边对齐
UINT MenuAlign() {
    if (!Vertical()) return TPM_BOTTOMALIGN;
    return s_edge == kEdgeLeft ? TPM_LEFTALIGN | TPM_TOPALIGN : TPM_RIGHTALIGN | TPM_TOPALIGN;
}

POINT MenuAnchor(const RectF& r) {  // 客户区坐标
    if (!Vertical()) return {static_cast<LONG>(r.X + r.Width / 2), static_cast<LONG>(r.Y)};
    const Layout& L = s_L;
    float x = s_edge == kEdgeLeft ? L.panelX + L.panelW + Px(4) : L.panelX - Px(4);
    return {static_cast<LONG>(x), static_cast<LONG>(r.Y)};
}

// 右键格子：尽量和系统任务栏的右键菜单一样。
// 运行中的窗口：跳转列表的“最近”项；应用名（再开一个）、以管理员身份运行、打开文件所在的位置；
// 还原 / 最小化 / 最大化；“最大化时不隐藏任务栏”（关着“最大化或全屏时隐藏任务栏”时是“最大化时隐藏任务栏”）；关闭这个窗口，同一个程序开着好几个窗口时还能一次全关。
// 没运行的固定应用、打字筛选出来的应用：“最近”项；打开、以管理员身份运行、打开文件所在的位置。
// at：菜单的位置（客户区坐标），对齐方式见 MenuAlign
void ShowItemMenu(int idx, POINT at) {
    if (idx == 0) {
        ShowStartContextMenu();
        return;
    }
    if (idx < 0 || idx > ItemCount()) return;
    // 菜单开着时格子可能重排（比如应用列表读完了重新筛选），先抄一份，之后按窗口句柄找
    const Tile item = s_items[idx - 1];
    HWND hwnd = item.hwnd;
    if (!hwnd && item.launch.empty()) return;

    AppTarget app = ResolveAppTarget(hwnd, item.launch, item.pinName);
    std::vector<RecentItem> recent = RecentItems(app.appIds, kMaxRecent);
    if (!s_open) return;  // 读外壳时可能处理了别的消息，期间已经收起了

    // 同一个程序：有 AppUserModelID 的按它比（应用商店应用的窗口都属于同一个 ApplicationFrameHost 进程），
    // 没有的按程序路径比
    std::vector<HWND> same;
    std::wstring exeName;
    if (hwnd) {
        auto appKey = [](HWND h) {
            std::wstring id = GetWindowAumid(h);
            return id.empty() ? ToLower(GetProcessPath(AppWindowTarget(h))) : id;
        };
        std::wstring key = appKey(hwnd);
        for (const Tile& t : s_items)
            if (t.hwnd && !key.empty() && appKey(t.hwnd) == key) same.push_back(t.hwnd);
        // 应用商店应用最小化时偶尔找不到它自己的进程，别把 ApplicationFrameHost 当成它加进排除名单
        exeName = WindowExeName(hwnd);
        if (exeName == L"applicationframehost.exe") exeName.clear();
    }

    HMENU menu = CreatePopupMenu();
    // 两组之间的分隔线：前面有东西、最后一项又不是分隔线时才加
    auto separate = [&] {
        int n = GetMenuItemCount(menu);
        if (n > 0 && !(GetMenuState(menu, n - 1, MF_BYPOSITION) & MF_SEPARATOR))
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    };
    if (!recent.empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, Tr(L"最近"));
        for (size_t i = 0; i < recent.size(); ++i)
            AppendMenuW(menu, MF_STRING, kCmdRecent + i, MenuText(recent[i].name).c_str());
    }
    separate();
    if (!app.launch.empty()) AppendMenuW(menu, MF_STRING, kCmdLaunch, hwnd ? MenuText(app.name).c_str() : Tr(L"打开"));
    if (!app.runAs.empty()) AppendMenuW(menu, MF_STRING, kCmdRunAs, Tr(L"以管理员身份运行"));
    if (!app.location.empty()) AppendMenuW(menu, MF_STRING, kCmdLocation, Tr(L"打开文件所在的位置"));
    if (hwnd) {
        separate();
        bool iconic = IsIconic(hwnd), zoomed = IsZoomed(hwnd);
        bool canMaximize = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_MAXIMIZEBOX) != 0;
        AppendMenuW(menu, MF_STRING | (iconic || zoomed ? 0 : MF_GRAYED), kCmdRestore, Tr(L"还原"));
        AppendMenuW(menu, MF_STRING | (iconic ? MF_GRAYED : 0), kCmdMinimize, Tr(L"最小化"));
        AppendMenuW(menu, MF_STRING | (!zoomed && canMaximize ? 0 : MF_GRAYED), kCmdMaximize, Tr(L"最大化"));
        if (!exeName.empty())
            AppendMenuW(menu, MF_STRING | (IsExceptionExe(exeName) ? MF_CHECKED : 0), kCmdExclude,
                        Tr(g_settings.autoHideOnFullscreen ? L"最大化时不隐藏任务栏" : L"最大化时隐藏任务栏"));
        separate();
        AppendMenuW(menu, MF_STRING, kCmdClose, Tr(L"关闭窗口"));
        if (same.size() > 1) {
            AppendMenuW(menu, MF_STRING, kCmdCloseAll, TrF(L"关闭全部 %d 个窗口", static_cast<int>(same.size())).c_str());
        }
        SetMenuDefaultItem(menu, kCmdClose, FALSE);
    } else {
        SetMenuDefaultItem(menu, kCmdLaunch, FALSE);
    }

    s_sel = idx;
    s_menuOpen = true;  // 缩略图会挡住菜单
    HideThumb();
    Redraw();
    ClientToScreen(s_hwnd, &at);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | MenuAlign(), at.x, at.y, 0, s_hwnd,
                              nullptr);
    DestroyMenu(menu);
    s_menuOpen = false;

    // 下面这些只用上面抄下来的东西，不碰格子；趁本窗口还在前台时启动，新程序才能顺利拿到前台
    if (cmd >= kCmdRecent && cmd < kCmdRecent + recent.size()) {
        OpenRecentItem(recent[cmd - kCmdRecent], app.exe);
        HideNow();
        return;
    }
    switch (cmd) {
        case kCmdExclude: SetException(exeName, !IsExceptionExe(exeName)); return;
        case kCmdLaunch: Launch(app.launch); return;
        case kCmdRunAs:
            RunAsAdmin(s_hwnd, app.runAs);
            HideNow();
            return;
        case kCmdLocation:
            OpenFileLocation(app.location);
            HideNow();
            return;
        case kCmdRestore: WindowCommand(hwnd, SC_RESTORE); return;
        case kCmdMinimize: WindowCommand(hwnd, SC_MINIMIZE); return;
        case kCmdMaximize: WindowCommand(hwnd, SC_MAXIMIZE); return;
    }
    if (!s_open) return;
    // CloseItem 会去掉格子、后面的序号跟着变，每次按窗口句柄重新找
    auto closeWindow = [](HWND h) {
        for (int i = 0; i < ItemCount(); ++i)
            if (s_items[i].hwnd == h) {
                CloseItem(i + 1);
                return;
            }
    };
    if (cmd == kCmdClose) {
        closeWindow(hwnd);
    } else if (cmd == kCmdCloseAll) {
        for (HWND h : same) closeWindow(h);
    }
}

// 用键盘（菜单键、Shift+F10）打开选中格子的右键菜单，菜单从格子上方弹出
void ShowSelectedMenu() {
    if (s_sel < 0 || s_sel > ItemCount() || !IsTileVisible(s_sel)) return;
    ShowItemMenu(s_sel, MenuAnchor(TileRect(s_sel)));
}

void ShowTrayMenu(int cell, const TrayApp& app, bool offerAppMenu);

// 点托盘格子：左键先收起迷你任务栏，再去系统托盘里点它。
// 右键在那一格上方弹迷你任务栏自己的菜单，不碰系统托盘；选“程序菜单”才去系统托盘里右键它（TrayAppMenu）
void ActivateTray(int i, bool right) {
    if (i < 0 || i >= TrayCells() || !TrayOnPage(i)) return;
    TrayApp app = s_tray[i];
    if (!right) {
        HideNow();
        Tray_Click(app, false);
        return;
    }
    ShowTrayMenu(i, app, true);
}

// 去系统托盘里右键它、弹出它自己的菜单。迷你任务栏留着，菜单关了再看要不要收起（WatchTrayMenu）
void TrayAppMenu(int cell, const TrayApp& app) {
    if (Tray_Busy()) return;
    HideThumb();
    s_trayMode = true;
    s_trayModeAt = GetTickCount();
    s_trayIdleAt = 0;
    s_trayRefocused = false;
    s_trayHot = cell;
    SetTimer(s_hwnd, kTimerTrayMenu, kTrayWatchMs, nullptr);
    Redraw();
    Tray_Click(app, true, s_hwnd);
}

bool IsShellUi(HWND h) {
    std::wstring cls = GetClassNameStr(h);
    return cls == L"Shell_TrayWnd" || cls == L"Shell_SecondaryTrayWnd" || cls == L"TopLevelWindowForOverflowXamlIsland" ||
           cls == L"NotifyIconOverflowWindow";
}

// 像弹出菜单的窗口：系统画的菜单，或者没有标题栏的工具窗口（自己画菜单的程序，比如 Qt、网页做的界面）
bool MenuLike(HWND h) {
    if (!h || !IsWindowVisible(h)) return false;
    if (GetClassNameStr(h) == L"#32768") return true;
    LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE), ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    return (style & WS_POPUP) && (style & WS_CAPTION) != WS_CAPTION && (ex & WS_EX_TOOLWINDOW);
}

bool AnyMenuShowing() {
    for (HWND h = FindWindowExW(nullptr, nullptr, L"#32768", nullptr); h; h = FindWindowExW(nullptr, h, L"#32768", nullptr))
        if (IsWindowVisible(h)) return true;
    HWND overflow = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr);
    return overflow && IsWindowVisible(overflow);
}

// 右键托盘图标以后每隔一会儿看一眼：还在系统托盘里点、程序的菜单或者展开的隐藏图标还开着就接着等。
// 都关了以后：前台是正经窗口（从菜单里打开了窗口，或者点了别处）就收起；
// 前台留在程序藏着的窗口、系统托盘上（菜单取消了）就把前台拿回来，迷你任务栏接着用。
// 拿不回来（系统不让）就接着看：之后点了迷你任务栏照常用，前台换成正经窗口时收起
void WatchTrayMenu() {
    if (!s_open || !s_trayMode) {
        EndTrayMode();
        return;
    }
    DWORD now = GetTickCount();
    HWND fg = GetForegroundWindow();
    bool busy = Tray_Busy() && now - s_trayModeAt < kTrayBusyMaxMs;
    if (busy || AnyMenuShowing() || (fg != s_hwnd && MenuLike(fg))) {
        s_trayIdleAt = 0;
        return;
    }
    if (!s_trayIdleAt) s_trayIdleAt = now ? now : 1;
    if (now - s_trayIdleAt < kTrayIdleMs) return;  // 点完了，程序的菜单可能还没出来
    if (s_trayHot >= 0) {  // 菜单关了，那一格不再亮着
        s_trayHot = -1;
        Redraw();
    }
    if (fg == s_hwnd) {
        EndTrayMode();
        return;
    }
    if (fg && IsWindowVisible(fg) && !IsShellUi(fg)) {
        Log(L"托盘：菜单关了，前台换成 %ls，收起迷你任务栏", GetClassNameStr(fg).c_str());
        EndTrayMode();
        Popup_Hide();
        return;
    }
    if (s_trayRefocused) return;
    s_trayRefocused = true;
    ForceForeground(s_hwnd);
    bool back = GetForegroundWindow() == s_hwnd;
    Log(L"托盘：菜单关了（前台是 %ls），迷你任务栏%ls", fg ? GetClassNameStr(fg).c_str() : L"空的",
        back ? L"拿回前台接着用" : L"没拿回前台，等用户点它或者点别处");
    if (back) {
        EndTrayMode();
        SetFocus(s_hwnd);
    }
}

// 在那一格上方弹迷你任务栏自己的菜单，迷你任务栏留着；选了一项再收起。
// offerAppMenu = 加一项“程序菜单”（系统托盘里刚弹不出来时就不加了）
void ShowTrayMenu(int cell, const TrayApp& app, bool offerAppMenu) {
    EndTrayMode();
    if (cell < 0 || cell >= TrayCells() || s_tray[cell].exe != app.exe) {
        cell = -1;
        for (int i = 0; i < TrayCells(); ++i)
            if (s_tray[i].exe == app.exe) cell = i;
    }
    POINT at;
    if (cell >= 0 && TrayOnPage(cell)) {
        at = MenuAnchor(TrayCellRect(cell));
        ClientToScreen(s_hwnd, &at);
    } else {
        GetCursorPos(&at);
    }
    s_trayHot = cell;
    s_menuOpen = true;
    HideThumb();
    Redraw();
    bool appMenu = Tray_Menu(app, s_hwnd, at, [] { HideNow(); }, offerAppMenu, MenuAlign());
    s_menuOpen = false;
    s_trayHot = -1;
    Redraw();
    if (appMenu && s_open) TrayAppMenu(cell, app);
}

bool IsStartUi(HWND h) { return h && GetClassNameStr(h) == L"Windows.UI.Core.CoreWindow"; }

// 弹出动画放完：打开开始菜单。藏着的系统任务栏不放出来，迷你任务栏留着，开始菜单关了再收起
void BeginStart() {
    s_startPending = false;
    if (!s_open) return;
    s_startMode = true;
    s_startSeen = false;
    s_startAt = GetTickCount();
    Fullscreen_QuietStartMenu();
    SendStartMenu();
    SetTimer(s_hwnd, kTimerStart, kStartWatchMs, nullptr);
}

void NoteStartSeen(HWND start) {
    if (s_startSeen) return;
    s_startSeen = true;
    RECT r = {};
    GetWindowRect(start, &r);
    Log(L"开始菜单出来了（%ld,%ld - %ld,%ld），迷你任务栏面板顶边在 %d", r.left, r.top, r.right, r.bottom,
        s_pos.y + static_cast<int>(s_L.margin));
}

// 开始菜单关了（按 Win、Esc，点了别处，或者从里面打开了程序）：迷你任务栏一起收起。
// 前台退回到迷你任务栏上的话（开始菜单是在它前台时打开的），交还给原来的窗口
void StartClosed(HWND fg) {
    Log(L"开始菜单关了（前台换成 %ls），收起迷你任务栏", fg == s_hwnd ? L"迷你任务栏" : GetClassNameStr(fg).c_str());
    EndStartMode();
    Popup_Hide();
    if (fg == s_hwnd && s_prevForeground && IsWindow(s_prevForeground)) ForceForeground(s_prevForeground);
}

// 开始菜单开着时每隔一会儿看一眼前台
void WatchStart() {
    if (!s_open || !s_startMode) {
        EndStartMode();
        return;
    }
    HWND fg = GetForegroundWindow();
    bool waited = GetTickCount() - s_startAt > kStartWaitMs;
    if (IsStartUi(fg)) {
        NoteStartSeen(fg);
    } else if (s_startSeen) {
        StartClosed(fg);
    } else if (waited) {
        // 开始菜单一直没出来：迷你任务栏还在前台就接着当平常的用，前台已经换成别的窗口就收起
        Log(L"补发 Win 以后开始菜单没出来");
        if (fg == s_hwnd) EndStartMode();
        else StartClosed(fg);
    }
}

LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            if (s_preview) return HTTRANSPARENT;  // 预览时鼠标穿过去，还是在设置窗口上拖滑块
            break;

        case WM_MOUSEACTIVATE:
            if (s_preview) return MA_NOACTIVATEANDEAT;  // 预览时点到它也不抢前台
            break;

        case WM_ACTIVATE:
            if (s_open && s_trayMode) {
                // 右键了托盘图标：系统托盘、程序的菜单拿走前台时不收起，WatchTrayMenu 等菜单关了再看。
                // 又回到迷你任务栏上（点了它）就接着当平常的用
                if (LOWORD(wParam) != WA_INACTIVE && !Tray_Busy()) {
                    EndTrayMode();
                    SetFocus(hwnd);
                    Redraw();
                }
                return 0;
            }
            if (s_open && s_startMode) {
                // 开始菜单拿走了前台：看着它，它关了再收起
                if (LOWORD(wParam) == WA_INACTIVE) {
                    HWND to = reinterpret_cast<HWND>(lParam);
                    if (IsStartUi(to)) NoteStartSeen(to);
                    return 0;
                }
                if (!s_startSeen) return 0;
                if (LOWORD(wParam) == WA_CLICKACTIVE || MouseButtonDown()) {
                    // 开始菜单开着时点了迷你任务栏：开始菜单自己关了，接着当平常的迷你任务栏用
                    s_startClosedAt = NowMs();
                    EndStartMode();
                    SetFocus(hwnd);
                } else {
                    StartClosed(hwnd);  // 开始菜单关了，前台退回到迷你任务栏上
                }
                return 0;
            }
            if (LOWORD(wParam) == WA_INACTIVE && s_open) {
                HWND to = reinterpret_cast<HWND>(lParam);
                if (!to) to = GetForegroundWindow();
                // 刚弹出就失去前台，又没按着鼠标键（不是用户点了别处）：点别处收起后再弹出时，
                // 前台可能马上又被刚才点过的窗口拿回去，看起来就像这次长按没反应。抢回来一次
                if (!s_refocused && NowMs() - s_shownAt < kRefocusMs && !MouseButtonDown()) {
                    s_refocused = true;
                    Log(L"迷你任务栏刚弹出就失去前台（换成 %ls %ls），抢回来", GetClassNameStr(to).c_str(),
                        GetProcessPath(to).c_str());
                    SetTimer(hwnd, kTimerRefocus, kRefocusDelayMs, nullptr);
                    return 0;
                }
                Popup_Hide();
            }
            return 0;

        case WM_APP_FRAME:
            s_framePosted = false;
            Frame();
            return 0;

        case WM_TIMER:
            if (wParam == kTimerClock) {
                if (s_levels && s_drag != kVolumeTrack) RefreshVolume();  // 别处改了音量（键盘上的音量键）也跟着变
                Redraw();
            } else if (wParam == kTimerRefocus) {
                KillTimer(hwnd, kTimerRefocus);
                if (s_open && GetForegroundWindow() != hwnd) {
                    if (!SetForegroundWindow(hwnd)) ForceForeground(hwnd);
                    if (GetForegroundWindow() == hwnd) {
                        SetFocus(hwnd);
                    } else {
                        Log(L"迷你任务栏没抢回前台，收起");
                        Popup_Hide();
                    }
                }
            } else if (wParam == kTimerPreview) {
                KillTimer(hwnd, kTimerPreview);
                if (s_preview) Popup_Hide();
            } else if (wParam == kTimerThumb) {
                if (s_thumbWant) ShowThumbNow();
                else HideThumb();
            } else if (wParam == kTimerStart) {
                WatchStart();
            } else if (wParam == kTimerTrayMenu) {
                WatchTrayMenu();
            } else if (wParam == kTimerTray) {
                if (g_settings.trayIcons && !s_open) Tray_Refresh();
            }
            return 0;

        case WM_APP_TRAY_MENU: {
            std::unique_ptr<TrayApp> app(reinterpret_cast<TrayApp*>(lParam));
            if (app && s_open && !s_preview) ShowTrayMenu(s_trayHot, *app, false);
            else if (app) Log(L"托盘：迷你任务栏已经收起，不再弹 %ls 的菜单", app->name.c_str());
            return 0;
        }

        case WM_APP_APPS:
            if (s_open && !s_filter.empty()) ApplyFilter();  // 应用列表刚读完：把匹配的应用补上
            return 0;

        case WM_APP_RADIOS:
            if (s_open) {
                s_radioFailed |= static_cast<int>(wParam);
                for (int i = 0; i < kRadioCount; ++i) s_radio[i] = Radios_Get(i);
                Redraw();
            }
            return 0;

        case WM_APP_BRIGHTNESS:
            if (reinterpret_cast<HMONITOR>(lParam) == s_monitor && s_drag != kBrightTrack &&
                !(s_brightnessEdited && static_cast<INT_PTR>(wParam) >= 0)) {
                int value = static_cast<int>(static_cast<INT_PTR>(wParam));
                s_brightness = value < 0 ? -1 : std::min(value, 100);
                if (value >= 0) RememberBrightness(s_monitor, s_brightness);
                Redraw();
            }
            return 0;

        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            if (s_drag != kNone) {
                SetSliderAt(SliderOf(s_drag), x, y);
                return 0;
            }
            // 正在用键盘选时，位置没变的是系统补发的（比如缩略图窗口出现了、面板重新居中），不是用户在动鼠标：
            // 别打乱键盘选中的项。按屏幕坐标比，面板自己挪了位置时光标没动也算没变。
            // 不是键盘选的时候照常处理：关掉一个窗口、面板重新居中以后，悬停的格子要跟着光标下面的格子走
            POINT screen = {x, y};
            ClientToScreen(hwnd, &screen);
            if (s_keyNav && screen.x == s_lastMouse.x && screen.y == s_lastMouse.y) return 0;
            s_lastMouse = screen;
            s_keyNav = false;
            int hit = HitTest(x, y);
            int control = hit < 0 ? ControlAt(x, y) : kNone;
            int radio = hit < 0 && control == kNone ? RadioAt(x, y) : -1;
            int tray = hit < 0 && control == kNone && radio < 0 ? TrayAt(x, y) : -1;
            bool overTray = hit < 0 && control == kNone && radio < 0 && OverTray(x, y);
            if (hit != s_hover || control != s_hoverControl || radio != s_hoverRadio || tray != s_hoverTray ||
                overTray != s_overTray) {
                if (radio != s_hoverRadio && s_hoverRadio >= 0) s_radioFailed &= ~(1 << s_radioIds[s_hoverRadio]);
                s_hover = hit;
                s_hoverControl = control;
                s_hoverRadio = radio;
                s_hoverTray = tray;
                s_overTray = overTray;
                if (hit >= 0) s_sel = hit;
                Redraw();
            }
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE:
            s_lastMouse = {INT_MIN, INT_MIN};  // 回来时哪怕在同一个位置也要重新算
            s_hover = -1;
            s_hoverControl = kNone;
            s_hoverRadio = -1;
            s_radioFailed = 0;
            s_hoverTray = -1;
            s_overTray = false;
            Redraw();
            return 0;

        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam), control = ControlAt(x, y);
            if ((control == kVolumeTrack || control == kBrightTrack) && SliderEnabled(SliderOf(control))) {
                s_drag = control;
                SetCapture(hwnd);
                SetSliderAt(SliderOf(control), x, y);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (s_drag != kNone) {
                s_drag = kNone;
                ReleaseCapture();
                Redraw();
                return 0;
            }
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            int control = ControlAt(x, y);
            if (control == kVolumeIcon && s_hasVolume) {
                s_muted = !s_muted;
                Volume_SetMute(s_muted);
                Redraw();
                return 0;
            }
            if (int radio = RadioAt(x, y); radio >= 0) {
                ToggleRadio(radio);
                return 0;
            }
            int hit = HitTest(x, y);
            if (hit >= 0) ActivateIndex(hit, (wParam & MK_SHIFT) != 0);
            else ActivateTray(TrayAt(x, y), false);
            return 0;
        }

        case WM_CAPTURECHANGED:
            if (s_drag != kNone && reinterpret_cast<HWND>(lParam) != hwnd) {
                s_drag = kNone;
                Redraw();
            }
            return 0;

        case WM_MBUTTONUP:
            CloseItem(HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)));
            return 0;

        case WM_RBUTTONUP: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int hit = HitTest(pt.x, pt.y);
            if (hit >= 0) ShowItemMenu(hit, pt);
            else if (int radio = RadioAt(pt.x, pt.y); radio >= 0) OpenRadioPanel(radio);
            else ActivateTray(TrayAt(pt.x, pt.y), true);
            return 0;  // 不交给 DefWindowProc，免得它再发一次 WM_CONTEXTMENU
        }

        case WM_CONTEXTMENU:
            // 鼠标右键在上面处理了；这里是菜单键（DefWindowProc 在它松开时发来，坐标是 -1, -1）
            if (GET_X_LPARAM(lParam) == -1 && GET_Y_LPARAM(lParam) == -1 && s_open) ShowSelectedMenu();
            return 0;

        case WM_SYSKEYDOWN:
            // Shift+F10 和菜单键一样（F10 是系统键，不走 WM_KEYDOWN）。自己处理掉，不让 DefWindowProc 再发 WM_CONTEXTMENU、
            // 松开 F10 时进菜单模式
            if (wParam == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000)) {
                if (s_open) ShowSelectedMenu();
                return 0;
            }
            break;

        case WM_MOUSEWHEEL: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};  // 滚轮消息给的是屏幕坐标
            ScreenToClient(hwnd, &pt);
            int control = ControlAt(pt.x, pt.y);
            s_wheelRest += GET_WHEEL_DELTA_WPARAM(wParam);
            int up = s_wheelRest / WHEEL_DELTA;
            s_wheelRest -= up * WHEEL_DELTA;
            if (up == 0) return 0;
            if (control != kNone) {
                NudgeSlider(SliderOf(control), up);
            } else if (OverTray(pt.x, pt.y) && TrayScrolls()) {
                FlipTray(-up);  // 往下滚是下一页（往后滚）
            } else if (ItemCount() > s_visible) {
                s_first = std::max(0, std::min(s_first - up, ItemCount() - s_visible));
                // 格子在鼠标下面挪了：重新看鼠标下是哪个，缩略图也按新位置摆
                s_hover = HitTest(pt.x, pt.y);
                if (s_hover >= 0) {
                    s_sel = s_hover;
                    s_keyNav = false;
                }
                s_thumbIdx = -1;
                Redraw();
            }
            return 0;
        }

        case WM_KEYDOWN:
            switch (wParam) {
                case VK_LEFT:
                    if (!FlipTrayUnderMouse(-1)) MoveSelection(-1);
                    break;
                case VK_RIGHT:
                    if (!FlipTrayUnderMouse(1)) MoveSelection(1);
                    break;
                case VK_UP:  // 竖排时上下键选
                    if (Vertical()) MoveSelection(-1);
                    break;
                case VK_DOWN:
                    if (Vertical()) MoveSelection(1);
                    break;
                case VK_TAB: MoveSelection((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1); break;
                case VK_HOME: MoveSelection(-s_sel); break;
                case VK_END: MoveSelection(ItemCount() - s_sel); break;
                case VK_SPACE:
                    if (!s_filter.empty()) break;  // 打字筛选时空格是要打的字，交给 WM_CHAR
                    [[fallthrough]];
                case VK_RETURN:
                    if (!s_filter.empty() && s_items.empty()) break;  // 没有匹配的
                    ActivateIndex(s_sel, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                    break;
                case VK_DELETE:
                    if (s_sel > 0 && s_sel <= ItemCount() && s_items[s_sel - 1].hwnd) CloseItem(s_sel);
                    break;
                case VK_ESCAPE:
                    if (!s_filter.empty()) {  // 先清掉打的字
                        s_filter.clear();
                        ApplyFilter();
                        break;
                    }
                    // 先收起再把焦点交还给原来的窗口，随后的失活消息就不会当成“刚弹出就失去前台”
                    Popup_Hide();
                    if (s_prevForeground && IsWindow(s_prevForeground)) ForceForeground(s_prevForeground);
                    break;
            }
            return 0;

        case WM_CHAR: {
            if (s_compact) return 0;  // 开始菜单模式没有显示打的字的地方
            wchar_t ch = static_cast<wchar_t>(wParam);
            if (ch == L'\b') {
                if (s_filter.empty()) return 0;
                s_filter.pop_back();
                if (!s_filter.empty() && IS_HIGH_SURROGATE(s_filter.back())) s_filter.pop_back();
            } else if (ch == 0x7F) {  // Ctrl+Backspace：全删掉
                if (s_filter.empty()) return 0;
                s_filter.clear();
            } else if (ch >= 0x20 && !(ch == L' ' && s_filter.empty())) {
                s_filter += ch;
            } else {
                return 0;  // 回车、Esc、Tab 之类在 WM_KEYDOWN 里处理了
            }
            ApplyFilter();
            return 0;
        }

        case WM_DPICHANGED:
            return 0;  // 尺寸由 Popup_Show 按目标显示器自己算

        case WM_CLOSE:
            Popup_Hide();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 固定的应用按任务栏顺序排在前面（已打开的换成它的各个窗口），其余窗口跟在后面
std::vector<Tile> BuildTiles(const std::vector<WindowEntry>& windows) {
    std::vector<Tile> tiles;
    std::vector<bool> used(windows.size(), false);
    if (g_settings.showPinnedApps) {
        for (const PinnedApp& pin : LoadPinnedApps(static_cast<int>(std::lround(Px(32))))) {
            bool running = false;
            for (size_t i = 0; i < windows.size(); ++i) {
                if (used[i] || !PinMatchesWindow(pin, windows[i])) continue;
                used[i] = running = true;
                const WindowEntry& w = windows[i];
                tiles.push_back({w.title, w.icon ? w.icon : pin.icon, w.hwnd, w.active, pin.launch, pin.name, pin.icon});
            }
            if (!running) tiles.push_back({pin.name, pin.icon, nullptr, false, pin.launch, pin.name, pin.icon});
        }
    }
    for (size_t i = 0; i < windows.size(); ++i) {
        if (used[i]) continue;
        const WindowEntry& w = windows[i];
        tiles.push_back({w.title, w.icon, w.hwnd, w.active, L"", L"", nullptr});
    }
    for (Tile& t : tiles) {
        std::wstring exe = t.hwnd ? WindowExeName(t.hwnd) : NormalizeExeName(t.launch);
        t.search = ToLower(t.title + L"\n" + t.pinName + L"\n" + exe);
    }
    return tiles;
}

}  // namespace

void Popup_Init() {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = PopupProc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    s_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName, L"TaskbarPopup", WS_POPUP,
                             0, 0, 0, 0, nullptr, nullptr, g_instance, nullptr);
    if (s_hwnd) {
        // 动画自己画；系统的淡入淡出会和它叠在一起，收起后马上截背景时还会截到残影
        BOOL disable = TRUE;
        DwmSetWindowAttribute(s_hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));
    }

    for (const wchar_t* name : {L"Microsoft YaHei UI", L"Segoe UI"}) {
        s_fontFamily = std::make_unique<FontFamily>(name);
        if (s_fontFamily->GetLastStatus() == Ok && s_fontFamily->IsAvailable()) break;
        s_fontFamily.reset();
    }
    if (!s_fontFamily) s_fontFamily.reset(FontFamily::GenericSansSerif()->Clone());

    // 托盘图标在后台读，之后每隔一会儿再读一遍
    if (g_settings.trayIcons) Tray_Refresh();
    // Wi-Fi 和蓝牙也先读一遍：第一次弹出时就知道这台电脑上有没有
    if (g_settings.showRadios && s_hwnd) Radios_Query(s_hwnd);
    if (s_hwnd) SetTimer(s_hwnd, kTimerTray, kTrayRefreshMs, nullptr);

    // 提前读一遍固定的应用和图标，第一次弹出时就不卡
    if (g_settings.showPinnedApps) {
        POINT origin = {0, 0};
        s_scale = PopupScale(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY));
        LoadPinnedApps(static_cast<int>(std::lround(Px(32))));
    }
}

void Popup_Destroy() {
    Thumb_Destroy();
    s_items.clear();
    s_allItems.clear();
    s_tray.clear();
    Pinned_ClearCache();
    s_fontFamily.reset();
    ReleaseCanvas();
    if (s_hwnd) DestroyWindow(s_hwnd);
    s_hwnd = nullptr;
}

namespace {

// 弹出。preview：设置窗口里调大小时的预览，不抢前台。
// startMenu：任务栏藏着时短按了 Win，只留一行格子（竖排时一列）贴着屏幕边弹出来，弹完打开开始菜单
void Open(bool preview, bool startMenu = false) {
    if (!s_hwnd) return;
    s_preview = preview;
    s_edge = preview ? s_previewEdge : g_settings.popupEdge;
    KillTimer(s_hwnd, kTimerPreview);
    EndStartMode();
    s_compact = startMenu;
    s_startPending = startMenu;
    HWND fg = GetForegroundWindow();
    if (fg != s_hwnd) s_prevForeground = fg;

    // 弹在鼠标所在的显示器上；任务栏露着时弹在它上面，藏起来时贴着屏幕底边
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    s_bounds = Taskbar_IsShownOn(mon) ? mi.rcWork : mi.rcMonitor;
    s_scale = PopupScale(mon);

    // 音量马上就能读到；亮度在后台读，读到之前先显示上次的值
    s_levels = g_settings.showLevels && !s_compact;
    s_monitor = mon;
    s_brightness = -2;
    for (const auto& known : s_knownBrightness)
        if (known.first == mon) s_brightness = known.second;
    if (s_levels) {
        RefreshVolume();
        Brightness_Query(s_hwnd, mon);
    }
    s_brightnessEdited = false;
    s_hoverControl = kNone;
    s_drag = kNone;
    s_wheelRest = 0;

    // Wi-Fi 和蓝牙先用上次读到的（启动时就读过一遍），这台电脑上没有的不放按钮；弹出以后再读一遍
    s_radioCount = 0;
    if (g_settings.showRadios && !s_compact) {
        for (int i = 0; i < kRadioCount; ++i) {
            s_radio[i] = Radios_Get(i);
            if (s_radio[i].status != kRadioMissing) s_radioIds[s_radioCount++] = i;
        }
        if (!preview) Radios_Query(s_hwnd);
    }
    s_hoverRadio = -1;
    s_radioFailed = 0;

    // 托盘图标用后台读好的（面板宽度弹出时就定了，弹出期间读到的新结果留到下次）
    s_tray = g_settings.trayIcons ? Tray_Hidden() : std::vector<TrayApp>();
    s_trayStyle = g_settings.trayStyle;
    s_hoverTray = -1;
    s_overTray = false;
    s_trayPage = 0;
    s_trayFirst = 0;
    s_flipFrom = -1;
    EndTrayMode();
    if (g_settings.trayIcons && !preview) Tray_Refresh();

    s_allItems = BuildTiles(EnumerateWindows(s_prevForeground, static_cast<int>(std::lround(Px(32)))));
    s_items = s_allItems;
    s_filter.clear();
    s_lockedItemsW = 0;
    s_keyNav = false;
    s_menuOpen = false;
    s_lastMouse = {INT_MIN, INT_MIN};
    if (!preview) Apps_Refresh(s_hwnd);  // 打字筛选时要搜的所有应用，在后台先读好
    ComputeLayout();
    UpdatePlacement();

    // 截下玻璃后面的屏幕内容：这时弹窗自己不能在屏幕上
    if (IsWindowVisible(s_hwnd)) {
        ShowWindow(s_hwnd, SW_HIDE);
        WaitForVBlank();
    }
    RECT window = {s_pos.x, s_pos.y, s_pos.x + s_width, s_pos.y + s_height};
    if (preview) {
        // 预览时大小会一直变：按最大的大小把底边（竖排时左边或右边）一整条截下来，变大小时不用再截
        float grow = static_cast<float>(kMaxPopupScale) / std::max(1, s_previewPercent);
        LONG w = static_cast<LONG>(std::ceil(s_width * grow)), h = static_cast<LONG>(std::ceil(s_height * grow));
        if (s_edge == kEdgeLeft) window = {s_bounds.left, s_bounds.top, s_bounds.left + w, s_bounds.bottom};
        else if (s_edge == kEdgeRight) window = {s_bounds.right - w, s_bounds.top, s_bounds.right, s_bounds.bottom};
        else window = {s_bounds.left, s_bounds.bottom - h, s_bounds.right, s_bounds.bottom};
    }
    s_glass.SetLight(preview ? s_previewLight : ThemeIsLight(g_settings.theme, true));
    s_glass.SetFrosted(preview ? s_previewFrosted : g_settings.glassStyle == kGlassFrosted);
    s_glass.SetTransparency(preview ? s_previewTransparency : g_settings.glassTransparency);
    s_glass.Capture(window, s_scale);

    s_first = 0;
    s_hover = -1;
    s_sel = DefaultSelection();
    EnsureVisible(s_sel);
    s_pillIndex = -1;

    s_open = true;
    s_shownAt = NowMs();
    s_refocused = false;
    s_anim = Anim::Showing;
    s_animStart = s_shownAt;
    Render(SlideDistance(), 0);
    if (preview) {
        ShowWindow(s_hwnd, SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(s_hwnd, SW_SHOW);
        ForceForeground(s_hwnd);
        if (GetForegroundWindow() != s_hwnd)
            Log(L"迷你任务栏没拿到前台，前台是 %ls %ls", GetClassNameStr(GetForegroundWindow()).c_str(),
                GetProcessPath(GetForegroundWindow()).c_str());
        SetFocus(s_hwnd);
    }
    RequestFrame();
    SetTimer(s_hwnd, kTimerClock, 1000, nullptr);
}

}  // namespace

void Popup_Show() { Open(false); }

void Popup_PreviewGlass(bool frosted, bool light, int transparency) {
    bool restyle = frosted != s_previewFrosted;
    s_previewFrosted = frosted;
    s_previewLight = light;
    s_previewTransparency = transparency;
    if (!s_hwnd || !s_open || !s_preview) return;
    if (restyle) {  // 毛玻璃模糊得更厉害，要重新截背景：收起，接下来的 Popup_Preview 再弹出来
        HideNow();
        return;
    }
    s_glass.SetLight(light);
    s_glass.SetTransparency(transparency);
    s_baseValid = false;
    Redraw();
}

void Popup_Preview(int percent, int edge, int align, bool hold) {
    if (!s_hwnd || (s_open && !s_preview)) return;  // 真的弹出着就不打扰
    percent = std::clamp(percent, kMinPopupScale, kMaxPopupScale);
    if (s_open && edge != s_previewEdge) HideNow();  // 换了边：重新截背景，从新的那条边弹出来
    if (!s_open) {
        s_previewPercent = percent;
        s_previewEdge = edge;
        s_previewAlign = align;
        Open(true);
    } else if (percent != s_previewPercent || align != s_previewAlign) {
        s_previewPercent = percent;
        s_previewAlign = align;
        s_scale = PopupScale(s_monitor);
        ComputeLayout();
        UpdatePlacement();
        s_glass.SetScale(s_scale);
        Redraw();
    }
    KillTimer(s_hwnd, kTimerPreview);
    if (!hold) SetTimer(s_hwnd, kTimerPreview, kPreviewHideMs, nullptr);
}

void Popup_EndPreview() {
    if (s_preview && s_open) Popup_Hide();
}

void Popup_Hide() {
    if (!s_open) return;
    EndStartMode();
    EndTrayMode();
    s_open = false;
    bool atRest = s_anim == Anim::None;
    s_hideFromSlide = atRest ? 0 : s_lastSlide;
    s_hideFromAlpha = atRest ? 255 : s_lastAlpha;
    s_anim = Anim::Hiding;
    s_animStart = NowMs();
    KillTimer(s_hwnd, kTimerClock);
    RequestFrame();
}

void Popup_Toggle() {
    if (s_open) Popup_Hide();
    else Popup_Show();
}

void Popup_StartMenu(DWORD vk) {
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    // 任务栏已经放出来了、这块屏的任务栏露着，或者开始菜单已经开着（这一下是关掉它）：照常交给系统
    if (!s_hwnd || !Fullscreen_TaskbarHidden() || Taskbar_IsShownOn(mon) || IsStartUi(GetForegroundWindow()) ||
        s_startMode) {
        SendWinTap(vk);
        return;
    }
    if (s_open && !s_preview) {
        ActivateIndex(0);  // 迷你任务栏已经开着：和点它上面的开始按钮一样
        return;
    }
    Log(L"任务栏藏着时短按 Win：先弹出迷你任务栏，再打开开始菜单");
    Open(false, true);
}

}  // namespace app
