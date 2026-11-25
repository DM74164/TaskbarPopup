// 可用鼠标操作的实时 DWM 缩略图。预览始终保留迷你任务栏的焦点。
#include "common.h"

#include <cmath>
#include <utility>

namespace app {
namespace {

constexpr wchar_t kClassName[] = L"TaskbarPopupThumb";
constexpr UINT kDarkMode = 20;  // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr UINT kCornerPreference = 33;  // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr UINT_PTR kPeekTimer = 1;
constexpr UINT kPeekDelayMs = 450;

struct Card {
    HWND source = nullptr;
    HTHUMBNAIL thumb = nullptr;
    RECT rect = {};
    RECT image = {};
    RECT caption = {};
    RECT close = {};
    std::wstring title;
};

struct Grid {
    int pad = 0, gap = 0, columns = 0, rows = 0;
    int width = 0, imageH = 0, captionH = 0, footerH = 0;

    int PageSize() const { return columns * rows; }
    SIZE FrameSize() const {
        return {2 * pad + columns * width + (columns - 1) * gap,
                2 * pad + rows * (imageH + captionH) + (rows - 1) * gap + footerH};
    }
};

struct Gallery {
    HWND owner = nullptr;
    std::vector<HWND> sources;
    POINT at = {};
    ThumbPlace place = kThumbAbove;
    RECT bounds = {};
    float scale = 1;
    bool light = false;
    int page = 0;
    int pages = 1;
    int wheelRest = 0;
    std::vector<Card> cards;
    RECT footer = {};
    int fontPx = 12;
    bool tracking = false;
    int hot = -1;
    bool hotClose = false;
    int pressed = -1;
    UINT pressedButton = 0;
    bool pressedClose = false;
};

HWND s_wnd = nullptr;
HWND s_peekWnd = nullptr;
HTHUMBNAIL s_peekThumb = nullptr;
Gallery s_gallery;
bool s_failLogged = false;

int Pixel(float dip, float scale) { return std::max(1, static_cast<int>(std::lround(dip * scale))); }
int Width(const RECT& rect) { return rect.right - rect.left; }
int Height(const RECT& rect) { return rect.bottom - rect.top; }

void FillColor(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void PaintGallery(HDC dc) {
    bool light = s_gallery.light;
    HFONT font = CreateFontW(-s_gallery.fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    COLORREF text = light ? RGB(25, 25, 28) : RGB(246, 246, 247);
    COLORREF subtle = light ? RGB(89, 89, 97) : RGB(190, 190, 197);
    constexpr UINT kTextFlags = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
    for (size_t i = 0; i < s_gallery.cards.size(); ++i) {
        const Card& card = s_gallery.cards[i];
        bool hot = static_cast<int>(i) == s_gallery.hot;
        if (hot) FillColor(dc, card.rect, light ? RGB(211, 227, 248) : RGB(61, 83, 112));
        FillColor(dc, card.image, light ? RGB(228, 228, 231) : RGB(57, 57, 62));
        if (!card.thumb) {
            RECT image = card.image;
            SetTextColor(dc, subtle);
            DrawTextW(dc, Tr(L"预览不可用"), -1, &image, kTextFlags | DT_CENTER);
        }
        RECT caption = card.caption;
        SetTextColor(dc, text);
        DrawTextW(dc, card.title.c_str(), -1, &caption, kTextFlags);
        if (hot) {
            RECT close = card.close;
            if (s_gallery.hotClose) {
                FillColor(dc, close, RGB(196, 43, 28));
                SetTextColor(dc, RGB(255, 255, 255));
            }
            DrawTextW(dc, L"\u00D7", -1, &close, kTextFlags | DT_CENTER);
        }
    }
    if (s_gallery.pages > 1) {
        RECT footer = s_gallery.footer;
        std::wstring label = TrF(L"%d / %d · 滚轮翻页", s_gallery.page + 1, s_gallery.pages);
        SetTextColor(dc, subtle);
        DrawTextW(dc, label.c_str(), -1, &footer, kTextFlags | DT_CENTER);
    }
    if (oldFont) SelectObject(dc, oldFont);
    if (font) DeleteObject(font);
}

int CardAt(POINT pt) {
    const auto& cards = s_gallery.cards;
    for (size_t i = 0; i < cards.size(); ++i)
        if (PtInRect(&cards[i].rect, pt)) return static_cast<int>(i);
    return -1;
}

void HidePeek();
void ShowPeek();

void UpdateHotCard(POINT pt) {
    int hot = CardAt(pt);
    bool close = hot >= 0 && PtInRect(&s_gallery.cards[hot].close, pt);
    if (hot == s_gallery.hot && close == s_gallery.hotClose) return;
    HidePeek();
    s_gallery.hot = hot;
    s_gallery.hotClose = close;
    InvalidateRect(s_wnd, nullptr, FALSE);
    if (hot >= 0 && !close && s_gallery.pressed < 0 && s_gallery.cards[hot].thumb)
        SetTimer(s_wnd, kPeekTimer, kPeekDelayMs, nullptr);
}

void NotifyOwner(ThumbEvent event, HWND source = nullptr) {
    if (s_gallery.owner)
        SendMessageW(s_gallery.owner, WM_APP_THUMB, static_cast<WPARAM>(event), reinterpret_cast<LPARAM>(source));
}

LRESULT CALLBACK ThumbProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const bool gallery = hwnd == s_wnd;
    switch (msg) {
        case WM_NCHITTEST: return gallery ? HTCLIENT : HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            FillColor(dc, ps.rcPaint, s_gallery.light ? RGB(243, 243, 243) : RGB(44, 44, 44));
            if (gallery) PaintGallery(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    if (!gallery) return DefWindowProcW(hwnd, msg, wParam, lParam);
    POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    switch (msg) {
        case WM_MOUSEMOVE: {
            if (!s_gallery.tracking) {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                s_gallery.tracking = TrackMouseEvent(&track) != FALSE;
                NotifyOwner(ThumbEvent::Enter);
            }
            UpdateHotCard(pt);
            return 0;
        }
        case WM_MOUSELEAVE:
            s_gallery.tracking = false;
            s_gallery.hot = s_gallery.pressed = -1;
            s_gallery.hotClose = false;
            HidePeek();
            InvalidateRect(hwnd, nullptr, FALSE);
            NotifyOwner(ThumbEvent::Leave);
            return 0;
        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN:
            HidePeek();
            s_gallery.pressed = CardAt(pt);
            s_gallery.pressedButton = msg == WM_LBUTTONDOWN ? WM_LBUTTONUP : WM_MBUTTONUP;
            s_gallery.pressedClose = s_gallery.pressed >= 0 && PtInRect(&s_gallery.cards[s_gallery.pressed].close, pt);
            return 0;
        case WM_LBUTTONUP:
        case WM_MBUTTONUP: {
            int hit = CardAt(pt);
            int pressed = std::exchange(s_gallery.pressed, -1);
            if (hit < 0 || hit != pressed || msg != s_gallery.pressedButton) return 0;
            const Card& card = s_gallery.cards[hit];
            bool close = PtInRect(&card.close, pt);
            if (msg == WM_MBUTTONUP || close == s_gallery.pressedClose)
                NotifyOwner(msg == WM_MBUTTONUP || close ? ThumbEvent::Close : ThumbEvent::Activate, card.source);
            return 0;
        }
        case WM_MOUSEWHEEL:
            Thumb_Scroll(s_gallery.owner, GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        case WM_TIMER:
            if (wParam == kPeekTimer) ShowPeek();
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND CreatePreviewWindow(DWORD extraStyle = 0) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = ThumbProc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | extraStyle, kClassName, L"",
                                WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_instance, nullptr);
    if (!hwnd) return nullptr;
    DWORD round = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, kCornerPreference, &round, sizeof(round));
    BOOL noTransitions = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &noTransitions, sizeof(noTransitions));
    return hwnd;
}

void Unregister(HTHUMBNAIL& thumb) {
    if (thumb) DwmUnregisterThumbnail(thumb);
    thumb = nullptr;
}

void HidePeek() {
    if (s_wnd) KillTimer(s_wnd, kPeekTimer);
    if (s_peekWnd && IsWindowVisible(s_peekWnd)) ShowWindow(s_peekWnd, SW_HIDE);
    Unregister(s_peekThumb);
}

void ClearCards() {
    HidePeek();
    for (Card& card : s_gallery.cards) Unregister(card.thumb);
    s_gallery.cards.clear();
    s_gallery.hot = s_gallery.pressed = -1;
    s_gallery.hotClose = false;
}

SIZE Register(HWND dest, HWND source, HTHUMBNAIL& thumb) {
    if (!IsWindow(source)) return {};
    HRESULT hr = DwmRegisterThumbnail(dest, source, &thumb);
    if (FAILED(hr)) {
        thumb = nullptr;
        if (!s_failLogged) Log(L"缩略图：DwmRegisterThumbnail 失败 0x%08X", static_cast<unsigned>(hr));
        s_failLogged = true;
        return {};
    }
    SIZE size = {};
    if (FAILED(DwmQueryThumbnailSourceSize(thumb, &size)) || size.cx < 8 || size.cy < 8) {
        Unregister(thumb);
        return {};
    }
    return size;
}

SIZE FitSize(SIZE source, int width, int height) {
    float scale = std::min({static_cast<float>(width) / source.cx, static_cast<float>(height) / source.cy, 1.0f});
    return {std::max(1L, static_cast<LONG>(source.cx * scale)), std::max(1L, static_cast<LONG>(source.cy * scale))};
}

void PositionThumbnail(HTHUMBNAIL& thumb, SIZE source, RECT image) {
    if (!thumb) return;
    SIZE size = FitSize(source, Width(image), Height(image));
    LONG x = image.left + (Width(image) - size.cx) / 2;
    LONG y = image.top + (Height(image) - size.cy) / 2;
    DWM_THUMBNAIL_PROPERTIES props = {};
    props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_OPACITY;
    props.rcDestination = {x, y, x + size.cx, y + size.cy};
    props.fVisible = TRUE;
    props.fSourceClientAreaOnly = FALSE;
    props.opacity = 255;
    if (FAILED(DwmUpdateThumbnailProperties(thumb, &props))) Unregister(thumb);
}

RECT AvailableArea(const Gallery& gallery) {
    RECT area = gallery.bounds;
    InflateRect(&area, -Pixel(8, gallery.scale), -Pixel(8, gallery.scale));
    RECT adjacent = area;
    switch (gallery.place) {
        case kThumbAbove: adjacent.bottom = std::min(area.bottom, gallery.at.y); break;
        case kThumbLeft: adjacent.right = std::min(area.right, gallery.at.x); break;
        case kThumbRight: adjacent.left = std::max(area.left, gallery.at.x); break;
    }
    // 太靠边时用可用屏幕区域，缩略图依然不会跑到别的屏幕。
    return Width(adjacent) >= Pixel(136, gallery.scale) && Height(adjacent) >= Pixel(104, gallery.scale) ? adjacent : area;
}

POINT FramePosition(ThumbPlace place, POINT pos, const RECT& area, SIZE size) {
    switch (place) {
        case kThumbAbove:
            pos.x -= size.cx / 2;
            pos.y -= size.cy;
            break;
        case kThumbLeft: pos.x -= size.cx; break;
        case kThumbRight: break;
    }
    pos.x = std::clamp(pos.x, area.left, area.right - size.cx);
    pos.y = std::clamp(pos.y, area.top, area.bottom - size.cy);
    return pos;
}

void ShowPeek() {
    HidePeek();
    const Gallery& gallery = s_gallery;
    if (gallery.hot < 0 || gallery.hotClose || gallery.pressed >= 0 || !Thumb_IsShown(gallery.owner)) return;
    const Card& card = gallery.cards[gallery.hot];
    SIZE source{};
    if (!card.thumb || FAILED(DwmQueryThumbnailSourceSize(card.thumb, &source)) || source.cx <= 0 || source.cy <= 0) return;

    RECT frame{}, area = gallery.bounds;
    if (!GetWindowRect(s_wnd, &frame)) return;
    const int gap = Pixel(8, gallery.scale), pad = Pixel(4, gallery.scale);
    InflateRect(&area, -gap, -gap);
    switch (gallery.place) {
        case kThumbAbove: area.bottom = std::min(area.bottom, frame.top - gap); break;
        case kThumbLeft: area.right = std::min(area.right, frame.left - gap); break;
        case kThumbRight: area.left = std::max(area.left, frame.right + gap); break;
    }
    const int width = std::min(Width(area) - 2 * pad, Pixel(560, gallery.scale));
    const int height = std::min(Height(area) - 2 * pad, Pixel(360, gallery.scale));
    if (width <= 0 || height <= 0) return;
    SIZE image = FitSize(source, width, height);
    SIZE small = FitSize(source, Width(card.image), Height(card.image));
    if (image.cx <= small.cx && image.cy <= small.cy) return;

    if (!s_peekWnd) s_peekWnd = CreatePreviewWindow(WS_EX_TRANSPARENT);
    if (!s_peekWnd) return;
    source = Register(s_peekWnd, card.source, s_peekThumb);
    PositionThumbnail(s_peekThumb, source, {pad, pad, pad + image.cx, pad + image.cy});
    if (!s_peekThumb) return;

    SIZE size = {image.cx + 2 * pad, image.cy + 2 * pad};
    POINT anchor{frame.left + Width(frame) / 2, area.bottom};
    if (gallery.place != kThumbAbove) {
        anchor.x = gallery.place == kThumbLeft ? area.right : area.left;
        anchor.y = frame.top + (Height(frame) - size.cy) / 2;
    }
    POINT at = FramePosition(gallery.place, anchor, area, size);
    BOOL dark = !gallery.light;
    DwmSetWindowAttribute(s_peekWnd, kDarkMode, &dark, sizeof(dark));
    InvalidateRect(s_peekWnd, nullptr, FALSE);
    if (!SetWindowPos(s_peekWnd, s_wnd, at.x, at.y, size.cx, size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW)) HidePeek();
}

Grid MakeGrid(int count, SIZE area, float scale, bool vertical) {
    Grid grid;
    grid.pad = Pixel(8, scale);
    grid.gap = Pixel(8, scale);
    grid.captionH = Pixel(23, scale);
    int width = area.cx - 2 * grid.pad, height = area.cy - 2 * grid.pad;
    int minW = Pixel(120, scale);
    grid.columns = std::clamp((width + grid.gap) / (minW + grid.gap), 1, std::min(count, vertical ? 3 : 4));
    grid.width = std::min(Pixel(216, scale), (width - (grid.columns - 1) * grid.gap) / grid.columns);
    int wantedRows = std::min(3, (count + grid.columns - 1) / grid.columns);
    int minRow = Pixel(62, scale) + grid.captionH;
    auto fitRows = [&] {
        return std::clamp((height - grid.footerH + grid.gap) / (minRow + grid.gap), 1, wantedRows);
    };
    grid.rows = fitRows();
    if (count > grid.PageSize()) {
        grid.footerH = Pixel(23, scale);
        grid.rows = fitRows();
    }
    int maxImageH = (height - grid.footerH - (grid.rows - 1) * grid.gap) / grid.rows - grid.captionH;
    grid.imageH = std::min({Pixel(128, scale), grid.width * 9 / 16, maxImageH});
    return grid;
}

void ArrangeCard(Card& card, HWND source, RECT rect, int captionH, float scale, int ordinal) {
    int inset = Pixel(3, scale);
    LONG closeLeft = rect.right - captionH;
    card.source = source;
    card.rect = rect;
    card.caption = {rect.left + inset, rect.top, closeLeft - inset, rect.top + captionH};
    card.close = {closeLeft, rect.top, rect.right, rect.top + captionH};
    card.image = {rect.left + 1, rect.top + captionH, rect.right - 1, rect.bottom - 1};
    card.title = GetWindowTitle(source);
    if (card.title.empty()) card.title = TrF(L"窗口 %d", ordinal);
}

bool ShowPage() {
    ClearCards();
    Gallery& gallery = s_gallery;
    const int count = static_cast<int>(gallery.sources.size());
    RECT area = AvailableArea(gallery);
    const int width = Width(area), height = Height(area);
    if (!count || width < 40 || height < 40) return false;

    float scale = std::min({gallery.scale, width / 150.0f, height / 110.0f});
    gallery.fontPx = Pixel(12, scale);
    SIZE size = {};
    if (count == 1) {
        Card& card = gallery.cards.emplace_back();
        HWND window = gallery.sources.front();
        SIZE source = Register(s_wnd, window, card.thumb);
        if (!card.thumb) return false;
        int pad = Pixel(6, scale), captionH = Pixel(23, scale);
        SIZE image = FitSize(source, std::min(Pixel(240, scale), width - 2 * pad - 2),
                             std::min(Pixel(150, scale), height - 2 * pad - captionH - 1));
        int cardW = std::max(static_cast<int>(image.cx) + 2, Pixel(120, scale));
        int cardH = image.cy + captionH + 1;
        ArrangeCard(card, window, {pad, pad, pad + cardW, pad + cardH}, captionH, scale, 1);
        PositionThumbnail(card.thumb, source, card.image);
        if (!card.thumb) return false;
        size = {cardW + 2 * pad, cardH + 2 * pad};
        gallery.page = 0;
        gallery.pages = 1;
    } else {
        Grid grid = MakeGrid(count, {width, height}, scale, gallery.place != kThumbAbove);
        if (grid.width <= 2 || grid.imageH <= 1) return false;
        size = grid.FrameSize();
        int pageSize = grid.PageSize();
        gallery.pages = (count + pageSize - 1) / pageSize;
        gallery.page = std::clamp(gallery.page, 0, gallery.pages - 1);
        gallery.footer = {grid.pad, size.cy - grid.pad - grid.footerH, size.cx - grid.pad, size.cy - grid.pad};

        int first = gallery.page * pageSize, n = std::min(pageSize, count - first);
        gallery.cards.reserve(n);
        for (int i = 0; i < n; ++i) {
            HWND source = gallery.sources[first + i];
            int x = grid.pad + (i % grid.columns) * (grid.width + grid.gap);
            int y = grid.pad + (i / grid.columns) * (grid.imageH + grid.captionH + grid.gap);
            Card& card = gallery.cards.emplace_back();
            ArrangeCard(card, source, {x, y, x + grid.width, y + grid.imageH + grid.captionH}, grid.captionH, scale, first + i + 1);
            SIZE sourceSize = Register(s_wnd, source, card.thumb);
            PositionThumbnail(card.thumb, sourceSize, card.image);
        }
    }
    POINT at = FramePosition(gallery.place, gallery.at, area, size);
    BOOL dark = !gallery.light;
    DwmSetWindowAttribute(s_wnd, kDarkMode, &dark, sizeof(dark));
    InvalidateRect(s_wnd, nullptr, FALSE);
    if (!SetWindowPos(s_wnd, HWND_TOPMOST, at.x, at.y, size.cx, size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW)) return false;
    POINT mouse{};
    if (gallery.tracking && GetCursorPos(&mouse) && ScreenToClient(s_wnd, &mouse)) UpdateHotCard(mouse);
    return true;
}

void PruneSources() {
    auto& sources = s_gallery.sources;
    sources.erase(std::remove_if(sources.begin(), sources.end(), [](HWND hwnd) { return !IsWindow(hwnd); }), sources.end());
}

void StopTracking() {
    if (!s_gallery.tracking) return;
    TRACKMOUSEEVENT track{sizeof(track), TME_CANCEL | TME_LEAVE, s_wnd, 0};
    TrackMouseEvent(&track);
    s_gallery.tracking = false;
}

}  // namespace

bool Thumb_Show(HWND owner, const std::vector<HWND>& sources, POINT at, ThumbPlace place, const RECT& bounds,
                float scale, bool light) {
    if (!s_wnd && owner && !sources.empty()) s_wnd = CreatePreviewWindow();
    if (!owner || sources.empty() || !s_wnd) {
        Thumb_Hide(owner);
        return false;
    }
    bool same = owner == s_gallery.owner && sources == s_gallery.sources;
    if (!same) {
        StopTracking();
        s_gallery.page = s_gallery.wheelRest = 0;
    }
    s_gallery.owner = owner;
    s_gallery.sources = sources;
    s_gallery.at = at;
    s_gallery.place = place;
    s_gallery.bounds = bounds;
    s_gallery.scale = scale;
    s_gallery.light = light;
    PruneSources();
    if (ShowPage()) return true;
    Thumb_Hide(owner);
    return false;
}

bool Thumb_IsShown(HWND owner) { return s_gallery.owner == owner && !s_gallery.cards.empty() && IsWindowVisible(s_wnd); }

bool Thumb_ContainsPoint(HWND owner, POINT screen) {
    RECT rect{};
    return Thumb_IsShown(owner) && GetWindowRect(s_wnd, &rect) && PtInRect(&rect, screen);
}

bool Thumb_Scroll(HWND owner, int delta) {
    if (!Thumb_IsShown(owner) || s_gallery.pages <= 1) return false;
    s_gallery.wheelRest += delta;
    int up = s_gallery.wheelRest / WHEEL_DELTA;
    s_gallery.wheelRest %= WHEEL_DELTA;
    if (up == 0) return true;
    int page = std::clamp(s_gallery.page - up, 0, s_gallery.pages - 1);
    size_t count = s_gallery.sources.size();
    PruneSources();
    if (page != s_gallery.page || count != s_gallery.sources.size()) {
        s_gallery.page = page;
        if (!ShowPage()) Thumb_Hide(owner);
    }
    return true;
}

void Thumb_RemoveWindow(HWND owner, HWND source) {
    if (!Thumb_IsShown(owner)) return;
    auto& sources = s_gallery.sources;
    auto removed = std::remove(sources.begin(), sources.end(), source);
    if (removed == sources.end()) return;
    sources.erase(removed, sources.end());
    PruneSources();
    if (!ShowPage()) Thumb_Hide(owner);
}

void Thumb_Hide(HWND owner) {
    if (s_gallery.owner != owner) return;
    StopTracking();
    if (s_wnd && IsWindowVisible(s_wnd)) ShowWindow(s_wnd, SW_HIDE);
    ClearCards();
    s_gallery = Gallery{};
}

void Thumb_Destroy() {
    Thumb_Hide(s_gallery.owner);
    if (s_wnd) DestroyWindow(s_wnd);
    if (s_peekWnd) DestroyWindow(s_peekWnd);
    s_wnd = nullptr;
    s_peekWnd = nullptr;
}

}  // namespace app
