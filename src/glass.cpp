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

// 一维盒式模糊，越界取边缘像素
void BoxBlurLine(const DWORD* src, DWORD* dst, int n, size_t stride, int r) {
    auto at = [&](int i) { return src[static_cast<size_t>(std::clamp(i, 0, n - 1)) * stride]; };
    int window = 2 * r + 1;
    int sb = 0, sg = 0, sr = 0;
    for (int i = -r; i <= r; ++i) {
        DWORD p = at(i);
        sb += p & 0xFF;
        sg += (p >> 8) & 0xFF;
        sr += (p >> 16) & 0xFF;
    }
    for (int i = 0; i < n; ++i) {
        dst[static_cast<size_t>(i) * stride] = 0xFF000000 | ((sr / window) << 16) | ((sg / window) << 8) | (sb / window);
        DWORD add = at(i + r + 1), sub = at(i - r);
        sb += static_cast<int>(add & 0xFF) - static_cast<int>(sub & 0xFF);
        sg += static_cast<int>((add >> 8) & 0xFF) - static_cast<int>((sub >> 8) & 0xFF);
        sr += static_cast<int>((add >> 16) & 0xFF) - static_cast<int>((sub >> 16) & 0xFF);
    }
}

// 横竖各做三遍盒式模糊，效果接近高斯模糊
void Blur(std::vector<DWORD>& px, int w, int h, int r) {
    std::vector<DWORD> tmp(px.size());
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) BoxBlurLine(&px[static_cast<size_t>(y) * w], &tmp[static_cast<size_t>(y) * w], w, 1, r);
        for (int x = 0; x < w; ++x) BoxBlurLine(&tmp[x], &px[x], h, static_cast<size_t>(w), r);
    }
}

// 提高饱和度，背景透过玻璃显得更鲜亮
void Saturate(std::vector<DWORD>& px, float amount) {
    for (DWORD& p : px) {
        float b = static_cast<float>(p & 0xFF), g = static_cast<float>((p >> 8) & 0xFF),
              r = static_cast<float>((p >> 16) & 0xFF);
        float l = 0.299f * r + 0.587f * g + 0.114f * b;
        auto adj = [&](float c) { return static_cast<DWORD>(std::clamp(l + (c - l) * amount, 0.0f, 255.0f)); };
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

void Glass::Capture(const RECT& window, const RECT& panel, float scale) {
    m_scale = scale;
    m_texels.clear();  // 明暗、缩放可能变了，材质要重新算
    m_texW = m_texH = 0;
    // 多截一圈，模糊和边缘折射取样都不会碰到截图的边
    int pad = static_cast<int>(std::lround(28 * scale));
    RECT area = {window.left - pad, window.top - pad, window.right + pad, window.bottom + pad};
    MONITORINFO mi = {sizeof(mi)};
    if (GetMonitorInfoW(MonitorFromRect(&window, MONITOR_DEFAULTTONEAREST), &mi)) IntersectRect(&area, &area, &mi.rcMonitor);
    m_area = area;
    m_w = area.right - area.left;
    m_h = area.bottom - area.top;
    if (m_w <= 0 || m_h <= 0) {
        m_blur.clear();
        m_w = m_h = 0;
        m_light = false;
        return;
    }
    if (!CaptureScreen(area, m_blur)) m_blur.assign(static_cast<size_t>(m_w) * m_h, 0xFF202024);
    Blur(m_blur, m_w, m_h, std::max(1, static_cast<int>(std::lround(7 * scale))));
    Saturate(m_blur, 1.45f);

    // 面板下面的背景平均亮度决定用浅色还是深色玻璃
    double sum = 0;
    int count = 0;
    for (int y = std::max(panel.top, area.top); y < std::min(panel.bottom, area.bottom); y += 2) {
        for (int x = std::max(panel.left, area.left); x < std::min(panel.right, area.right); x += 2) {
            DWORD p = m_blur[static_cast<size_t>(y - area.top) * m_w + (x - area.left)];
            sum += (0.299 * ((p >> 16) & 0xFF) + 0.587 * ((p >> 8) & 0xFF) + 0.114 * (p & 0xFF)) / 255.0;
            ++count;
        }
    }
    m_light = count > 0 && sum / count > 0.62;
}

void Glass::Prepare(int width, int height, const Gdiplus::RectF& panel, float radius) {
    if (!m_texels.empty() && width == m_texW && height == m_texH && radius == m_texRadius && panel.Equals(m_texPanel))
        return;
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
    const float shadowAlpha = m_light ? 0.22f : 0.32f;
    const float sheen = m_light ? 0.12f : 0.07f;
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
                    shadow = shadowAlpha * std::exp(-q * q / twoSigma2);
                    if (shadow < 0.002f) shadow = 0;
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
            float light = Clamp01(sheen * (1 - v) * (1 - v) + spec * (0.85f * rim + 0.25f * edge) + 0.16f * rim);
            t.light = static_cast<BYTE>(std::lround(light * 255));
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
                row[x] = static_cast<DWORD>(t.shadow) << 24;
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
            float light = t.light / 255.0f, cover = t.cover / 255.0f;
            float a = cover + t.shadow / 255.0f * (1 - cover);
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
