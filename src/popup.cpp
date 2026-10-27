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
// 时钟前面还有正在播放的媒体（封面在上、歌名、上一首 / 播放暂停 / 下一首在下）、输入法的中英文（点一下切换）和电量，
// 都可以在设置里关掉。
// 设置里打开“鼠标停在屏幕边上时弹出”时，任务栏藏着、鼠标在迷你任务栏那条边上停一下就弹出（不抢前台），
// 没点过它、鼠标走开就收起。
// 拖面板的边（不贴着屏幕的那几条）像拖窗口边框一样改变大小，双击边恢复原来的大小。
// 材质有液态玻璃和 Win11 原版（平的亚克力，细边框，高亮块是平的，字更清楚），圆角大小在设置里调。
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
constexpr UINT_PTR kTimerEdge = 9;      // 鼠标停在屏幕边上弹出：看鼠标在不在边上
constexpr UINT kEdgeWatchMs = 80;
constexpr DWORD kEdgeDwellMs = 350;     // 在边上停这么久才弹
constexpr UINT_PTR kTimerHoverLeave = 10;  // 鼠标停在边上弹出的：看鼠标走开了没有
constexpr UINT kHoverWatchMs = 100;
constexpr DWORD kHoverLeaveMs = 600;    // 走开这么久就收起

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
// Win11 原版：底色浓，字不透明、不加阴影，次要的字也深一些
const Palette kWin11Dark = {Color(255, 255, 255, 255), Color(205, 255, 255, 255), Color(45, 255, 255, 255),
                            Color(28, 255, 255, 255),  Color(14, 255, 255, 255),  Color(60, 255, 255, 255),
                            Color(200, 255, 255, 255), Color(255, 96, 205, 255),  Color(0, 0, 0, 0),
                            Color(90, 255, 255, 255),  Color(255, 69, 69, 69),    Color(70, 255, 255, 255)};
const Palette kWin11Light = {Color(255, 26, 26, 26),   Color(195, 26, 26, 26),    Color(40, 0, 0, 0),
                             Color(26, 0, 0, 0),       Color(10, 0, 0, 0),        Color(200, 255, 255, 255),
                             Color(150, 26, 26, 26),   Color(255, 0, 95, 184),    Color(0, 0, 0, 0),
                             Color(70, 0, 0, 0),       Color(255, 255, 255, 255), Color(45, 0, 0, 0)};

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
    // 时钟前面的几样：媒体（封面、歌名、三个按钮）、输入法、电量，这次没有的长度为 0。横排时 X 是左边、W 是宽，竖排时 Y 是顶边、H 是高
    float mediaX, mediaW, imeX, imeW, battX, battW;
    float mediaY, mediaH, imeY, imeH, battY, battH;
    // 横排、有调节条那一行时，媒体占格子和调节条两行（调节条让出这一列），Wi-Fi 蓝牙放得下就放到媒体另一边
    // （窗口格子不在的那一边）的下面
    bool mediaTall, radiosOther;
    // 各模块（Module）沿排列方向的起点和长度，这次没有的长度为 0
    float modPos[kModuleCount], modLen[kModuleCount];
    // 要画的分隔线：在这些位置前面半个间隔处
    float sepAt[kModuleCount];
    int sepCount;
    float topGap;  // 横排、不显示标题行时面板上方给显示名字的小牌子留的地方
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
float s_tileGrow = 1;          // 格子那段有空时格子放大补位的倍数，见 FitTiles
bool s_keyNav = false;         // 最近是用键盘选的（选中项也显示缩略图）
bool s_menuOpen = false;
POINT s_lastMouse = {INT_MIN, INT_MIN};  // 上一次 WM_MOUSEMOVE 的位置：窗口出现、移动时系统会补发一条位置没变的
HWND s_thumbWant = nullptr;    // 想显示缩略图的窗口
HWND s_thumbShown = nullptr;   // 正显示着缩略图的窗口
int s_thumbIdx = -1;
HWND s_prevForeground = nullptr;
RECT s_bounds = {};  // 弹窗摆放的范围：任务栏露着时是工作区，藏起来时是整块屏幕
int s_trayRowMax = 8;  // 一行大图标时托盘图标最多同时显示几个（竖排时屏幕放不下就少显示几个）
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
PopupLook s_previewLook;            // 预览用的外观，见 Popup_PreviewLook
PopupLook s_look;                   // 这次的外观（弹出时按设置或预览定下来）
int s_edge = kEdgeBottom;           // 这次弹在哪条边（弹出时按设置定下来）
int s_align = kAlignSystem;         // 这次在底边时怎么对齐（弹出时按设置定下来）
int s_offset = 500;                 // 自定义位置时面板中心在边上的千分之几处
double s_shownAt = 0;      // 这次弹出的时间
// 编辑模式（弹出后按 F2）：拖面板的边改大小（粗细）和长度，别的点击不管用，回车、Esc 或再按 F2 退出
bool s_editing = false;
enum ResizeEdge { kResizeLeft = 1, kResizeTop = 2, kResizeRight = 4, kResizeBottom = 8 };
int s_resize = 0;          // 正在拖的边（ResizeEdge 组合），0 = 没在拖
int s_resizeFrom = 100;    // 开始拖时的大小（百分比）和长度（千分之几）
int s_resizeLenFrom = 0;
float s_resizeDist = 1;    // 开始拖时鼠标离固定的那条屏幕边多远（改大小用）
float s_resizeLen = 0;     // 开始拖时面板的长度，像素（改长度用）
POINT s_resizeAt = {};     // 开始拖时鼠标在屏幕上的位置
DWORD s_edgeClickAt = 0;   // 上一次在边上（或者挪面板的地方）按下的时间和位置：双击恢复
POINT s_edgeClickPt = {};
// 编辑模式里鼠标下、正在拖的东西：模块（Module）、调节条、Wi-Fi 蓝牙按钮，或者边、别的地方（挪面板）
enum EditPart { kPartNone = -1, kPartLevels = kModuleCount, kPartRadios, kPartScaleEdge, kPartLengthEdge, kPartPanel };
int s_editHover = kPartNone;
int s_moveModule = kPartNone;  // 正在拖着换位置的模块（调节条、Wi-Fi 蓝牙只能拖出去隐藏）
bool s_moveOut = false;        // 拖到了面板外面：松开就隐藏
bool s_movePanel = false;      // 正在拖面板挪位置
POINT s_moveAt = {};           // 开始拖时鼠标在屏幕上的位置
POINT s_moveFromPos = {};      // 开始拖面板时窗口的位置、对齐和自定义位置
int s_moveFromAlign = kAlignSystem, s_moveFromOffset = 500;
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
HMONITOR s_forceMonitor = nullptr;  // 不为空：这次弹在这块屏幕上（快捷键“在下一块屏幕弹出”）
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
int s_radioPolls = 0;              // 弹出以后为了等 Wi-Fi 连上网络又读了几遍

// 媒体按钮、输入法、电量。媒体按钮的编号就是 MediaCommand
enum Extra { kExtraNone = -1, kExtraIme = 3, kExtraBattery, kExtraCover };  // kExtraCover：封面和歌名
constexpr float kCoverSize = 36;      // 横排时封面的边长
constexpr float kMediaColW = 112;     // 横排占两行时媒体那一列的宽
constexpr float kMediaRowW = 142;     // 横排只有一行时：封面在左，歌名和按钮在右
constexpr float kSideCoverSize = 40;  // 竖排时封面的边长，下面两行小字的歌名
constexpr float kSideTitleH = 26;
bool s_showMedia = false;  // 这次弹出有没有这几样（弹出时定下来）
bool s_showIme = false;
bool s_showBattery = false;
bool s_mediaOpen = false;  // 媒体展开了上一首、下一首（点封面切换，每次弹出都先收着，只留播放 / 暂停）
MediaInfo s_media;
ImeInfo s_ime;
BatteryInfo s_battery;
std::shared_ptr<Bitmap> s_coverFrom;  // 缩好的封面是从哪张来的
std::unique_ptr<Bitmap> s_coverScaled;  // 按这次的缩放缩好的封面，画的时候不用再缩
HWND s_imeTarget = nullptr;   // 显示的是哪个窗口的输入法（弹出前的前台窗口）
int s_hoverExtra = kExtraNone;

// 鼠标停在屏幕边上弹出
DWORD s_edgeSince = 0;     // 鼠标从什么时候起停在边上，0 = 不在
bool s_edgeArmed = true;   // 弹过一次以后要先离开那条边才会再弹
bool s_hoverOpen = false;  // 这次是鼠标停在边上弹出来的、还没点过：不抢前台，鼠标走开就收起
DWORD s_hoverAwayAt = 0;   // 鼠标走开的时间，0 = 还在上面

// 任务栏藏着时短按 Win：迷你任务栏弹完再打开开始菜单，开始菜单开着时它一直在
bool s_compact = false;       // 这次弹出只有一行格子、贴着屏幕底边，给开始菜单让地方
bool s_startPending = false;  // 弹出动画放完就打开开始菜单
bool s_startMode = false;     // 开始菜单打开了（或者正在打开），迷你任务栏跟着它收起
bool s_startSeen = false;     // 开始菜单到过前台
std::wstring s_startTyped;    // 开始菜单出来之前打的字（回车记成 \r），出来以后补发给它
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

// 读图标用的大小（像素）：格子边长的三分之二，图标大小调大了也清楚
int IconPx() { return static_cast<int>(std::lround(Px(32.0f * s_look.iconSize / 100))); }

// 显示器的缩放比例再乘上设置里的迷你任务栏大小
float PopupScale(HMONITOR monitor) {
    return MonitorScale(monitor) * (s_preview ? s_previewPercent : g_settings.popupScale) / 100.0f;
}

bool Win11Style() { return s_glass.Style() == kGlassWin11; }

// 强调色：自己选的，或者系统的
Color LookAccent() {
    if (!s_look.customAccent) return SystemAccent(s_glass.Light());
    return Color(255, GetRValue(s_look.accent), GetGValue(s_look.accent), GetBValue(s_look.accent));
}

const Palette& Colors() {
    static Palette pal;
    if (Win11Style()) pal = s_glass.Light() ? kWin11Light : kWin11Dark;
    else pal = s_glass.Light() ? kLight : kDark;
    pal.accent = LookAccent();
    return pal;
}

// 强调色底上的字、图标：浅的强调色上用深色，深的上用白色
Color OnAccent() {
    Color a = LookAccent();
    return ColorLuma(RGB(a.GetR(), a.GetG(), a.GetB())) > 150 ? Color(235, 0, 0, 0) : Color(255, 255, 255, 255);
}

// 圆角半径（像素，按 100% 大小算）
int PopupRadius() { return s_look.radius; }

// 横排、不显示标题行：名字显示在面板上方的小牌子里
bool BubbleTitle() { return s_edge == kEdgeBottom && !s_compact && !s_look.showTitle; }

// 弹在屏幕左右两边：竖着排，从侧边滑出来
bool Vertical() { return s_edge != kEdgeBottom; }
// 这次弹出的那条边用哪份“显示哪些、什么顺序”（底部、侧边各一份）。预览时用设置窗口里改着的
ModuleSet& Mods() { return (s_preview ? s_look.modules : g_settings.modules)[ModuleSideOf(s_edge)]; }

// 面板贴着的那条屏幕边有多长（横排是宽，竖排是高）
float ScreenSide() { return static_cast<float>(Vertical() ? s_bounds.bottom - s_bounds.top : s_bounds.right - s_bounds.left); }

// 编辑模式里拖出来的面板长度（像素），0 = 跟着内容
float CustomLength() { return g_settings.popupLength > 0 ? ScreenSide() * g_settings.popupLength / kMaxPopupLength : 0; }

const wchar_t* const kWeekdays[7] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};

// 时钟的文字：第一行时间，下面是日期和星期（横排时写在同一行；竖排时面板窄，日期只写月和日，星期另起一行）
std::wstring ClockText(const SYSTEMTIME& st) {
    wchar_t text[64];
    if (s_look.clockSeconds) swprintf(text, 64, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    else swprintf(text, 64, L"%02d:%02d", st.wHour, st.wMinute);
    std::wstring clock = text, below;
    if (s_look.clockDate) {
        if (Vertical()) swprintf(text, 64, L"%d/%d", st.wMonth, st.wDay);
        else swprintf(text, 64, L"%d/%d/%d", st.wYear, st.wMonth, st.wDay);
        below = text;
    }
    if (s_look.clockWeekday) below += (below.empty() ? L"" : Vertical() ? L"\n" : L" ") + std::wstring(Tr(kWeekdays[st.wDayOfWeek % 7]));
    return below.empty() ? clock : clock + L"\n" + below;
}

Font ClockFont() { return Font(s_fontFamily.get(), Px(11.5f), FontStyleRegular, UnitPixel); }

// 时钟那一块的大小：横排按最宽的一行（时间按 00:00 量，走字时宽度不变）定宽，竖排按行数定高
void ClockSize(float& w, float& h) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    st.wHour = st.wMinute = st.wSecond = 0;
    std::wstring text = ClockText(st);
    int lines = 1 + static_cast<int>(std::count(text.begin(), text.end(), L'\n'));
    h = Px(14) * lines + Px(12);  // 两行时 40
    Bitmap bmp(1, 1, PixelFormat32bppPARGB);
    Graphics g(&bmp);
    g.SetTextRenderingHint(Win11Style() ? TextRenderingHintAntiAliasGridFit : TextRenderingHintAntiAlias);
    Font font = ClockFont();
    StringFormat format(StringFormatFlagsNoWrap);
    RectF measured;
    g.MeasureString(text.c_str(), -1, &font, PointF(0, 0), &format, &measured);
    w = std::max(lines > 1 ? Px(72) : Px(56), measured.Width + Px(12));
}

// 输入法、电量、时钟挨着放时中间不要分隔线
bool StatusModule(int m) { return m == kModIme || m == kModBattery || m == kModClock; }

// 模块沿排列方向的长度（窗口格子那一段是 itemsW），这次没有的为 0
float ModuleLen(const Layout& L, int m) {
    bool v = Vertical();
    switch (m) {
        case kModTray: return L.trayW;
        case kModMedia: return v ? L.mediaH : L.mediaW;
        case kModIme: return v ? L.imeH : L.imeW;
        case kModBattery: return v ? L.battH : L.battW;
        case kModClock: return v ? L.clockH : L.clockW;
        default: return L.itemsW;
    }
}

bool ModuleShown(int m) { return m == kModWindows || ModuleLen(s_L, m) > 0; }

// 这次显示的模块按顺序排好，算出每个前面的间隔（第一个前面是和开始按钮之间的）
int ModuleSequence(const Layout& L, int seq[kModuleCount], float sepBefore[kModuleCount]) {
    int count = 0;
    for (int m : Mods().order)
        if (m == kModWindows || ModuleLen(L, m) > 0) seq[count++] = m;
    for (int i = 0; i < count; ++i)
        sepBefore[i] = i == 0 || !(StatusModule(seq[i - 1]) && StatusModule(seq[i])) ? L.sep : 0;
    return count;
}

