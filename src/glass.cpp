// 液态玻璃材质。
//
// 弹出前先截下弹窗后面的屏幕内容，模糊一遍并提高饱和度。再用圆角矩形的有向距离场
// 算出每个像素离边缘多远：靠近边缘的一圈像凸透镜一样把背景往里折射（红绿蓝三个通道的折射量略有不同，
// 带出一点色散），再叠上淡淡的着色、顶部的光泽、沿边缘的高光，外面画一圈柔和的阴影。
//
// 这些只和面板形状有关的量在 Prepare 里一次算好；动画的每一帧只需要按位移去背景里取色，
// 面板滑动时背景保持不动，像一块真的玻璃在上面移动。
#include "common.h"

#include <cmath>
#include <cstring>

namespace app {
namespace {

inline float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// 圆角矩形的有向距离（内部为负），(x, y) 相对于矩形中心，hx/hy 是去掉圆角后的半宽半高。
// nx/ny 返回指向外侧的单位法线
float RoundRectDistance(float x, float y, float hx, float hy, float r, float& nx, float& ny) {
    float sx = x < 0 ? -1.0f : 1.0f, sy = y < 0 ? -1.0f : 1.0f;
    float qx = std::fabs(x) - hx, qy = std::fabs(y) - hy;
    if (qx > 0 && qy > 0) {
        float len = std::sqrt(qx * qx + qy * qy);
        nx = sx * qx / len;
        ny = sy * qy / len;
        return len - r;
    }
    if (qx > qy) {
        nx = sx;
        ny = 0;
        return qx - r;
    }
    nx = 0;
    ny = sy;
    return qy - r;
}

float RoundRectDistance(float x, float y, float hx, float hy, float r) {
    float nx, ny;
    return RoundRectDistance(x, y, hx, hy, r, nx, ny);
}

// 横向盒式模糊一行，越界取边缘像素
void BoxBlurRow(const DWORD* src, DWORD* dst, int n, int r) {
    const int window = 2 * r + 1, inv = 65536 / window;  // 除法换成乘法
    int sb = 0, sg = 0, sr = 0;
    for (int i = -r; i <= r; ++i) {
        DWORD p = src[std::clamp(i, 0, n - 1)];
        sb += p & 0xFF;
        sg += (p >> 8) & 0xFF;
        sr += (p >> 16) & 0xFF;
    }
    for (int i = 0; i < n; ++i) {
        dst[i] = 0xFF000000 | (((sr * inv) >> 16) << 16) | (((sg * inv) >> 16) << 8) | ((sb * inv) >> 16);
        DWORD add = src[std::min(i + r + 1, n - 1)], sub = src[std::max(i - r, 0)];
        sb += static_cast<int>(add & 0xFF) - static_cast<int>(sub & 0xFF);
        sg += static_cast<int>((add >> 8) & 0xFF) - static_cast<int>((sub >> 8) & 0xFF);
        sr += static_cast<int>((add >> 16) & 0xFF) - static_cast<int>((sub >> 16) & 0xFF);
    }
}

// 纵向盒式模糊整张图：每列一组累加和，一行一行往下推，按内存顺序访问
void BoxBlurColumns(const DWORD* src, DWORD* dst, int w, int h, int r, std::vector<int>& sums) {
    const int window = 2 * r + 1, inv = 65536 / window;
    sums.assign(static_cast<size_t>(w) * 3, 0);
    int* s = sums.data();
    auto row = [&](int y) { return src + static_cast<size_t>(std::clamp(y, 0, h - 1)) * w; };
    for (int i = -r; i <= r; ++i) {
        const DWORD* p = row(i);
        for (int x = 0; x < w; ++x) {
            s[3 * x] += p[x] & 0xFF;
            s[3 * x + 1] += (p[x] >> 8) & 0xFF;
            s[3 * x + 2] += (p[x] >> 16) & 0xFF;
        }
    }
    for (int y = 0; y < h; ++y) {
        DWORD* out = dst + static_cast<size_t>(y) * w;
        const DWORD* add = row(y + r + 1);
        const DWORD* sub = row(y - r);
        for (int x = 0; x < w; ++x) {
            int* c = s + 3 * x;
            out[x] = 0xFF000000 | (((c[2] * inv) >> 16) << 16) | (((c[1] * inv) >> 16) << 8) | ((c[0] * inv) >> 16);
            c[0] += static_cast<int>(add[x] & 0xFF) - static_cast<int>(sub[x] & 0xFF);
            c[1] += static_cast<int>((add[x] >> 8) & 0xFF) - static_cast<int>((sub[x] >> 8) & 0xFF);
            c[2] += static_cast<int>((add[x] >> 16) & 0xFF) - static_cast<int>((sub[x] >> 16) & 0xFF);
        }
    }
}

// 横竖各做三遍盒式模糊，效果接近高斯模糊
void Blur(std::vector<DWORD>& px, int w, int h, int r) {
    std::vector<DWORD> tmp(px.size());
    std::vector<int> sums;
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) BoxBlurRow(&px[static_cast<size_t>(y) * w], &tmp[static_cast<size_t>(y) * w], w, r);
        BoxBlurColumns(tmp.data(), px.data(), w, h, r, sums);
    }
}

