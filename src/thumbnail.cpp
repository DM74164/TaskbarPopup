// 迷你任务栏上鼠标停在窗口图标上时，在它上方显示这个窗口的实时缩略图（DWM 缩略图，和系统任务栏的预览一样）。
// 缩略图放在一个单独的小窗口里：不抢焦点、鼠标点击穿透到下面的迷你任务栏。
#include "common.h"

namespace app {
namespace {

constexpr wchar_t kClassName[] = L"TaskbarPopupThumb";

HWND s_wnd = nullptr;
HTHUMBNAIL s_thumb = nullptr;
HWND s_source = nullptr;
COLORREF s_background = RGB(44, 44, 44);
bool s_failLogged = false;

LRESULT CALLBACK ThumbProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;  // 同一线程的迷你任务栏在下面，鼠标交给它
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            HBRUSH brush = CreateSolidBrush(s_background);
            FillRect(dc, &ps.rcPaint, brush);
            DeleteObject(brush);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool EnsureWindow() {
    if (s_wnd) return true;
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = ThumbProc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    s_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"", WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, g_instance, nullptr);
    if (!s_wnd) return false;
    DWORD round = 2;  // DWMWCP_ROUND：Win11 的圆角和阴影
    DwmSetWindowAttribute(s_wnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    BOOL noTransitions = TRUE;
    DwmSetWindowAttribute(s_wnd, DWMWA_TRANSITIONS_FORCEDISABLED, &noTransitions, sizeof(noTransitions));
    return true;
}

void Unregister() {
    if (s_thumb) DwmUnregisterThumbnail(s_thumb);
    s_thumb = nullptr;
    s_source = nullptr;
}

}  // namespace

bool Thumb_Show(HWND source, int centerX, int bottomY, const RECT& bounds, float scale, bool light) {
    if (!source || !IsWindow(source) || !EnsureWindow()) {
        Thumb_Hide();
        return false;
    }
    if (source != s_source) {
        Unregister();
        HRESULT hr = DwmRegisterThumbnail(s_wnd, source, &s_thumb);
        if (FAILED(hr)) {
            s_thumb = nullptr;
            if (!s_failLogged) Log(L"缩略图：DwmRegisterThumbnail 失败 0x%08X", static_cast<unsigned>(hr));
            s_failLogged = true;
            Thumb_Hide();
            return false;
        }
        s_source = source;
    }
    SIZE size = {};
    if (FAILED(DwmQueryThumbnailSourceSize(s_thumb, &size)) || size.cx < 8 || size.cy < 8) {
        Thumb_Hide();
        return false;
    }

    // 按比例缩进 240×150 的框里，四周留一圈边
    float maxW = 240 * scale, maxH = 150 * scale;
    float k = std::min({maxW / size.cx, maxH / size.cy, 1.0f});
    int w = std::max(1, static_cast<int>(size.cx * k)), h = std::max(1, static_cast<int>(size.cy * k));
    int pad = static_cast<int>(6 * scale);
    int winW = w + 2 * pad, winH = h + 2 * pad;
    int edge = static_cast<int>(8 * scale);
    int x = std::max<int>(bounds.left + edge, std::min<int>(centerX - winW / 2, bounds.right - edge - winW));
    int y = std::max<int>(bounds.top + edge, bottomY - winH);

    COLORREF background = light ? RGB(243, 243, 243) : RGB(44, 44, 44);
    if (background != s_background) {
        s_background = background;
        BOOL dark = light ? FALSE : TRUE;
        DwmSetWindowAttribute(s_wnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        InvalidateRect(s_wnd, nullptr, TRUE);
    }

    DWM_THUMBNAIL_PROPERTIES props = {};
    props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_OPACITY;
    props.rcDestination = {pad, pad, pad + w, pad + h};
    props.fVisible = TRUE;
    props.fSourceClientAreaOnly = FALSE;
    props.opacity = 255;
    DwmUpdateThumbnailProperties(s_thumb, &props);
    SetWindowPos(s_wnd, HWND_TOPMOST, x, y, winW, winH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

void Thumb_Hide() {
    if (s_wnd && IsWindowVisible(s_wnd)) ShowWindow(s_wnd, SW_HIDE);
    Unregister();
}

void Thumb_Destroy() {
    Unregister();
    if (s_wnd) DestroyWindow(s_wnd);
    s_wnd = nullptr;
}

}  // namespace app