// 从开始按钮后面起按顺序摆好各模块，记下要画的分隔线（挨着占两行的媒体的不画，它有自己的底）。返回最后一个的末端
float PlaceModules(Layout& L, float start) {
    int seq[kModuleCount];
    float sepBefore[kModuleCount];
    int count = ModuleSequence(L, seq, sepBefore);
    for (int m = 0; m < kModuleCount; ++m) L.modPos[m] = L.modLen[m] = 0;
    L.sepCount = 0;
    float at = start;
    for (int i = 0; i < count; ++i) {
        at += sepBefore[i];
        bool byMedia = L.mediaTall && (seq[i] == kModMedia || (i > 0 && seq[i - 1] == kModMedia));
        if (sepBefore[i] > 0 && !byMedia) L.sepAt[L.sepCount++] = at;
        L.modPos[seq[i]] = at;
        L.modLen[seq[i]] = ModuleLen(L, seq[i]);
        at += L.modLen[seq[i]];
    }
    return at;
}

// 编辑模式里开着却暂时没有内容的托盘（没有隐藏的图标）也占个位置，好拖着排顺序
bool TrayHolder() { return s_editing && s_tray.empty() && Mods().tray && !s_compact; }

void ComputeLayout() {
    Layout& L = s_L;
    bool vertical = Vertical();
    L.margin = Px(24);  // 给阴影留位置，也是面板离屏幕边的距离
    L.pad = s_compact ? Px(4) : Px(8);
    L.tile = Px(48.0f * s_look.iconSize / 100 * s_tileGrow);
    L.gap = Px(static_cast<float>(s_look.iconGap));
    L.titleH = s_compact || vertical || !s_look.showTitle ? 0 : Px(24);  // 竖排时标题显示在面板旁边的小牌子上
    L.topGap = BubbleTitle() ? Px(40) : 0;
    L.sep = Px(9);
    L.clockW = L.clockH = 0;
    if (Mods().clock) ClockSize(L.clockW, L.clockH);
    L.emptyW = vertical ? Px(96) : Px(130);
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
    s_trayRowMax = kMaxTrayRow;
    if (s_trayStyle == kTrayRow) {
        L.cellW = vertical ? L.tile : L.tile * 40 / 48;
        L.cellH = vertical ? L.tile * 40 / 48 : L.tile;
        L.trayW = std::min(trayCount, s_trayRowMax) * (vertical ? L.cellH : L.cellW);
    } else {
        L.cellW = L.cellH = L.tile / 2;
        L.trayW = trayCount > 0 ? L.tile : 0;
    }
    if (TrayHolder()) L.trayW = s_trayStyle == kTrayRow ? (vertical ? L.cellH : L.cellW) : L.tile;
    int n = static_cast<int>(s_items.size());
    L.mediaTall = s_showMedia && !vertical && L.sliderH > 0;
    L.mediaW = !s_showMedia ? 0 : Px(L.mediaTall ? kMediaColW : kMediaRowW);
    L.mediaH = s_showMedia ? Px(kSideCoverSize + 2 + kSideTitleH) + (s_mediaOpen ? 3 : 1) * Px(28) : 0;
    L.imeW = s_showIme ? Px(30) : 0;
    L.imeH = s_showIme ? Px(30) : 0;
    L.battW = s_showBattery ? Px(42) : 0;
    L.battH = s_showBattery ? Px(36) : 0;
    L.radiosOther = false;
    L.itemsW = 0;

    // 除了窗口格子那一段以外的长度：两头的边距、开始按钮、各模块和它们之间的间隔
    int seq[kModuleCount];
    float sepBefore[kModuleCount];
    int count = ModuleSequence(L, seq, sepBefore);
    float fixed = 2 * L.pad + L.tile;
    for (int i = 0; i < count; ++i) fixed += sepBefore[i] + ModuleLen(L, seq[i]);

    if (vertical) {
        // 从上到下：开始按钮、各模块（默认是格子、托盘图标、媒体、输入法、电量、时钟），最后是两根竖的调节条、Wi-Fi 和蓝牙按钮
        float radiosH = s_radioCount > 0 ? Px(8) + s_radioCount * L.radioH + (s_radioCount - 1) * L.radioGap : 0;
        // 面板最长多长：拖过长度就是那么长（不超过屏幕），否则是屏幕高的九成
        float custom = CustomLength(), screen = ScreenSide() - 2 * L.margin;
        float maxLen = custom > 0 ? std::min(custom, screen) : ScreenSide() * 0.9f - 2 * L.margin;
        // 不够长时至少留一个窗口格子：托盘图标少显示几个（多的滚着看），还不够就把调节条变短
        float room = maxLen - L.tile - (fixed - L.trayW + L.rowGap + L.sliderH + radiosH);
        if (s_trayStyle == kTrayRow && trayCount > 0 && L.trayW > room) {
            float was = L.trayW;
            s_trayRowMax = std::clamp(static_cast<int>(room / L.cellH), 1, kMaxTrayRow);
            L.trayW = std::min(trayCount, s_trayRowMax) * L.cellH;
            fixed += L.trayW - was;
        }
        if (s_levels && L.trayW > room) L.sliderH = std::max(Px(72), L.sliderH - (L.trayW - room));
        s_trayFirst = std::clamp(s_trayFirst, 0, std::max(0, trayCount - s_trayRowMax));
        float others = fixed + L.rowGap + L.sliderH + radiosH;
        float avail = std::max(L.tile, maxLen - others);
        s_visible = std::min(n, std::max(1, static_cast<int>((avail + L.gap) / (L.tile + L.gap))));
        if (s_lockedItemsW > 0 && custom <= 0)
            s_visible = std::min(n, std::max(1, static_cast<int>((s_lockedItemsW + L.gap) / (L.tile + L.gap))));
        float tilesH = n == 0 ? L.emptyW : s_visible * (L.tile + L.gap) - L.gap;
        if (custom > 0) L.itemsW = std::max(avail, tilesH);  // 拖过长度：格子那段占满，格子在中间
        else L.itemsW = s_lockedItemsW > 0 ? s_lockedItemsW : tilesH;
        L.panelW = 2 * L.pad + L.tile;
        L.panelH = others + L.itemsW;
        L.panelX = s_edge == kEdgeLeft ? L.margin : L.bubbleW + L.margin;
        L.panelY = L.margin;
        L.colX = L.panelX + L.pad;
        L.startY = L.panelY + L.pad;
        float end = PlaceModules(L, L.startY + L.tile);
        L.itemsY = L.modPos[kModWindows];
        L.tilesY = L.itemsY + (L.itemsW - tilesH) / 2;
        L.trayY = L.modPos[kModTray];
        L.mediaY = L.modPos[kModMedia];
        L.imeY = L.modPos[kModIme];
        L.battY = L.modPos[kModBattery];
        L.clockY = L.modPos[kModClock];
        L.sliderY = end + L.rowGap;
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
        L.radius = std::min(Px(static_cast<float>(PopupRadius())), L.panelW / 2);
        return;
    }

    // 媒体占两行时，调节条放在媒体那一列有窗口格子的那一边（格子可以变长，让出地方），
    // Wi-Fi 蓝牙放得下就放在另一边的下面
    int mediaAt = -1, windowsAt = -1;
    for (int i = 0; i < count; ++i) {
        if (seq[i] == kModMedia) mediaAt = i;
        if (seq[i] == kModWindows) windowsAt = i;
    }
    bool windowsFirst = windowsAt < mediaAt;
    float mediaStart0 = 0, sepAfterMedia = 0;  // 格子那段长度为 0 时媒体的起点
    if (L.mediaTall) {
        mediaStart0 = L.pad + L.tile;
        for (int i = 0; i < mediaAt; ++i) mediaStart0 += sepBefore[i] + ModuleLen(L, seq[i]);
        mediaStart0 += sepBefore[mediaAt];
        sepAfterMedia = mediaAt + 1 < count ? sepBefore[mediaAt + 1] : 0;
        float afterRoom = fixed - (mediaStart0 + L.mediaW) - sepAfterMedia - L.pad;
        float beforeRoom = mediaStart0 - sepBefore[mediaAt] - L.pad;
        float otherRoom = windowsFirst ? afterRoom : beforeRoom;
        L.radiosOther = radiosW > 0 && radiosW + Px(4) <= otherRoom;
        float need = (s_levels ? Px(380) : Px(260)) + (radiosW > 0 && !L.radiosOther ? radiosW + Px(12) : 0);
        // 有格子那一边（格子长度为 0 时，从面板外沿到媒体那边的间隔）至少要这么长（外沿的边距不算，和以前一样）
        float side0 = windowsFirst ? mediaStart0 - sepBefore[mediaAt] : fixed - (mediaStart0 + L.mediaW);
        L.minPanelW = fixed + std::max(0.0f, need - L.pad - side0);
    }

    float others = fixed;
    // 面板最长多长：拖过长度就是那么长（至少放得下一个格子和调节条，不超过屏幕），否则是屏幕宽的八成五
    float custom = CustomLength();
    float maxLen = ScreenSide() * 0.85f - 2 * L.margin;
    if (custom > 0) maxLen = std::max(std::max(L.minPanelW, others + L.tile), std::min(custom, ScreenSide() - 2 * L.margin));
    float avail = maxLen - others;
    int maxFit = std::max(1, static_cast<int>((avail + L.gap) / (L.tile + L.gap)));
    s_visible = std::min(n, maxFit);

    // 格子少的时候面板按最小宽度来，格子在中间那段里居中
    if (s_lockedItemsW > 0 && custom <= 0) {
        int fit = std::max(1, static_cast<int>((s_lockedItemsW + L.gap) / (L.tile + L.gap)));
        s_visible = std::min(n, fit);
    }
    float tilesW = n == 0 ? L.emptyW : s_visible * (L.tile + L.gap) - L.gap;
    if (custom > 0) L.itemsW = std::max(avail, tilesW);  // 拖过长度：格子那段占满，格子在中间
    else L.itemsW = s_lockedItemsW > 0 ? s_lockedItemsW : std::max(tilesW, L.minPanelW - others);
    L.panelW = others + L.itemsW;
    L.panelH = 2 * L.pad + L.titleH + L.tile + L.rowGap + L.sliderH;
    L.panelX = L.margin;
    L.panelY = L.margin + L.topGap;
    PlaceModules(L, L.panelX + L.pad + L.tile);
    L.itemsX = L.modPos[kModWindows];
    L.tilesX = L.itemsX + (L.itemsW - tilesW) / 2;
    L.trayX = L.modPos[kModTray];
    L.mediaX = L.modPos[kModMedia];
    L.imeX = L.modPos[kModIme];
    L.battX = L.modPos[kModBattery];
    L.clockX = L.modPos[kModClock];
    L.tileY = L.panelY + L.pad + L.titleH;
    L.sliderY = L.tileY + L.tile + L.rowGap;

    // 音量在左半边，亮度在右半边，Wi-Fi 和蓝牙按钮在最右边。媒体占两行时调节条只排在有格子的那一边，
    // Wi-Fi 蓝牙放得下就放在另一边（放不下就还是排在调节条后面）
    float panelLeft = L.panelX + L.pad + Px(2), panelRight = L.panelX + L.panelW - L.pad - Px(2);
    float left = panelLeft, right = panelRight;
    if (L.mediaTall) {
        float mediaLeft = L.mediaX - sepBefore[mediaAt] / 2 - Px(8);
        float mediaRight = L.mediaX + L.mediaW + (sepAfterMedia > 0 ? sepAfterMedia / 2 : 0) + Px(8);
        if (windowsFirst) right = mediaLeft;
        else left = mediaRight;
    }
    float cy = L.sliderY + L.sliderH / 2, icon = Px(30);
    float radioX = s_levels ? right - radiosW : (left + right - radiosW) / 2;
    if (L.radiosOther) radioX = windowsFirst ? panelRight - radiosW : panelLeft;
    for (int k = 0; k < s_radioCount; ++k)
        s_radioRects[k] = RectF(radioX + k * (L.radioW + L.radioGap), cy - L.radioH / 2, L.radioW, L.radioH);
    if (radiosW > 0 && !L.radiosOther) right -= radiosW + Px(12);
    float mid = (left + right) / 2;
    auto make = [&](float x0, float x1) {
        return Slider{RectF(x0, cy - icon / 2, icon, icon), x0 + icon + Px(10), x1 - Px(10), cy};
    };
    s_sliders[0] = make(left, mid - Px(8));
    s_sliders[1] = make(mid + Px(8), right);

    s_width = static_cast<int>(std::ceil(L.panelW + 2 * L.margin));
    s_height = static_cast<int>(std::ceil(L.panelH + L.margin + L.topGap + L.bottomGap));
    L.radius = std::min(Px(static_cast<float>(PopupRadius())), L.panelH / 2);
}

// 排好布局。格子那段有空（关掉了一些模块，面板按调节条那行的最小长度来，或者拖过长度）时，格子按比例放大补上：
// 面板长度不变、格子都放得下，最多放大到图标大小 150%。筛选时格子大小不变；开始菜单上面那一行不放大
void FitTiles() {
    if (s_lockedItemsW > 0) {
        ComputeLayout();
        return;
    }
    s_tileGrow = 1;
    ComputeLayout();
    if (s_compact) return;
    int n = static_cast<int>(s_items.size());
    float maxGrow = static_cast<float>(kMaxIconSize) / std::max(1, s_look.iconSize);
    if (n == 0 || s_visible < n || maxGrow <= 1) return;
    auto length = [] { return Vertical() ? s_L.panelH : s_L.panelW; };
    float base = length(), lo = 1, hi = maxGrow;
    for (int i = 0; i < 8; ++i) {
        s_tileGrow = (lo + hi) / 2;
        ComputeLayout();
        if (s_visible == n && length() <= base + 0.5f) lo = s_tileGrow;
        else hi = s_tileGrow;
    }
    s_tileGrow = lo < 1.02f ? 1 : lo;
    ComputeLayout();
}