// 缩小一半：每 2×2 个像素取平均，奇数边上的取边缘像素
std::vector<DWORD> HalfSize(const std::vector<DWORD>& px, int w, int h, int hw, int hh) {
    std::vector<DWORD> out(static_cast<size_t>(hw) * hh);
    for (int y = 0; y < hh; ++y) {
        const DWORD* r0 = &px[static_cast<size_t>(2 * y) * w];
        const DWORD* r1 = &px[static_cast<size_t>(std::min(2 * y + 1, h - 1)) * w];
        DWORD* o = &out[static_cast<size_t>(y) * hw];
        for (int x = 0; x < hw; ++x) {
            int x0 = 2 * x, x1 = std::min(2 * x + 1, w - 1);
            DWORD a = r0[x0], b = r0[x1], c = r1[x0], d = r1[x1];
            DWORD lo = ((a & 0xFF00FF) + (b & 0xFF00FF) + (c & 0xFF00FF) + (d & 0xFF00FF) + 0x020002) >> 2;
            DWORD mid = ((a & 0xFF00) + (b & 0xFF00) + (c & 0xFF00) + (d & 0xFF00) + 0x200) >> 2;
            o[x] = 0xFF000000 | (lo & 0xFF00FF) | (mid & 0xFF00);
        }
    }
    return out;
}

// 放大回 w×h：双线性插值，半尺寸图的像素中心在原图的 2x+0.5 处
void DoubleSize(const std::vector<DWORD>& half, int hw, int hh, std::vector<DWORD>& out, int w, int h) {
    out.resize(static_cast<size_t>(w) * h);
    // 每个坐标在半尺寸图里的两个邻居：i1 近（权重 3/4），i0 远（权重 1/4）
    auto taps = [](int n, int hn, std::vector<int>& i0, std::vector<int>& i1) {
        i0.resize(n);
        i1.resize(n);
        for (int i = 0; i < n; ++i) {
            int k = std::min(i / 2, hn - 1);
            i1[i] = k;
            i0[i] = i % 2 == 0 ? std::max(k - 1, 0) : std::min(k + 1, hn - 1);
        }
    };
    std::vector<int> x0, x1, y0, y1;
    taps(w, hw, x0, x1);
    taps(h, hh, y0, y1);
    for (int y = 0; y < h; ++y) {
        const DWORD* ra = &half[static_cast<size_t>(y0[y]) * hw];  // 远的一行
        const DWORD* rb = &half[static_cast<size_t>(y1[y]) * hw];  // 近的一行
        DWORD* o = &out[static_cast<size_t>(y) * w];
        for (int x = 0; x < w; ++x) {
            // 四个点的权重：近的 3×3，横向远的 1×3，纵向远的 3×1，对角 1×1，合计 16
            DWORD p00 = rb[x1[x]], p01 = rb[x0[x]], p10 = ra[x1[x]], p11 = ra[x0[x]];
            DWORD lo = (9 * (p00 & 0xFF00FF) + 3 * (p01 & 0xFF00FF) + 3 * (p10 & 0xFF00FF) + (p11 & 0xFF00FF) + 0x080008) >> 4;
            DWORD mid = (9 * (p00 & 0xFF00) + 3 * (p01 & 0xFF00) + 3 * (p10 & 0xFF00) + (p11 & 0xFF00) + 0x800) >> 4;
            o[x] = 0xFF000000 | (lo & 0xFF00FF) | (mid & 0xFF00);
        }
    }
}

