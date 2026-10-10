// 设置窗口：和迷你任务栏一样的液态玻璃面板，开关、滑块、名单都是自己画的。
//
// 分层窗口贴的是“玻璃 + 内容”的整张图：玻璃按窗口后面的屏幕内容折射。后面的画面定时截一遍
// （截的时候让本窗口暂时不进截图），变了就重画；拖动时按新位置去截好的背景里取色，玻璃跟着实时变。
#include "common.h"
#include "version.h"

#include <cmath>

using namespace Gdiplus;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace app {
namespace {

constexpr wchar_t kClassName[] = L"TaskbarPopupSettings";
constexpr UINT_PTR kTimerAnim = 1;  // 淡入、开关滑块的动画
constexpr UINT_PTR kTimerLive = 2;  // 定时刷新玻璃后面的背景
constexpr double kFadeMs = 180.0 * TP_ANIM_SCALE;
constexpr double kKnobMs = 150.0 * TP_ANIM_SCALE;

// ---- 尺寸（96 DPI 下的像素，乘 s_scale 得到物理像素） ----
constexpr float kShadow = 28;   // 面板外面留给阴影的一圈
constexpr float kPad = 20;      // 面板内边距
constexpr float kColW = 330;    // 一栏的宽度
constexpr float kHeaderH = 52;  // 标题栏（可以拖动）
constexpr float kGroupTitleH = 24;
constexpr float kGroupGap = 12;
constexpr float kRowPad = 14;  // 卡片里左右的边距
constexpr float kChipH = 28;

// 开关
enum Toggle { kAutoHide, kLongPress, kPinned, kLevels, kAutoStart, kUpdates, kToggleCount };
struct ToggleText {
    const wchar_t* label;
    const wchar_t* sub;  // 可以为空
};
const ToggleText kToggleText[kToggleCount] = {
    {L"最大化或全屏时隐藏任务栏", L"窗口铺满整块屏幕，要用任务栏时长按 Win"},
    {L"长按 Win 键弹出迷你任务栏", L"左键单击托盘图标也能弹出"},
    {L"显示固定在任务栏的应用", nullptr},
    {L"显示音量和亮度调节", nullptr},
    {L"开机自动启动", nullptr},
    {L"自动检查更新", L"每天看一次 GitHub 上有没有新版本"},
};

// 滑块
enum SliderId { kLongPressMs, kScale, kSliderCount };
struct SliderSpec {
    const wchar_t* label;
    int minValue, maxValue, step;
};
const SliderSpec kSliderSpec[kSliderCount] = {
    {L"长按时长", kMinLongPressMs, kMaxLongPressMs, 100},
    {L"迷你任务栏大小", kMinPopupScale, kMaxPopupScale, 10},
};

// 能点的东西
enum Kind { kNoItem, kToggleRow, kSliderRow, kCheckNow, kChip, kAddChip, kOk, kCancel, kClose };
struct Item {
    Kind kind = kNoItem;
    int index = 0;  // 第几个开关 / 滑块 / 程序
    RectF rect;     // 窗口里的位置（物理像素）
    bool operator==(const Item& o) const { return kind == o.kind && index == o.index; }
};

// 窗口里的各项值。确定时只改用户在窗口里动过的项：窗口开着的时候在迷你任务栏或托盘菜单里改的设置不会被盖掉
struct Values {
    bool on[kToggleCount] = {};
    int slider[kSliderCount] = {};
    std::vector<std::wstring> exclude;
};
Values s_v, s_initial;

struct Palette {
    Color text, subtle, card, cardRim, separator, accent, onAccent, trackOff, knob, button, buttonHot, textShadow, hover;
};
const Palette kDark = {Color(240, 255, 255, 255), Color(165, 255, 255, 255), Color(20, 255, 255, 255),
                       Color(46, 255, 255, 255),  Color(32, 255, 255, 255),  Color(255, 96, 205, 255),
                       Color(255, 16, 22, 30),    Color(60, 255, 255, 255),  Color(255, 255, 255, 255),
                       Color(30, 255, 255, 255),  Color(56, 255, 255, 255),  Color(90, 0, 0, 0),
                       Color(22, 255, 255, 255)};
const Palette kLight = {Color(235, 18, 18, 22),    Color(160, 18, 18, 22),    Color(105, 255, 255, 255),
                        Color(210, 255, 255, 255), Color(30, 0, 0, 0),        Color(255, 0, 95, 184),
                        Color(255, 255, 255, 255), Color(50, 0, 0, 0),        Color(255, 255, 255, 255),
                        Color(120, 255, 255, 255), Color(190, 255, 255, 255), Color(110, 255, 255, 255),
                        Color(16, 0, 0, 0)};

HWND s_hwnd = nullptr;
float s_scale = 1;
int s_width = 0, s_height = 0;  // 窗口大小（含阴影）
POINT s_pos = {};
RectF s_panel;  // 面板在窗口里的位置
float s_radius = 0;

std::vector<Item> s_items;
std::vector<RectF> s_cards;  // 每组的卡片
struct Text {
    std::wstring text;
    RectF rect;
    int style;  // 0 组标题，1 提示，2 标题，3 版本号
};
std::vector<Text> s_texts;
struct SliderGeom {
    float x0, x1, cy;
    RectF label;
};
SliderGeom s_sliderGeom[kSliderCount];
std::vector<float> s_rowLines;  // 卡片里两行之间的分隔线：每条 x0, x1, y 三个数

Item s_hover, s_press, s_focus;
bool s_showFocus = false;  // 用键盘切换过焦点才画焦点框
int s_drag = -1;           // 正在拖的滑块
float s_knob[kToggleCount] = {};  // 开关滑块的位置 0~1，动画用
double s_knobFrom[kToggleCount] = {}, s_knobStart[kToggleCount] = {};
double s_openedAt = 0;
BYTE s_alpha = 255;
double s_capturedAt = 0;

Glass s_glass;
std::vector<DWORD> s_base;  // 玻璃，悬停等重画直接复用
bool s_baseValid = false;
HDC s_canvasDC = nullptr;
HBITMAP s_canvas = nullptr;
HGDIOBJ s_canvasOld = nullptr;
DWORD* s_bits = nullptr;
int s_canvasW = 0, s_canvasH = 0;
std::unique_ptr<FontFamily> s_fontFamily;

float Px(float dip) { return dip * s_scale; }
const Palette& Colors() { return s_glass.Light() ? kLight : kDark; }

bool SliderEnabled(int which) { return which != kLongPressMs || s_v.on[kLongPress]; }

std::wstring SliderText(int which) {
    wchar_t text[32];
    if (which == kLongPressMs) swprintf(text, 32, L"%d 毫秒", s_v.slider[which]);
    else swprintf(text, 32, L"%d%%", s_v.slider[which]);
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

// 一组：标题 + 卡片。rows 里每个函数往卡片里加一行，返回这一行的高度
float AddGroup(float x, float y, const wchar_t* title, const std::vector<float (*)(float, float, float)>& rows) {
    s_texts.push_back({title, RectF(x + Px(4), y, Px(kColW) - Px(8), Px(kGroupTitleH)), 0});
    y += Px(kGroupTitleH);
    float top = y;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i > 0) s_rowLines.insert(s_rowLines.end(), {x + Px(kRowPad), x + Px(kColW) - Px(kRowPad), y});
        y += rows[i](x, y, Px(kColW));
    }
    s_cards.push_back(RectF(x, top, Px(kColW), y - top));
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

// 名单：每个程序一个小胶囊（点一下去掉），最后是“添加程序”，排不下就换行
float ChipsRow(float x, float y, float w) {
    float cx = x + Px(kRowPad), cy = y + Px(12);
    float right = x + w - Px(kRowPad);
    auto place = [&](float chipW) {
        if (cx + chipW > right && cx > x + Px(kRowPad)) {
            cx = x + Px(kRowPad);
            cy += Px(kChipH) + Px(8);
        }
        RectF r(cx, cy, std::min(chipW, right - cx), Px(kChipH));
        cx += chipW + Px(8);
        return r;
    };
    for (size_t i = 0; i < s_v.exclude.size(); ++i) {
        float chipW = MeasureWidth(s_v.exclude[i], Px(12.5f)) + Px(12 + 6 + 14);
        s_items.push_back({kChip, static_cast<int>(i), place(chipW)});
    }
    s_items.push_back({kAddChip, 0, place(MeasureWidth(L"＋ 添加程序", Px(12.5f)) + Px(24))});
    float bottom = cy + Px(kChipH) + Px(8);
    s_texts.push_back({L"也可以在迷你任务栏里右键运行中的程序来添加或去掉",
                       RectF(x + Px(kRowPad), bottom, w - 2 * Px(kRowPad), Px(32)), 1});
    return bottom + Px(36) - y;
}

void BuildLayout() {
    s_items.clear();
    s_cards.clear();
    s_texts.clear();
    s_rowLines.clear();

    float margin = Px(kShadow);
    float panelW = Px(kPad) * 3 + Px(kColW) * 2;
    s_radius = Px(22);
    float px0 = margin + Px(kPad), px1 = px0 + Px(kColW) + Px(kPad);
    float top = margin + Px(kHeaderH);

    s_texts.push_back({L"TaskbarPopup 设置", RectF(margin + Px(kPad), margin + Px(12), Px(260), Px(28)), 2});
    s_items.push_back({kClose, 0, RectF(margin + panelW - Px(12) - Px(32), margin + Px(10), Px(32), Px(32))});

    float left =
        AddGroup(px0, top, L"任务栏", {[](float x, float y, float w) { return ToggleRow(kAutoHide, x, y, w); }});
    left = AddGroup(px0, left, L"迷你任务栏",
                    {[](float x, float y, float w) { return ToggleRow(kLongPress, x, y, w); },
                     [](float x, float y, float w) { return SliderRow(kLongPressMs, x, y, w); },
                     [](float x, float y, float w) { return SliderRow(kScale, x, y, w); },
                     [](float x, float y, float w) { return ToggleRow(kPinned, x, y, w); },
                     [](float x, float y, float w) { return ToggleRow(kLevels, x, y, w); }});
    float right = AddGroup(px1, top, L"启动和更新",
                           {[](float x, float y, float w) { return ToggleRow(kAutoStart, x, y, w); },
                            [](float x, float y, float w) { return ToggleRow(kUpdates, x, y, w); }});
    right = AddGroup(px1, right, L"最大化时不隐藏任务栏的程序", {ChipsRow});

    // “立即检查”放在“自动检查更新”那一行的开关左边
    for (const Item& it : s_items)
        if (it.kind == kToggleRow && it.index == kUpdates) {
            float w = MeasureWidth(L"立即检查", Px(12.5f)) + Px(16);
            float x = it.rect.X + it.rect.Width - Px(kRowPad) - Px(40) - Px(10) - w;
            RectF r(x, it.rect.Y + (it.rect.Height - Px(26)) / 2, w, Px(26));
            s_items.push_back({kCheckNow, 0, r});
            break;
        }

    float contentBottom = std::max(left, right) - Px(kGroupGap);
    float buttonsY = contentBottom + Px(16);
    float bw = Px(92), bh = Px(34);
    float panelRight = margin + panelW - Px(kPad);
    s_items.push_back({kOk, 0, RectF(panelRight - bw, buttonsY, bw, bh)});
    s_items.push_back({kCancel, 0, RectF(panelRight - 2 * bw - Px(10), buttonsY, bw, bh)});
    const char* version = TP_VERSION_STR;
    s_texts.push_back({L"版本 " + std::wstring(version, version + strlen(version)),
                       RectF(margin + Px(kPad) + Px(4), buttonsY, Px(200), bh), 3});

    float panelH = buttonsY + bh + Px(kPad) - margin;
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

bool Enabled(const Item& it) { return it.kind != kSliderRow || SliderEnabled(it.index); }

Item HitTest(int x, int y) {
    PointF pt(static_cast<float>(x), static_cast<float>(y));
    // “立即检查”在开关那一行上面，先看它
    for (const Item& it : s_items)
        if (it.kind == kCheckNow && it.rect.Contains(pt)) return it;
    for (const Item& it : s_items)
        if (it.kind != kCheckNow && it.rect.Contains(pt) && Enabled(it)) return it;
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
        DrawText(g, t.label, Px(13.5f), FontStyleRegular, RectF(x, r.Y + Px(8), textW, Px(20)), pal.text);
        DrawText(g, t.sub, Px(11.5f), FontStyleRegular, RectF(x, r.Y + Px(29), textW, Px(17)), pal.subtle);
    } else {
        DrawText(g, t.label, Px(13.5f), FontStyleRegular, RectF(r.X + Px(kRowPad), r.Y, textW, r.Height), pal.text);
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
    DrawText(g, spec.label, Px(13.5f), FontStyleRegular, sg.label, text);
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

void DrawChip(Graphics& g, const Item& it) {
    const Palette& pal = Colors();
    const RectF& r = it.rect;
    bool hot = s_hover == it || s_press == it;
    if (it.kind == kAddChip) {
        FillRound(g, r, r.Height / 2, hot ? pal.buttonHot : pal.button);
        DrawText(g, L"＋ 添加程序", Px(12.5f), FontStyleRegular, r, pal.accent, StringAlignmentCenter);
        return;
    }
    DrawCard(g, r, r.Height / 2, hot ? pal.buttonHot : pal.button, pal.cardRim);
    RectF textRect(r.X + Px(12), r.Y, r.Width - Px(12 + 6 + 14), r.Height);
    DrawText(g, s_v.exclude[it.index], Px(12.5f), FontStyleRegular, textRect, pal.text);
    // 右边的小叉：鼠标放上去时变成强调色
    float cx = r.X + r.Width - Px(13), cy = r.Y + r.Height / 2, a = Px(3.5f);
    Pen pen(hot ? pal.accent : pal.subtle, Px(1.4f));
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, cx - a, cy - a, cx + a, cy + a);
    g.DrawLine(&pen, cx - a, cy + a, cx + a, cy - a);
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
        DrawText(g, L"立即检查", Px(12.5f), FontStyleRegular, r, pal.accent, StringAlignmentCenter);
        return;
    }
    if (it.kind == kOk) {
        Color fill = pal.accent;
        if (hot) fill = Mix(fill, Color(255, 255, 255, 255), 0.15f);
        GraphicsPath path;
        AddRoundRect(path, r, r.Height / 2);
        PointF top(0, r.Y), bottom(0, r.Y + r.Height);
        LinearGradientBrush brush(top, bottom, Mix(fill, Color(255, 255, 255, 255), 0.12f), fill);
        g.FillPath(&brush, &path);
        DrawText(g, L"确定", Px(13.5f), FontStyleRegular, r, pal.onAccent, StringAlignmentCenter);
        return;
    }
    DrawCard(g, r, r.Height / 2, hot ? pal.buttonHot : pal.button, pal.cardRim);
    DrawText(g, L"取消", Px(13.5f), FontStyleRegular, r, pal.text, StringAlignmentCenter);
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
            default: DrawText(g, t.text, Px(11.5f), FontStyleRegular, t.rect, pal.subtle); break;
        }
    }
    for (const Item& it : s_items) {
        switch (it.kind) {
            case kToggleRow: DrawToggleRow(g, it); break;
            case kSliderRow: DrawSliderRow(g, it); break;
            case kChip:
            case kAddChip: DrawChip(g, it); break;
            default: DrawButton(g, it); break;
        }
    }
    if (s_showFocus)
        if (const Item* f = FindItem(s_focus)) {
            RectF r = f->rect;
            r.Inflate(Px(2), Px(2));
            float radius = f->kind == kToggleRow || f->kind == kSliderRow ? Px(10) : r.Height / 2;
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

// 先画玻璃（不变时用缓存），再用 GDI+ 画上面的内容，最后带逐像素透明度贴到屏幕
void Render() {
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
    {
        Bitmap canvas(s_width, s_height, s_width * 4, PixelFormat32bppPARGB, reinterpret_cast<BYTE*>(s_bits));
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        DrawContent(g);
    }
    SIZE size = {s_width, s_height};
    POINT src = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, s_alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(s_hwnd, nullptr, &s_pos, &size, s_canvasDC, &src, 0, &blend, ULW_ALPHA);
}

// ---- 玻璃的背景 ----
//
// 截窗口后面的屏幕时让本窗口暂时不进截图（Win10 2004 起支持），屏幕上照常显示。
// 不支持的系统上截到的会是自己，一叠加就越来越花，这时只用打开窗口那一刻截的背景

int s_excludeWorks = 0;  // 0 = 还不知道，1 = 管用，-1 = 不管用
bool s_moving = false;   // 正在拖动窗口
int s_still = 0;         // 定时刷新时背景连续几次没变

RECT PanelOnScreen() {
    return {s_pos.x + static_cast<LONG>(s_panel.X), s_pos.y + static_cast<LONG>(s_panel.Y),
            s_pos.x + static_cast<LONG>(s_panel.X + s_panel.Width), s_pos.y + static_cast<LONG>(s_panel.Y + s_panel.Height)};
}

bool BeginExclude() {
    if (s_excludeWorks < 0) return false;
    if (!SetWindowDisplayAffinity(s_hwnd, WDA_EXCLUDEFROMCAPTURE)) {  // Win10 2004 以前没有这个功能
        s_excludeWorks = -1;
        Log(L"设置窗口：系统不支持截图时排除本窗口（错误 %lu），玻璃背景不再实时刷新", GetLastError());
        return false;
    }
    DwmFlush();  // 等新的设置在下一次合成里生效
    DwmFlush();
    return true;
}

void EndExclude() { SetWindowDisplayAffinity(s_hwnd, WDA_NONE); }

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

// 截下窗口后面的屏幕内容做玻璃的背景；extra：四周再多截这么多像素（拖动时用，挪一段距离内不用重新截）
void CaptureBehind(int extra = 0) {
    bool visible = IsWindowVisible(s_hwnd) && !IsIconic(s_hwnd);
    if (visible) {
        if (!BeginExclude()) return;
        if (!ExcludeWorks()) {
            EndExclude();
            return;
        }
    }
    RECT window = {s_pos.x - extra, s_pos.y - extra, s_pos.x + s_width + extra, s_pos.y + s_height + extra};
    s_glass.Capture(window, PanelOnScreen(), s_scale);
    if (visible) EndExclude();
    s_baseValid = false;
    s_capturedAt = NowMs();
}

// 拖动中：窗口挪出了已经截好的范围才重新截（窗口四周再留 400 像素）
void FollowMove() {
    int pad = static_cast<int>(std::lround(Px(28)));
    RECT need = {s_pos.x - pad, s_pos.y - pad, s_pos.x + s_width + pad, s_pos.y + s_height + pad};
    MONITORINFO mi = {sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromRect(&need, MONITOR_DEFAULTTONEAREST), &mi)) IntersectRect(&need, &need, &mi.rcMonitor);
    const RECT& have = s_glass.Area();
    if (need.left < have.left || need.top < have.top || need.right > have.right || need.bottom > have.bottom)
        CaptureBehind(static_cast<int>(Px(400)));
    s_baseValid = false;  // 玻璃按新位置去背景里取色
    Render();
}

// 定时看看后面的画面变了没有（视频、动画、别的窗口挪动）：变了就重画，一直不变就放慢
void LiveTick() {
    UINT next = 400;
    if (!s_moving && IsWindowVisible(s_hwnd) && !IsIconic(s_hwnd) && s_alpha == 255 && s_excludeWorks >= 0) {
        if (!BeginExclude()) {
            KillTimer(s_hwnd, kTimerLive);
            return;
        }
        if (!ExcludeWorks()) {
            EndExclude();
            KillTimer(s_hwnd, kTimerLive);
            return;
        }
        bool changed = s_glass.Refresh(PanelOnScreen());
        EndExclude();
        if (changed) {
            s_baseValid = false;
            Render();
            s_still = 0;
        } else {
            ++s_still;
        }
        next = changed ? 60 : (s_still < 15 ? 120 : 400);
        if (GetForegroundWindow() != s_hwnd) next = std::max<UINT>(next, 150);
    }
    SetTimer(s_hwnd, kTimerLive, next, nullptr);
}

// 布局变了（名单增删、换了显示器）：上边缘不动，大小跟着变，背景重新截
void Relayout(bool keepTop) {
    int oldW = s_width, oldH = s_height;
    POINT center = {s_pos.x + s_width / 2, s_pos.y + s_height / 2};
    BuildLayout();
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

// ---- 动画 ----

bool Animating(double now) {
    if (now - s_openedAt < kFadeMs) return true;
    for (int i = 0; i < kToggleCount; ++i)
        if (s_knob[i] != (s_v.on[i] ? 1.0f : 0.0f)) return true;
    return false;
}

void StepAnimation() {
    double now = NowMs();
    double fade = std::min(1.0, (now - s_openedAt) / kFadeMs);
    s_alpha = static_cast<BYTE>(std::lround(255 * fade));
    for (int i = 0; i < kToggleCount; ++i) {
        float target = s_v.on[i] ? 1.0f : 0.0f;
        if (s_knob[i] == target) continue;
        double t = std::min(1.0, (now - s_knobStart[i]) / kKnobMs);
        double e = 1 - (1 - t) * (1 - t) * (1 - t);
        s_knob[i] = static_cast<float>(s_knobFrom[i] + (target - s_knobFrom[i]) * e);
        if (t >= 1) s_knob[i] = target;
    }
    Render();
    if (!Animating(now)) KillTimer(s_hwnd, kTimerAnim);
}

void StartAnimation() { SetTimer(s_hwnd, kTimerAnim, 15, nullptr); }

// ---- 操作 ----

void SetToggle(int which, bool on) {
    if (s_v.on[which] == on) return;
    s_v.on[which] = on;
    s_knobFrom[which] = s_knob[which];
    s_knobStart[which] = NowMs();
    StartAnimation();
}

void SetSlider(int which, int value) {
    const SliderSpec& spec = kSliderSpec[which];
    value = spec.minValue + (value - spec.minValue + spec.step / 2) / spec.step * spec.step;
    s_v.slider[which] = std::clamp(value, spec.minValue, spec.maxValue);
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

void AddExclude(const std::wstring& path) {
    std::wstring name = NormalizeExeName(path);
    if (name.empty() || std::find(s_v.exclude.begin(), s_v.exclude.end(), name) != s_v.exclude.end()) return;
    s_v.exclude.push_back(name);
    Relayout(true);
}

void RemoveExclude(int index) {
    if (index < 0 || index >= static_cast<int>(s_v.exclude.size())) return;
    s_v.exclude.erase(s_v.exclude.begin() + index);
    s_hover = s_press = {};
    // 焦点在被去掉的那个上：移到同一位置的下一个（没有了就落到“添加程序”上）
    if (s_focus.kind == kChip && s_focus.index >= static_cast<int>(s_v.exclude.size()))
        s_focus = s_v.exclude.empty() ? Item{kAddChip, 0, {}} : Item{kChip, static_cast<int>(s_v.exclude.size()) - 1, {}};
    Relayout(true);
}

// 选一个 exe 文件
std::wstring BrowseExe() {
    std::wstring result;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return result;
    COMDLG_FILTERSPEC filter[] = {{L"程序 (*.exe)", L"*.exe"}};
    dialog->SetFileTypes(1, filter);
    dialog->SetTitle(L"选择最大化时不隐藏任务栏的程序");
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
    for (const std::wstring& name : ctx.names)
        if (std::find(s_v.exclude.begin(), s_v.exclude.end(), name) == s_v.exclude.end()) list.push_back(name);
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
    if (running.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"没有其它开着窗口的程序");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1, L"浏览...");
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

void Apply() {
    // 只改窗口里动过的项，其余的保持现在的值（窗口开着的时候可能在别处改过）
    auto changed = [](int t) { return s_v.on[t] != s_initial.on[t]; };
    if (changed(kAutoHide)) g_settings.autoHideOnFullscreen = s_v.on[kAutoHide];
    if (changed(kLongPress)) g_settings.longPressPopup = s_v.on[kLongPress];
    if (changed(kPinned)) g_settings.showPinnedApps = s_v.on[kPinned];
    if (changed(kLevels)) g_settings.showLevels = s_v.on[kLevels];
    if (changed(kUpdates)) g_settings.checkUpdates = s_v.on[kUpdates];
    if (s_v.slider[kLongPressMs] != s_initial.slider[kLongPressMs]) g_settings.longPressMs = s_v.slider[kLongPressMs];
    if (s_v.slider[kScale] != s_initial.slider[kScale]) g_settings.popupScale = s_v.slider[kScale];

    // 名单按增删合并到现在的名单上，不整个替换
    auto contains = [](const std::vector<std::wstring>& list, const std::wstring& name) {
        return std::find(list.begin(), list.end(), name) != list.end();
    };
    std::vector<std::wstring>& current = g_settings.excludeApps;
    for (const std::wstring& name : s_initial.exclude)
        if (!contains(s_v.exclude, name)) current.erase(std::remove(current.begin(), current.end(), name), current.end());
    for (const std::wstring& name : s_v.exclude)
        if (!contains(s_initial.exclude, name) && !contains(current, name)) current.push_back(name);

    SaveSettings();
    if (changed(kAutoStart)) SetAutoStart(s_v.on[kAutoStart]);
    ApplySettings();
}

void Activate(const Item& it) {
    switch (it.kind) {
        case kToggleRow: SetToggle(it.index, !s_v.on[it.index]); break;
        case kCheckNow: Update_CheckNow(true); break;
        case kChip: RemoveExclude(it.index); return;
        case kAddChip: ShowAddMenu(it.rect); return;
        case kOk:
            Apply();
            DestroyWindow(s_hwnd);
            return;
        case kCancel:
        case kClose: DestroyWindow(s_hwnd); return;
        default: break;
    }
    Render();
}

// 能用 Tab 走到的项，按显示顺序
std::vector<Item> FocusOrder() {
    std::vector<Item> list;
    for (const Item& it : s_items)
        if (it.kind != kClose && Enabled(it)) list.push_back(it);
    // 确定、取消在布局里是先确定后取消，Tab 时先取消再确定
    auto ok = std::find_if(list.begin(), list.end(), [](const Item& i) { return i.kind == kOk; });
    auto cancel = std::find_if(list.begin(), list.end(), [](const Item& i) { return i.kind == kCancel; });
    if (ok != list.end() && cancel != list.end() && ok < cancel) std::iter_swap(ok, cancel);
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
        case VK_TAB: MoveFocus(shift ? -1 : 1); return;
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
            return;
        case VK_SPACE:
            if (f && f->kind != kSliderRow) Activate(*f);
            return;
        case VK_RETURN:
            // 焦点在按钮、开关这类上时按它；否则等于“确定”
            if (f && f->kind != kSliderRow) Activate(*f);
            else Activate({kOk, 0, {}});
            return;
    }
}

void SetHover(const Item& it) {
    if (it == s_hover) return;
    s_hover = it;
    Render();
}

void ReleaseAll() {
    KillTimer(s_hwnd, kTimerAnim);
    KillTimer(s_hwnd, kTimerLive);
    s_moving = false;
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

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST: {
            // 标题栏（关闭按钮除外）可以拖动窗口
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            PointF p(static_cast<float>(pt.x), static_cast<float>(pt.y));
            RectF header(s_panel.X, s_panel.Y, s_panel.Width, Px(kHeaderH));
            const Item* close = FindItem({kClose, 0, {}});
            if (header.Contains(p) && !(close && close->rect.Contains(p))) return HTCAPTION;
            return HTCLIENT;
        }

        case WM_ENTERSIZEMOVE:
            // 拖动时玻璃跟着位置实时重画：先把窗口四周一大片背景截好
            s_moving = true;
            CaptureBehind(static_cast<int>(Px(400)));
            return 0;

        case WM_EXITSIZEMOVE: {
            s_moving = false;
            RECT r;
            GetWindowRect(hwnd, &r);
            s_pos = {r.left, r.top};
            CaptureBehind();
            Render();
            return 0;
        }

        case WM_MOVE: {
            RECT r;
            GetWindowRect(hwnd, &r);
            s_pos = {r.left, r.top};
            if (s_moving && s_excludeWorks >= 0) FollowMove();
            return 0;
        }

        case WM_ACTIVATE:
            // 回到前台时后面的内容可能变了（换了壁纸、挪了窗口），重新截一遍
            if (LOWORD(wParam) != WA_INACTIVE && !HIWORD(wParam) && s_capturedAt && NowMs() - s_capturedAt > 500 &&
                NowMs() - s_openedAt > kFadeMs) {
                CaptureBehind();
                Render();
            }
            // 每次回到前台都把键盘焦点放回来（DefWindowProc 本来会做），不然按键会变成系统键、按一下响一声
            if (LOWORD(wParam) != WA_INACTIVE && !HIWORD(wParam)) SetFocus(hwnd);
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wParam == VK_F4 && msg == WM_SYSKEYDOWN) break;  // Alt+F4 照常关闭
            OnKey(wParam);
            return 0;

        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
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
            Item it = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
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
            Item it = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            Item pressed = s_press;
            bool dragged = s_drag >= 0;
            s_press = {};
            s_drag = -1;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (dragged || pressed.kind == kSliderRow) {
                Render();
                return 0;
            }
            if (pressed.kind != kNoItem && it == pressed) Activate(it);
            else Render();
            return 0;
        }

        case WM_CAPTURECHANGED:
            if ((s_drag >= 0 || s_press.kind != kNoItem) && reinterpret_cast<HWND>(lParam) != hwnd) {
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

        case WM_TIMER:
            if (wParam == kTimerAnim) StepAnimation();
            else if (wParam == kTimerLive) LiveTick();
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
            if (LOWORD(wParam) == IDOK) Activate({kOk, 0, {}});
            else if (LOWORD(wParam) == IDCANCEL) DestroyWindow(hwnd);
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            ReleaseAll();
            s_hwnd = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

HWND SettingsDialog_Hwnd() { return s_hwnd; }

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

    s_initial.on[kAutoHide] = g_settings.autoHideOnFullscreen;
    s_initial.on[kLongPress] = g_settings.longPressPopup;
    s_initial.on[kPinned] = g_settings.showPinnedApps;
    s_initial.on[kLevels] = g_settings.showLevels;
    s_initial.on[kAutoStart] = IsAutoStartEnabled();
    s_initial.on[kUpdates] = g_settings.checkUpdates;
    s_initial.slider[kLongPressMs] = g_settings.longPressMs;
    s_initial.slider[kScale] = g_settings.popupScale;
    s_initial.exclude = g_settings.excludeApps;
    s_v = s_initial;
    for (int i = 0; i < kToggleCount; ++i) s_knob[i] = s_v.on[i] ? 1.0f : 0.0f;
    s_hover = s_press = s_focus = {};
    s_showFocus = false;
    s_drag = -1;
    s_still = 0;

    // 居中到鼠标所在的显示器
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    LayoutFor(mon);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const RECT& work = mi.rcWork;
    s_pos = {work.left + (work.right - work.left - s_width) / 2, work.top + (work.bottom - work.top - s_height) / 2};

    s_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_APPWINDOW, kClassName, L"TaskbarPopup 设置",
                             WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, s_pos.x, s_pos.y, s_width, s_height, nullptr, nullptr,
                             g_instance, nullptr);
    if (!s_hwnd) {
        s_fontFamily.reset();
        return;
    }
    // 淡入自己画；系统的窗口动画会和它叠在一起
    BOOL disable = TRUE;
    DwmSetWindowAttribute(s_hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));

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