bool IsWindows11() {
    DWORD build = WindowsBuild();
    return !build || build >= 22000;  // 取不到版本时按 Win11 算
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
int EffectiveAlign() {
    if (s_align != kAlignSystem) return s_align;
    return SystemTaskbarCentered() ? kAlignCenter : kAlignLeft;
}

// 自定义位置：面板中心在边上 s_offset 千分之几处，整个窗口不超出屏幕。from / to 是那条边的两头，size 是窗口的长度
int CustomPlace(LONG from, LONG to, int size) {
    int at = static_cast<int>(std::lround(from + (to - from) * static_cast<double>(s_offset) / kMaxPopupOffset - size / 2.0));
    return std::max<int>(from, std::min<int>(at, to - size));
}

void UpdatePlacement() {
    if (Vertical()) {
        int x = s_edge == kEdgeLeft ? s_bounds.left : s_bounds.right - s_width;
        int y = std::max<int>(s_bounds.top, s_bounds.top + (s_bounds.bottom - s_bounds.top - s_height) / 2);
        if (s_align == kAlignCustom && s_height < s_bounds.bottom - s_bounds.top) y = CustomPlace(s_bounds.top, s_bounds.bottom, s_height);
        s_pos = {x, y};
        s_baseValid = false;
        return;
    }
    int align = EffectiveAlign();
    int x = s_bounds.left + (s_bounds.right - s_bounds.left - s_width) / 2;
    if (align == kAlignLeft) x = s_bounds.left;
    else if (align == kAlignRight) x = s_bounds.right - s_width;
    else if (align == kAlignCustom && s_width < s_bounds.right - s_bounds.left) x = CustomPlace(s_bounds.left, s_bounds.right, s_width);
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
    if (index == 0) return RectF(L.panelX + L.pad, L.tileY, L.tile, L.tile);
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

// ---- 媒体按钮、输入法、电量 ----

// 媒体那一段里各部分的位置：封面在上，下面是歌名，最下面三个按钮（kMediaPrevious 上一首、kMediaPlayPause 播放 / 暂停、
// kMediaNext 下一首；竖排时三个按钮上下排）。横排只有一行时封面在左，歌名和按钮在右。
// 平时只有播放 / 暂停（居中），点封面才展开上一首、下一首
struct MediaParts {
    RectF cover, title, button[3];
};

bool MediaButtonShown(int k) { return k == kMediaPlayPause || s_mediaOpen; }

MediaParts MediaLayout() {
    const Layout& L = s_L;
    MediaParts m;
    float bh = Px(28);
    if (Vertical()) {
        float side = Px(kSideCoverSize), bw = Px(36);
        m.cover = RectF(L.colX + (L.tile - side) / 2, L.mediaY, side, side);
        m.title = RectF(L.panelX + Px(3), L.mediaY + side + Px(2), L.panelW - Px(6), Px(kSideTitleH));
        for (int k = 0; k < 3; ++k)
            m.button[k] = RectF(L.colX + (L.tile - bw) / 2, m.title.GetBottom() + (s_mediaOpen ? k : 0) * bh, bw, bh);
        return m;
    }
    float side = Px(kCoverSize), bw = Px(30);
    if (L.mediaTall) {
        float bottom = L.tileY + L.tile + L.rowGap + L.sliderH;
        m.cover = RectF(L.mediaX + (L.mediaW - side) / 2, L.tileY + Px(2), side, side);
        m.title = RectF(L.mediaX, m.cover.GetBottom() + Px(1), L.mediaW, Px(16));
        float x0 = L.mediaX + (L.mediaW - 3 * bw) / 2;
        for (int k = 0; k < 3; ++k) m.button[k] = RectF(x0 + k * bw, bottom - bh, bw, bh);
        return m;
    }
    m.cover = RectF(L.mediaX, L.tileY + (L.tile - side) / 2, side, side);
    float x = m.cover.GetRight() + Px(6);
    m.title = RectF(x, L.tileY + Px(2), L.mediaX + L.mediaW - x, Px(16));
    float x0 = s_mediaOpen ? x : m.title.X + (m.title.Width - bw) / 2 - bw;
    for (int k = 0; k < 3; ++k) m.button[k] = RectF(x0 + k * bw, L.tileY + L.tile - bh - Px(1), bw, bh);
    return m;
}

RectF MediaRect(int k) { return MediaLayout().button[k]; }

RectF ExtraRect(int extra) {
    const Layout& L = s_L;
    bool v = Vertical();
    if (extra == kExtraIme) return v ? RectF(L.panelX, L.imeY, L.panelW, L.imeH) : RectF(L.imeX, L.tileY, L.imeW, L.tile);
    if (extra == kExtraBattery)
        return v ? RectF(L.panelX, L.battY, L.panelW, L.battH) : RectF(L.battX, L.tileY, L.battW, L.tile);
    if (extra == kExtraCover) {  // 封面和歌名（横排只有一行时歌名在按钮上面，只算封面）
        MediaParts m = MediaLayout();
        if (!v && !L.mediaTall) return m.cover;
        RectF r;
        RectF::Union(r, m.cover, m.title);
        return r;
    }
    return MediaRect(extra);
}

bool ExtraShown(int extra) {
    if (extra == kExtraIme) return s_showIme;
    if (extra == kExtraBattery) return s_showBattery;
    if (extra >= kMediaPrevious && extra <= kMediaNext) return s_showMedia && MediaButtonShown(extra);
    return s_showMedia && extra == kExtraCover;
}

// 鼠标下的媒体按钮、输入法或电量，没有为 kExtraNone
int ExtraAt(int x, int y) {
    for (int e : {static_cast<int>(kMediaPrevious), static_cast<int>(kMediaPlayPause), static_cast<int>(kMediaNext),
                  static_cast<int>(kExtraCover), static_cast<int>(kExtraIme), static_cast<int>(kExtraBattery)})
        if (ExtraShown(e) && ExtraRect(e).Contains(static_cast<REAL>(x), static_cast<REAL>(y))) return e;
    return kExtraNone;
}

bool MediaEnabled(int k) {
    if (!s_media.present) return false;
    return k == kMediaPrevious ? s_media.canPrevious : k == kMediaNext ? s_media.canNext : s_media.canPlayPause;
}

std::wstring MediaText() {
    if (!s_media.present) return Tr(L"没有在播放的媒体");
    std::wstring text = s_media.title;
    if (!s_media.artist.empty()) text += (text.empty() ? L"" : L" – ") + s_media.artist;
    if (text.empty()) return Tr(s_media.playing ? L"正在播放" : L"已暂停");
    return s_media.playing ? text : TrF(L"已暂停：%ls", text.c_str());
}

std::wstring BatteryText() {
    const BatteryInfo& b = s_battery;
    if (b.percent < 0) return Tr(L"电量");
    if (b.charging) return TrF(L"电量 %d%%，正在充电", b.percent);
    if (b.plugged) return TrF(L"电量 %d%%，已接通电源", b.percent);
    if (b.secondsLeft > 0)
        return TrF(L"电量 %d%%，还能用 %d 小时 %d 分钟", b.percent, b.secondsLeft / 3600, b.secondsLeft / 60 % 60);
    return TrF(L"电量 %d%%", b.percent);
}

std::wstring ExtraText(int extra) {
    if (extra == kExtraIme) return s_ime.text;
    if (extra == kExtraBattery) return BatteryText();
    return MediaText();
}

// ---- 托盘图标 ----

int TrayCells() { return static_cast<int>(s_tray.size()); }

// 一行大图标时同时显示几个
int TrayRowVisible() { return std::min(TrayCells(), s_trayRowMax); }

int TrayPages() { return s_trayStyle == kTrayRow ? 1 : (static_cast<int>(s_tray.size()) + 3) / 4; }

// 托盘图标多到要翻页（堆叠）或者滚动（一行大图标）
bool TrayScrolls() { return s_trayStyle == kTrayRow ? TrayCells() > s_trayRowMax : TrayPages() > 1; }

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
    if (!Win11Style()) {  // Win11 原版的底色浓，不用阴影
        RectF shadowRect = rect;
        shadowRect.Offset(0, std::max(1.0f, Px(1)));
        SolidBrush shadow(Colors().textShadow);
        g.DrawString(text, -1, &font, shadowRect, &format, &shadow);
    }
    SolidBrush brush(color);
    g.DrawString(text, -1, &font, rect, &format, &brush);
}

// 格子里各种底色块的圆角：跟着面板的圆角变（面板方一点，块也方一点）；Win11 原版和系统任务栏的按钮一样是 4
float PillRadius() { return Win11Style() ? Px(4) : std::clamp(s_L.radius * 0.55f, Px(4), Px(12)); }

// 高亮块：一颗小玻璃珠，上亮下暗，带一圈细亮边。Win11 原版是平的一块，和系统任务栏鼠标指着的按钮一样
void DrawPill(Graphics& g) {
    if (s_pillIndex < 0) return;
    const Palette& pal = Colors();
    RectF r = Vertical() ? RectF(s_L.colX, s_pillX, s_L.tile, s_L.tile) : RectF(s_pillX, s_L.tileY, s_L.tile, s_L.tile);
    r.Inflate(-0.5f, -0.5f);
    GraphicsPath path;
    AddRoundRect(path, r, PillRadius());
    if (Win11Style()) {
        bool light = s_glass.Light();
        SolidBrush fill(light ? Color(165, 255, 255, 255) : Color(24, 255, 255, 255));
        g.FillPath(&fill, &path);
        Pen rim(light ? Color(20, 0, 0, 0) : Color(14, 255, 255, 255), std::max(1.0f, Px(1)));
        g.DrawPath(&rim, &path);
        return;
    }
    PointF top(0, r.Y - 1), bottom(0, r.Y + r.Height + 1);
    LinearGradientBrush fill(top, bottom, pal.pillTop, pal.pillBottom);
    g.FillPath(&fill, &path);
    LinearGradientBrush rimBrush(top, bottom, pal.pillRim, Color(pal.pillRim.GetA() / 5, 255, 255, 255));
    Pen rim(&rimBrush, std::max(1.0f, Px(1)));
    g.DrawPath(&rim, &path);
}

// 在任务栏上闪着（来了消息）的窗口用的橙色
Color UrgentColor(BYTE alpha = 255) { return s_glass.Light() ? Color(alpha, 235, 115, 0) : Color(alpha, 255, 150, 40); }

bool IsUrgentItem(int i) { return s_items[i].hwnd && Attention_IsUrgent(s_items[i].hwnd); }

// 别的屏幕上的窗口：格子右上角一个小圆，写着它在第几块屏幕（和设置里“屏幕 2”的编号一样）
void DrawScreenBadge(Graphics& g, const RectF& tile, HWND hwnd) {
    if (!hwnd || !g_settings.markScreens || GetSystemMetrics(SM_CMONITORS) < 2) return;
    HMONITOR m = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    int number = m == s_monitor ? 0 : MonitorNumber(m);
    if (number <= 0) return;
    bool light = s_glass.Light();
    float d = std::max(Px(14), tile.Width * 0.3f);
    RectF r(tile.X + tile.Width - d - Px(1), tile.Y + Px(1), d, d);
    FillRound(g, r, d / 2, light ? Color(235, 255, 255, 255) : Color(235, 48, 48, 54));
    GraphicsPath rim;
    AddRoundRect(rim, r, d / 2);
    Pen pen(light ? Color(40, 0, 0, 0) : Color(60, 255, 255, 255), std::max(1.0f, Px(1)));
    g.DrawPath(&pen, &rim);
    Font font(s_fontFamily.get(), d * 0.6f, FontStyleBold, UnitPixel);
    StringFormat format(StringFormatFlagsNoWrap);
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    SolidBrush brush(Colors().text);
    std::wstring text = std::to_wstring(number);
    g.DrawString(text.c_str(), -1, &font, r, &format, &brush);
}

void DrawTile(Graphics& g, int idx) {
    const Palette& pal = Colors();
    RectF r = TileRect(idx);
    float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
    if (idx == 0) {
        // Windows 标志（跟着格子大小变）
        float sq = r.Width / 6, gap = r.Width / 24;
        float x0 = cx - sq - gap / 2, y0 = cy - sq - gap / 2;
        SolidBrush b(pal.accent);
        g.FillRectangle(&b, x0, y0, sq, sq);
        g.FillRectangle(&b, x0 + sq + gap, y0, sq, sq);
        g.FillRectangle(&b, x0, y0 + sq + gap, sq, sq);
        g.FillRectangle(&b, x0 + sq + gap, y0 + sq + gap, sq, sq);
        return;
    }

    const Tile& item = s_items[idx - 1];
    bool urgent = IsUrgentItem(idx - 1);
    if (urgent) {  // 和系统任务栏上闪烁的按钮一样垫一块橙色的底
        RectF bg = r;
        bg.Inflate(-0.5f, -0.5f);
        FillRound(g, bg, PillRadius(), UrgentColor(s_glass.Light() ? 70 : 80));
    }
    float is = r.Width / 2;
    RectF iconRect(cx - is / 2, cy - is / 2, is, is);
    if (item.icon) g.DrawImage(item.icon.get(), iconRect);
    else FillRound(g, iconRect, Px(4), pal.separator);
    DrawScreenBadge(g, r, item.hwnd);

    // 和 Win11 任务栏一样：当前窗口是一条长的强调色短线，其它打开的窗口是短点，没运行的固定应用没有。
    // 竖排时竖着画在靠屏幕边的那一侧
    float len = item.active || urgent ? Px(16) : Px(6);
    RectF bar(cx - len / 2, r.Y + r.Height - Px(6), len, Px(3));
    if (Vertical()) bar = RectF(s_edge == kEdgeLeft ? r.X + Px(3) : r.X + r.Width - Px(6), cy - len / 2, Px(3), len);
    if (urgent) FillRound(g, bar, Px(1.5f), UrgentColor());
    else if (item.active) FillRound(g, bar, Px(1.5f), pal.accent);
    else if (item.hwnd) FillRound(g, bar, Px(1.5f), pal.running);
}

// ---- 音量和亮度 ----

bool SliderEnabled(int which) { return which == 0 ? s_hasVolume : s_brightness >= 0; }

float SliderValue(int which) {
    if (which == 0) return s_hasVolume ? s_volume : 0;
    return s_brightness >= 0 ? s_brightness / 100.0f : 0;
}

// 亮度条同时调所有屏幕（设置里打开了，而且接着不止一块屏）
bool AllBrightness() { return g_settings.brightnessAllScreens && GetSystemMetrics(SM_CMONITORS) > 1; }

std::wstring SliderText(int which) {
    if (which == 0) {
        if (!s_hasVolume) return Tr(L"没有可用的播放设备");
        if (s_muted) return Tr(L"已静音（点喇叭取消）");
        return TrF(L"音量 %d%%", static_cast<int>(std::lround(s_volume * 100)));
    }
    if (s_brightness == -2) return Tr(L"正在读取亮度…");
    if (s_brightness < 0) return Tr(L"这块屏幕不支持调节亮度");
    return TrF(AllBrightness() ? L"所有屏幕亮度 %d%%" : L"亮度 %d%%", s_brightness);
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
        std::vector<HMONITOR> screens = AllBrightness() ? MonitorsInOrder() : std::vector<HMONITOR>{s_monitor};
        for (HMONITOR m : screens) {
            RememberBrightness(m, percent);
            Brightness_Set(m, percent);
        }
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
        float radius = Win11Style() ? Px(4) : box.Height / 2;  // Win11 原版和系统快速设置里的按钮一样是方的
        if (on) {
            const Color& a = pal.accent;
            FillRound(g, box, radius, r.switching ? Color(140, a.GetR(), a.GetG(), a.GetB()) : a);
        } else if (usable) {
            FillRound(g, box, radius, pal.pillTop);
        }
        if (k == s_hoverRadio && usable) FillRound(g, box, radius, on ? Color(45, 255, 255, 255) : pal.pillBottom);
        Color glyph = pal.subtle;
        if (on) glyph = OnAccent();
        else if (usable && !r.switching) glyph = pal.text;
        if (id == kRadioWifi) DrawWifi(g, box, glyph);
        else DrawBluetooth(g, box, glyph);
    }
}