// 提高饱和度，背景透过玻璃显得更鲜亮（amount 按 1/256 的定点数算）
void Saturate(std::vector<DWORD>& px, float amount) {
    const int k = static_cast<int>(std::lround(amount * 256));
    for (DWORD& p : px) {
        int b = p & 0xFF, g = (p >> 8) & 0xFF, r = (p >> 16) & 0xFF;
        int l = (77 * r + 150 * g + 29 * b) >> 8;
        auto adj = [&](int c) { return static_cast<DWORD>(std::clamp(l + (((c - l) * k) >> 8), 0, 255)); };
        p = 0xFF000000 | (adj(r) << 16) | (adj(g) << 8) | adj(b);
    }
}

// 在 w×h 的图上双线性取一个通道；x、y 是连续坐标（像素中心在 .5），越界取边缘
float SampleChannel(const std::vector<DWORD>& px, int w, int h, float x, float y, int shift) {
    float fx = std::clamp(x - 0.5f, 0.0f, static_cast<float>(w - 1));
    float fy = std::clamp(y - 0.5f, 0.0f, static_cast<float>(h - 1));
    int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    float tx = fx - x0, ty = fy - y0;
    const DWORD* r0 = &px[static_cast<size_t>(y0) * w];
    const DWORD* r1 = &px[static_cast<size_t>(y1) * w];
    float a = static_cast<float>((r0[x0] >> shift) & 0xFF), b = static_cast<float>((r0[x1] >> shift) & 0xFF);
    float c = static_cast<float>((r1[x0] >> shift) & 0xFF), d = static_cast<float>((r1[x1] >> shift) & 0xFF);
    float top = a + (b - a) * tx, bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

}  // namespace

bool Glass::Shoot(const RECT& window, float scale, GlassShot& out, const GlassShot* same) {
    // 多截一圈，模糊和边缘折射取样都不会碰到截图的边
    int pad = static_cast<int>(std::lround(28 * scale));
    RECT area = {window.left - pad, window.top - pad, window.right + pad, window.bottom + pad};
    MONITORINFO mi = {sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromRect(&window, MONITOR_DEFAULTTONEAREST), &mi)) IntersectRect(&area, &area, &mi.rcMonitor);
    out.area = area;
    out.w = std::max(0L, area.right - area.left);
    out.h = std::max(0L, area.bottom - area.top);
    out.raw.clear();
    out.blur.clear();
    if (out.w <= 0 || out.h <= 0) {
        out.w = out.h = 0;
        return true;
    }
    if (!CaptureScreen(area, out.raw)) {
        if (same) return false;
        out.raw.assign(static_cast<size_t>(out.w) * out.h, 0xFF202024);
    }
    if (same && EqualRect(&same->area, &area) && same->raw.size() == out.raw.size() &&
        memcmp(same->raw.data(), out.raw.data(), out.raw.size() * sizeof(DWORD)) == 0)
        return false;
    // 缩小一半再模糊、提高饱和度，再放大回来：模糊得很厉害，看不出区别，快好几倍
    int hw = (out.w + 1) / 2, hh = (out.h + 1) / 2;
    std::vector<DWORD> half = HalfSize(out.raw, out.w, out.h, hw, hh);
    Blur(half, hw, hh, std::max(1, static_cast<int>(std::lround(3.5f * scale))));
    Saturate(half, 1.45f);
    DoubleSize(half, hw, hh, out.blur, out.w, out.h);
    return true;
}

void Glass::Capture(const RECT& window, const RECT& panel, float scale) {
    GlassShot shot;
    Shoot(window, scale, shot);
    Adopt(std::move(shot), panel, scale, true);
}

void Glass::Adopt(GlassShot&& shot, const RECT& panel, float scale, bool fresh) {
    m_scale = scale;
    m_area = shot.area;
    m_w = shot.w;
    m_h = shot.h;
    m_raw.swap(shot.raw);
    m_blur.swap(shot.blur);
    UpdateLight(panel, fresh);
}

