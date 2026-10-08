// 迷你任务栏：液态玻璃质感的分层窗口，从屏幕底部中央弹上来。
// 先按任务栏上的顺序列出固定的应用（已经打开的换成它的窗口），再列出其余打开的窗口。
// 单击切换窗口（再点当前窗口则最小化）或启动应用，Shift+单击再开一个，中键关闭窗口，
// 方向键 + 回车选择，Esc 或点到别处收起。直接打字筛选窗口和应用（也搜开始菜单里的所有应用）。
// 鼠标停在窗口图标上时在上方显示它的实时缩略图。
// 下面一行是音量和亮度调节条：拖动或在上面滚滚轮调节，点喇叭静音。
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
constexpr double kShowMs = 320.0 * TP_ANIM_SCALE;
constexpr double kHideMs = 170.0 * TP_ANIM_SCALE;
constexpr double kPillMs = 160.0 * TP_ANIM_SCALE;

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
};

// 调节条：0 是音量，1 是亮度
struct Slider {
    RectF icon;      // 左边的图标（喇叭可以点）
    float x0, x1;    // 槽的两端
    float cy;        // 槽的中线
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
Anim s_anim = Anim::None;
double s_animStart = 0;
float s_lastSlide = 0;  // 最近一帧的位置和透明度：弹出到一半就收起时从这里接着走
BYTE s_lastAlpha = 255;
float s_hideFromSlide = 0;
BYTE s_hideFromAlpha = 255;
bool s_framePosted = false;
std::unique_ptr<FontFamily> s_fontFamily;

// 音量和亮度
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

const Palette& Colors() { return s_glass.Light() ? kLight : kDark; }

void ComputeLayout() {
    Layout& L = s_L;
    L.margin = Px(24);  // 给阴影留位置，也是面板离屏幕底边的距离
    L.pad = Px(8);
    L.tile = Px(48);
    L.gap = Px(4);
    L.titleH = Px(24);
    L.sep = Px(9);
    L.clockW = Px(72);
    L.emptyW = Px(130);
    L.radius = Px(22);
    L.rowGap = Px(2);
    L.sliderH = Px(34);
    L.minPanelW = Px(380);  // 两根调节条要有地方拖

    float fixed = 2 * L.margin + 2 * L.pad + L.tile + 2 * L.sep + L.clockW;
    float avail = (s_bounds.right - s_bounds.left) * 0.85f - fixed;
    int maxFit = std::max(1, static_cast<int>((avail + L.gap) / (L.tile + L.gap)));
    int n = static_cast<int>(s_items.size());
    s_visible = std::min(n, maxFit);

    // 格子少的时候面板按最小宽度来，格子在中间那段里居中
    float others = 2 * L.pad + L.tile + 2 * L.sep + L.clockW;
    if (s_lockedItemsW > 0) {
        int fit = std::max(1, static_cast<int>((s_lockedItemsW + L.gap) / (L.tile + L.gap)));
        s_visible = std::min(n, fit);
    }
    float tilesW = n == 0 ? L.emptyW : s_visible * (L.tile + L.gap) - L.gap;
    L.itemsW = s_lockedItemsW > 0 ? s_lockedItemsW : std::max(tilesW, L.minPanelW - others);
    L.panelW = others + L.itemsW;
    L.panelH = 2 * L.pad + L.titleH + L.tile + L.rowGap + L.sliderH;
    L.itemsX = L.margin + L.pad + L.tile + L.sep;
    L.tilesX = L.itemsX + (L.itemsW - tilesW) / 2;
    L.clockX = L.itemsX + L.itemsW + L.sep;
    L.tileY = L.margin + L.pad + L.titleH;
    L.sliderY = L.tileY + L.tile + L.rowGap;

    // 音量在左半边，亮度在右半边
    float left = L.margin + L.pad + Px(2), right = L.margin + L.panelW - L.pad - Px(2);
    float mid = (left + right) / 2, cy = L.sliderY + L.sliderH / 2, icon = Px(30);
    auto make = [&](float x0, float x1) {
        return Slider{RectF(x0, cy - icon / 2, icon, icon), x0 + icon + Px(10), x1 - Px(10), cy};
    };
    s_sliders[0] = make(left, mid - Px(8));
    s_sliders[1] = make(mid + Px(8), right);

    s_width = static_cast<int>(std::ceil(L.panelW + 2 * L.margin));
    s_height = static_cast<int>(std::ceil(L.panelH + 2 * L.margin));
}

// 面板水平居中、贴着底边（屏幕底边，或者露着的任务栏的上沿）
void UpdatePlacement() {
    s_pos = {s_bounds.left + (s_bounds.right - s_bounds.left - s_width) / 2, s_bounds.bottom - s_height};
    s_baseValid = false;
}

float SlideDistance() { return s_L.panelH + s_L.margin + Px(8); }

int ItemCount() { return static_cast<int>(s_items.size()); }

bool IsTileVisible(int index) {
    if (index == 0) return true;
    int i = index - 1;
    return i >= s_first && i < s_first + s_visible;
}

RectF TileRect(int index) {
    if (index == 0) return RectF(s_L.margin + s_L.pad, s_L.tileY, s_L.tile, s_L.tile);
    int slot = index - 1 - s_first;
    return RectF(s_L.tilesX + slot * (s_L.tile + s_L.gap), s_L.tileY, s_L.tile, s_L.tile);
}

int HitTest(int x, int y) {
    for (int idx = 0; idx <= ItemCount(); ++idx) {
        if (!IsTileVisible(idx)) continue;
        RectF r = TileRect(idx);
        if (r.Contains(static_cast<REAL>(x), static_cast<REAL>(y))) return idx;
    }
    return -1;
}

int ControlAt(int x, int y) {
    REAL fx = static_cast<REAL>(x), fy = static_cast<REAL>(y);
    for (int i = 0; i < 2; ++i) {
        const Slider& sl = s_sliders[i];
        if (sl.icon.Contains(fx, fy)) return i == 0 ? kVolumeIcon : kBrightIcon;
        RectF track(sl.x0 - Px(10), sl.cy - Px(15), sl.x1 - sl.x0 + Px(20), Px(30));
        if (track.Contains(fx, fy)) return i == 0 ? kVolumeTrack : kBrightTrack;
    }
    return kNone;
}

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
    float x = TileRect(shown).X;
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
    RectF r(s_pillX, s_L.tileY, s_L.tile, s_L.tile);
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