// 媒体按钮的图标：上一首、播放 / 暂停、下一首
void DrawMediaGlyph(Graphics& g, int k, const RectF& box, const Color& c) {
    float u = Px(1);
    float cx = box.X + box.Width / 2, cy = box.Y + box.Height / 2;
    SolidBrush brush(c);
    if (k == kMediaPlayPause) {
        if (s_media.playing) {
            FillRound(g, RectF(cx - 5 * u, cy - 6 * u, 3.5f * u, 12 * u), 1.2f * u, c);
            FillRound(g, RectF(cx + 1.5f * u, cy - 6 * u, 3.5f * u, 12 * u), 1.2f * u, c);
        } else {
            PointF tri[3] = {PointF(cx - 4 * u, cy - 6.5f * u), PointF(cx + 6 * u, cy), PointF(cx - 4 * u, cy + 6.5f * u)};
            g.FillPolygon(&brush, tri, 3);
        }
        return;
    }
    // 三角朝着要去的方向，尖上一道竖线
    float d = k == kMediaNext ? 1.0f : -1.0f;
    PointF tri[3] = {PointF(cx - 3.5f * u * d, cy - 5 * u), PointF(cx + 3.5f * u * d, cy),
                     PointF(cx - 3.5f * u * d, cy + 5 * u)};
    g.FillPolygon(&brush, tri, 3);
    FillRound(g, RectF(d > 0 ? cx + 3.5f * u : cx - 5.5f * u, cy - 5 * u, 2 * u, 10 * u), u, c);
}

// 封面：圆角方块，播放器没给封面时画个音符
void DrawCover(Graphics& g, const RectF& r) {
    const Palette& pal = Colors();
    GraphicsPath path;
    AddRoundRect(path, r, Px(7));
    int side = static_cast<int>(std::lround(r.Width));
    if (s_media.cover && (s_media.cover != s_coverFrom || !s_coverScaled ||
                          static_cast<int>(s_coverScaled->GetWidth()) != side)) {
        s_coverFrom = s_media.cover;
        s_coverScaled = std::make_unique<Bitmap>(side, side, PixelFormat32bppPARGB);
        Graphics sg(s_coverScaled.get());
        sg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        sg.SetPixelOffsetMode(PixelOffsetModeHalf);
        ImageAttributes edges;
        edges.SetWrapMode(WrapModeTileFlipXY);
        sg.DrawImage(s_coverFrom.get(), Rect(0, 0, side, side), 0, 0, s_coverFrom->GetWidth(), s_coverFrom->GetHeight(),
                     UnitPixel, &edges);
    }
    if (s_media.cover && s_coverScaled) {
        TextureBrush brush(s_coverScaled.get(), WrapModeClamp);
        brush.TranslateTransform(r.X, r.Y);
        g.FillPath(&brush, &path);
        return;
    }
    SolidBrush back(pal.pillBottom), ink(pal.subtle);
    g.FillPath(&back, &path);
    // 音符：圆头、竖杆、小旗
    float u = r.Width / 36, cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
    g.FillEllipse(&ink, cx - 6.5f * u, cy + 1.5f * u, 8 * u, 6.5f * u);
    g.FillRectangle(&ink, cx, cy - 9 * u, 1.6f * u, 14 * u);
    PointF flag[4] = {PointF(cx + 1.6f * u, cy - 9 * u), PointF(cx + 7 * u, cy - 5.5f * u), PointF(cx + 7 * u, cy - 3 * u),
                      PointF(cx + 1.6f * u, cy - 6 * u)};
    g.FillPolygon(&ink, flag, 4);
}

// 歌名：没有就写在放还是暂停了；横排一行，竖排最多两行小字，放不下的后面省略
void DrawMediaTitle(Graphics& g, const RectF& r) {
    std::wstring text = s_media.title;
    if (text.empty()) text = s_media.present ? Tr(s_media.playing ? L"正在播放" : L"已暂停") : Tr(L"没有在播放的媒体");
    bool vertical = Vertical();
    Font font(s_fontFamily.get(), vertical ? Px(9.5f) : Px(11), FontStyleRegular, UnitPixel);
    StringFormat format(vertical ? StringFormatFlagsLineLimit : StringFormatFlagsNoWrap);
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(vertical ? StringAlignmentNear : StringAlignmentCenter);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    DrawLabel(g, text.c_str(), font, r, format, Colors().text);
}

// 没有在放的东西、播放器不支持的按钮画暗的，点了没反应
void DrawMedia(Graphics& g) {
    if (!s_showMedia) return;
    const Palette& pal = Colors();
    MediaParts m = MediaLayout();
    if (s_L.mediaTall) {  // 占两行时垫一块淡淡的底，看得出是一组
        const Layout& L = s_L;
        FillRound(g, RectF(L.mediaX - Px(2), L.tileY, L.mediaW + Px(4), L.tile + L.rowGap + L.sliderH), PillRadius(),
                  pal.pillBottom);
    }
    if (s_hoverExtra == kExtraCover) {  // 封面能点：点了展开、收起上一首和下一首
        RectF r = ExtraRect(kExtraCover);
        r.Inflate(Px(2), Px(2));
        FillRound(g, r, PillRadius(), pal.pillTop);
    }
    DrawCover(g, m.cover);
    DrawMediaTitle(g, m.title);
    for (int k = kMediaPrevious; k <= kMediaNext; ++k) {
        if (!MediaButtonShown(k)) continue;
        RectF box = m.button[k];
        bool enabled = MediaEnabled(k);
        if (k == s_hoverExtra && enabled) {
            float d = std::min(box.Width, box.Height) - Px(2);
            FillRound(g, RectF(box.X + (box.Width - d) / 2, box.Y + (box.Height - d) / 2, d, d), d / 2, pal.pillTop);
        }
        DrawMediaGlyph(g, k, box, enabled ? pal.text : pal.subtle);
    }
}

// 输入法：中 / 英（别的语言是缩写），能切换时鼠标指着有一块淡淡的底
void DrawIme(Graphics& g, const StringFormat& center) {
    if (!s_showIme) return;
    const Palette& pal = Colors();
    RectF r = ExtraRect(kExtraIme);
    if (s_hoverExtra == kExtraIme && s_ime.canToggle) {
        float w = std::min(r.Width - Px(2), Px(30)), h = std::min(r.Height - Px(2), Px(30));
        FillRound(g, RectF(r.X + (r.Width - w) / 2, r.Y + (r.Height - h) / 2, w, h), Px(8), pal.pillTop);
    }
    Font font(s_fontFamily.get(), s_ime.label.size() > 1 ? Px(10.5f) : Px(14), FontStyleRegular, UnitPixel);
    DrawLabel(g, s_ime.label.c_str(), font, r, center, pal.text);
}

// 电量：电池的样子，剩多少填多少（充电、接着电源时绿色，快没电时红色），下面写百分比
void DrawBattery(Graphics& g, const Font& font, const StringFormat& center) {
    if (!s_showBattery) return;
    const Palette& pal = Colors();
    const BatteryInfo& b = s_battery;
    bool light = s_glass.Light();
    RectF r = ExtraRect(kExtraBattery);
    float u = Px(1), bw = 20 * u, bh = 10 * u, textH = 14 * u;
    float top = r.Y + (r.Height - bh - 2 * u - textH) / 2;
    RectF body(r.X + (r.Width - bw) / 2 - u, top, bw, bh);
    GraphicsPath path;
    AddRoundRect(path, body, 2.5f * u);
    Pen pen(pal.text, 1.2f * u);
    g.DrawPath(&pen, &path);
    FillRound(g, RectF(body.X + bw + 0.8f * u, top + bh / 2 - 2 * u, 1.8f * u, 4 * u), 0.8f * u, pal.text);
    if (b.percent > 0) {
        Color fill = pal.text;
        if (b.charging || b.plugged) fill = light ? Color(255, 16, 140, 60) : Color(255, 80, 210, 110);
        else if (b.percent <= 20) fill = light ? Color(255, 200, 40, 30) : Color(255, 240, 80, 70);
        float inner = (bw - 4 * u) * std::min(b.percent, 100) / 100.0f;
        FillRound(g, RectF(body.X + 2 * u, body.Y + 2 * u, std::max(inner, 1.5f * u), bh - 4 * u), 0.8f * u, fill);
    }
    wchar_t text[16] = L"–";
    if (b.percent >= 0) swprintf(text, 16, L"%d%%", b.percent);
    DrawLabel(g, text, font, RectF(r.X, top + bh + 2 * u, r.Width, textH), center, pal.text);
}