// 按面板下面背景的平均亮度决定用浅色还是深色玻璃。不是 fresh 时留一点余量，
// 背景在临界亮度附近变来变去（视频、拖到明暗交界）时不会来回闪
bool Glass::UpdateLight(const RECT& panel, bool fresh) {
    const RECT& area = m_area;
    double sum = 0;
    int count = 0;
    for (int y = std::max(panel.top, area.top); y < std::min(panel.bottom, area.bottom); y += 3) {
        const DWORD* row = &m_blur[static_cast<size_t>(y - area.top) * m_w];
        for (int x = std::max(panel.left, area.left); x < std::min(panel.right, area.right); x += 3) {
            DWORD p = row[x - area.left];
            sum += 0.299 * ((p >> 16) & 0xFF) + 0.587 * ((p >> 8) & 0xFF) + 0.114 * (p & 0xFF);
            ++count;
        }
    }
    bool old = m_light;
    if (count == 0) {
        if (fresh) m_light = false;
    } else {
        double level = sum / count / 255.0;
        double threshold = fresh ? 0.62 : (m_light ? 0.58 : 0.66);
        m_light = level > threshold;
    }
    return m_light != old;
}

void Glass::Prepare(int width, int height, const Gdiplus::RectF& panel, float radius) {
    if (!m_texels.empty() && width == m_texW && height == m_texH && radius == m_texRadius && m_scale == m_texScale &&
        panel.Equals(m_texPanel))
        return;
    m_texScale = m_scale;
    m_texW = std::max(0, width);
    m_texH = std::max(0, height);
    m_texPanel = panel;
    m_texRadius = radius;
    m_texels.assign(static_cast<size_t>(m_texW) * m_texH, Texel{});

    const float s = m_scale;
    const float bevel = 18 * s;        // 边缘弯曲的宽度
    const float refraction = 14 * s;   // 最外缘的折射位移
    const float rimWidth = 1.3f * s;   // 高光细边的宽度
    const float shadowSigma = 11 * s, shadowDy = 6 * s;
    const float lx = -0.5f, ly = -0.866f;  // 光从左上方照过来

    const float cx = panel.X + panel.Width / 2, cy = panel.Y + panel.Height / 2;
    const float hx = panel.Width / 2 - radius, hy = panel.Height / 2 - radius;
    const float shadowReach = 3 * shadowSigma + shadowDy;
    const float twoSigma2 = 2 * shadowSigma * shadowSigma;

    for (int y = 0; y < m_texH; ++y) {
        Texel* row = &m_texels[static_cast<size_t>(y) * m_texW];
        float py = y + 0.5f;
        for (int x = 0; x < m_texW; ++x) {
            float px = x + 0.5f;
            float nx, ny;
            float d = RoundRectDistance(px - cx, py - cy, hx, hy, radius, nx, ny);
            float cover = Clamp01(0.5f - d);

            float shadow = 0;
            if (cover < 1) {
                float ds = RoundRectDistance(px - cx, py - cy - shadowDy, hx, hy, radius);
                if (ds < shadowReach) {
                    float q = std::max(0.0f, ds + 2 * s);
                    shadow = std::exp(-q * q / twoSigma2);  // 深浅色玻璃的阴影浓淡不同，Render 时再乘
                    if (shadow < 0.008f) shadow = 0;
                }
            }
            Texel& t = row[x];
            t.cover = static_cast<BYTE>(std::lround(cover * 255));
            t.shadow = static_cast<BYTE>(std::lround(shadow * 255));
            if (!t.cover) continue;

            float depth = std::max(0.0f, -d);
            if (depth < bevel) {
                // 沿法线往里取样：边缘像凸透镜一样把背景放大
                float k = 1 - depth / bevel;
                float disp = refraction * k * k;
                t.dx = -nx * disp;
                t.dy = -ny * disp;
                t.bevel = 1;
            }

            // 高光：顶部一层光泽；边缘朝光的一侧有一道亮边，对侧有一道弱一些的反光；斜面上有柔和的泛光
            float v = Clamp01((py - panel.Y) / panel.Height);
            float ndl = nx * lx + ny * ly;
            float facing = std::max(0.0f, ndl), away = std::max(0.0f, -ndl);
            float spec = facing * facing * facing + 0.5f * away * away * away;
            float rim = depth < 8 * rimWidth ? std::exp(-depth / rimWidth) : 0;
            float edge = depth < bevel ? (1 - depth / bevel) * (1 - depth / bevel) : 0;
            float light = Clamp01(spec * (0.85f * rim + 0.25f * edge) + 0.16f * rim);
            t.light = static_cast<BYTE>(std::lround(light * 255));
            t.gloss = static_cast<BYTE>(std::lround((1 - v) * (1 - v) * 255));  // 顶部光泽，强弱 Render 时按明暗乘
        }
    }
}