    // 和 Win11 任务栏一样：当前窗口是一条长的强调色短线，其它打开的窗口是短点，没运行的固定应用没有
    float barY = r.Y + r.Height - Px(6);
    if (item.active) FillRound(g, RectF(cx - Px(8), barY, Px(16), Px(3)), Px(1.5f), pal.accent);
    else if (item.hwnd) FillRound(g, RectF(cx - Px(3), barY, Px(6), Px(3)), Px(1.5f), pal.running);
}

// ---- 音量和亮度 ----

bool SliderEnabled(int which) { return which == 0 ? s_hasVolume : s_brightness >= 0; }

float SliderValue(int which) {
    if (which == 0) return s_hasVolume ? s_volume : 0;
    return s_brightness >= 0 ? s_brightness / 100.0f : 0;
}

std::wstring SliderText(int which) {
    wchar_t buf[64];
    if (which == 0) {
        if (!s_hasVolume) return L"没有可用的播放设备";
        if (s_muted) return L"已静音（点喇叭取消）";
        swprintf(buf, 64, L"音量 %d%%", static_cast<int>(std::lround(s_volume * 100)));
    } else {
        if (s_brightness == -2) return L"正在读取亮度…";
        if (s_brightness < 0) return L"这块屏幕不支持调节亮度";
        swprintf(buf, 64, L"亮度 %d%%", s_brightness);
    }
    return buf;
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

void SetSliderFromX(int which, int x) {
    const Slider& sl = s_sliders[which];
    SetSlider(which, (x - sl.x0) / std::max(1.0f, sl.x1 - sl.x0));
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

    float h = Px(4);
    RectF track(sl.x0, sl.cy - h / 2, sl.x1 - sl.x0, h);
    FillRound(g, track, h / 2, pal.track);
    if (!enabled) return;

    float x = sl.x0 + (sl.x1 - sl.x0) * SliderValue(which);
    bool dim = which == 0 && s_muted;
    const Color& fill = dim ? pal.subtle : pal.accent;
    if (x > sl.x0 + 0.5f) FillRound(g, RectF(sl.x0, track.Y, x - sl.x0, h), h / 2, fill);

    // 滑块：和 Win11 一样，外圈一个圆片，中间一个强调色的点，鼠标放上去时点变大
    float R = Px(9);
    SolidBrush shadow(Color(40, 0, 0, 0));
    g.FillEllipse(&shadow, x - R, sl.cy - R + Px(1), 2 * R, 2 * R);
    SolidBrush thumb(pal.thumb);
    g.FillEllipse(&thumb, x - R, sl.cy - R, 2 * R, 2 * R);
    Pen rim(pal.thumbRim, std::max(1.0f, Px(1)));
    g.DrawEllipse(&rim, x - R, sl.cy - R, 2 * R, 2 * R);
    float core = hot ? Px(6) : Px(4.5f);
    SolidBrush dot(fill);
    g.FillEllipse(&dot, x - core, sl.cy - core, 2 * core, 2 * core);
}

void DrawContent(Graphics& g) {
    const Layout& L = s_L;
    const Palette& pal = Colors();

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
    else if (shown == 0) title = L"开始";
    else if (shown > 0 && shown <= ItemCount()) title = s_items[shown - 1].title;
    StringFormat oneLine(StringFormatFlagsNoWrap);
    oneLine.SetAlignment(StringAlignmentCenter);
    oneLine.SetLineAlignment(StringAlignmentCenter);
    oneLine.SetTrimming(StringTrimmingEllipsisCharacter);
    RectF titleRect(L.margin + L.pad + Px(10), L.margin + L.pad, L.panelW - 2 * L.pad - Px(20), L.titleH);
    if (!s_filter.empty()) {
        // 打字筛选：左边一个搜索框显示打的字，右边是选中项的名字
        StringFormat left(StringFormatFlagsNoWrap);
        left.SetLineAlignment(StringAlignmentCenter);
        RectF measured;
        g.MeasureString(s_filter.c_str(), -1, &titleFont, PointF(0, 0), &left, &measured);
        float boxW = std::min(titleRect.Width * 0.5f, measured.Width + Px(30));
        RectF box(titleRect.X - Px(6), titleRect.Y + Px(2), boxW, titleRect.Height - Px(4));
        FillRound(g, box, box.Height / 2, pal.pillTop);
        Pen pen(pal.text, Px(1.3f));
        float r = Px(3.5f), cx = box.X + Px(10), cy = box.Y + box.Height / 2 - Px(0.5f), d = r * 0.7f;
        g.DrawEllipse(&pen, cx - r, cy - r, 2 * r, 2 * r);
        g.DrawLine(&pen, cx + d, cy + d, cx + d + Px(2.5f), cy + d + Px(2.5f));
        // 打得太长时显示后面的部分
        RectF textRect(box.X + Px(18), box.Y, box.Width - Px(22), box.Height);
        left.SetAlignment(measured.Width > textRect.Width ? StringAlignmentFar : StringAlignmentNear);
        DrawLabel(g, s_filter.c_str(), titleFont, textRect, left, pal.text);
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

    // 窗口多到放不下时，两侧的小三角提示还能滚动
    SolidBrush hint(pal.subtle);
    float midY = L.tileY + L.tile / 2, tri = Px(4);
    if (s_first > 0) {
        float x = L.itemsX - Px(2);
        PointF pts[3] = {PointF(x - tri, midY), PointF(x, midY - tri), PointF(x, midY + tri)};
        g.FillPolygon(&hint, pts, 3);
    }
    if (s_first + s_visible < ItemCount()) {
        float x = L.itemsX + L.itemsW + Px(2);
        PointF pts[3] = {PointF(x + tri, midY), PointF(x, midY - tri), PointF(x, midY + tri)};
        g.FillPolygon(&hint, pts, 3);
    }

    Pen sepPen(pal.separator, 1.0f);
    float y1 = L.tileY + Px(10), y2 = L.tileY + L.tile - Px(10);
    g.DrawLine(&sepPen, L.itemsX - L.sep / 2, y1, L.itemsX - L.sep / 2, y2);
    g.DrawLine(&sepPen, L.clockX - L.sep / 2, y1, L.clockX - L.sep / 2, y2);

    if (s_items.empty())
        DrawLabel(g, s_filter.empty() ? L"没有打开的窗口" : L"没有匹配的窗口或应用", titleFont, RectF(L.itemsX, L.tileY, L.itemsW, L.tile), center, pal.subtle);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t clock[64];
    swprintf(clock, 64, L"%02d:%02d\n%d/%d/%d", st.wHour, st.wMinute, st.wYear, st.wMonth, st.wDay);
    DrawLabel(g, clock, smallFont, RectF(L.clockX, L.tileY, L.clockW, L.tile), center, pal.text);

    DrawSlider(g, 0);
    DrawSlider(g, 1);
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
    if (!s_open || s_anim != Anim::None || s_drag != kNone || s_menuOpen) return -1;
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
    RectF r = TileRect(idx);
    int centerX = s_pos.x + static_cast<int>(r.X + r.Width / 2);
    int bottomY = s_pos.y + static_cast<int>(s_L.margin - Px(8));
    HWND h = s_items[idx - 1].hwnd;
    s_thumbShown = Thumb_Show(h, centerX, bottomY, s_bounds, s_scale, s_glass.Light()) ? h : nullptr;
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

    int dy = static_cast<int>(std::lround(slide));
    size_t count = static_cast<size_t>(s_width) * s_height;
    if (dy == 0 && s_baseValid && s_base.size() == count) {
        memcpy(s_bits, s_base.data(), count * sizeof(DWORD));
    } else {
        s_glass.Prepare(s_width, s_height, RectF(s_L.margin, s_L.margin, s_L.panelW, s_L.panelH), s_L.radius);
        s_glass.Render(s_bits, s_pos, dy);
        if (dy == 0) {
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
        g.TranslateTransform(0, static_cast<REAL>(dy));
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

void HideNow() {
    HideThumb();
    s_open = false;
    s_anim = Anim::None;
    s_drag = kNone;
    if (GetCapture() == s_hwnd) ReleaseCapture();
    KillTimer(s_hwnd, kTimerClock);
    ShowWindow(s_hwnd, SW_HIDE);
}

// 动画的一帧：按时间算出位置画出来，还没播完就等下一次屏幕刷新再画下一帧
void Frame() {
    double now = NowMs();
    float slide = 0;
    BYTE alpha = 255;
    bool more = false;
    if (s_anim == Anim::Showing) {
        double t = std::min(1.0, (now - s_animStart) / kShowMs);
        slide = static_cast<float>((1 - EaseOutBack(t)) * SlideDistance());
        alpha = static_cast<BYTE>(std::lround(255 * std::min(1.0, t * 3)));
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
    more = more || PillMoving();
    s_lastSlide = slide;
    s_lastAlpha = alpha;
    Render(slide, alpha);
    if (more) {
        WaitForVBlank();
        RequestFrame();
    }
}

void Redraw() {
    if (!s_open) return;
    SyncPill();
    if (s_anim != Anim::None || PillMoving()) RequestFrame();  // 交给动画帧去画
    else Render(0, 255);
}

void MoveSelection(int delta) {
    int count = ItemCount() + 1;
    s_sel = ((s_sel + delta) % count + count) % count;
    s_hover = -1;
    s_hoverControl = kNone;  // 鼠标停在调节条上时，标题也要换成选中的窗口
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

void ActivateIndex(int idx, bool newInstance = false) {
    if (idx == 0) {
        HideNow();
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
        // 以管理员身份运行的程序（比如任务管理器），普通权限发不进去：窗口还在，格子也留着
        Log(L"关闭窗口失败（%lu）：%ls", GetLastError(), item.title.c_str());
        MessageBeep(MB_ICONWARNING);
        return;
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
    s_pillIndex = -1;
    Redraw();
}

// 右键运行中的格子：小菜单，关闭这个窗口；同一个程序开着好几个窗口时还能一次全关；
// 把这个程序加进 / 移出“最大化时不隐藏任务栏”的名单
void ShowItemMenu(int idx, POINT at) {
    if (idx <= 0 || idx > ItemCount() || !s_items[idx - 1].hwnd) return;
    // 同一个程序：有 AppUserModelID 的按它比（应用商店应用的窗口都属于同一个 ApplicationFrameHost 进程），
    // 没有的按程序路径比
    auto appKey = [](HWND h) {
        std::wstring id = GetWindowAumid(h);
        return id.empty() ? ToLower(GetProcessPath(AppWindowTarget(h))) : id;
    };
    std::wstring key = appKey(s_items[idx - 1].hwnd);
    std::vector<HWND> same;
    for (const Tile& t : s_items)
        if (t.hwnd && !key.empty() && appKey(t.hwnd) == key) same.push_back(t.hwnd);
    // 应用商店应用最小化时偶尔找不到它自己的进程，别把 ApplicationFrameHost 当成它加进排除名单
    std::wstring exeName = WindowExeName(s_items[idx - 1].hwnd);
    if (exeName == L"applicationframehost.exe") exeName.clear();

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"关闭窗口");
    if (same.size() > 1) {
        wchar_t text[64];
        swprintf(text, 64, L"关闭全部 %d 个窗口", static_cast<int>(same.size()));
        AppendMenuW(menu, MF_STRING, 2, text);
    }
    if (!exeName.empty()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (IsExcludedExe(exeName) ? MF_CHECKED : 0), 3, L"最大化时不隐藏任务栏");
    }
    SetMenuDefaultItem(menu, 1, FALSE);
    s_sel = idx;
    s_menuOpen = true;  // 缩略图会挡住菜单
    HideThumb();
    Redraw();
    ClientToScreen(s_hwnd, &at);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | TPM_BOTTOMALIGN, at.x, at.y, 0,
                              s_hwnd, nullptr);
    DestroyMenu(menu);
    s_menuOpen = false;
    if (cmd == 3) {
        SetExcluded(exeName, !IsExcludedExe(exeName));
        return;
    }
    if (!s_open) return;
    if (cmd == 1) {
        CloseItem(idx);
    } else if (cmd == 2) {
        // CloseItem 会去掉格子、后面的序号跟着变，每次重新找
        for (HWND h : same)
            for (int i = 0; i < ItemCount(); ++i)
                if (s_items[i].hwnd == h) {
                    CloseItem(i + 1);
                    break;
                }
    }
}

LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) Popup_Hide();
            return 0;

        case WM_APP_FRAME:
            s_framePosted = false;
            Frame();
            return 0;

        case WM_TIMER:
            if (wParam == kTimerClock) {
                if (s_drag != kVolumeTrack) RefreshVolume();  // 别处改了音量（键盘上的音量键）也跟着变
                Redraw();
            } else if (wParam == kTimerThumb) {
                if (s_thumbWant) ShowThumbNow();
                else HideThumb();
            }
            return 0;

        case WM_APP_APPS:
            if (s_open && !s_filter.empty()) ApplyFilter();  // 应用列表刚读完：把匹配的应用补上
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
                SetSliderFromX(SliderOf(s_drag), x);
                return 0;
            }
            // 位置没变的是系统补发的（比如缩略图窗口出现了），不是用户在动鼠标：别打乱键盘选中的项
            if (x == s_lastMouse.x && y == s_lastMouse.y) return 0;
            s_lastMouse = {x, y};
            s_keyNav = false;
            int hit = HitTest(x, y);
            int control = hit < 0 ? ControlAt(x, y) : kNone;
            if (hit != s_hover || control != s_hoverControl) {
                s_hover = hit;
                s_hoverControl = control;
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
            Redraw();
            return 0;

        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam), control = ControlAt(x, GET_Y_LPARAM(lParam));
            if ((control == kVolumeTrack || control == kBrightTrack) && SliderEnabled(SliderOf(control))) {
                s_drag = control;
                SetCapture(hwnd);
                SetSliderFromX(SliderOf(control), x);
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
            int hit = HitTest(x, y);
            if (hit >= 0) ActivateIndex(hit, (wParam & MK_SHIFT) != 0);
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
            ShowItemMenu(HitTest(pt.x, pt.y), pt);
            return 0;
        }

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
                case VK_LEFT: MoveSelection(-1); break;
                case VK_RIGHT: MoveSelection(1); break;
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
                    // 焦点交还给原来的窗口，随后的失活消息会触发收起
                    if (s_prevForeground && IsWindow(s_prevForeground)) ForceForeground(s_prevForeground);
                    Popup_Hide();
                    break;
            }
            return 0;

        case WM_CHAR: {
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

    // 提前读一遍固定的应用和图标，第一次弹出时就不卡
    if (g_settings.showPinnedApps) {
        POINT origin = {0, 0};
        s_scale = MonitorScale(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY));
        LoadPinnedApps(static_cast<int>(std::lround(Px(32))));
    }
}

void Popup_Destroy() {
    Thumb_Destroy();
    s_items.clear();
    s_allItems.clear();
    Pinned_ClearCache();
    s_fontFamily.reset();
    ReleaseCanvas();
    if (s_hwnd) DestroyWindow(s_hwnd);
    s_hwnd = nullptr;
}

void Popup_Show() {
    if (!s_hwnd) return;
    HWND fg = GetForegroundWindow();
    if (fg != s_hwnd) s_prevForeground = fg;

    // 弹在鼠标所在的显示器上；任务栏露着时弹在它上面，藏起来时贴着屏幕底边
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    s_bounds = Taskbar_IsShownOn(mon) ? mi.rcWork : mi.rcMonitor;
    s_scale = MonitorScale(mon);

    // 音量马上就能读到；亮度在后台读，读到之前先显示上次的值
    RefreshVolume();
    s_monitor = mon;
    s_brightness = -2;
    for (const auto& known : s_knownBrightness)
        if (known.first == mon) s_brightness = known.second;
    Brightness_Query(s_hwnd, mon);
    s_brightnessEdited = false;
    s_hoverControl = kNone;
    s_drag = kNone;
    s_wheelRest = 0;

    s_allItems = BuildTiles(EnumerateWindows(s_prevForeground, static_cast<int>(std::lround(Px(32)))));
    s_items = s_allItems;
    s_filter.clear();
    s_lockedItemsW = 0;
    s_keyNav = false;
    s_menuOpen = false;
    s_lastMouse = {INT_MIN, INT_MIN};
    Apps_Refresh(s_hwnd);  // 打字筛选时要搜的所有应用，在后台先读好
    ComputeLayout();
    UpdatePlacement();

    // 截下玻璃后面的屏幕内容：这时弹窗自己不能在屏幕上
    if (IsWindowVisible(s_hwnd)) {
        ShowWindow(s_hwnd, SW_HIDE);
        WaitForVBlank();
    }
    RECT window = {s_pos.x, s_pos.y, s_pos.x + s_width, s_pos.y + s_height};
    RECT panel = {s_pos.x + static_cast<LONG>(s_L.margin), s_pos.y + static_cast<LONG>(s_L.margin),
                  s_pos.x + static_cast<LONG>(s_L.margin + s_L.panelW), s_pos.y + static_cast<LONG>(s_L.margin + s_L.panelH)};
    s_glass.Capture(window, panel, s_scale);

    s_first = 0;
    s_hover = -1;
    s_sel = DefaultSelection();
    EnsureVisible(s_sel);
    s_pillIndex = -1;

    s_open = true;
    s_anim = Anim::Showing;
    s_animStart = NowMs();
    Render(SlideDistance(), 0);
    ShowWindow(s_hwnd, SW_SHOW);
    ForceForeground(s_hwnd);
    if (GetForegroundWindow() != s_hwnd)
        Log(L"迷你任务栏没拿到前台，前台是 %ls %ls", GetClassNameStr(GetForegroundWindow()).c_str(),
            GetProcessPath(GetForegroundWindow()).c_str());
    SetFocus(s_hwnd);
    RequestFrame();
    SetTimer(s_hwnd, kTimerClock, 1000, nullptr);
}

void Popup_Hide() {
    if (!s_open) return;
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

}  // namespace app