// 托盘图标。一行大图标放不下时两头画个小三角，表示那边还有（滚轮滚着看）；
// 堆叠时只画当前这一页，垫一块淡淡的底，不止一页时底边画几个小点表示第几页，翻页时新一页从旁边滑进来
void DrawTray(Graphics& g) {
    int cells = TrayCells();
    const Palette& pal = Colors();
    if (cells == 0) {
        if (TrayHolder()) {  // 编辑模式里的空位
            PointF o = TrayOrigin();
            RectF area = Vertical() ? RectF(o.X, o.Y, s_L.tile, s_L.trayW) : RectF(o.X, o.Y, s_L.trayW, s_L.tile);
            area.Inflate(-0.5f, -0.5f);
            FillRound(g, area, PillRadius(), pal.pillBottom);
        }
        return;
    }
    bool row = s_trayStyle == kTrayRow;
    int hot = s_hoverTray >= 0 ? s_hoverTray : s_trayHot;
    if (!row) {
        RectF back = TrayGroupRect();
        back.Inflate(-0.5f, -0.5f);
        FillRound(g, back, PillRadius(), pal.pillBottom);
    }
    if (hot >= 0 && hot < cells && TrayOnPage(hot) && !TrayFlipping()) {
        RectF r = TrayCellRect(hot);
        r.Inflate(-Px(1), -Px(1));
        FillRound(g, r, row ? Px(10) : Px(6), pal.pillTop);
    }
    float icon = row ? s_L.tile / 2 : s_L.tile / 3;
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

// 显示标题的小牌子：竖排时贴着面板里侧，上下对着 anchor（鼠标所指的格子、调节条等的中线）；
// 横排不显示标题行时在面板上方，左右对着 anchor。实心底，放在什么背景上都看得清；打字筛选时前面是搜索框
void DrawBubble(Graphics& g, float anchor, const std::wstring& title, const Font& font) {
    const Layout& L = s_L;
    const Palette& pal = Colors();
    bool filtering = !s_filter.empty();
    if (title.empty() && !filtering) return;
    StringFormat oneLine(StringFormatFlagsNoWrap);
    oneLine.SetLineAlignment(StringAlignmentCenter);
    oneLine.SetTrimming(StringTrimmingEllipsisCharacter);
    float maxW = Vertical() ? L.bubbleW - Px(12) : std::max(Px(120), std::min(L.panelW, Px(440))), padX = Px(10), h = Px(30);
    float searchW = filtering ? SearchBoxWidth(g, font, maxW * 0.5f) : 0;
    float titleW = 0;
    if (!title.empty()) {
        RectF measured;
        g.MeasureString(title.c_str(), -1, &font, PointF(0, 0), &oneLine, &measured);
        titleW = measured.Width + (filtering ? Px(6) : 0);
    }
    float w = std::min(maxW, 2 * padX + searchW + titleW);
    float x = s_edge == kEdgeLeft ? L.panelX + L.panelW + Px(8) : L.panelX - Px(8) - w;
    float y = std::clamp(anchor - h / 2, L.panelY, L.panelY + L.panelH - h);
    if (!Vertical()) {
        x = std::clamp(anchor - w / 2, L.panelX, std::max(L.panelX, L.panelX + L.panelW - w));
        y = L.panelY - Px(8) - h;
    }
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

void DrawEditFrame(Graphics& g);
std::wstring EditText();
bool EditPartShown(int part);
RectF EditPartRect(int part);

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

    // 顶部一行显示鼠标所指或选中项的标题；在调节条上时显示音量 / 亮度；编辑模式里说鼠标下的东西怎么拖
    std::wstring title;
    int shown = s_hover >= 0 ? s_hover : s_sel;
    if (s_editing) title = EditText();
    else if (s_drag != kNone) title = SliderText(SliderOf(s_drag));
    else if (s_hoverControl != kNone) title = SliderText(SliderOf(s_hoverControl));
    else if (s_hoverRadio >= 0) title = RadioText(s_radioIds[s_hoverRadio]);
    else if (s_hoverExtra != kExtraNone) title = ExtraText(s_hoverExtra);
    else if (s_hoverTray >= 0 || s_overTray) title = TrayTitle(s_hoverTray);
    else if (shown == 0) title = Tr(L"开始");
    else if (shown > 0 && shown <= ItemCount()) title = s_items[shown - 1].title;
    StringFormat oneLine(StringFormatFlagsNoWrap);
    oneLine.SetAlignment(StringAlignmentCenter);
    oneLine.SetLineAlignment(StringAlignmentCenter);
    oneLine.SetTrimming(StringTrimmingEllipsisCharacter);
    RectF titleRect(L.panelX + L.pad + Px(10), L.panelY + L.pad, L.panelW - 2 * L.pad - Px(20), L.titleH);
    if (vertical || BubbleTitle()) {
        // 竖排（或者横排不显示标题行）：标题在面板旁边（上方）的小牌子上，对着它说的那一项。打字筛选时只在指着或选了一项时
        // 显示名字；开始菜单模式只在鼠标指着时显示；横排时只在指着、用键盘选或者打字时显示
        bool pointing = s_hover >= 0 || s_hoverTray >= 0 || s_overTray || s_hoverControl != kNone || s_drag != kNone ||
                        s_hoverRadio >= 0 || s_hoverExtra != kExtraNone || s_editing;
        auto mid = [&](const RectF& r) { return vertical ? r.Y + r.Height / 2 : r.X + r.Width / 2; };
        float anchor = vertical ? L.itemsY + L.tile / 2 : L.panelX + L.panelW / 2;
        if (s_editing) {
            int part = s_moveModule != kPartNone ? s_moveModule : s_editHover;
            if (part >= 0 && part < kPartScaleEdge && EditPartShown(part)) anchor = mid(EditPartRect(part));
            else if (vertical) anchor = L.startY + L.tile / 2;
        } else if (s_drag != kNone || s_hoverControl != kNone) {
            const Slider& sl = s_sliders[SliderOf(s_drag != kNone ? s_drag : s_hoverControl)];
            anchor = vertical ? (sl.a0 + sl.a1) / 2 : (sl.icon.X + sl.a1) / 2;
        } else if (s_hoverRadio >= 0) {
            anchor = mid(s_radioRects[s_hoverRadio]);
        } else if (s_hoverExtra != kExtraNone) {
            anchor = mid(ExtraRect(s_hoverExtra));
        } else if (s_hoverTray >= 0) {
            anchor = mid(TrayCellRect(s_hoverTray));
        } else if (s_overTray) {
            anchor = vertical ? L.trayY + L.trayW / 2 : L.trayX + L.trayW / 2;
        } else if (shown >= 0 && shown <= ItemCount() && IsTileVisible(shown)) {
            anchor = mid(TileRect(shown));
        }
        if (!s_filter.empty() && !(s_hover >= 0 || s_sel > 0) && !s_editing) title.clear();
        bool show = vertical ? !s_compact || pointing : pointing || s_keyNav || !s_filter.empty();
        if (show) DrawBubble(g, anchor, title, titleFont);
    } else if (L.titleH <= 0) {
        // 开始菜单模式没有标题这一行
    } else if (!s_filter.empty() && !s_editing) {
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

    // 窗口多到放不下时，两头的小三角提示还能滚动；那边有闪着要提醒的窗口时是橙色
    SolidBrush hint(pal.subtle), urgentHint(UrgentColor());
    float tri = Px(4);
    bool before = s_first > 0, after = s_first + s_visible < ItemCount();
    bool urgentBefore = false, urgentAfter = false;
    for (int i = 0; i < ItemCount(); ++i) {
        if (i >= s_first && i < s_first + s_visible) continue;
        if (IsUrgentItem(i)) (i < s_first ? urgentBefore : urgentAfter) = true;
    }
    if (vertical) {
        float midX = L.colX + L.tile / 2, y0 = L.itemsY - Px(2), y1 = L.itemsY + L.itemsW + Px(2);
        PointF up[3] = {PointF(midX, y0 - tri), PointF(midX - tri, y0), PointF(midX + tri, y0)};
        PointF down[3] = {PointF(midX, y1 + tri), PointF(midX - tri, y1), PointF(midX + tri, y1)};
        if (before) g.FillPolygon(urgentBefore ? &urgentHint : &hint, up, 3);
        if (after) g.FillPolygon(urgentAfter ? &urgentHint : &hint, down, 3);
    } else {
        float midY = L.tileY + L.tile / 2, x0 = L.itemsX - Px(2), x1 = L.itemsX + L.itemsW + Px(2);
        PointF left[3] = {PointF(x0 - tri, midY), PointF(x0, midY - tri), PointF(x0, midY + tri)};
        PointF right[3] = {PointF(x1 + tri, midY), PointF(x1, midY - tri), PointF(x1, midY + tri)};
        if (before) g.FillPolygon(urgentBefore ? &urgentHint : &hint, left, 3);
        if (after) g.FillPolygon(urgentAfter ? &urgentHint : &hint, right, 3);
    }

    // 分隔线：横排是竖线，竖排是横线（占两行的媒体有自己的底，挨着它的不画；输入法、电量和时钟挨着时中间不画）
    Pen sepPen(pal.separator, 1.0f);
    for (int i = 0; i < L.sepCount; ++i) {
        float at = L.sepAt[i] - L.sep / 2;
        if (vertical) g.DrawLine(&sepPen, L.colX + Px(10), at, L.colX + L.tile - Px(10), at);
        else g.DrawLine(&sepPen, at, L.tileY + Px(10), at, L.tileY + L.tile - Px(10));
    }
    DrawTray(g);

    if (s_items.empty()) {
        const wchar_t* empty = Tr(s_filter.empty() ? L"没有打开的窗口" : L"没有匹配的窗口或应用");
        if (vertical) DrawLabel(g, empty, smallFont, RectF(L.panelX + Px(4), L.itemsY, L.panelW - Px(8), L.itemsW), center, pal.subtle);
        else DrawLabel(g, empty, titleFont, RectF(L.itemsX, L.tileY, L.itemsW, L.tile), center, pal.subtle);
    }

    // 时钟
    if (L.clockW > 0) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        std::wstring clock = ClockText(st);
        RectF clockRect = vertical ? RectF(L.panelX, L.clockY, L.panelW, L.clockH) : RectF(L.clockX, L.tileY, L.clockW, L.tile);
        // 不自动换行：有些大小下日期刚好比时钟那块宽一点点，会被折成两行
        StringFormat clockFormat(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
        clockFormat.SetAlignment(StringAlignmentCenter);
        clockFormat.SetLineAlignment(StringAlignmentCenter);
        DrawLabel(g, clock.c_str(), smallFont, clockRect, clockFormat, pal.text);
    }
    DrawMedia(g);
    DrawIme(g, center);
    DrawBattery(g, smallFont, center);

    if (s_levels) {
        DrawSlider(g, 0);
        DrawSlider(g, 1);
    }
    DrawRadios(g);
    if (s_editing) DrawEditFrame(g);
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
    if (!s_open || s_anim != Anim::None || s_drag != kNone || s_editing || s_menuOpen || s_startMode) return -1;  // 开始菜单会盖住缩略图
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
        // Win11 原版的字贴着像素画，小字更清楚
        g.SetTextRenderingHint(Win11Style() ? TextRenderingHintAntiAliasGridFit : TextRenderingHintAntiAlias);
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
    s_startTyped.clear();
    KillTimer(s_hwnd, kTimerStart);
}

void EndTrayMode() {
    s_trayMode = false;
    s_trayHot = -1;
    KillTimer(s_hwnd, kTimerTrayMenu);
}

void EndHoverOpen() {
    s_hoverOpen = false;
    KillTimer(s_hwnd, kTimerHoverLeave);
}

void EndResize();
void EndModuleDrag(bool drop);
void EndPanelMove(bool drop);

void HideNow() {
    EndResize();
    EndModuleDrag(false);
    EndPanelMove(false);
    s_editing = false;
    s_editHover = kPartNone;
    HideThumb();
    EndStartMode();
    EndTrayMode();
    EndHoverOpen();
    s_open = false;
    s_preview = false;
    KillTimer(s_hwnd, kTimerPreview);
    if (AnyModuleSet(&ModuleSet::tray)) Tray_Refresh();  // 下次弹出时用新的
    s_anim = Anim::None;
    s_drag = kNone;
    if (GetCapture() == s_hwnd) ReleaseCapture();
    KillTimer(s_hwnd, kTimerClock);
    ShowWindow(s_hwnd, SW_HIDE);
    Attention_Update();
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
    s_hoverExtra = kExtraNone;
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
        int iconPx = IconPx();
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
    s_hoverExtra = kExtraNone;
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

void EditRelayout();

// 点了媒体按钮、封面、输入法：发命令、展开收起上一首下一首或者切换中英文，马上按新的样子画
void ClickExtra(int extra) {
    if (extra == kExtraCover) {
        s_mediaOpen = !s_mediaOpen;
        if (Vertical()) EditRelayout();  // 竖排时媒体那段变高变矮；横排大小不变，按钮换个地方画
        else Redraw();
        return;
    }
    if (extra == kExtraIme) {
        if (!s_ime.canToggle) return;
        if (!Ime_Toggle(s_imeTarget)) Log(L"输入法：切换中英文没成功");
        s_ime = Ime_Get(s_imeTarget);
    } else if (extra >= kMediaPrevious && extra <= kMediaNext) {
        if (!MediaEnabled(extra)) return;
        Media_Command(extra);
        s_media = Media_Get();
    } else {
        return;
    }
    Redraw();
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
    kCmdMoveTo = 200,  // 移到另一块屏幕：kCmdMoveTo + 它在 MonitorsInOrder 里的序号
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

// 把窗口挪到另一块屏幕上，再和点格子一样切到它
void MoveToScreen(HWND h, HMONITOR to) {
    if (!IsWindow(h)) {
        HideNow();
        return;
    }
    s_open = false;  // 切换焦点会触发失活，这里不要再播收起动画
    MoveWindowToMonitor(h, to);
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
// 还原 / 最小化 / 最大化、移到另一块屏幕（不止两块时选哪一块）；“最大化时不隐藏任务栏”（关着“最大化或全屏时隐藏任务栏”时是“最大化时隐藏任务栏”）；关闭这个窗口，同一个程序开着好几个窗口时还能一次全关。
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

    std::vector<HMONITOR> screens = hwnd ? MonitorsInOrder() : std::vector<HMONITOR>();
    HMONITOR here = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;

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
        if (screens.size() == 2) {
            AppendMenuW(menu, MF_STRING, kCmdMoveTo + (screens[0] == here ? 1 : 0), Tr(L"移到另一块屏幕"));
        } else if (screens.size() > 2) {
            HMENU sub = CreatePopupMenu();
            for (size_t i = 0; i < screens.size(); ++i)
                AppendMenuW(sub, MF_STRING | (screens[i] == here ? MF_GRAYED : 0), kCmdMoveTo + i,
                            MenuText(MonitorLabel(screens[i])).c_str());
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), Tr(L"移到另一块屏幕"));
        }
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
    if (cmd >= kCmdMoveTo && cmd < kCmdMoveTo + screens.size()) {
        MoveToScreen(hwnd, screens[cmd - kCmdMoveTo]);
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
    for (const wchar_t* cls : {L"TopLevelWindowForOverflowXamlIsland", L"NotifyIconOverflowWindow"}) {  // Win11 / Win10
        HWND overflow = FindWindowW(cls, nullptr);
        if (overflow && IsWindowVisible(overflow)) return true;
    }
    return false;
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
    if (!s_startTyped.empty()) {  // 弹出期间就开始打字了：补发给开始菜单的搜索框
        SendText(s_startTyped);
        s_startTyped.clear();
    }
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

void WatchEdge();
void WatchHoverLeave();

// ---- 编辑模式：拖边改大小和长度 ----

// 按最大的大小，迷你任务栏那条屏幕边上一整条：大小一直在变（预览、拖边）时背景截这么大，变大小时不用再截
RECT GrowArea() {
    float grow = static_cast<float>(kMaxPopupScale) / std::max(1, s_preview ? s_previewPercent : g_settings.popupScale);
    LONG w = static_cast<LONG>(std::ceil(s_width * grow)), h = static_cast<LONG>(std::ceil(s_height * grow));
    const RECT& b = s_bounds;
    if (s_edge == kEdgeLeft) return {b.left, b.top, b.left + w, b.bottom};
    if (s_edge == kEdgeRight) return {b.right - w, b.top, b.right, b.bottom};
    return {b.left, b.bottom - h, b.right, b.bottom};
}

// 改大小（粗细）的边：横排是上边，竖排是里侧的边；改长度的边：横排是左右两边，竖排是上下两边
int ScaleEdges() { return Vertical() ? (s_edge == kEdgeLeft ? kResizeRight : kResizeLeft) : kResizeTop; }
int LengthEdges() { return Vertical() ? (kResizeTop | kResizeBottom) : (kResizeLeft | kResizeRight); }

// 能拖的边：靠屏幕一侧对齐时那一侧不动
int ResizableEdges() {
    int edges = ScaleEdges() | LengthEdges();
    if (!Vertical()) {
        int align = EffectiveAlign();
        if (align == kAlignLeft) edges &= ~kResizeLeft;
        else if (align == kAlignRight) edges &= ~kResizeRight;
    }
    return edges;
}

// 编辑模式里鼠标下是面板的哪几条边（ResizeEdge 组合）：边外的阴影里和边里各算几像素，两条边挨着的角上两条都算
int ResizeEdgeAt(int x, int y) {
    if (!s_editing || s_anim != Anim::None) return 0;
    const Layout& L = s_L;
    float out = Px(8), in = Px(14);
    float left = L.panelX, top = L.panelY, right = L.panelX + L.panelW, bottom = L.panelY + L.panelH;
    if (x < left - out || x > right + out || y < top - out || y > bottom + out) return 0;
    int edges = 0;
    if (x < left + in) edges |= kResizeLeft;
    if (x > right - in) edges |= kResizeRight;
    if (y < top + in) edges |= kResizeTop;
    if (y > bottom - in) edges |= kResizeBottom;
    return edges & ResizableEdges();
}

LPCWSTR ResizeCursor(int edges) {
    bool h = (edges & (kResizeLeft | kResizeRight)) != 0, v = (edges & (kResizeTop | kResizeBottom)) != 0;
    if (h && v) return ((edges & kResizeLeft) != 0) == ((edges & kResizeTop) != 0) ? IDC_SIZENWSE : IDC_SIZENESW;
    return v ? IDC_SIZENS : IDC_SIZEWE;
}

// 改大小：按鼠标离固定的那条屏幕边的距离成比例变（pt 是屏幕坐标）
float ScaleDistance(POINT pt) {
    const RECT& b = s_bounds;
    if (Vertical()) return static_cast<float>(s_edge == kEdgeLeft ? pt.x - b.left : b.right - pt.x);
    return static_cast<float>(b.bottom - pt.y);
}

// 改长度：拖的那条边跟着鼠标走，居中时（竖排一直是上下居中）另一头一起动
float LengthAt(POINT pt) {
    bool centered = Vertical() || EffectiveAlign() == kAlignCenter || EffectiveAlign() == kAlignCustom;
    float d = static_cast<float>(Vertical() ? pt.y - s_resizeAt.y : pt.x - s_resizeAt.x);
    if (s_resize & LengthEdges() & (kResizeLeft | kResizeTop)) d = -d;
    return s_resizeLen + (centered ? 2 : 1) * d;
}

// 标题上显示的大小和长度
std::wstring SizeText() {
    if (g_settings.popupLength <= 0) return TrF(L"大小 %d%%，长度自动", g_settings.popupScale);
    return TrF(L"大小 %d%%，长度 %d%%", g_settings.popupScale, (g_settings.popupLength + 5) / 10);
}

// 换个大小、长度马上重新排、重画（图标还是弹出时读的，缩放着画）。变大时最多大到屏幕放得下。
// fit：在拖长度，记成面板实际的长度（拖得太短时内容放不下，按放得下的算）
void SetPopupSize(int percent, int length, bool fit = false) {
    int was = g_settings.popupScale, wasLen = g_settings.popupLength;
    percent = std::clamp(percent, kMinPopupScale, kMaxPopupScale);
    length = std::clamp(length, 0, kMaxPopupLength);
    if (percent == was && length == wasLen) return;
    float old = s_scale, locked = s_lockedItemsW;
    g_settings.popupLength = length;
    for (;; --percent) {
        g_settings.popupScale = percent;
        s_scale = PopupScale(s_monitor);
        s_lockedItemsW = locked * s_scale / old;
        FitTiles();
        if (percent <= was || (s_width <= s_bounds.right - s_bounds.left && s_height <= s_bounds.bottom - s_bounds.top))
            break;
    }
    if (fit && length > 0) {
        float actual = Vertical() ? s_L.panelH : s_L.panelW;
        g_settings.popupLength = std::clamp(static_cast<int>(std::lround(actual / ScreenSide() * kMaxPopupLength)), 1,
                                            kMaxPopupLength);
    }
    if (percent == was && g_settings.popupLength == wasLen) {
        g_settings.popupLength = wasLen;
        s_scale = old;
        s_lockedItemsW = locked;
        FitTiles();
        return;
    }
    s_first = std::clamp(s_first, 0, std::max(0, ItemCount() - s_visible));
    UpdatePlacement();
    s_glass.SetScale(s_scale);
    s_pillIndex = -1;  // 高亮块直接到新位置，不滑过去
    Redraw();
}

// 编辑模式里改了布局（模块、位置）：重新排、摆好、重画（背景已经按最大的大小截了一整条）
void EditRelayout() {
    FitTiles();
    s_first = std::clamp(s_first, 0, std::max(0, ItemCount() - s_visible));
    UpdatePlacement();
    s_glass.Extend(GrowArea());
    s_baseValid = false;
    s_pillIndex = -1;
    Redraw();
}

// 编辑模式里开着却暂时没有内容的媒体（没在放）也显示出来，好拖着排顺序；预览时也显示，看得出开着
void SyncMediaShown() { s_showMedia = Mods().media && !s_compact && (s_editing || s_preview || s_media.present); }

void SetEditing(bool on) {
    if (on && (!s_open || s_preview || s_compact)) return;
    if (on == s_editing) return;
    EndResize();
    EndModuleDrag(false);
    EndPanelMove(false);
    s_editing = on;
    s_editHover = kPartNone;
    if (on) {
        HideThumb();
        s_hover = -1;
        s_hoverControl = kNone;
        s_hoverRadio = -1;
        s_hoverExtra = kExtraNone;
        s_hoverTray = -1;
        s_overTray = false;
        Log(L"迷你任务栏进入编辑模式");
    }
    SyncMediaShown();
    EditRelayout();
}

// 模块、调节条、Wi-Fi 蓝牙按钮这次有没有显示
bool EditPartShown(int part) {
    if (part >= 0 && part < kModuleCount) return ModuleShown(part);
    if (part == kPartLevels) return s_levels;
    if (part == kPartRadios) return s_radioCount > 0;
    return false;
}

// 模块、调节条、Wi-Fi 蓝牙按钮占的地方
RectF EditPartRect(int part) {
    const Layout& L = s_L;
    bool v = Vertical();
    if (part == kPartLevels) {
        if (v) return RectF(L.colX, L.sliderY, L.tile, L.sliderH);
        return RectF(s_sliders[0].icon.X - Px(2), L.sliderY, s_sliders[1].a1 + Px(12) - s_sliders[0].icon.X, L.sliderH);
    }
    if (part == kPartRadios) {
        RectF r = s_radioRects[0];
        for (int k = 1; k < s_radioCount; ++k) RectF::Union(r, r, s_radioRects[k]);
        r.Inflate(Px(3), Px(3));
        return r;
    }
    float pos = L.modPos[part], len = L.modLen[part];
    if (v) return RectF(L.panelX + Px(3), pos, L.panelW - Px(6), len);
    float h = part == kModMedia && L.mediaTall ? L.tile + L.rowGap + L.sliderH : L.tile;
    return RectF(pos, L.tileY, len, h);
}

// 编辑模式里鼠标下是什么：边、能拖的模块（窗口格子那一段不算，拖它是挪面板）、调节条、Wi-Fi 蓝牙，面板上别的地方
int EditPartAt(int x, int y) {
    if (int edges = ResizeEdgeAt(x, y)) return edges & ScaleEdges() ? kPartScaleEdge : kPartLengthEdge;
    PointF pt(static_cast<REAL>(x), static_cast<REAL>(y));
    for (int part = 0; part < kPartScaleEdge; ++part)
        if (part != kModWindows && EditPartShown(part) && EditPartRect(part).Contains(pt)) return part;
    const Layout& L = s_L;
    if (RectF(L.panelX, L.panelY, L.panelW, L.panelH).Contains(pt)) return kPartPanel;
    return kPartNone;
}

const wchar_t* PartName(int part) {
    switch (part) {
        case kModTray: return Tr(L"托盘图标");
        case kModMedia: return Tr(L"媒体");
        case kModIme: return Tr(L"输入法");
        case kModBattery: return Tr(L"电量");
        case kModClock: return Tr(L"时钟");
        case kPartLevels: return Tr(L"音量和亮度");
        case kPartRadios: return Tr(L"Wi-Fi 和蓝牙");
        default: return Tr(L"窗口");
    }
}

// 面板现在停在哪：居中、靠左、靠右，或者自定义的位置
std::wstring PlaceText() {
    if (Vertical()) return Tr(s_align == kAlignCustom ? L"自定义位置" : L"居中");
    switch (EffectiveAlign()) {
        case kAlignLeft: return Tr(L"靠左");
        case kAlignRight: return Tr(L"靠右");
        case kAlignCustom: return Tr(L"自定义位置");
        default: return Tr(L"居中");
    }
}

// 编辑模式里标题上的说明：正在拖的、鼠标下的东西怎么拖
std::wstring EditText() {
    if (s_resize) return SizeText();
    if (s_movePanel) return PlaceText();
    if (s_moveModule != kPartNone) return s_moveOut ? TrF(L"松开隐藏%ls", PartName(s_moveModule)) : Tr(L"放到想要的位置");
    switch (s_editHover) {
        case kPartScaleEdge: return Tr(L"拖动改大小，双击恢复");
        case kPartLengthEdge: return Tr(L"拖动改长度，双击恢复");
        case kPartLevels:
        case kPartRadios: return Tr(L"拖出面板隐藏");
        case kPartPanel: return Tr(L"拖动挪位置，双击恢复");
        case kPartNone: return Tr(L"编辑模式：回车完成，右键更多");
        default: return Tr(L"拖动换位置，拖出面板隐藏");
    }
}

// 在编辑模式里按下鼠标（双击也在这里认）。边上：改大小、长度；模块上：换位置、拖出去隐藏；别处：挪面板
bool EditClickTwice(POINT screen) {
    DWORD now = GetTickCount();
    bool twice = s_edgeClickAt && now - s_edgeClickAt <= GetDoubleClickTime() &&
                 std::abs(screen.x - s_edgeClickPt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                 std::abs(screen.y - s_edgeClickPt.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2;
    s_edgeClickAt = twice ? 0 : now;
    s_edgeClickPt = screen;
    return twice;
}

// ---- 编辑模式：拖模块换顺序、拖出去隐藏 ----

void BeginModuleDrag(int part, POINT screen) {
    s_moveModule = part;
    s_moveOut = false;
    s_moveAt = screen;
    SetCapture(s_hwnd);
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    Redraw();
}

// 把模块挪到第 k 个（这次显示着的里头，不算它自己）后面。没显示的模块在顺序里的位置不动
void MoveModule(int part, int k) {
    int* order = Mods().order;
    std::vector<int> rest;
    for (int i = 0; i < kModuleCount; ++i)
        if (order[i] != part) rest.push_back(order[i]);
    size_t at = 0;
    if (k == 0) {
        while (at < rest.size() && !ModuleShown(rest[at])) ++at;
    } else {
        for (int seen = 0; at < rest.size();)
            if (ModuleShown(rest[at++]) && ++seen == k) break;
    }
    rest.insert(rest.begin() + static_cast<std::ptrdiff_t>(at), part);
    std::copy(rest.begin(), rest.end(), order);
}

void DragModule(POINT screen) {
    const Layout& L = s_L;
    PointF pt(static_cast<REAL>(screen.x - s_pos.x), static_cast<REAL>(screen.y - s_pos.y));
    RectF keep(L.panelX, L.panelY, L.panelW, L.panelH);
    keep.Inflate(Px(16), Px(16));
    bool out = !keep.Contains(pt);
    if (out != s_moveOut) {
        s_moveOut = out;
        Redraw();
    }
    if (out || s_moveModule >= kModuleCount) return;
    // 鼠标越过了别的模块的中线就换到它那一边（多过几个像素才算，免得面板挪了一点又换回来）
    float along = Vertical() ? pt.Y : pt.X, hysteresis = Px(6);
    std::vector<float> centers;
    int cur = -1;
    for (int m : Mods().order) {
        if (!ModuleShown(m)) continue;
        if (m == s_moveModule) {
            cur = static_cast<int>(centers.size());
            continue;
        }
        centers.push_back(L.modPos[m] + L.modLen[m] / 2);
    }
    int k = 0;
    while (k < static_cast<int>(centers.size()) && centers[k] < along) ++k;
    if (k > cur && along < centers[k - 1] + hysteresis) k = cur;
    if (k < cur && along > centers[k] - hysteresis) k = cur;
    if (k == cur || cur < 0) return;
    MoveModule(s_moveModule, k);
    EditRelayout();
}

void ShowPart(int part, bool on);

// drop：松开了鼠标（拖到外面就隐藏）；否则是被打断了，换过的顺序照样留着
void EndModuleDrag(bool drop) {
    if (s_moveModule == kPartNone) return;
    int part = s_moveModule;
    bool out = s_moveOut;
    s_moveModule = kPartNone;  // 先清掉：放开鼠标捕获时还会再进来
    s_moveOut = false;
    if (GetCapture() == s_hwnd) ReleaseCapture();
    if (drop && out) {
        ShowPart(part, false);
        return;
    }
    SaveSettings();
    SettingsDialog_SettingsChanged();
    Redraw();
}

// 编辑模式里隐藏、显示一个模块（调节条、Wi-Fi 蓝牙）：改这条边那份设置、保存，这次弹出的也跟着变
void ShowPart(int part, bool on) {
    ModuleSet& mods = Mods();
    switch (part) {
        case kModTray:
            mods.tray = on;
            s_tray = on ? Tray_Hidden() : std::vector<TrayApp>();
            s_trayPage = s_trayFirst = 0;
            if (on) Tray_Refresh();
            break;
        case kModMedia:
            mods.media = on;
            Media_Configure(AnyModuleSet(&ModuleSet::media), s_hwnd);
            s_media = on ? Media_Get() : MediaInfo();
            if (on) Media_Query();
            break;
        case kModIme:
            mods.ime = on;
            s_showIme = on;
            if (on) s_ime = Ime_Get(s_imeTarget);
            break;
        case kModBattery:
            mods.battery = on;
            s_battery = Battery_Get();
            s_showBattery = on && s_battery.present;
            break;
        case kModClock: mods.clock = on; break;
        case kPartLevels:
            mods.levels = on;
            s_levels = on;
            if (on) {
                RefreshVolume();
                Brightness_Query(s_hwnd, s_monitor);
            }
            break;
        case kPartRadios:
            mods.radios = on;
            s_radioCount = 0;
            for (int i = 0; on && i < kRadioCount; ++i) {
                s_radio[i] = Radios_Get(i);
                if (s_radio[i].status != kRadioMissing && s_radio[i].status != kRadioReading) s_radioIds[s_radioCount++] = i;
            }
            if (on) Radios_Query(s_hwnd);
            break;
        default: return;
    }
    Log(L"编辑模式里%ls了%ls", on ? L"显示" : L"隐藏", PartName(part));
    SyncMediaShown();
    s_editHover = kPartNone;
    SaveSettings();
    SettingsDialog_SettingsChanged();
    EditRelayout();
}

// ---- 编辑模式：拖面板挪位置 ----

// 位置记到所有屏幕共用的，或者这块屏幕自己的（每块屏幕分别设置时）
void StorePlace() {
    if (g_settings.popupPerScreen) {
        std::wstring key = MonitorKey(s_monitor);
        ScreenPlace* p = FindScreenPlace(g_settings.screens, key);
        if (!p) {
            g_settings.screens.push_back({key, s_edge, g_settings.popupAlign, g_settings.popupOffset});
            p = &g_settings.screens.back();
        }
        p->align = s_align;
        p->offset = s_offset;
    } else {
        g_settings.popupAlign = s_align;
        g_settings.popupOffset = s_offset;
    }
}

void BeginPanelMove(POINT screen) {
    s_movePanel = true;
    s_moveAt = screen;
    s_moveFromPos = s_pos;
    s_moveFromAlign = s_align;
    s_moveFromOffset = s_offset;
    SetCapture(s_hwnd);
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    Redraw();
}

// 面板跟着鼠标沿着屏幕边走。靠近正中间、两头时吸过去，记成居中、靠左、靠右（原来就是这样的话保持原来的，比如跟随系统）
void DragPanel(POINT screen) {
    bool v = Vertical();
    const RECT& b = s_bounds;
    LONG from = v ? b.top : b.left, to = v ? b.bottom : b.right;
    int size = v ? s_height : s_width;
    if (size >= to - from) return;
    int at = (v ? s_moveFromPos.y : s_moveFromPos.x) + (v ? screen.y - s_moveAt.y : screen.x - s_moveAt.x);
    at = std::clamp<int>(at, from, to - size);
    int snap = static_cast<int>(Px(16));
    int center = static_cast<int>(from + (to - from - size) / 2);
    int align = kAlignCustom;
    if (std::abs(at - center) <= snap) align = kAlignCenter;
    else if (!v && at - from <= snap) align = kAlignLeft;
    else if (!v && to - size - at <= snap) align = kAlignRight;
    // 吸到了原来对齐的位置：保持原来的设置
    int fromAlign = s_moveFromAlign;
    s_align = fromAlign;
    bool same = align == kAlignCenter ? (v ? fromAlign != kAlignCustom : EffectiveAlign() == kAlignCenter)
                                      : align != kAlignCustom && EffectiveAlign() == align;
    if (!same) s_align = v && align == kAlignCenter ? kAlignSystem : align;
    if (s_align == kAlignCustom)
        s_offset = std::clamp(static_cast<int>(std::lround((at + size / 2.0 - from) * kMaxPopupOffset / (to - from))), 0,
                              kMaxPopupOffset);
    POINT was = s_pos;
    UpdatePlacement();
    if (was.x != s_pos.x || was.y != s_pos.y) s_baseValid = false;
    Redraw();
}

void EndPanelMove(bool drop) {
    if (!s_movePanel) return;
    s_movePanel = false;
    if (GetCapture() == s_hwnd) ReleaseCapture();
    if (s_align != s_moveFromAlign || s_offset != s_moveFromOffset) {
        if (!drop) {  // 被打断了：放回去
            s_align = s_moveFromAlign;
            s_offset = s_moveFromOffset;
            UpdatePlacement();
        } else {
            StorePlace();
            Log(L"拖动挪了迷你任务栏：%ls（%d）", PlaceText().c_str(), s_offset);
            SaveSettings();
            SettingsDialog_SettingsChanged();
        }
    }
    Redraw();
}

// 双击挪面板的地方：恢复默认位置（底边跟随系统，竖排上下居中）
void ResetPlace() {
    if (s_align == kAlignSystem) return;
    s_align = kAlignSystem;
    s_offset = 500;
    StorePlace();
    Log(L"迷你任务栏恢复默认位置");
    SaveSettings();
    SettingsDialog_SettingsChanged();
    EditRelayout();
}

void ResetOrder() {
    for (int i = 0; i < kModuleCount; ++i) Mods().order[i] = i;
    Log(L"迷你任务栏恢复默认顺序");
    SaveSettings();
    SettingsDialog_SettingsChanged();
    EditRelayout();
}

// 编辑模式里的右键菜单：显示哪些模块、恢复默认顺序和位置、完成
void ShowEditMenu(POINT client) {
    enum { kCmdOrder = 1, kCmdPlace, kCmdDone, kCmdPart = 100 };
    HMENU menu = CreatePopupMenu();
    const int parts[] = {kModTray, kModMedia, kModIme, kModBattery, kModClock, kPartLevels, kPartRadios};
    const ModuleSet& mods = Mods();
    const bool shown[] = {mods.tray, mods.media, mods.ime, mods.battery, mods.clock, mods.levels, mods.radios};
    for (size_t i = 0; i < ARRAYSIZE(parts); ++i)
        AppendMenuW(menu, MF_STRING | (shown[i] ? MF_CHECKED : 0), kCmdPart + parts[i], PartName(parts[i]));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    bool defaultOrder = true;
    for (int i = 0; i < kModuleCount; ++i) defaultOrder = defaultOrder && mods.order[i] == i;
    AppendMenuW(menu, MF_STRING | (defaultOrder ? MF_GRAYED : 0), kCmdOrder, Tr(L"恢复默认顺序"));
    AppendMenuW(menu, MF_STRING | (s_align == kAlignSystem ? MF_GRAYED : 0), kCmdPlace, Tr(L"恢复默认位置"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdDone, Tr(L"完成"));
    POINT at = {client.x + s_pos.x, client.y + s_pos.y};
    s_menuOpen = true;
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | MenuAlign(), at.x, at.y, 0, s_hwnd,
                              nullptr);
    s_menuOpen = false;
    DestroyMenu(menu);
    if (!s_open || !s_editing) return;
    if (cmd >= kCmdPart) {
        int part = static_cast<int>(cmd) - kCmdPart;
        for (size_t i = 0; i < ARRAYSIZE(parts); ++i)
            if (parts[i] == part) ShowPart(part, !shown[i]);
    } else if (cmd == kCmdOrder) {
        ResetOrder();
    } else if (cmd == kCmdPlace) {
        ResetPlace();
    } else if (cmd == kCmdDone) {
        SetEditing(false);
    }
}

void BeginResize(int edges, POINT screen) {
    // 双击边恢复：改大小的边恢复 100%，改长度的边恢复成跟着内容
    bool twice = EditClickTwice(screen);
    s_glass.Extend(GrowArea());  // 背景按最大的大小截一整条（迷你任务栏自己盖着的那块用弹出时截的）
    s_baseValid = false;
    if (twice) {
        int percent = edges & ScaleEdges() ? kDefaultPopupScale : g_settings.popupScale;
        int length = edges & LengthEdges() ? 0 : g_settings.popupLength;
        if (percent != g_settings.popupScale || length != g_settings.popupLength) {
            SetPopupSize(percent, length);
            Log(L"双击迷你任务栏的边恢复：%ls", SizeText().c_str());
            SaveSettings();
        }
        return;
    }
    s_resize = edges;
    s_resizeFrom = g_settings.popupScale;
    s_resizeLenFrom = g_settings.popupLength;
    s_resizeDist = std::max(Px(8), ScaleDistance(screen));
    s_resizeLen = Vertical() ? s_L.panelH : s_L.panelW;
    s_resizeAt = screen;
    SetCapture(s_hwnd);
    SetCursor(LoadCursorW(nullptr, ResizeCursor(edges)));
    Redraw();
}

// 编辑模式里按下了鼠标左键（client：窗口里的位置）
void EditMouseDown(int x, int y) {
    DWORD pos = GetMessagePos();
    POINT screen = {GET_X_LPARAM(pos), GET_Y_LPARAM(pos)};
    if (int edges = ResizeEdgeAt(x, y)) {
        BeginResize(edges, screen);
        return;
    }
    int part = EditPartAt(x, y);
    if (part == kPartPanel) {
        if (EditClickTwice(screen)) ResetPlace();
        else BeginPanelMove(screen);
    } else if (part != kPartNone) {
        s_edgeClickAt = 0;
        BeginModuleDrag(part, screen);
    }
}

void DragResize(POINT screen) {
    int percent = g_settings.popupScale, length = g_settings.popupLength;
    if (s_resize & ScaleEdges()) percent = static_cast<int>(std::lround(s_resizeFrom * ScaleDistance(screen) / s_resizeDist));
    bool fit = (s_resize & LengthEdges()) != 0;
    if (fit)
        length = std::clamp(static_cast<int>(std::lround(LengthAt(screen) / ScreenSide() * kMaxPopupLength)), 1,
                            kMaxPopupLength);
    SetPopupSize(percent, length, fit);
}

void EndResize() {
    if (!s_resize) return;
    s_resize = 0;  // 先清掉：放开鼠标捕获时还会再进来
    if (GetCapture() == s_hwnd) ReleaseCapture();
    if (g_settings.popupScale != s_resizeFrom || g_settings.popupLength != s_resizeLenFrom) {
        Log(L"拖边改了迷你任务栏：%ls", SizeText().c_str());
        SaveSettings();
    }
    Redraw();
}

// 编辑模式：面板四周一圈虚线，能拖的边中间一个小把手
void DrawEditFrame(Graphics& g) {
    const Layout& L = s_L;
    const Color& accent = Colors().accent;
    float inset = Px(1.5f);
    RectF r(L.panelX + inset, L.panelY + inset, L.panelW - 2 * inset, L.panelH - 2 * inset);
    GraphicsPath path;
    AddRoundRect(path, r, std::max(0.0f, L.radius - inset));
    Pen pen(accent, std::max(1.0f, Px(1.5f)));
    REAL dash[2] = {4, 3};
    pen.SetDashPattern(dash, 2);
    g.DrawPath(&pen, &path);
    int edges = ResizableEdges();
    float len = Px(36), th = Px(5);
    float cx = L.panelX + L.panelW / 2, cy = L.panelY + L.panelH / 2;
    auto bar = [&](float x, float y, bool across) {
        FillRound(g, across ? RectF(x - len / 2, y - th / 2, len, th) : RectF(x - th / 2, y - len / 2, th, len), th / 2, accent);
    };
    if (edges & kResizeTop) bar(cx, r.Y, true);
    if (edges & kResizeBottom) bar(cx, r.Y + r.Height, true);
    if (edges & kResizeLeft) bar(r.X, cy, false);
    if (edges & kResizeRight) bar(r.X + r.Width, cy, false);
    // 能拖的模块：细虚线框；鼠标下的、正拖着的描强调色、铺一层底，要拖出去隐藏时是红的
    bool busy = s_resize || s_movePanel || s_moveModule != kPartNone;
    for (int part = 0; part < kPartScaleEdge; ++part) {
        if (part == kModWindows || !EditPartShown(part)) continue;
        bool active = busy ? part == s_moveModule : part == s_editHover;
        Color c = part == s_moveModule && s_moveOut ? Color(255, 232, 72, 72) : accent;
        RectF m = EditPartRect(part);
        GraphicsPath mp;
        AddRoundRect(mp, m, std::min(PillRadius(), std::min(m.Width, m.Height) / 2));
        if (active) {
            SolidBrush fill(Color(50, c.GetR(), c.GetG(), c.GetB()));
            g.FillPath(&fill, &mp);
        }
        Pen mpen(active ? c : Colors().subtle, std::max(1.0f, Px(1)));
        if (!active) mpen.SetDashPattern(dash, 2);
        g.DrawPath(&mpen, &mp);
    }
}

LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            if (s_preview) return HTTRANSPARENT;  // 预览时鼠标穿过去，还是在设置窗口上拖滑块
            break;

        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (int edges = ResizeEdgeAt(pt.x, pt.y)) {
                    SetCursor(LoadCursorW(nullptr, ResizeCursor(edges)));
                    return TRUE;
                }
                if (s_editing && EditPartAt(pt.x, pt.y) != kPartNone) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                    return TRUE;
                }
            }
            break;

        case WM_MOUSEACTIVATE:
            if (s_preview) return MA_NOACTIVATEANDEAT;  // 预览时点到它也不抢前台
            break;

        case WM_ACTIVATE:
            if (s_open && s_hoverOpen && LOWORD(wParam) != WA_INACTIVE) {
                // 鼠标停在边上弹出的，点了它：接着当平常的迷你任务栏用，点别处才收起
                EndHoverOpen();
                SetFocus(hwnd);
                return 0;
            }
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
                if (s_showBattery) s_battery = Battery_Get();
                if (s_showIme) s_ime = Ime_Get(s_imeTarget);  // 在别处（比如按 Shift）切换过
                if (s_showMedia) Media_Query();
                // Wi-Fi 刚打开、还没连上网络：每 2 秒再读一遍，连上了标题就显示网络名
                const RadioInfo& wifi = s_radio[kRadioWifi];
                if (std::find(s_radioIds, s_radioIds + s_radioCount, kRadioWifi) != s_radioIds + s_radioCount &&
                    wifi.status == kRadioOn && !wifi.switching && wifi.network.empty() && s_radioPolls < 30 &&
                    ++s_radioPolls % 2 == 0)
                    Radios_Query(s_hwnd);
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
                if (AnyModuleSet(&ModuleSet::tray) && !s_open) Tray_Refresh();
            } else if (wParam == kTimerEdge) {
                WatchEdge();
            } else if (wParam == kTimerHoverLeave) {
                WatchHoverLeave();
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

        case WM_APP_MEDIA:
            if (s_open && s_showMedia) {
                s_media = Media_Get();
                Redraw();
            }
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
            if (s_resize || s_moveModule != kPartNone || s_movePanel) {
                // 窗口跟着大小、位置挪了，按屏幕坐标算
                DWORD pos = GetMessagePos();
                POINT screen = {GET_X_LPARAM(pos), GET_Y_LPARAM(pos)};
                if (s_resize) DragResize(screen);
                else if (s_movePanel) DragPanel(screen);
                else DragModule(screen);
                return 0;
            }
            if (s_editing) {  // 编辑模式里不选格子，只看鼠标下是什么能拖的
                int part = EditPartAt(x, y);
                if (part != s_editHover) {
                    s_editHover = part;
                    Redraw();
                }
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                return 0;
            }
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
            int extra = hit < 0 && control == kNone && radio < 0 ? ExtraAt(x, y) : kExtraNone;
            bool free = hit < 0 && control == kNone && radio < 0 && extra == kExtraNone;
            int tray = free ? TrayAt(x, y) : -1;
            bool overTray = free && OverTray(x, y);
            if (hit != s_hover || control != s_hoverControl || radio != s_hoverRadio || extra != s_hoverExtra ||
                tray != s_hoverTray || overTray != s_overTray) {
                if (radio != s_hoverRadio && s_hoverRadio >= 0) s_radioFailed &= ~(1 << s_radioIds[s_hoverRadio]);
                s_hover = hit;
                s_hoverControl = control;
                s_hoverRadio = radio;
                s_hoverExtra = extra;
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
            s_editHover = kPartNone;
            s_hover = -1;
            s_hoverControl = kNone;
            s_hoverRadio = -1;
            s_radioFailed = 0;
            s_hoverExtra = kExtraNone;
            s_hoverTray = -1;
            s_overTray = false;
            Redraw();
            return 0;

        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam), control = ControlAt(x, y);
            if (s_editing) {  // 编辑模式里只能拖边、拖模块、挪面板
                EditMouseDown(x, y);
                return 0;
            }
            if ((control == kVolumeTrack || control == kBrightTrack) && SliderEnabled(SliderOf(control))) {
                s_drag = control;
                SetCapture(hwnd);
                SetSliderAt(SliderOf(control), x, y);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (s_resize || s_editing) {
                EndResize();
                EndModuleDrag(true);
                EndPanelMove(true);
                if (s_editing) {
                    s_editHover = EditPartAt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                    Redraw();
                }
                return 0;
            }
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
            if (int extra = ExtraAt(x, y); extra != kExtraNone) {
                ClickExtra(extra);
                return 0;
            }
            int hit = HitTest(x, y);
            if (hit >= 0) ActivateIndex(hit, (wParam & MK_SHIFT) != 0);
            else ActivateTray(TrayAt(x, y), false);
            return 0;
        }

        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lParam) != hwnd) {
                EndResize();
                EndModuleDrag(false);
                EndPanelMove(false);
            }
            if (s_drag != kNone && reinterpret_cast<HWND>(lParam) != hwnd) {
                s_drag = kNone;
                Redraw();
            }
            return 0;

        case WM_MBUTTONUP:
            if (s_editing) return 0;
            CloseItem(HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)));
            return 0;

        case WM_RBUTTONUP: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (s_editing) {
                if (!s_resize && s_moveModule == kPartNone && !s_movePanel) ShowEditMenu(pt);
                return 0;
            }
            int hit = HitTest(pt.x, pt.y);
            if (hit >= 0) ShowItemMenu(hit, pt);
            else if (int radio = RadioAt(pt.x, pt.y); radio >= 0) OpenRadioPanel(radio);
            else ActivateTray(TrayAt(pt.x, pt.y), true);
            return 0;  // 不交给 DefWindowProc，免得它再发一次 WM_CONTEXTMENU
        }

        case WM_CONTEXTMENU:
            // 鼠标右键在上面处理了；这里是菜单键（DefWindowProc 在它松开时发来，坐标是 -1, -1）
            if (GET_X_LPARAM(lParam) == -1 && GET_Y_LPARAM(lParam) == -1 && s_open && !s_editing) ShowSelectedMenu();
            return 0;

        case WM_SYSKEYDOWN:
            // Shift+F10 和菜单键一样（F10 是系统键，不走 WM_KEYDOWN）。自己处理掉，不让 DefWindowProc 再发 WM_CONTEXTMENU、
            // 松开 F10 时进菜单模式
            if (wParam == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000)) {
                if (s_open && !s_editing) ShowSelectedMenu();
                return 0;
            }
            break;

        case WM_MOUSEWHEEL: {
            if (s_editing) return 0;
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
            // 短按 Win 弹出、开始菜单还没出来：回车、空格是要给开始菜单的（在 WM_CHAR 里记下），别打开或最小化选中的窗口
            if (s_compact && !s_startSeen && (s_startPending || s_startMode) && (wParam == VK_RETURN || wParam == VK_SPACE))
                return 0;
            // F2：进出编辑模式（拖边改大小和长度）。编辑模式里回车、Esc 退出，别的键不管用
            if (wParam == VK_F2) {
                SetEditing(!s_editing);
                return 0;
            }
            if (s_editing) {
                if (wParam == VK_RETURN || wParam == VK_ESCAPE) SetEditing(false);
                return 0;
            }
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
            if (s_editing) return 0;
            wchar_t ch = static_cast<wchar_t>(wParam);
            if (s_compact) {  // 开始菜单模式没有显示打的字的地方：开始菜单还没出来就先记下，出来以后补发给它
                if (!s_startSeen && (s_startPending || s_startMode) && (ch >= 0x20 || ch == L'\r')) {
                    s_startTyped += ch;
                    if (s_startPending) BeginStart();  // 打字了就不等动画，马上打开开始菜单
                }
                return 0;
            }
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
        for (const PinnedApp& pin : LoadPinnedApps(IconPx())) {
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
    if (AnyModuleSet(&ModuleSet::tray)) Tray_Refresh();
    // Wi-Fi 和蓝牙也先读一遍：第一次弹出时就知道这台电脑上有没有
    if (AnyModuleSet(&ModuleSet::radios) && s_hwnd) Radios_Query(s_hwnd);
    if (s_hwnd) SetTimer(s_hwnd, kTimerTray, kTrayRefreshMs, nullptr);

    // 提前读一遍固定的应用和图标，第一次弹出时就不卡
    if (g_settings.showPinnedApps) {
        POINT origin = {0, 0};
        s_look = LookFromSettings();
        s_scale = PopupScale(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY));
        LoadPinnedApps(IconPx());
    }
}