DWORD Glass::Fetch(int x, int y) const {
    if (m_blur.empty()) return 0xFF202024;
    x = std::clamp(x, 0, m_w - 1);
    y = std::clamp(y, 0, m_h - 1);
    return m_blur[static_cast<size_t>(y) * m_w + x];
}

// 边缘一圈：三个通道按略有不同的位移各取一次，带出色散
void Glass::SampleBevel(float x, float y, const Texel& t, float rgb[3]) const {
    if (m_blur.empty()) {
        rgb[0] = rgb[1] = rgb[2] = 32;
        return;
    }
    rgb[0] = SampleChannel(m_blur, m_w, m_h, x + t.dx * 0.9f, y + t.dy * 0.9f, 16);
    rgb[1] = SampleChannel(m_blur, m_w, m_h, x + t.dx, y + t.dy, 8);
    rgb[2] = SampleChannel(m_blur, m_w, m_h, x + t.dx * 1.1f, y + t.dy * 1.1f, 0);
}

void Glass::Render(DWORD* pixels, POINT origin, int slide) const {
    const int w = m_texW, h = m_texH;
    const float tint[3] = {m_light ? 252.0f : 16.0f, m_light ? 252.0f : 16.0f, m_light ? 255.0f : 20.0f};
    const float tintAlpha = m_light ? 0.36f : 0.38f;
    const float shadowAlpha = m_light ? 0.22f : 0.32f;
    const float sheen = (m_light ? 0.12f : 0.07f) / 255.0f;
    // 窗口像素 → 背景截图里的坐标
    const int offX = origin.x - m_area.left, offY = origin.y - m_area.top;

    for (int y = 0; y < h; ++y) {
        DWORD* row = pixels + static_cast<size_t>(y) * w;
        int ty = y - slide;  // 面板往下挪了 slide 行，材质跟着挪，背景不动
        if (ty < 0 || ty >= h) {
            std::fill(row, row + w, 0);
            continue;
        }
        const Texel* tex = &m_texels[static_cast<size_t>(ty) * w];
        for (int x = 0; x < w; ++x) {
            const Texel& t = tex[x];
            if (!t.cover) {
                row[x] = static_cast<DWORD>(t.shadow * shadowAlpha + 0.5f) << 24;
                continue;
            }
            float c[3];
            if (t.bevel) {
                SampleBevel(static_cast<float>(x + offX) + 0.5f, static_cast<float>(y + offY) + 0.5f, t, c);
            } else {
                DWORD p = Fetch(x + offX, y + offY);
                c[0] = static_cast<float>((p >> 16) & 0xFF);
                c[1] = static_cast<float>((p >> 8) & 0xFF);
                c[2] = static_cast<float>(p & 0xFF);
            }
            float light = std::min(1.0f, t.light / 255.0f + t.gloss * sheen), cover = t.cover / 255.0f;
            float a = cover + t.shadow * shadowAlpha / 255.0f * (1 - cover);
            DWORD out = static_cast<DWORD>(a * 255 + 0.5f) << 24;
            for (int k = 0; k < 3; ++k) {
                float col = c[k] + (tint[k] - c[k]) * tintAlpha;
                col += (255 - col) * light;
                out |= static_cast<DWORD>(std::clamp(col * cover, 0.0f, 255.0f) + 0.5f) << (16 - 8 * k);
            }
            row[x] = out;
        }
    }
}

}  // namespace app