void Popup_Destroy() {
    Thumb_Destroy();
    s_media = MediaInfo();  // 封面是 GDI+ 位图，要在 GDI+ 关掉之前放掉
    s_coverFrom.reset();
    s_coverScaled.reset();
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

// 弹在哪块屏幕上。预览弹在设置窗口所在的屏幕上（每块屏幕分别设置时改的就是它）；
// 设成跟着前台窗口时，前台是桌面、任务栏、本程序这类没有“所在屏幕”的窗口的话还是跟着鼠标。
// startMenu：短按 Win 要接着打开开始菜单，照旧跟着鼠标
HMONITOR MonitorForPopup(HWND fg, bool preview, bool startMenu = false) {
    if (s_forceMonitor && !preview) return s_forceMonitor;
    if (preview)
        if (HWND dialog = SettingsDialog_Hwnd()) return MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST);
    if (g_settings.popupMonitor == kPopupAtActive && !startMenu && fg && IsWindowVisible(fg) && !IsIconic(fg) &&
        !IsOwnProcess(fg)) {
        std::wstring cls = GetClassNameStr(fg);
        if (cls != L"Progman" && cls != L"WorkerW" && cls != L"Shell_TrayWnd" && cls != L"Shell_SecondaryTrayWnd")
            return MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    }
    POINT pt;
    GetCursorPos(&pt);
    return MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
}

// 弹出。preview：设置窗口里调大小时的预览，不抢前台。
// startMenu：任务栏藏着时短按了 Win，只留一行格子（竖排时一列）贴着屏幕边弹出来，弹完打开开始菜单。
// hover：鼠标停在屏幕边上弹出的，弹在鼠标所在的屏幕上，不抢前台，点了它才拿前台
// 按这条边那份设置定下这次显示哪些模块，读好它们现在的状态（弹出时；预览时设置窗口里改了开关也再来一遍）
void LoadModules(bool preview) {
    // 音量马上就能读到；亮度在后台读，读到之前先显示上次的值
    const ModuleSet& mods = Mods();
    s_levels = mods.levels && !s_compact;
    s_brightness = -2;
    for (const auto& known : s_knownBrightness)
        if (known.first == s_monitor) s_brightness = known.second;
    if (s_levels) {
        RefreshVolume();
        Brightness_Query(s_hwnd, s_monitor);
    }
    s_brightnessEdited = false;
    s_hoverControl = kNone;
    s_drag = kNone;
    s_wheelRest = 0;

    // Wi-Fi 和蓝牙先用上次读到的（启动时就读过一遍），这台电脑上没有的、还没读到的不放按钮；弹出以后再读一遍
    s_radioCount = 0;
    if (mods.radios && !s_compact) {
        for (int i = 0; i < kRadioCount; ++i) {
            s_radio[i] = Radios_Get(i);
            if (s_radio[i].status != kRadioMissing && s_radio[i].status != kRadioReading) s_radioIds[s_radioCount++] = i;
        }
        if (!preview) Radios_Query(s_hwnd);
    }
    s_hoverRadio = -1;
    s_radioFailed = 0;
    s_radioPolls = 0;

    // 媒体按钮看后台最近读到的有没有在放（面板宽度弹出时就定了），弹出以后再读一遍；
    // 输入法看弹出前的前台窗口；没有电池不显示电量
    s_media = mods.media ? Media_Get() : MediaInfo();
    s_mediaOpen = false;
    SyncMediaShown();
    if (mods.media && !preview) Media_Query();
    s_imeTarget = s_prevForeground;
    s_showIme = mods.ime && !s_compact;
    if (s_showIme) s_ime = Ime_Get(s_imeTarget);
    s_battery = Battery_Get();
    s_showBattery = mods.battery && !s_compact && s_battery.present;
    s_hoverExtra = kExtraNone;

    // 托盘图标用后台读好的（面板宽度弹出时就定了，弹出期间读到的新结果留到下次）
    s_tray = mods.tray ? Tray_Hidden() : std::vector<TrayApp>();
    s_trayStyle = s_look.trayStyle;
    s_hoverTray = -1;
    s_overTray = false;
    s_trayPage = 0;
    s_trayFirst = 0;
    s_flipFrom = -1;
    EndTrayMode();
    if (mods.tray && !preview) Tray_Refresh();
}

void Open(bool preview, bool startMenu = false, bool hover = false) {
    if (!s_hwnd) return;
    s_preview = preview;
    EndHoverOpen();
    HWND fg = GetForegroundWindow();
    HMONITOR mon = MonitorForPopup(fg, preview, startMenu || hover);
    PopupPlaceFor(mon, s_edge, s_align, s_offset);
    if (preview) {
        s_edge = s_previewEdge;
        s_align = s_previewAlign;
    }
    s_look = preview ? s_previewLook : LookFromSettings();
    // 开始菜单靠左时会盖住贴在左边的那一列：这时从底边弹
    if (startMenu && s_edge == kEdgeLeft && !SystemTaskbarCentered()) s_edge = kEdgeBottom;
    KillTimer(s_hwnd, kTimerPreview);
    EndStartMode();
    s_compact = startMenu;
    s_startPending = startMenu;
    if (fg != s_hwnd) s_prevForeground = fg;

    // 任务栏露着时弹在它上面，藏起来时贴着屏幕底边
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    s_bounds = Taskbar_IsShownOn(mon) ? mi.rcWork : mi.rcMonitor;
    s_scale = PopupScale(mon);

    s_monitor = mon;
    LoadModules(preview);

    s_allItems = BuildTiles(EnumerateWindows(s_prevForeground, IconPx(), s_monitor));
    s_items = s_allItems;
    s_filter.clear();
    s_lockedItemsW = 0;
    s_keyNav = false;
    s_menuOpen = false;
    s_lastMouse = {INT_MIN, INT_MIN};
    if (!preview) Apps_Refresh(s_hwnd);  // 打字筛选时要搜的所有应用，在后台先读好
    FitTiles();
    UpdatePlacement();

    // 截下玻璃后面的屏幕内容：这时弹窗自己不能在屏幕上
    if (IsWindowVisible(s_hwnd)) {
        ShowWindow(s_hwnd, SW_HIDE);
        WaitForVBlank();
    }
    RECT window = {s_pos.x, s_pos.y, s_pos.x + s_width, s_pos.y + s_height};
    if (preview) window = GrowArea();  // 预览时大小会一直变
    s_glass.SetLight(s_look.light);
    s_glass.SetStyle(s_look.style);
    s_glass.SetTransparency(s_look.transparency);
    s_glass.SetTint(s_look.customTint, s_look.tint);
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
    if (preview || hover) {
        ShowWindow(s_hwnd, SW_SHOWNOACTIVATE);
        if (hover) {
            s_hoverOpen = true;
            s_hoverAwayAt = 0;
            SetTimer(s_hwnd, kTimerHoverLeave, kHoverWatchMs, nullptr);
        }
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
    if (!preview) Attention_Update();  // 这块屏上提醒的光让给迷你任务栏
}

// 鼠标在不在这块屏幕上迷你任务栏那条边上（最外面一排像素）
bool AtPopupEdge(POINT pt, HMONITOR mon) {
    MONITORINFO mi = {sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) return false;
    int edge = kEdgeBottom, align = kAlignSystem, offset = 500;
    PopupPlaceFor(mon, edge, align, offset);
    const RECT& m = mi.rcMonitor;
    if (edge == kEdgeLeft) return pt.x <= m.left;
    if (edge == kEdgeRight) return pt.x >= m.right - 1;
    return pt.y >= m.bottom - 1;
}

bool CursorHidden() {
    CURSORINFO ci = {sizeof(ci)};
    return GetCursorInfo(&ci) && !(ci.flags & CURSOR_SHOWING);
}

// 任务栏藏着时鼠标在迷你任务栏那条边上停一下就弹出。按着鼠标键（在拖东西）、光标藏着、
// 前台是那块屏上全屏的游戏或视频时不弹；弹过一次以后鼠标要先离开那条边
void WatchEdge() {
    if (!g_settings.edgePopup || s_open || !Fullscreen_TaskbarHidden()) {
        s_edgeSince = 0;
        return;
    }
    POINT pt;
    if (!GetCursorPos(&pt)) return;
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
    if (!mon || !AtPopupEdge(pt, mon) || Taskbar_IsShownOn(mon)) {
        s_edgeSince = 0;
        s_edgeArmed = true;
        return;
    }
    HWND fg = GetForegroundWindow();
    if (!s_edgeArmed || MouseButtonDown() || CursorHidden() ||
        (IsFullscreenApp(fg) && MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) == mon)) {
        s_edgeSince = 0;
        return;
    }
    DWORD now = GetTickCount();
    if (!s_edgeSince) {
        s_edgeSince = now ? now : 1;
        return;
    }
    if (now - s_edgeSince < kEdgeDwellMs) return;
    s_edgeSince = 0;
    s_edgeArmed = false;
    Log(L"鼠标停在屏幕边上：弹出迷你任务栏");
    Open(false, false, true);
}

// 鼠标停在边上弹出、还没点过它：鼠标离开迷你任务栏一会儿就收起。点了它（拿到了前台）以后照平常的来
void WatchHoverLeave() {
    if (!s_open || !s_hoverOpen) {
        EndHoverOpen();
        return;
    }
    if (GetForegroundWindow() == s_hwnd) {
        EndHoverOpen();
        return;
    }
    POINT pt;
    GetCursorPos(&pt);
    RECT r = {s_pos.x, s_pos.y, s_pos.x + s_width, s_pos.y + s_height};
    if (PtInRect(&r, pt) || s_menuOpen || s_drag != kNone || s_trayMode) {
        s_hoverAwayAt = 0;
        return;
    }
    DWORD now = GetTickCount();
    if (!s_hoverAwayAt) {
        s_hoverAwayAt = now ? now : 1;
        return;
    }
    if (now - s_hoverAwayAt < kHoverLeaveMs) return;
    Log(L"鼠标离开了迷你任务栏（鼠标停在边上弹出的），收起");
    Popup_Hide();
}

}  // namespace

void Popup_Show() { Open(false); }

void Popup_Configure() {
    if (!s_hwnd) return;
    if (g_settings.edgePopup) {
        SetTimer(s_hwnd, kTimerEdge, kEdgeWatchMs, nullptr);
    } else {
        KillTimer(s_hwnd, kTimerEdge);
        s_edgeSince = 0;
    }
    Media_Configure(AnyModuleSet(&ModuleSet::media), s_hwnd);
}

void Popup_Refresh() { Redraw(); }

HMONITOR Popup_Monitor() { return s_open && !s_preview ? s_monitor : nullptr; }

void Popup_NextScreen() {
    if (!s_hwnd) return;
    if (GetSystemMetrics(SM_CMONITORS) < 2) {  // 只有一块屏幕：和弹出迷你任务栏的快捷键一样
        Popup_Toggle();
        return;
    }
    HMONITOR from = s_open && !s_preview ? s_monitor : MonitorForPopup(GetForegroundWindow(), false);
    HMONITOR to = NextMonitor(from);
    if (!to) return;
    Log(L"按了在下一块屏幕弹出的快捷键：弹在%ls", MonitorLabel(to).c_str());
    if (s_open) {
        HideNow();
        WaitForVBlank();  // 等屏幕上没了它再截背景
    }
    s_forceMonitor = to;
    Open(false);
    s_forceMonitor = nullptr;
}

void Popup_DisplayChanged() {
    s_knownBrightness.clear();  // 屏幕的句柄可能换了，记着的亮度对不上
    if (s_hwnd && s_open) HideNow();  // 位置、大小都是按原来的屏幕算的
}

PopupLook LookFromSettings() {
    const Settings& st = g_settings;
    PopupLook look;
    look.style = st.glassStyle;
    look.light = st.customTint ? ColorLuma(st.tintColor) > 150 : ThemeIsLight(st.theme, true);
    look.transparency = st.glassTransparency;
    look.radius = st.popupRadius;
    look.iconSize = st.iconSize;
    look.iconGap = st.iconGap;
    look.showTitle = st.showTitle;
    look.customTint = st.customTint;
    look.tint = st.tintColor;
    look.customAccent = st.customAccent;
    look.accent = st.accentColor;
    look.modules[kSideBottom] = st.modules[kSideBottom];
    look.modules[kSideEdge] = st.modules[kSideEdge];
    look.clockSeconds = st.clockSeconds;
    look.clockWeekday = st.clockWeekday;
    look.clockDate = st.clockDate;
    look.trayStyle = st.trayStyle;
    return look;
}

namespace {

bool SameModules(const ModuleSet& a, const ModuleSet& b) {
    return a.tray == b.tray && a.levels == b.levels && a.radios == b.radios && a.media == b.media && a.ime == b.ime &&
           a.battery == b.battery && a.clock == b.clock && std::equal(a.order, a.order + kModuleCount, b.order);
}

}  // namespace

void Popup_PreviewLook(const PopupLook& look) {
    bool restyle = look.style != s_previewLook.style;
    s_previewLook = look;
    if (!s_hwnd || !s_open || !s_preview) return;
    if (restyle) {  // 各种材质模糊得不一样，要重新截背景：收起，接下来的 Popup_Preview 再弹出来
        HideNow();
        WaitForVBlank();  // 等屏幕上没了它再截，不然新背景里会有它的残影
        return;
    }
    int side = ModuleSideOf(s_edge);
    bool reload = !SameModules(look.modules[side], s_look.modules[side]) || look.trayStyle != s_look.trayStyle;
    s_look = look;
    s_glass.SetLight(look.light);
    s_glass.SetTransparency(look.transparency);
    s_glass.SetTint(look.customTint, look.tint);
    if (reload) LoadModules(true);  // 改了显示哪些：重新定下这次显示的模块
    FitTiles();
    UpdatePlacement();
    s_baseValid = false;
    s_pillIndex = -1;
    Redraw();
}

void Popup_Preview(int percent, int edge, int align, bool hold) {
    if (!s_hwnd || (s_open && !s_preview)) return;  // 真的弹出着就不打扰
    percent = std::clamp(percent, kMinPopupScale, kMaxPopupScale);
    // 换了边，或者设置窗口挪到了别的屏幕上：重新截背景，从新的那条边、新的屏幕弹出来
    if (s_open && (edge != s_previewEdge || MonitorForPopup(nullptr, true) != s_monitor)) {
        HideNow();
        WaitForVBlank();
    }
    if (!s_open) {
        s_previewPercent = percent;
        s_previewEdge = edge;
        s_previewAlign = align;
        Open(true);
    } else if (percent != s_previewPercent || align != s_previewAlign) {
        s_previewPercent = percent;
        s_previewAlign = align;
        s_align = align;
        s_scale = PopupScale(s_monitor);
        FitTiles();
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
    EndHoverOpen();
    s_open = false;
    bool atRest = s_anim == Anim::None;
    s_hideFromSlide = atRest ? 0 : s_lastSlide;
    s_hideFromAlpha = atRest ? 255 : s_lastAlpha;
    s_anim = Anim::Hiding;
    s_animStart = NowMs();
    KillTimer(s_hwnd, kTimerClock);
    RequestFrame();
    if (!s_preview) Attention_Update();
}

void Popup_Toggle() {
    if (s_open) Popup_Hide();
    else Popup_Show();
}

void Popup_StartMenu(DWORD vk) {
    HMONITOR mon = MonitorForPopup(GetForegroundWindow(), false, true);
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
