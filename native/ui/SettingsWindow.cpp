#include "ui/SettingsWindow.h"

#include "core/WheelLayout.h"
#include "core/WheelVisualGeometry.h"
#include "platform/windows/DiagnosticLog.h"
#include "platform/windows/ElevationService.h"
#include "platform/windows/ShortcutDropHelper.h"
#include "platform/windows/ShortcutDropTarget.h"
#include "platform/windows/ShortcutIconResolver.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <memory>
#include <numeric>
#include <tuple>
#include <vector>

namespace smk::ui {
namespace {

constexpr wchar_t kClassName[] = L"SuperMiddleKeyNativeSettings";
constexpr wchar_t kPageClassName[] = L"SuperMiddleKeySettingsPage";
constexpr wchar_t kPreviewClassName[] = L"SuperMiddleKeySettingsPreview";
constexpr int kSave = 2001, kClose = 2002, kTab = 2003;
constexpr int kShapeCircle = 2010, kShapeRectangle = 2011, kSectors = 2012;
constexpr int kRadius = 2013, kDeadZone = 2014, kOpacity = 2015;
constexpr int kQuickCopy = 2016, kCaptureImages = 2017, kAutoStart = 2018, kAdministrator = 2019;
constexpr int kCleanSpreadsheetText = 2020;
constexpr int kExtended = 2030, kBreakout = 2031, kPreview = 2032, kSlotMode = 2033;
constexpr int kSlotName = 2034, kSlotAction = 2035, kSlotBehavior = 2036, kBrowserUrl = 2037;
constexpr int kSlotValue = 2038, kRepositoryLink = 2040;
constexpr int kUpdateCheck = 2041, kUpdateInstall = 2042, kUpdatePause = 2043;
constexpr int kUpdateBackground = 2044, kUpdateSwitchNode = 2045, kUpdateCancel = 2046, kUpdateAcceleration = 2047;
constexpr int kTabButtonBase = 2060;
constexpr UINT_PTR kHandoffTimer = 11;
constexpr UINT_PTR kSwitchAnimationTimer = 12;
constexpr UINT kHotkeyCapturedMessage = WM_APP + 41;
constexpr UINT kUpdateStateMessage = WM_APP + 52;
constexpr UINT kSwitchAnimationFrameMs = 15;
constexpr double kSwitchAnimationDurationMs = 110.0;
constexpr double kCircleCornerRadius = 10.0;

constexpr unsigned kBackground = 0x06101F;
constexpr unsigned kTop = 0x0A1729;
constexpr unsigned kCard = 0x0E1D31;
constexpr unsigned kControl = 0x14243A;
constexpr unsigned kBorder = 0x263B58;
constexpr unsigned kAccent = 0x0B64F6;
constexpr unsigned kGlow = 0x43B7FF;
constexpr unsigned kText = 0xF3F7FF;
constexpr unsigned kSecondaryText = 0xAAB7CC;
constexpr unsigned kRed = 0xB84A5A;
constexpr unsigned kDisabled = 0x4D586A;

int px(double dip, UINT dpi) {
    return static_cast<int>(std::lround(dip * static_cast<double>(dpi) / 96.0));
}

double dip(int pixels, UINT dpi) {
    return pixels * 96.0 / std::max(1u, dpi);
}

D2D1_COLOR_F color(unsigned value, float alpha = 1.0f) {
    return D2D1::ColorF(((value >> 16) & 0xff) / 255.0f,
        ((value >> 8) & 0xff) / 255.0f, (value & 0xff) / 255.0f, alpha);
}

D2D1_COLOR_F color(const std::wstring& value, float alpha = 1.0f) {
    if (value.size() == 7 && value.front() == L'#') {
        wchar_t* end = nullptr;
        const auto parsed = wcstoul(value.c_str() + 1, &end, 16);
        if (end == value.c_str() + value.size()) return color(parsed, alpha);
    }
    return color(kAccent, alpha);
}

D2D1_RECT_F rect_f(const UiRect& rect, double offset_y = 0.0) {
    return D2D1::RectF(static_cast<float>(rect.x), static_cast<float>(rect.y - offset_y),
        static_cast<float>(rect.right()), static_cast<float>(rect.bottom() - offset_y));
}

D2D1_RECT_F paint_clip_rect(const RECT& rect, UINT dpi_value) {
    return D2D1::RectF(static_cast<float>(dip(rect.left, dpi_value)),
        static_cast<float>(dip(rect.top, dpi_value)),
        static_cast<float>(dip(rect.right, dpi_value)),
        static_cast<float>(dip(rect.bottom, dpi_value)));
}

RECT pixel_rect(const UiRect& rect, UINT dpi_value) {
    return {px(rect.x, dpi_value), px(rect.y, dpi_value),
        px(rect.right(), dpi_value), px(rect.bottom(), dpi_value)};
}

D2D1_ROUNDED_RECT rounded(const UiRect& rect, float radius, double offset_y = 0.0) {
    return D2D1::RoundedRect(rect_f(rect, offset_y), radius, radius);
}

UiRect shifted(UiRect rect, double offset_y) {
    rect.y -= offset_y;
    return rect;
}

UiRect inset(UiRect rect, double amount) {
    rect.x += amount;
    rect.y += amount;
    rect.width = std::max(0.0, rect.width - amount * 2.0);
    rect.height = std::max(0.0, rect.height - amount * 2.0);
    return rect;
}

void move_control(HWND control, const UiRect& rect, UINT dpi, double scroll = 0.0) {
    if (!control) return;
    MoveWindow(control, px(rect.x, dpi), px(rect.y - scroll, dpi),
        std::max(1, px(rect.width, dpi)), std::max(1, px(rect.height, dpi)), TRUE);
}

void move_combo_control(HWND control, const UiRect& rect, UINT dpi, double scroll = 0.0) {
    if (!control) return;
    const int item_count = std::max(0, static_cast<int>(SendMessageW(control, CB_GETCOUNT, 0, 0)));
    constexpr double collapsed_height = 40.0;
    constexpr double item_height = 34.0;
    constexpr double popup_border = 4.0;
    // Win32 includes the collapsed selection field in the combo window's
    // integral-height calculation. Reserve one additional row so the popup
    // can expose every item instead of clipping the final row.
    const double total_height = collapsed_height + (item_count + 1) * item_height + popup_border;
    MoveWindow(control, px(rect.x, dpi), px(rect.y - scroll, dpi),
        std::max(1, px(rect.width, dpi)), std::max(1, px(total_height, dpi)), TRUE);
}

void move_centered_edit(HWND control, const UiRect& frame, HFONT font, UINT dpi, double scroll = 0.0) {
    if (!control) return;
    const RECT frame_pixels = pixel_rect(shifted(frame, scroll), dpi);
    const int frame_height = std::max(1L, frame_pixels.bottom - frame_pixels.top);
    int content_height = px(24.0, dpi);
    if (HDC dc = GetDC(control)) {
        const auto previous = font ? SelectObject(dc, font) : nullptr;
        TEXTMETRICW metrics{};
        if (GetTextMetricsW(dc, &metrics))
            content_height = metrics.tmHeight + metrics.tmExternalLeading + px(4.0, dpi);
        if (previous) SelectObject(dc, previous);
        ReleaseDC(control, dc);
    }
    content_height = std::clamp(content_height, 1, std::max(1, frame_height - px(4.0, dpi)));
    const int inset = px(2.0, dpi);
    const int top = frame_pixels.top + (frame_height - content_height) / 2;
    const int content_width = static_cast<int>(frame_pixels.right - frame_pixels.left) - inset * 2;
    MoveWindow(control, frame_pixels.left + inset, top,
        std::max(1, content_width), content_height, TRUE);
}

HWND make_control(HINSTANCE instance, HWND parent, const wchar_t* type, const wchar_t* text,
    DWORD style, int id, DWORD extended = 0) {
    return CreateWindowExW(extended, type, text, WS_CHILD | WS_VISIBLE | style,
        0, 0, 1, 1, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
}

HWND make_label(HINSTANCE instance, HWND parent, const wchar_t* text, int id = 0, DWORD extra = 0) {
    return make_control(instance, parent, L"STATIC", text, SS_LEFT | extra, id);
}

std::wstring window_text(HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return {};
    std::wstring value(static_cast<std::size_t>(length + 1), L'\0');
    GetWindowTextW(window, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}

std::wstring format_byte_count(double value) {
    constexpr const wchar_t* units[]{L"B", L"KiB", L"MiB", L"GiB"};
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    return std::format(L"{:.1f} {}", value, units[unit]);
}

void draw_text(ID2D1RenderTarget* target, IDWriteTextFormat* format, ID2D1Brush* brush,
    std::wstring_view text, const UiRect& bounds,
    DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING,
    DWRITE_PARAGRAPH_ALIGNMENT paragraph = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
    if (!target || !format || !brush || text.empty()) return;
    format->SetTextAlignment(alignment);
    format->SetParagraphAlignment(paragraph);
    const auto rect = rect_f(bounds);
    target->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format, rect, brush,
        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

D2D1_POINT_2F d2d_point(const smk::core::VisualPoint& point) {
    return D2D1::Point2F(static_cast<float>(point.x), static_cast<float>(point.y));
}

Microsoft::WRL::ComPtr<ID2D1PathGeometry> create_sector_path(
    ID2D1Factory* factory, const smk::core::CircleSectorGeometry& points) {
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (!factory || FAILED(factory->CreatePathGeometry(geometry.GetAddressOf()))
        || FAILED(geometry->Open(sink.GetAddressOf()))) return {};
    sink->BeginFigure(d2d_point(points.outer_start), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddArc(D2D1::ArcSegment(d2d_point(points.outer_end),
        D2D1::SizeF(static_cast<float>(points.outer_radius), static_cast<float>(points.outer_radius)), 0,
        D2D1_SWEEP_DIRECTION_CLOCKWISE, points.large_outer_arc ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(d2d_point(points.outer_end_control), d2d_point(points.end_outer_radial)));
    sink->AddLine(d2d_point(points.end_inner_radial));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(d2d_point(points.inner_end_control), d2d_point(points.inner_end)));
    sink->AddArc(D2D1::ArcSegment(d2d_point(points.inner_start),
        D2D1::SizeF(static_cast<float>(points.inner_radius), static_cast<float>(points.inner_radius)), 0,
        D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, points.large_inner_arc ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(d2d_point(points.inner_start_control), d2d_point(points.start_inner_radial)));
    sink->AddLine(d2d_point(points.start_outer_radial));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(d2d_point(points.outer_start_control), d2d_point(points.outer_start)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return {};
    return geometry;
}

bool is_browser_shortcut(const std::wstring& path) {
    auto name = std::filesystem::path(path).stem().wstring();
    std::transform(name.begin(), name.end(), name.begin(), towlower);
    for (const auto* token : {L"chrome", L"edge", L"firefox", L"waterfox", L"brave", L"opera", L"vivaldi"})
        if (name.find(token) != std::wstring::npos) return true;
    return false;
}

enum class IconKind { link, image, power, user, refresh, repository, info };

void draw_line_icon(ID2D1RenderTarget* target, IconKind kind, const UiRect& box, ID2D1Brush* brush) {
    if (!target || !brush) return;
    const float left = static_cast<float>(box.x), top = static_cast<float>(box.y);
    const float right = static_cast<float>(box.right()), bottom = static_cast<float>(box.bottom());
    const float cx = (left + right) * 0.5f, cy = (top + bottom) * 0.5f;
    const float w = right - left, h = bottom - top;
    const float stroke = std::max(1.2f, std::min(w, h) * 0.075f);
    switch (kind) {
    case IconKind::link:
        target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left + w * .08f, top + h * .28f,
            left + w * .58f, top + h * .72f), 4, 4), brush, stroke);
        target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left + w * .42f, top + h * .18f,
            left + w * .92f, top + h * .62f), 4, 4), brush, stroke);
        target->DrawLine(D2D1::Point2F(left + w * .38f, cy), D2D1::Point2F(left + w * .66f, cy), brush, stroke);
        break;
    case IconKind::image:
        target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left + 2, top + 2, right - 2, bottom - 2), 3, 3), brush, stroke);
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(left + w * .72f, top + h * .30f), w * .07f, h * .07f), brush, stroke);
        target->DrawLine(D2D1::Point2F(left + w * .12f, bottom - h * .18f), D2D1::Point2F(left + w * .40f, top + h * .48f), brush, stroke);
        target->DrawLine(D2D1::Point2F(left + w * .40f, top + h * .48f), D2D1::Point2F(left + w * .60f, bottom - h * .28f), brush, stroke);
        target->DrawLine(D2D1::Point2F(left + w * .60f, bottom - h * .28f), D2D1::Point2F(right - w * .10f, bottom - h * .12f), brush, stroke);
        break;
    case IconKind::power:
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + h * .06f), w * .35f, h * .35f), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx, top + h * .04f), D2D1::Point2F(cx, cy), brush, stroke * 1.15f);
        break;
    case IconKind::user:
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, top + h * .32f), w * .16f, h * .16f), brush, stroke);
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, bottom - h * .22f), w * .34f, h * .23f), brush, stroke);
        break;
    case IconKind::refresh:
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), w * .34f, h * .34f), brush, stroke);
        target->DrawLine(D2D1::Point2F(right - w * .12f, top + h * .32f), D2D1::Point2F(right - w * .12f, top + h * .08f), brush, stroke);
        target->DrawLine(D2D1::Point2F(right - w * .12f, top + h * .08f), D2D1::Point2F(right - w * .34f, top + h * .14f), brush, stroke);
        break;
    case IconKind::repository:
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), w * .38f, h * .38f), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx - w * .12f, top + h * .26f), D2D1::Point2F(cx - w * .12f, bottom - h * .24f), brush, stroke);
        target->DrawLine(D2D1::Point2F(cx - w * .12f, cy), D2D1::Point2F(cx + w * .18f, top + h * .38f), brush, stroke);
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx - w * .12f, top + h * .25f), 2.2f, 2.2f), brush, stroke);
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx + w * .18f, top + h * .38f), 2.2f, 2.2f), brush, stroke);
        break;
    case IconKind::info:
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), w * .36f, h * .36f), brush, stroke);
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, top + h * .30f), 1.8f, 1.8f), brush);
        target->DrawLine(D2D1::Point2F(cx, top + h * .44f), D2D1::Point2F(cx, bottom - h * .24f), brush, stroke);
        break;
    }
}

} // namespace

SettingsWindow::~SettingsWindow() {
    discard_managed_shortcut_candidates();
    detach_update_controller();
    hotkey_capture_.stop();
    shortcut_drop_.reset();
    discard_render_resources();
    if (slot_icon_handle_) DestroyIcon(slot_icon_handle_);
    for (auto icon : preview_slot_icons_) if (icon) DestroyIcon(icon);
    if (window_) DestroyWindow(window_);
    if (font_) DeleteObject(font_);
    if (background_brush_gdi_) DeleteObject(background_brush_gdi_);
    if (card_brush_gdi_) DeleteObject(card_brush_gdi_);
    if (control_brush_gdi_) DeleteObject(control_brush_gdi_);
}

bool SettingsWindow::create(HINSTANCE instance, SaveCallback save_callback,
    smk::updater::UpdateController* update_controller, std::wstring version_text) {
    instance_ = instance;
    save_ = std::move(save_callback);
    update_controller_ = update_controller;
    version_text_ = std::move(version_text);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&controls);
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_factory_.GetAddressOf()))
        || FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf())))) return false;

    WNDCLASSEXW page_class{sizeof(page_class)};
    page_class.hInstance = instance;
    page_class.lpfnWndProc = page_proc;
    page_class.lpszClassName = kPageClassName;
    page_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&page_class);

    WNDCLASSEXW preview_class{sizeof(preview_class)};
    preview_class.hInstance = instance;
    preview_class.lpfnWndProc = preview_proc;
    preview_class.lpszClassName = kPreviewClassName;
    preview_class.hCursor = LoadCursorW(nullptr, IDC_HAND);
    RegisterClassExW(&preview_class);

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_DBLCLKS;
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = kClassName;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    window_class.hIconSm = window_class.hIcon;
    RegisterClassExW(&window_class);

    dpi_ = GetDpiForSystem();
    window_ = CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"超级中键设置",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, px(960, dpi_), px(720, dpi_),
        nullptr, nullptr, instance, this);
    if (!window_) return false;
    dpi_ = GetDpiForWindow(window_);
    SetWindowPos(window_, nullptr, 0, 0, px(960, dpi_), px(720, dpi_),
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    BOOL dark = TRUE;
    (void)DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
    int rounded_preference = 2;
    (void)DwmSetWindowAttribute(window_, 33, &rounded_preference, sizeof(rounded_preference));
    background_brush_gdi_ = CreateSolidBrush(RGB(0x06, 0x10, 0x1F));
    card_brush_gdi_ = CreateSolidBrush(RGB(0x0E, 0x1D, 0x31));
    control_brush_gdi_ = CreateSolidBrush(RGB(0x14, 0x24, 0x3A));
    recreate_font();
    create_controls();
    HRESULT drop_error = S_OK;
    (void)shortcut_drop_.register_window(slot_value_, {
        [this] {
            if (SendMessageW(slot_mode_, CB_GETCURSEL, 0, 0) != 1) return false;
            const auto& slot = settings_.wheel.extended_wheel.slots[static_cast<std::size_t>(selected_slot_)];
            return slot.shortcut_path.empty();
        },
        [this](smk::windows::ShortcutDropVisualState state) {
            shortcut_drop_state_ = state;
            InvalidateRect(slot_value_, nullptr, FALSE);
        },
        [this](const std::wstring& path) { accept_shortcut_path(path); },
    }, &drop_error);
    if (FAILED(drop_error)) SMK_DIAGNOSTIC_EVENT("settings.drop_target", std::format(L"result=failed hresult={:#x}", static_cast<unsigned>(drop_error)));
    if (update_controller_) {
        update_controller_->set_observer([this](const smk::updater::UpdateViewState& state) {
            auto* copy = new smk::updater::UpdateViewState(state);
            if (!window_ || !PostMessageW(window_, kUpdateStateMessage, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
        });
    }
    return true;
}

void SettingsWindow::show(const smk::core::AppSettings& settings) {
    discard_managed_shortcut_candidates();
    cancel_hotkey_recording();
    settings_ = settings;
    committed_settings_ = settings;
    page_scroll_.fill(0.0);
    load_controls();
    if (update_controller_) apply_update_state(update_controller_->state());
    const HWND foreground = GetForegroundWindow();
    HMONITOR monitor = MonitorFromWindow(foreground ? foreground : window_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const RECT& work = info.rcWork;
    // Move the still-hidden window first so PerMonitorV2 can synchronously
    // update dpi_ before the requested DIP size is converted to pixels.
    SetWindowPos(window_, nullptr, work.left, work.top, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    const int available_width = work.right - work.left;
    const int available_height = work.bottom - work.top;
    const int minimum_width = px(760.0, dpi_);
    const int minimum_height = px(640.0, dpi_);
    const int width = std::min(px(960.0, dpi_), available_width);
    const int height = std::min(px(720.0, dpi_), available_height);
    const int fitted_width = available_width >= minimum_width ? std::max(minimum_width, width) : available_width;
    const int fitted_height = available_height >= minimum_height ? std::max(minimum_height, height) : available_height;
    SetWindowPos(window_, nullptr, work.left + (available_width - fitted_width) / 2,
        work.top + (available_height - fitted_height) / 2, fitted_width, fitted_height,
        SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(window_, SW_SHOWNORMAL);
    SetForegroundWindow(window_);
    redraw_visible_controls();
    SMK_DIAGNOSTIC_EVENT("settings.show", std::format(
        L"dpi={} shape={} sectors={} width={} height={} work_width={} work_height={}",
        dpi_, settings_.wheel.shape, settings_.wheel.sector_count,
        fitted_width, fitted_height, available_width, available_height));
}

LRESULT CALLBACK SettingsWindow::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<SettingsWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle_message(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK SettingsWindow::page_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<SettingsWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle_page_message(window, message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK SettingsWindow::preview_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<SettingsWindow*>(create->lpCreateParams);
        self->preview_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle_preview_message(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK SettingsWindow::page_subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR, DWORD_PTR context) {
    auto* self = reinterpret_cast<SettingsWindow*>(context);
    if (self && (message == WM_COMMAND || message == WM_HSCROLL || message == WM_DRAWITEM || message == WM_NOTIFY
        || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX))
        return SendMessageW(self->window_, message, wparam, lparam);
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK SettingsWindow::switch_subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR, DWORD_PTR context) {
    auto* self = reinterpret_cast<SettingsWindow*>(context);
    if (message == WM_ERASEBKGND) return 1;
    if (self && message == BM_GETCHECK) {
        const auto found = std::find_if(self->switch_animations_.begin(), self->switch_animations_.end(),
            [window](const SwitchAnimation& value) { return value.control == window; });
        return found != self->switch_animations_.end() && found->to >= 0.5 ? BST_CHECKED : BST_UNCHECKED;
    }
    if (self && message == BM_SETCHECK) {
        self->set_switch_value(window, wparam == BST_CHECKED);
        return 0;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK SettingsWindow::radio_subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR, DWORD_PTR context) {
    auto* self = reinterpret_cast<SettingsWindow*>(context);
    if (!self) return DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_ERASEBKGND) return 1;
    bool* selected = window == self->shape_circle_
        ? &self->circle_shape_selected_ : &self->rectangle_shape_selected_;
    if (message == BM_GETCHECK) return *selected ? BST_CHECKED : BST_UNCHECKED;
    if (message == BM_SETCHECK) {
        *selected = wparam == BST_CHECKED;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK SettingsWindow::slider_subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR, DWORD_PTR context) {
    auto* self = reinterpret_cast<SettingsWindow*>(context);
    if (self && message == WM_PAINT) {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        RECT bounds{}; GetClientRect(window, &bounds);
        self->draw_slider(window, paint.hdc, bounds);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    if (self && (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE
        || message == WM_KEYDOWN || message == WM_MOUSEWHEEL)) InvalidateRect(window, nullptr, FALSE);
    return result;
}

LRESULT CALLBACK SettingsWindow::edit_subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR, DWORD_PTR) {
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        InvalidateRect(window, nullptr, TRUE);
        InvalidateRect(GetParent(window), nullptr, FALSE);
    }
    return result;
}

LRESULT CALLBACK SettingsWindow::combo_subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR, DWORD_PTR context) {
    auto* self = reinterpret_cast<SettingsWindow*>(context);
    if (self && message == WM_PAINT) {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        RECT bounds{};
        GetClientRect(window, &bounds);
        self->draw_combo_box(window, paint.hdc, bounds);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    if (self && message == WM_PRINTCLIENT) {
        RECT bounds{};
        GetClientRect(window, &bounds);
        self->draw_combo_box(window, reinterpret_cast<HDC>(wparam), bounds);
        return 0;
    }
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    if (self && (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE
        || message == CB_SETCURSEL || message == CB_SELECTSTRING))
        InvalidateRect(window, nullptr, FALSE);
    return result;
}

LRESULT CALLBACK SettingsWindow::button_subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR, DWORD_PTR context) {
    auto* self = reinterpret_cast<SettingsWindow*>(context);
    if (!self) return DefSubclassProc(window, message, wparam, lparam);

    const auto paint = [&](HDC dc) {
        RECT bounds{};
        GetClientRect(window, &bounds);
        UINT state = 0;
        if (!IsWindowEnabled(window)) state |= ODS_DISABLED;
        if ((SendMessageW(window, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0) state |= ODS_SELECTED;
        if (GetFocus() == window) {
            state |= ODS_FOCUS;
            const LRESULT ui_state = SendMessageW(self->window_, WM_QUERYUISTATE, 0, 0);
            if ((ui_state & UISF_HIDEFOCUS) != 0) state |= ODS_NOFOCUSRECT;
        }
        DRAWITEMSTRUCT item{};
        item.CtlType = ODT_BUTTON;
        item.CtlID = static_cast<UINT>(GetDlgCtrlID(window));
        item.itemAction = ODA_DRAWENTIRE;
        item.itemState = state;
        item.hwndItem = window;
        item.hDC = dc;
        item.rcItem = bounds;
        self->draw_button(item);
    };

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT data{};
        BeginPaint(window, &data);
        paint(data.hdc);
        EndPaint(window, &data);
        return 0;
    }
    case WM_PRINTCLIENT:
        paint(reinterpret_cast<HDC>(wparam));
        return 0;
    default:
        break;
    }

    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_SETTEXT || message == WM_SETFOCUS || message == WM_KILLFOCUS
        || message == WM_ENABLE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP
        || message == BM_SETSTATE)
        InvalidateRect(window, nullptr, FALSE);
    return result;
}

LRESULT SettingsWindow::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_NCCALCSIZE:
        if (wparam) return 0;
        break;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        return hit_test_nonclient(point);
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize = {px(760, dpi_), px(640, dpi_)};
        return 0;
    }
    case WM_CLOSE:
        cancel_hotkey_recording();
        discard_managed_shortcut_candidates();
        if (update_controller_) update_controller_->settings_closed();
        ShowWindow(window_, SW_HIDE);
        return 0;
    case kUpdateStateMessage: {
        std::unique_ptr<smk::updater::UpdateViewState> state(reinterpret_cast<smk::updater::UpdateViewState*>(lparam));
        if (state) apply_update_state(*state);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE && hotkey_recorder_.recording()) {
            cancel_hotkey_recording();
            update_slot_editor();
        }
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint_window(); return 0;
    case WM_SIZE:
        layout();
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return 0;
    case WM_DPICHANGED: {
        const UINT previous = dpi_;
        dpi_ = HIWORD(wparam);
        const auto* rect = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window_, nullptr, rect->left, rect->top, rect->right - rect->left,
            rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        recreate_font(); refresh_dpi_metrics(); apply_font(window_); layout(); discard_render_resources();
        redraw_visible_controls();
        SMK_DIAGNOSTIC_EVENT("settings.dpi", std::format(L"previous={} current={}", previous, dpi_));
        (void)previous;
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!tracking_mouse_) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
            TrackMouseEvent(&track); tracking_mouse_ = true;
        }
        const double x = dip(GET_X_LPARAM(lparam), dpi_), y = dip(GET_Y_LPARAM(lparam), dpi_);
        int hover = -1;
        if (chrome_.minimize_button.contains(x, y)) hover = 0;
        else if (chrome_.maximize_button.contains(x, y)) hover = 1;
        else if (chrome_.close_caption_button.contains(x, y)) hover = 2;
        if (hover != hovered_chrome_item_) {
            const int previous = hovered_chrome_item_;
            hovered_chrome_item_ = hover;
            invalidate_chrome_item(previous);
            invalidate_chrome_item(hover);
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        tracking_mouse_ = false;
        const int previous = hovered_chrome_item_;
        hovered_chrome_item_ = -1;
        invalidate_chrome_item(previous);
        return 0;
    }
    case WM_LBUTTONUP: {
        const double x = dip(GET_X_LPARAM(lparam), dpi_), y = dip(GET_Y_LPARAM(lparam), dpi_);
        if (chrome_.minimize_button.contains(x, y)) ShowWindow(window_, SW_MINIMIZE);
        else if (chrome_.maximize_button.contains(x, y))
            ShowWindow(window_, IsZoomed(window_) ? SW_RESTORE : SW_MAXIMIZE);
        else if (chrome_.close_caption_button.contains(x, y)) SendMessageW(window_, WM_CLOSE, 0, 0);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        const double x = dip(GET_X_LPARAM(lparam), dpi_), y = dip(GET_Y_LPARAM(lparam), dpi_);
        if (chrome_.title_bar.contains(x, y) && x < chrome_.minimize_button.x)
            ShowWindow(window_, IsZoomed(window_) ? SW_RESTORE : SW_MAXIMIZE);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        auto dc = reinterpret_cast<HDC>(wparam);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(0xF3, 0xF7, 0xFF));
        return reinterpret_cast<LRESULT>(card_brush_gdi_);
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
        auto dc = reinterpret_cast<HDC>(wparam);
        SetBkMode(dc, OPAQUE); SetBkColor(dc, RGB(0x14, 0x24, 0x3A));
        SetTextColor(dc, RGB(0xF3, 0xF7, 0xFF));
        return reinterpret_cast<LRESULT>(control_brush_gdi_);
    }
    case WM_DRAWITEM:
        draw_owner_control(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam)); return TRUE;
    case WM_HSCROLL:
        update_value_labels(); InvalidateRect(preview_, nullptr, FALSE);
        if (lparam) InvalidateRect(reinterpret_cast<HWND>(lparam), nullptr, FALSE);
        return 0;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lparam)->idFrom == kTab
            && reinterpret_cast<NMHDR*>(lparam)->code == TCN_SELCHANGE)
            select_tab(TabCtrl_GetCurSel(tab_));
        return 0;
    case WM_TIMER:
        if (wparam == kHandoffTimer) { poll_shortcut_drop_handoff(); return 0; }
        if (wparam == kSwitchAnimationTimer) { advance_switch_animations(); return 0; }
        break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (hotkey_recorder_.recording()) { record_hotkey(wparam, lparam); return 0; }
        if (wparam == VK_ESCAPE) { SendMessageW(window_, WM_CLOSE, 0, 0); return 0; }
        break;
    case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR:
        if (hotkey_recorder_.recording()) return 0;
        break;
    case WM_IME_STARTCOMPOSITION: case WM_IME_COMPOSITION:
    case WM_IME_ENDCOMPOSITION: case WM_IME_CHAR:
        if (hotkey_recorder_.recording()) return 0;
        break;
    case kHotkeyCapturedMessage:
        if (hotkey_recorder_.recording()) record_hotkey(wparam, lparam);
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wparam), notification = HIWORD(wparam);
        if (notification == CBN_SELCHANGE
            && (id == kSectors || id == kSlotMode || id == kSlotBehavior)
            && lparam)
            InvalidateRect(reinterpret_cast<HWND>(lparam), nullptr, FALSE);
        if (id >= kTabButtonBase && id < kTabButtonBase + 3 && notification == BN_CLICKED) {
            select_tab(id - kTabButtonBase); return 0;
        }
        if (id == kRepositoryLink && notification == BN_CLICKED) {
            ShellExecuteW(window_, L"open", L"https://github.com/wet86y/clipboard-wheel", nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        if (notification == BN_CLICKED && update_controller_) {
            if (id == kUpdateCheck) { update_controller_->check(); return 0; }
            if (id == kUpdateInstall) { update_controller_->download_or_resume(); return 0; }
            if (id == kUpdatePause) {
                if (update_state_.state == smk::updater::UpdateState::paused) update_controller_->download_or_resume();
                else update_controller_->pause();
                return 0;
            }
            if (id == kUpdateBackground) { update_controller_->continue_in_background(); SendMessageW(window_, WM_CLOSE, 0, 0); return 0; }
            if (id == kUpdateSwitchNode) { update_controller_->next_node(); return 0; }
            if (id == kUpdateCancel) { update_controller_->cancel(); return 0; }
            if (id == kUpdateAcceleration) {
                animate_switch(about_acceleration_);
                return 0;
            }
        }
        if (id == kSave) { cancel_hotkey_recording(); save_controls(); return 0; }
        if (id == kClose) { SendMessageW(window_, WM_CLOSE, 0, 0); return 0; }
        if (id == kShapeCircle || id == kShapeRectangle) {
            if (!loading_ && notification == BN_CLICKED) {
                Button_SetCheck(shape_circle_, id == kShapeCircle ? BST_CHECKED : BST_UNCHECKED);
                Button_SetCheck(shape_rectangle_, id == kShapeRectangle ? BST_CHECKED : BST_UNCHECKED);
                refresh_sector_items(true);
            }
            InvalidateRect(pages_[0], nullptr, FALSE); InvalidateRect(preview_, nullptr, FALSE); return 0;
        }
        if (id == kSectors && notification == CBN_SELCHANGE) { InvalidateRect(preview_, nullptr, FALSE); return 0; }
        if (id == kQuickCopy || id == kCaptureImages || id == kCleanSpreadsheetText
            || id == kAutoStart || id == kAdministrator || id == kExtended) {
            animate_switch(reinterpret_cast<HWND>(lparam));
            if (id == kAdministrator) SetWindowTextW(admin_status_, L"保存后应用此模式");
            InvalidateRect(pages_[id == kExtended ? 1 : 0], nullptr, FALSE);
            InvalidateRect(preview_, nullptr, FALSE); return 0;
        }
        if (id == kSlotMode && notification == CBN_SELCHANGE) {
            cancel_hotkey_recording(); hotkey_recording_error_.clear();
            save_current_slot(); update_slot_editor(); return 0;
        }
        if ((id == kSlotName || id == kBrowserUrl) && notification == EN_CHANGE && !loading_) {
            save_current_slot(); InvalidateRect(preview_, nullptr, FALSE); return 0;
        }
        if (id == kSlotAction && notification == BN_CLICKED) {
            if (SendMessageW(slot_mode_, CB_GETCURSEL, 0, 0) == 0) begin_or_clear_hotkey(); else choose_shortcut();
            return 0;
        }
        if (id == kSlotValue && notification == STN_CLICKED) { start_shortcut_drop_handoff(); return 0; }
        break;
    }
    default: break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

LRESULT SettingsWindow::handle_page_message(HWND page, UINT message, WPARAM wparam, LPARAM lparam) {
    const auto found = std::find(pages_.begin(), pages_.end(), page);
    const int index = found == pages_.end() ? -1 : static_cast<int>(std::distance(pages_.begin(), found));
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: if (index >= 0) paint_page(page, index); return 0;
    case WM_MOUSEWHEEL: {
        if (index < 0) return 0;
        const double delta = -GET_WHEEL_DELTA_WPARAM(wparam) / 120.0 * 48.0;
        if (index == 2) {
            RECT bounds{}; GetClientRect(page, &bounds);
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(page, &point);
            const double width = dip(bounds.right, dpi_), height = dip(bounds.bottom, dpi_);
            const auto layout = make_about_page_layout(width, height, about_update_presentation());
            const auto viewport = shifted(layout.release_notes, page_scroll_[2]);
            if (viewport.contains(dip(point.x, dpi_), dip(point.y, dpi_))
                && scroll_release_notes(delta)) return 0;
        }
        scroll_page(index, delta);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (index >= 0) {
            RECT bounds{}; GetClientRect(page, &bounds);
            const double width = dip(bounds.right, dpi_), height = dip(bounds.bottom, dpi_);
            const double x = dip(GET_X_LPARAM(lparam), dpi_), y = dip(GET_Y_LPARAM(lparam), dpi_);
            if (index == 1) {
                const auto layout = make_wheel_page_layout(width, height);
                const double scroll = page_scroll_[1];
                if (shifted(layout.slot_name, scroll).contains(x, y)) SetFocus(slot_name_);
                else if (IsWindowVisible(browser_url_)
                    && shifted(layout.browser_url, scroll).contains(x, y)) SetFocus(browser_url_);
            }
            if (index == 2) {
                const auto layout = make_about_page_layout(width, height, about_update_presentation());
                update_release_notes_metrics(layout);
                const double maximum = std::max(0.0, release_notes_extent_ - layout.release_notes.height);
                const auto track = shifted(layout.release_notes_scroll_track, page_scroll_[2]);
                if (maximum > 0.5 && UiRect{track.x - 6.0, track.y, 16.0, track.height}.contains(x, y)) {
                    const double thumb_height = std::max(28.0,
                        track.height * layout.release_notes.height / release_notes_extent_);
                    const double thumb_y = track.y + (track.height - thumb_height) * release_notes_scroll_ / maximum;
                    if (UiRect{track.x - 2.0, thumb_y, 8.0, thumb_height}.contains(x, y)) {
                        release_notes_dragging_ = true;
                        release_notes_drag_offset_ = y - thumb_y;
                        SetCapture(page);
                    } else {
                        release_notes_scroll_ = std::clamp(
                            (y - track.y - thumb_height / 2.0) /
                                std::max(1.0, track.height - thumb_height) * maximum,
                            0.0, maximum);
                        InvalidateRect(page, nullptr, FALSE);
                    }
                    return 0;
                }
            }
            const double content = page_content_height(index), maximum = std::max(0.0, content - height);
            if (maximum > 0.0 && x >= width - 14.0) {
                const double thumb = std::max(36.0, height * height / content);
                page_scroll_[static_cast<std::size_t>(index)] = std::clamp(
                    (y - thumb / 2.0) / std::max(1.0, height - thumb) * maximum, 0.0, maximum);
                reposition_page_controls(index); InvalidateRect(page, nullptr, FALSE);
            }
        }
        return 0;
    case WM_MOUSEMOVE:
        if (index == 2 && release_notes_dragging_) {
            RECT bounds{}; GetClientRect(page, &bounds);
            const double width = dip(bounds.right, dpi_), height = dip(bounds.bottom, dpi_);
            const auto layout = make_about_page_layout(width, height, about_update_presentation());
            update_release_notes_metrics(layout);
            const double maximum = std::max(0.0, release_notes_extent_ - layout.release_notes.height);
            const auto track = shifted(layout.release_notes_scroll_track, page_scroll_[2]);
            const double thumb_height = std::max(28.0,
                track.height * layout.release_notes.height / std::max(1.0, release_notes_extent_));
            const double y = dip(GET_Y_LPARAM(lparam), dpi_);
            release_notes_scroll_ = std::clamp(
                (y - track.y - release_notes_drag_offset_) /
                    std::max(1.0, track.height - thumb_height) * maximum,
                0.0, maximum);
            InvalidateRect(page, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (index == 2 && release_notes_dragging_) {
            release_notes_dragging_ = false;
            if (GetCapture() == page) ReleaseCapture();
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (index == 2) release_notes_dragging_ = false;
        break;
    case WM_COMMAND: case WM_HSCROLL: case WM_DRAWITEM: case WM_NOTIFY:
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        return SendMessageW(window_, message, wparam, lparam);
    default: break;
    }
    return DefWindowProcW(page, message, wparam, lparam);
}

LRESULT SettingsWindow::handle_preview_message(UINT message, WPARAM, LPARAM lparam) {
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) { paint_preview(); return 0; }
    if (message == WM_LBUTTONDOWN) {
        update_preview_visual_layout();
        const double x = (dip(GET_X_LPARAM(lparam), dpi_) - preview_surface_center_.x) / preview_scale_;
        const double y = (dip(GET_Y_LPARAM(lparam), dpi_) - preview_surface_center_.y) / preview_scale_;
        const int slot = smk::core::extended_visual_slot_from_point(preview_visual_layout_, x, y);
        if (slot >= 0) {
            save_current_slot(); load_slot(slot); InvalidateRect(preview_, nullptr, FALSE);
        }
        return 0;
    }
    return DefWindowProcW(preview_, message, 0, lparam);
}

LRESULT SettingsWindow::hit_test_nonclient(POINT screen_point) const {
    POINT point = screen_point;
    ScreenToClient(window_, &point);
    RECT bounds{}; GetClientRect(window_, &bounds);
    const int border = px(7, dpi_);
    if (!IsZoomed(window_)) {
        const bool left = point.x < border, right = point.x >= bounds.right - border;
        const bool top = point.y < border, bottom = point.y >= bounds.bottom - border;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }
    const double x = dip(point.x, dpi_), y = dip(point.y, dpi_);
    if (chrome_.minimize_button.contains(x, y) || chrome_.maximize_button.contains(x, y)
        || chrome_.close_caption_button.contains(x, y)) return HTCLIENT;
    if (chrome_.title_bar.contains(x, y)) return HTCAPTION;
    return HTCLIENT;
}

void SettingsWindow::create_controls() {
    tab_ = make_control(instance_, window_, WC_TABCONTROLW, L"", WS_TABSTOP, kTab);
    ShowWindow(tab_, SW_HIDE);
    TCITEMW item{TCIF_TEXT};
    const wchar_t* tab_titles[]{L"基础设置", L"轮盘设置", L"关于"};
    for (const wchar_t* title : tab_titles) {
        item.pszText = const_cast<wchar_t*>(title);
        TabCtrl_InsertItem(tab_, TabCtrl_GetItemCount(tab_), &item);
    }
    for (int index = 0; index < 3; ++index) {
        tab_buttons_[static_cast<std::size_t>(index)] = make_control(instance_, window_, L"BUTTON", tab_titles[index],
            BS_OWNERDRAW | WS_TABSTOP, kTabButtonBase + index);
        pages_[static_cast<std::size_t>(index)] = CreateWindowExW(0, kPageClassName, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 1, 1, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(3000 + index)), instance_, this);
    }
    create_basic_page(); create_wheel_page(); create_about_page();
    switch_animations_ = {{{quick_copy_}, {capture_images_}, {clean_spreadsheet_text_},
        {auto_start_}, {administrator_}, {extended_enabled_}, {about_acceleration_}}};
    for (std::size_t index = 0; index < switch_animations_.size(); ++index)
        SetWindowSubclass(switch_animations_[index].control, switch_subclass_proc, index + 1, reinterpret_cast<DWORD_PTR>(this));
    save_button_ = make_control(instance_, window_, L"BUTTON", L"保存", BS_OWNERDRAW | WS_TABSTOP, kSave);
    close_button_ = make_control(instance_, window_, L"BUTTON", L"关闭", BS_OWNERDRAW | WS_TABSTOP, kClose);
    const std::array owner_buttons{
        tab_buttons_[0], tab_buttons_[1], tab_buttons_[2], save_button_, close_button_,
        slot_action_, about_check_update_, about_install_update_, about_pause_resume_, about_background_,
        about_switch_node_, about_cancel_, repository_link_,
    };
    for (std::size_t index = 0; index < owner_buttons.size(); ++index)
        SetWindowSubclass(owner_buttons[index], button_subclass_proc, index + 1,
            reinterpret_cast<DWORD_PTR>(this));
    apply_font(window_); select_tab(0); layout();
}

void SettingsWindow::create_basic_page() {
    HWND page = pages_[0];
    shape_circle_ = make_control(instance_, page, L"BUTTON", L"圆形", BS_AUTORADIOBUTTON | BS_OWNERDRAW | WS_GROUP | WS_TABSTOP, kShapeCircle);
    shape_rectangle_ = make_control(instance_, page, L"BUTTON", L"矩形", BS_AUTORADIOBUTTON | BS_OWNERDRAW | WS_TABSTOP, kShapeRectangle);
    SetWindowSubclass(shape_circle_, radio_subclass_proc, 1, reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(shape_rectangle_, radio_subclass_proc, 2, reinterpret_cast<DWORD_PTR>(this));
    sectors_ = make_control(instance_, page, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP, kSectors);
    SetWindowTheme(sectors_, L"DarkMode_CFD", nullptr);
    SetWindowSubclass(sectors_, combo_subclass_proc, 1, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(sectors_, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), px(38, dpi_));
    SendMessageW(sectors_, CB_SETITEMHEIGHT, 0, px(34, dpi_));
    radius_ = make_control(instance_, page, TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP, kRadius);
    dead_zone_ = make_control(instance_, page, TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP, kDeadZone);
    opacity_ = make_control(instance_, page, TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP, kOpacity);
    SendMessageW(radius_, TBM_SETRANGE, TRUE, MAKELPARAM(80, 360)); SendMessageW(radius_, TBM_SETTICFREQ, 10, 0);
    SendMessageW(dead_zone_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 120)); SendMessageW(dead_zone_, TBM_SETTICFREQ, 5, 0);
    SendMessageW(opacity_, TBM_SETRANGE, TRUE, MAKELPARAM(20, 100)); SendMessageW(opacity_, TBM_SETTICFREQ, 5, 0);
    for (std::size_t index = 0; index < 3; ++index) {
        HWND slider = index == 0 ? radius_ : index == 1 ? dead_zone_ : opacity_;
        SetWindowSubclass(slider, slider_subclass_proc, index + 1, reinterpret_cast<DWORD_PTR>(this));
    }
    radius_value_ = make_label(instance_, page, L"", 0, SS_OWNERDRAW);
    dead_zone_value_ = make_label(instance_, page, L"", 0, SS_OWNERDRAW);
    opacity_value_ = make_label(instance_, page, L"", 0, SS_OWNERDRAW);
    quick_copy_ = make_control(instance_, page, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, kQuickCopy);
    capture_images_ = make_control(instance_, page, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, kCaptureImages);
    clean_spreadsheet_text_ = make_control(
        instance_, page, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, kCleanSpreadsheetText);
    auto_start_ = make_control(instance_, page, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, kAutoStart);
    administrator_ = make_control(instance_, page, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, kAdministrator);
    admin_status_ = make_label(instance_, page, L"");
}

void SettingsWindow::create_wheel_page() {
    HWND page = pages_[1];
    extended_enabled_ = make_control(instance_, page, L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, kExtended);
    breakout_ = make_control(instance_, page, TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP, kBreakout);
    SendMessageW(breakout_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 80)); SendMessageW(breakout_, TBM_SETTICFREQ, 2, 0);
    SetWindowSubclass(breakout_, slider_subclass_proc, 20, reinterpret_cast<DWORD_PTR>(this));
    breakout_value_ = make_label(instance_, page, L"", 0, SS_OWNERDRAW);
    preview_ = CreateWindowExW(0, kPreviewClassName, L"", WS_CHILD | WS_VISIBLE,
        0, 0, 1, 1, page, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreview)), instance_, this);
    slot_title_ = make_label(instance_, page, L"槽位 上1");
    slot_mode_ = make_control(instance_, page, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP, kSlotMode);
    SetWindowTheme(slot_mode_, L"DarkMode_CFD", nullptr);
    SetWindowSubclass(slot_mode_, combo_subclass_proc, 2, reinterpret_cast<DWORD_PTR>(this));
    for (const wchar_t* text : {L"快捷键", L"应用"}) SendMessageW(slot_mode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    SendMessageW(slot_mode_, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), px(38, dpi_));
    SendMessageW(slot_mode_, CB_SETITEMHEIGHT, 0, px(34, dpi_));
    slot_name_ = make_control(instance_, page, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, kSlotName);
    slot_action_ = make_control(instance_, page, L"BUTTON", L"录制", BS_OWNERDRAW | WS_TABSTOP, kSlotAction);
    slot_value_ = make_control(instance_, page, L"STATIC", L"键位显示", SS_OWNERDRAW | SS_NOTIFY, kSlotValue);
    slot_behavior_label_ = make_label(instance_, page, L"应用第二次触发");
    slot_behavior_ = make_control(instance_, page, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP, kSlotBehavior);
    SetWindowTheme(slot_behavior_, L"DarkMode_CFD", nullptr);
    SetWindowSubclass(slot_behavior_, combo_subclass_proc, 3, reinterpret_cast<DWORD_PTR>(this));
    for (const wchar_t* text : {L"最小化", L"直接关闭"}) SendMessageW(slot_behavior_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    SendMessageW(slot_behavior_, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), px(38, dpi_));
    SendMessageW(slot_behavior_, CB_SETITEMHEIGHT, 0, px(34, dpi_));
    browser_url_label_ = make_label(instance_, page, L"浏览器启动网址（可选）");
    browser_url_ = make_control(instance_, page, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, kBrowserUrl);
    for (std::size_t index = 0; index < 2; ++index) {
        HWND edit = index == 0 ? slot_name_ : browser_url_;
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(px(10, dpi_), px(10, dpi_)));
        SetWindowSubclass(edit, edit_subclass_proc, index + 1, reinterpret_cast<DWORD_PTR>(this));
        SetWindowTheme(edit, L"DarkMode_CFD", nullptr);
    }
    SendMessageW(slot_name_, EM_SETCUEBANNER, TRUE,
        reinterpret_cast<LPARAM>(L"自定义名称（可选）"));
    SendMessageW(browser_url_, EM_SETCUEBANNER, TRUE,
        reinterpret_cast<LPARAM>(L"https://..."));
}

void SettingsWindow::create_about_page() {
    HWND page = pages_[2];
    about_check_update_ = make_control(instance_, page, L"BUTTON", L"检查更新", BS_OWNERDRAW | WS_TABSTOP, kUpdateCheck);
    about_install_update_ = make_control(instance_, page, L"BUTTON", L"下载更新", BS_OWNERDRAW | WS_TABSTOP, kUpdateInstall);
    about_pause_resume_ = make_control(instance_, page, L"BUTTON", L"暂停下载", BS_OWNERDRAW | WS_TABSTOP, kUpdatePause);
    about_background_ = make_control(instance_, page, L"BUTTON", L"转入后台", BS_OWNERDRAW | WS_TABSTOP, kUpdateBackground);
    about_switch_node_ = make_control(instance_, page, L"BUTTON", L"切换节点", BS_OWNERDRAW | WS_TABSTOP, kUpdateSwitchNode);
    about_cancel_ = make_control(instance_, page, L"BUTTON", L"取消", BS_OWNERDRAW | WS_TABSTOP, kUpdateCancel);
    about_acceleration_ = make_control(instance_, page, L"BUTTON", L"", BS_AUTOCHECKBOX | BS_OWNERDRAW | WS_TABSTOP, kUpdateAcceleration);
    repository_link_ = make_control(instance_, page, L"BUTTON", L"查看项目仓库与更新记录  ↗", BS_OWNERDRAW | WS_TABSTOP, kRepositoryLink);
    refresh_update_controls();
}

void SettingsWindow::recreate_font() {
    if (font_) DeleteObject(font_);
    font_ = CreateFontW(-px(14, dpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void SettingsWindow::refresh_dpi_metrics() {
    for (auto combo : {sectors_, slot_mode_, slot_behavior_}) {
        if (!combo) continue;
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), px(38, dpi_));
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, px(34, dpi_));
        InvalidateRect(combo, nullptr, TRUE);
    }
    for (auto edit : {slot_name_, browser_url_}) {
        if (edit) SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
            MAKELPARAM(px(10, dpi_), px(10, dpi_)));
    }
}

void SettingsWindow::apply_font(HWND parent) {
    if (parent != window_) SendMessageW(parent, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    EnumChildWindows(parent, [](HWND child, LPARAM font) -> BOOL {
        SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE); return TRUE;
    }, reinterpret_cast<LPARAM>(font_));
}

void SettingsWindow::layout() {
    if (!window_ || !pages_[0]) return;
    RECT client{}; GetClientRect(window_, &client);
    const double width = dip(client.right, dpi_), height = dip(client.bottom, dpi_);
    chrome_ = make_settings_chrome_layout(width, height);
    for (std::size_t index = 0; index < tab_buttons_.size(); ++index)
        move_control(tab_buttons_[index], chrome_.tabs[index], dpi_);
    move_control(save_button_, chrome_.save_button, dpi_);
    move_control(close_button_, chrome_.close_button, dpi_);
    for (auto page : pages_) move_control(page, chrome_.page_viewport, dpi_);
    layout_basic_page(); layout_wheel_page(); layout_about_page();
    SMK_DIAGNOSTIC_EVENT("settings.layout", std::format(
        L"dpi={} client_width={} client_height={} page_width={:.1f} page_height={:.1f}",
        dpi_, client.right, client.bottom, chrome_.page_viewport.width, chrome_.page_viewport.height));
}

void SettingsWindow::layout_basic_page() { reposition_page_controls(0); }
void SettingsWindow::layout_wheel_page() { reposition_page_controls(1); }
void SettingsWindow::layout_about_page() { reposition_page_controls(2); }

void SettingsWindow::reposition_page_controls(int index) {
    if (index < 0 || index >= 3) return;
    const double maximum = std::max(0.0, page_content_height(index) - chrome_.page_viewport.height);
    page_scroll_[static_cast<std::size_t>(index)] = std::clamp(page_scroll_[static_cast<std::size_t>(index)], 0.0, maximum);
    const double scroll = page_scroll_[static_cast<std::size_t>(index)];
    if (index == 0) {
        const auto value = make_basic_page_layout(chrome_.page_viewport.width);
        move_control(shape_circle_, value.circle_radio, dpi_, scroll);
        move_control(shape_rectangle_, value.rectangle_radio, dpi_, scroll);
        move_combo_control(sectors_, value.sector_combo, dpi_, scroll);
        const HWND sliders[]{radius_, dead_zone_, opacity_};
        const HWND labels[]{radius_value_, dead_zone_value_, opacity_value_};
        for (std::size_t i = 0; i < 3; ++i) { move_control(sliders[i], value.sliders[i], dpi_, scroll); move_control(labels[i], value.values[i], dpi_, scroll); }
        const HWND switches[]{quick_copy_, capture_images_, clean_spreadsheet_text_, auto_start_, administrator_};
        for (std::size_t i = 0; i < 5; ++i) move_control(switches[i], value.switches[i], dpi_, scroll);
        move_control(admin_status_, {320.0, 509.0, value.card.width - 430.0, 26.0}, dpi_, scroll);
    } else if (index == 1) {
        const auto value = make_wheel_page_layout(chrome_.page_viewport.width, chrome_.page_viewport.height);
        move_control(extended_enabled_, value.extended_switch, dpi_, scroll);
        move_control(breakout_, value.breakout_slider, dpi_, scroll);
        move_control(breakout_value_, value.breakout_value, dpi_, scroll);
        move_control(preview_, value.preview, dpi_, scroll);
        update_preview_visual_layout();
        move_control(slot_title_, {28.0, value.editor_card.y + 12.0, 260.0, 30.0}, dpi_, scroll);
        move_combo_control(slot_mode_, value.slot_mode, dpi_, scroll);
        move_centered_edit(slot_name_, value.slot_name, font_, dpi_, scroll);
        move_control(slot_action_, value.slot_action, dpi_, scroll);
        move_control(slot_value_, value.slot_value, dpi_, scroll);
        move_control(slot_behavior_label_, {28.0, value.editor_card.y + 168.0, 180.0, 40.0}, dpi_, scroll);
        move_combo_control(slot_behavior_, value.slot_behavior, dpi_, scroll);
        move_control(browser_url_label_, {28.0, value.editor_card.y + 220.0, 190.0, 40.0}, dpi_, scroll);
        move_centered_edit(browser_url_, value.browser_url, font_, dpi_, scroll);
    } else {
        const auto value = make_about_page_layout(chrome_.page_viewport.width,
            chrome_.page_viewport.height, about_update_presentation());
        update_release_notes_metrics(value);
        move_control(about_check_update_, value.check_update, dpi_, scroll);
        move_control(about_install_update_, value.install_update, dpi_, scroll);
        move_control(about_pause_resume_, value.pause_resume, dpi_, scroll);
        move_control(about_background_, value.background, dpi_, scroll);
        move_control(about_switch_node_, value.switch_node, dpi_, scroll);
        move_control(about_cancel_, value.cancel, dpi_, scroll);
        move_control(about_acceleration_, value.acceleration, dpi_, scroll);
        move_control(repository_link_, value.repository_link, dpi_, scroll);
    }
    redraw_page_controls(index);
}

void SettingsWindow::redraw_page_controls(int index, bool update_now) {
    if (index < 0 || index >= static_cast<int>(pages_.size())) return;
    const HWND page = pages_[static_cast<std::size_t>(index)];
    if (!page) return;
    UINT flags = RDW_INVALIDATE | RDW_ALLCHILDREN;
    if (update_now && IsWindowVisible(page)) flags |= RDW_UPDATENOW;
    RedrawWindow(page, nullptr, nullptr, flags);
}

void SettingsWindow::redraw_visible_controls() {
    if (!window_ || !IsWindowVisible(window_)) return;
    RedrawWindow(window_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void SettingsWindow::invalidate_chrome_item(int item) {
    if (!window_ || item < 0 || item > 2) return;
    const UiRect items[]{chrome_.minimize_button, chrome_.maximize_button, chrome_.close_caption_button};
    RECT invalid = pixel_rect(items[item], dpi_);
    InflateRect(&invalid, 1, 1);
    InvalidateRect(window_, &invalid, FALSE);
}

void SettingsWindow::select_tab(int index) {
    if (hotkey_recorder_.recording()) {
        cancel_hotkey_recording();
        update_slot_editor();
    }
    active_tab_ = std::clamp(index, 0, 2);
    TabCtrl_SetCurSel(tab_, active_tab_);
    for (int page = 0; page < 3; ++page) {
        ShowWindow(pages_[static_cast<std::size_t>(page)], page == active_tab_ ? SW_SHOW : SW_HIDE);
        InvalidateRect(tab_buttons_[static_cast<std::size_t>(page)], nullptr, FALSE);
    }
    redraw_page_controls(active_tab_, true);
    SetFocus(tab_buttons_[static_cast<std::size_t>(active_tab_)]);
    SMK_DIAGNOSTIC_EVENT("settings.tab", std::format(L"index={}", active_tab_));
}

void SettingsWindow::scroll_page(int index, double delta) {
    if (index < 0 || index >= 3) return;
    const double maximum = std::max(0.0, page_content_height(index) - chrome_.page_viewport.height);
    auto& value = page_scroll_[static_cast<std::size_t>(index)];
    const double next = std::clamp(value + delta, 0.0, maximum);
    if (std::abs(next - value) < 0.1) return;
    for (const HWND combo : {sectors_, slot_mode_, slot_behavior_})
        if (combo) SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
    value = next; reposition_page_controls(index);
}

double SettingsWindow::page_content_height(int index) const {
    if (index == 0) return make_basic_page_layout(chrome_.page_viewport.width).content_height;
    if (index == 1) {
        const auto layout = make_wheel_page_layout(chrome_.page_viewport.width, chrome_.page_viewport.height);
        return SendMessageW(slot_mode_, CB_GETCURSEL, 0, 0) == 1 ? layout.shortcut_content_height : layout.hotkey_content_height;
    }
    return make_about_page_layout(chrome_.page_viewport.width, chrome_.page_viewport.height,
        about_update_presentation()).content_height;
}

void SettingsWindow::load_controls() {
    loading_ = true;
    Button_SetCheck(shape_rectangle_, settings_.wheel.shape == L"rectangle" ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(shape_circle_, settings_.wheel.shape == L"rectangle" ? BST_UNCHECKED : BST_CHECKED);
    refresh_sector_items(false);
    SendMessageW(radius_, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::clamp(settings_.wheel.radius, 80.0, 360.0)));
    SendMessageW(dead_zone_, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::clamp(settings_.wheel.inner_dead_zone_radius, 0.0, 120.0)));
    SendMessageW(opacity_, TBM_SETPOS, TRUE, static_cast<LPARAM>(settings_.wheel.opacity * 100.0));
    Button_SetCheck(quick_copy_, settings_.wheel.quick_copy ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(capture_images_, settings_.clipboard.capture_images ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(clean_spreadsheet_text_,
        settings_.clipboard.clean_spreadsheet_plain_text ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(auto_start_, settings_.auto_start_enabled ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(administrator_, settings_.run_as_administrator_enabled ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(extended_enabled_, settings_.wheel.extended_wheel.enabled ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(about_acceleration_, settings_.update.use_acceleration_nodes ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(breakout_, TBM_SETPOS, TRUE, static_cast<LPARAM>(settings_.wheel.extended_wheel.breakout_buffer_pixels));
    SetWindowTextW(admin_status_, L"");
    loading_ = false;
    load_slot(0); update_value_labels();
    for (auto control : {quick_copy_, capture_images_, clean_spreadsheet_text_, auto_start_,
            administrator_, extended_enabled_, about_acceleration_})
        set_switch_value(control, Button_GetCheck(control) == BST_CHECKED);
    InvalidateRect(preview_, nullptr, FALSE);
}

void SettingsWindow::save_controls() {
    save_current_slot();
    settings_.wheel.shape = Button_GetCheck(shape_rectangle_) == BST_CHECKED ? L"rectangle" : L"circle";
    wchar_t sector[8]{}; GetWindowTextW(sectors_, sector, static_cast<int>(std::size(sector)));
    settings_.wheel.sector_count = _wtoi(sector);
    settings_.wheel.radius = static_cast<double>(SendMessageW(radius_, TBM_GETPOS, 0, 0));
    settings_.wheel.inner_dead_zone_radius = static_cast<double>(SendMessageW(dead_zone_, TBM_GETPOS, 0, 0));
    settings_.wheel.opacity = SendMessageW(opacity_, TBM_GETPOS, 0, 0) / 100.0;
    settings_.wheel.quick_copy = Button_GetCheck(quick_copy_) == BST_CHECKED;
    settings_.clipboard.capture_images = Button_GetCheck(capture_images_) == BST_CHECKED;
    settings_.clipboard.clean_spreadsheet_plain_text =
        Button_GetCheck(clean_spreadsheet_text_) == BST_CHECKED;
    settings_.auto_start_enabled = Button_GetCheck(auto_start_) == BST_CHECKED;
    settings_.run_as_administrator_enabled = Button_GetCheck(administrator_) == BST_CHECKED;
    settings_.wheel.extended_wheel.enabled = Button_GetCheck(extended_enabled_) == BST_CHECKED;
    settings_.update.use_acceleration_nodes = Button_GetCheck(about_acceleration_) == BST_CHECKED;
    settings_.wheel.extended_wheel.breakout_buffer_pixels = static_cast<double>(SendMessageW(breakout_, TBM_GETPOS, 0, 0));
    smk::core::normalize_settings(settings_);
    SMK_DIAGNOSTIC_EVENT("settings.save", std::format(L"shape={} sectors={} radius={} dead={} opacity={:.2f} extended={}",
        settings_.wheel.shape, settings_.wheel.sector_count, settings_.wheel.radius,
        settings_.wheel.inner_dead_zone_radius, settings_.wheel.opacity, settings_.wheel.extended_wheel.enabled));
    const auto accepted = save_ ? save_(settings_) : std::optional<smk::core::AppSettings>(settings_);
    if (accepted) {
        managed_shortcuts_.reconcile(committed_settings_, *accepted, managed_shortcut_candidates_);
        managed_shortcut_candidates_.clear();
        settings_ = *accepted;
        committed_settings_ = *accepted;
        load_controls();
        ShowWindow(window_, SW_HIDE);
    } else {
        discard_managed_shortcut_candidates();
        settings_ = committed_settings_;
        load_controls();
        SetWindowTextW(admin_status_, L"设置文件保存失败，请重试。");
        InvalidateRect(pages_[0], nullptr, FALSE);
    }
}

void SettingsWindow::detach_update_controller() noexcept {
    if (update_controller_) update_controller_->set_observer({});
    update_controller_ = nullptr;
    if (!window_) return;
    MSG message{};
    while (PeekMessageW(&message, window_, kUpdateStateMessage, kUpdateStateMessage, PM_REMOVE)) {
        delete reinterpret_cast<smk::updater::UpdateViewState*>(message.lParam);
    }
}

void SettingsWindow::refresh_sector_items(bool reset) {
    const bool rectangle = Button_GetCheck(shape_rectangle_) == BST_CHECKED;
    SendMessageW(sectors_, CB_RESETCONTENT, 0, 0);
    const int circle_values[]{4, 6, 8}, rectangle_values[]{4, 8};
    const int* values = rectangle ? rectangle_values : circle_values;
    const int count = rectangle ? 2 : 3;
    const int requested = reset ? (rectangle ? 8 : 6) : settings_.wheel.sector_count;
    int selected = 0;
    for (int index = 0; index < count; ++index) {
        const auto text = std::to_wstring(values[index]);
        SendMessageW(sectors_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        if (values[index] == requested) selected = index;
    }
    SendMessageW(sectors_, CB_SETCURSEL, selected, 0);
    if (chrome_.page_viewport.width > 0.0) {
        const auto layout = make_basic_page_layout(chrome_.page_viewport.width);
        move_combo_control(sectors_, layout.sector_combo, dpi_, page_scroll_[0]);
    }
    InvalidateRect(sectors_, nullptr, TRUE);
}

void SettingsWindow::update_value_labels() {
    SetWindowTextW(radius_value_, std::format(L"当前：{}", SendMessageW(radius_, TBM_GETPOS, 0, 0)).c_str());
    SetWindowTextW(dead_zone_value_, std::format(L"当前：{}", SendMessageW(dead_zone_, TBM_GETPOS, 0, 0)).c_str());
    SetWindowTextW(opacity_value_, std::format(L"当前：{:.2f}", SendMessageW(opacity_, TBM_GETPOS, 0, 0) / 100.0).c_str());
    SetWindowTextW(breakout_value_, std::format(L"当前：{}px", SendMessageW(breakout_, TBM_GETPOS, 0, 0)).c_str());
    for (auto value : {radius_value_, dead_zone_value_, opacity_value_, breakout_value_}) InvalidateRect(value, nullptr, FALSE);
}

void SettingsWindow::set_switch_value(HWND toggle, bool checked) {
    const auto found = std::find_if(switch_animations_.begin(), switch_animations_.end(),
        [toggle](const SwitchAnimation& value) { return value.control == toggle; });
    if (found == switch_animations_.end()) return;
    found->value = found->from = found->to = checked ? 1.0 : 0.0;
    found->active = false; InvalidateRect(toggle, nullptr, FALSE);
}

void SettingsWindow::animate_switch(HWND toggle) {
    const auto found = std::find_if(switch_animations_.begin(), switch_animations_.end(),
        [toggle](const SwitchAnimation& value) { return value.control == toggle; });
    if (found == switch_animations_.end()) return;
    found->from = found->value; found->to = found->to >= 0.5 ? 0.0 : 1.0;
    found->started_at = GetTickCount64(); found->active = true;
    SetTimer(window_, kSwitchAnimationTimer, kSwitchAnimationFrameMs, nullptr);
    InvalidateRect(toggle, nullptr, FALSE);
}

void SettingsWindow::advance_switch_animations() {
    bool active = false;
    const auto now = GetTickCount64();
    for (auto& animation : switch_animations_) {
        if (!animation.active) continue;
        const double progress = std::clamp((now - animation.started_at) / kSwitchAnimationDurationMs, 0.0, 1.0);
        const double eased = 1.0 - (1.0 - progress) * (1.0 - progress);
        animation.value = animation.from + (animation.to - animation.from) * eased;
        animation.active = progress < 1.0; active = active || animation.active;
        InvalidateRect(animation.control, nullptr, FALSE);
    }
    if (!active) KillTimer(window_, kSwitchAnimationTimer);
}

void SettingsWindow::draw_owner_control(const DRAWITEMSTRUCT& item) {
    if (item.CtlType == ODT_COMBOBOX) { draw_combo(item); return; }
    if (item.CtlType == ODT_STATIC) { draw_value_panel(item); return; }
    if (item.hwndItem == shape_circle_ || item.hwndItem == shape_rectangle_) { draw_radio(item); return; }
    const bool is_switch = std::any_of(switch_animations_.begin(), switch_animations_.end(),
        [&item](const SwitchAnimation& value) { return value.control == item.hwndItem; });
    if (is_switch) draw_switch(item); else draw_button(item);
}

void SettingsWindow::draw_switch(const DRAWITEMSTRUCT& item) {
    ID2D1DCRenderTarget* target = nullptr;
    if (!begin_dc_draw(item.hDC, item.rcItem, &target)) return;
    const double scale = dpi_ / 96.0;
    const double width = (item.rcItem.right - item.rcItem.left) / scale;
    const double height = (item.rcItem.bottom - item.rcItem.top) / scale;
    const auto found = std::find_if(switch_animations_.begin(), switch_animations_.end(),
        [&item](const SwitchAnimation& value) { return value.control == item.hwndItem; });
    const double position = found == switch_animations_.end() ? 0.0 : found->value;
    const auto lerp = [position](int off, int on) { return static_cast<unsigned>(std::lround(off + (on - off) * position)); };
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> track;
    target->CreateSolidColorBrush(D2D1::ColorF(lerp(0xB8, 0x0B) / 255.0f,
        lerp(0x4A, 0x64) / 255.0f, lerp(0x5A, 0xF6) / 255.0f), track.GetAddressOf());
    target->Clear(color(kCard));
    target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)),
        static_cast<float>(height / 2.0), static_cast<float>(height / 2.0)), track.Get());
    const float diameter = static_cast<float>(height - 4.0);
    const float left = static_cast<float>(2.0 + (width - height) * position);
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(left + diameter / 2.0f, static_cast<float>(height / 2.0)),
        diameter / 2.0f, diameter / 2.0f), text_brush_.Get());
    end_dc_draw();
}

void SettingsWindow::draw_button(const DRAWITEMSTRUCT& item) {
    ID2D1DCRenderTarget* target = nullptr;
    if (!begin_dc_draw(item.hDC, item.rcItem, &target)) return;
    const double scale = dpi_ / 96.0;
    const double width = (item.rcItem.right - item.rcItem.left) / scale;
    const double height = (item.rcItem.bottom - item.rcItem.top) / scale;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const int id = GetDlgCtrlID(item.hwndItem);
    const bool tab = id >= kTabButtonBase && id < kTabButtonBase + 3;
    const bool active_tab = tab && id - kTabButtonBase == active_tab_;
    const bool focused = (item.itemState & ODS_FOCUS) != 0
        && (item.itemState & ODS_NOFOCUSRECT) == 0;
    const auto text = window_text(item.hwndItem);
    const bool danger = text == L"清除" || text == L"结束录制";
    target->Clear(color(tab ? kTop
        : (GetParent(item.hwndItem) == window_ ? kBackground : kCard)));
    target->PushAxisAlignedClip(D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    ID2D1Brush* fill = disabled ? disabled_brush_.Get()
        : danger ? red_brush_.Get() : (id == kSave || id == kSlotAction || active_tab) ? accent_brush_.Get() : control_brush_.Get();
    if (pressed && fill) fill->SetOpacity(0.78f);
    const UiRect bounds{1.0, 1.0, std::max(1.0, width - 2.0), std::max(1.0, height - 2.0)};
    target->FillRoundedRectangle(rounded(bounds, tab ? 6.0f : 7.0f), fill);
    if (fill) fill->SetOpacity(1.0f);
    // A selected tab already communicates focus. Reuse that exact outer
    // boundary for keyboard focus as well instead of drawing a second inset
    // ring after mouse clicks.
    const bool emphasized_tab_border = tab && (active_tab || focused);
    const float border_width = emphasized_tab_border ? 1.4f : 1.0f;
    target->DrawRoundedRectangle(rounded(inset(bounds, border_width / 2.0),
        tab ? 6.0f : 7.0f), emphasized_tab_border ? glow_brush_.Get() : border_brush_.Get(), border_width);
    draw_text(target, body_format_.Get(), disabled ? secondary_text_brush_.Get() : text_brush_.Get(), text,
        {4.0, 0.0, width - 8.0, height}, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (focused && !tab)
        target->DrawRoundedRectangle(rounded(inset(bounds, 3.5), 4.5f), glow_brush_.Get(), 1.0f);
    target->PopAxisAlignedClip();
    end_dc_draw();
}

void SettingsWindow::draw_radio(const DRAWITEMSTRUCT& item) {
    ID2D1DCRenderTarget* target = nullptr;
    if (!begin_dc_draw(item.hDC, item.rcItem, &target)) return;
    const double scale = dpi_ / 96.0;
    const double width = (item.rcItem.right - item.rcItem.left) / scale;
    const double height = (item.rcItem.bottom - item.rcItem.top) / scale;
    target->Clear(color(kCard));
    const bool checked = Button_GetCheck(item.hwndItem) == BST_CHECKED;
    target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(13.0f, static_cast<float>(height / 2.0)), 9.0f, 9.0f),
        checked ? accent_brush_.Get() : secondary_text_brush_.Get(), checked ? 2.0f : 1.5f);
    if (checked) target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(13.0f, static_cast<float>(height / 2.0)), 4.0f, 4.0f), accent_brush_.Get());
    draw_text(target, body_format_.Get(), text_brush_.Get(), window_text(item.hwndItem),
        {30.0, 0.0, width - 30.0, height});
    end_dc_draw();
}

void SettingsWindow::draw_combo(const DRAWITEMSTRUCT& item) {
    if ((item.itemState & ODS_COMBOBOXEDIT) != 0 || item.itemID == static_cast<UINT>(-1)) return;
    ID2D1DCRenderTarget* target = nullptr;
    if (!begin_dc_draw(item.hDC, item.rcItem, &target)) return;
    const double scale = dpi_ / 96.0;
    const double width = (item.rcItem.right - item.rcItem.left) / scale;
    const double height = (item.rcItem.bottom - item.rcItem.top) / scale;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    target->Clear(color(kControl));
    if (selected)
        target->FillRoundedRectangle(rounded({2.0, 2.0, std::max(1.0, width - 4.0),
            std::max(1.0, height - 4.0)}, 4.0f), accent_brush_.Get());
    std::wstring text;
    int item_index = static_cast<int>(item.itemID);
    if (item_index < 0) item_index = static_cast<int>(SendMessageW(item.hwndItem, CB_GETCURSEL, 0, 0));
    if (item_index >= 0) {
        const LRESULT length = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item_index, 0);
        if (length >= 0) {
            text.resize(static_cast<std::size_t>(length + 1));
            SendMessageW(item.hwndItem, CB_GETLBTEXT, item_index, reinterpret_cast<LPARAM>(text.data()));
            text.resize(static_cast<std::size_t>(length));
        }
    }
    draw_text(target, body_format_.Get(), disabled ? secondary_text_brush_.Get() : text_brush_.Get(), text,
        {8.0, 0.0, std::max(1.0, width - 16.0), height}, DWRITE_TEXT_ALIGNMENT_CENTER);
    end_dc_draw();
}

void SettingsWindow::draw_combo_box(HWND combo, HDC dc, const RECT& bounds) {
    ID2D1DCRenderTarget* target = nullptr;
    if (!begin_dc_draw(dc, bounds, &target)) return;
    const double scale = dpi_ / 96.0;
    const double width = (bounds.right - bounds.left) / scale;
    const double height = (bounds.bottom - bounds.top) / scale;
    const bool enabled = IsWindowEnabled(combo) != FALSE;
    target->Clear(color(kControl));
    const UiRect control_bounds{0.5, 0.5, std::max(1.0, width - 1.0), std::max(1.0, height - 1.0)};
    target->FillRoundedRectangle(rounded(control_bounds, 6.0f),
        enabled ? control_brush_.Get() : disabled_brush_.Get());
    const float border_width = GetFocus() == combo ? 1.35f : 1.0f;
    target->DrawRoundedRectangle(rounded(inset(control_bounds, border_width / 2.0), 6.0f),
        GetFocus() == combo ? glow_brush_.Get() : border_brush_.Get(), border_width);
    std::wstring selected_text;
    const int selected = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (selected >= 0) {
        const LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, selected, 0);
        if (length >= 0) {
            selected_text.resize(static_cast<std::size_t>(length + 1));
            SendMessageW(combo, CB_GETLBTEXT, selected,
                reinterpret_cast<LPARAM>(selected_text.data()));
            selected_text.resize(static_cast<std::size_t>(length));
        }
    }
    draw_text(target, body_format_.Get(), enabled ? text_brush_.Get() : secondary_text_brush_.Get(),
        selected_text, {8.0, 0.0, std::max(1.0, width - 44.0), height},
        DWRITE_TEXT_ALIGNMENT_CENTER);
    const float cx = static_cast<float>(width - 18.0);
    const float cy = static_cast<float>(height / 2.0);
    auto* arrow = enabled ? secondary_text_brush_.Get() : disabled_brush_.Get();
    target->DrawLine(D2D1::Point2F(cx - 4, cy - 2), D2D1::Point2F(cx, cy + 2), arrow, 1.5f);
    target->DrawLine(D2D1::Point2F(cx, cy + 2), D2D1::Point2F(cx + 4, cy - 2), arrow, 1.5f);
    end_dc_draw();
}

void SettingsWindow::draw_value_panel(const DRAWITEMSTRUCT& item) {
    ID2D1DCRenderTarget* target = nullptr;
    if (!begin_dc_draw(item.hDC, item.rcItem, &target)) return;
    const double scale = dpi_ / 96.0;
    const double width = (item.rcItem.right - item.rcItem.left) / scale;
    const double height = (item.rcItem.bottom - item.rcItem.top) / scale;
    target->Clear(color(kCard));
    const UiRect bounds{0.5, 0.5, width - 1.0, height - 1.0};
    target->FillRoundedRectangle(rounded(bounds, 7), control_brush_.Get());
    ID2D1Brush* outline = border_brush_.Get();
    if (item.hwndItem == slot_value_) {
        if (hotkey_recorder_.recording()
            || shortcut_drop_state_ == smk::windows::ShortcutDropVisualState::accept) outline = glow_brush_.Get();
        else if (shortcut_drop_state_ == smk::windows::ShortcutDropVisualState::reject) outline = red_brush_.Get();
        else if (shortcut_drop_state_ == smk::windows::ShortcutDropVisualState::success) outline = accent_brush_.Get();
    }
    target->DrawRoundedRectangle(rounded(bounds, 7), outline, 1.0f);
    const bool slot_value = item.hwndItem == slot_value_;
    const bool has_icon = slot_value && slot_icon_handle_;
    const double text_left = has_icon ? 58.0 : 10.0;
    draw_text(target, body_format_.Get(), slot_value ? secondary_text_brush_.Get() : text_brush_.Get(),
        window_text(item.hwndItem), {text_left, 0.0, width - text_left - 10.0, height},
        item.hwndItem == slot_value_ ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_CENTER);
    end_dc_draw();
    if (has_icon) {
        const int icon_size = px(36.0, dpi_);
        const int control_height = item.rcItem.bottom - item.rcItem.top;
        DrawIconEx(item.hDC, px(12.0, dpi_), std::max(0, (control_height - icon_size) / 2),
            slot_icon_handle_, icon_size, icon_size, 0, nullptr, DI_NORMAL);
    }
}

void SettingsWindow::draw_slider(HWND slider, HDC dc, const RECT& bounds) {
    ID2D1DCRenderTarget* target = nullptr;
    if (!begin_dc_draw(dc, bounds, &target)) return;
    const double scale = dpi_ / 96.0;
    const double width = (bounds.right - bounds.left) / scale;
    target->Clear(color(kCard));
    const int minimum = static_cast<int>(SendMessageW(slider, TBM_GETRANGEMIN, 0, 0));
    const int maximum = static_cast<int>(SendMessageW(slider, TBM_GETRANGEMAX, 0, 0));
    const int position = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
    const double progress = maximum > minimum ? (position - minimum) / static_cast<double>(maximum - minimum) : 0.0;
    const float left = 10.0f, right = static_cast<float>(std::max(11.0, width - 10.0));
    const float y = 13.0f;
    target->DrawLine(D2D1::Point2F(left, y), D2D1::Point2F(right, y), border_brush_.Get(), 5.0f);
    target->DrawLine(D2D1::Point2F(left, y), D2D1::Point2F(static_cast<float>(left + (right - left) * progress), y), accent_brush_.Get(), 5.0f);
    for (int tick = 0; tick <= 10; ++tick) {
        const float x = left + (right - left) * tick / 10.0f;
        target->DrawLine(D2D1::Point2F(x, 25.0f), D2D1::Point2F(x, 29.0f), secondary_text_brush_.Get(), 1.0f);
    }
    const float thumb_x = static_cast<float>(left + (right - left) * progress);
    glow_brush_->SetOpacity(0.22f);
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb_x, y), 10.0f, 10.0f), glow_brush_.Get());
    glow_brush_->SetOpacity(1.0f);
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb_x, y), 7.0f, 7.0f), text_brush_.Get());
    end_dc_draw();
}

void SettingsWindow::save_current_slot() {
    if (loading_ || selected_slot_ < 0 || selected_slot_ >= smk::core::kExtendedSlotCount) return;
    auto& slot = settings_.wheel.extended_wheel.slots[static_cast<std::size_t>(selected_slot_)];
    wchar_t text[2048]{};
    GetWindowTextW(slot_name_, text, static_cast<int>(std::size(text))); slot.name = text;
    slot.mode = SendMessageW(slot_mode_, CB_GETCURSEL, 0, 0) == 0 ? L"hotkey" : L"shortcut";
    slot.second_trigger_behavior = SendMessageW(slot_behavior_, CB_GETCURSEL, 0, 0) == 1 ? L"close" : L"minimize";
    GetWindowTextW(browser_url_, text, static_cast<int>(std::size(text))); slot.browser_launch_url = text;
    slot.enabled = slot.mode == L"hotkey" ? !slot.hotkey.empty() : !slot.shortcut_path.empty();
}

void SettingsWindow::load_slot(int index) {
    loading_ = true; cancel_hotkey_recording();
    hotkey_recording_error_.clear();
    shortcut_drop_state_ = smk::windows::ShortcutDropVisualState::idle;
    selected_slot_ = std::clamp(index, 0, smk::core::kExtendedSlotCount - 1);
    const auto& slot = settings_.wheel.extended_wheel.slots[static_cast<std::size_t>(selected_slot_)];
    SetWindowTextW(slot_title_, (L"槽位 " + smk::core::extended_slot_position_label(selected_slot_)).c_str());
    SetWindowTextW(slot_name_, slot.name.c_str()); SetWindowTextW(browser_url_, slot.browser_launch_url.c_str());
    SendMessageW(slot_mode_, CB_SETCURSEL, slot.mode == L"shortcut" ? 1 : 0, 0);
    SendMessageW(slot_behavior_, CB_SETCURSEL, slot.second_trigger_behavior == L"close" ? 1 : 0, 0);
    loading_ = false; update_slot_editor();
}

void SettingsWindow::update_slot_editor() {
    const auto& slot = settings_.wheel.extended_wheel.slots[static_cast<std::size_t>(selected_slot_)];
    const bool hotkey = SendMessageW(slot_mode_, CB_GETCURSEL, 0, 0) == 0;
    if (hotkey) {
        update_shortcut_icon({});
        const auto recorded = hotkey_recorder_.display_text();
        SetWindowTextW(slot_action_, hotkey_recorder_.recording() ? L"结束录制" : slot.hotkey.empty() ? L"录制" : L"清除");
        SetWindowTextW(slot_value_, hotkey_recorder_.recording()
            ? (recorded.empty() ? L"录制中，请逐次按下所需键位" : recorded.c_str())
            : !hotkey_recording_error_.empty() ? hotkey_recording_error_.c_str()
            : slot.hotkey.empty() ? L"键位显示" : slot.hotkey.c_str());
    } else {
        update_shortcut_icon(slot.shortcut_path);
        SetWindowTextW(slot_action_, slot.shortcut_path.empty() ? L"选择" : L"清除");
        SetWindowTextW(slot_value_, slot.shortcut_path.empty()
            ? smk::windows::is_administrator() ? L"点击打开普通权限拖放窗口，或点“选择”" : L"拖入程序、文件、文件夹或 .lnk，或点“选择”"
            : std::filesystem::path(slot.shortcut_path).stem().c_str());
    }
    ShowWindow(slot_behavior_label_, hotkey ? SW_HIDE : SW_SHOW); ShowWindow(slot_behavior_, hotkey ? SW_HIDE : SW_SHOW);
    const bool browser = !hotkey && is_browser_shortcut(slot.shortcut_path);
    ShowWindow(browser_url_label_, browser ? SW_SHOW : SW_HIDE); ShowWindow(browser_url_, browser ? SW_SHOW : SW_HIDE);
    reposition_page_controls(1);
    InvalidateRect(slot_action_, nullptr, FALSE); InvalidateRect(slot_value_, nullptr, FALSE); InvalidateRect(preview_, nullptr, FALSE);
}

void SettingsWindow::update_shortcut_icon(const std::wstring& path) {
    if (slot_icon_handle_) { DestroyIcon(slot_icon_handle_); slot_icon_handle_ = nullptr; }
    if (!path.empty()) slot_icon_handle_ = smk::windows::resolve_shortcut_icon(path, px(36.0, dpi_));
    InvalidateRect(slot_value_, nullptr, FALSE);
}

void SettingsWindow::begin_or_clear_hotkey() {
    auto& slot = settings_.wheel.extended_wheel.slots[static_cast<std::size_t>(selected_slot_)];
    if (hotkey_recorder_.recording()) finish_hotkey_recording();
    else if (!slot.hotkey.empty()) {
        slot.hotkey.clear();
        hotkey_recording_error_.clear();
    } else {
        (void)start_hotkey_recording();
    }
    update_slot_editor();
}

bool SettingsWindow::start_hotkey_recording() {
    DWORD error = ERROR_SUCCESS;
    hotkey_recording_error_.clear();
    if (!hotkey_capture_.start(window_, kHotkeyCapturedMessage, &error)) {
        hotkey_recorder_.cancel();
        hotkey_recording_error_ = L"无法开始录制（错误 " + std::to_wstring(error) + L"）";
        SMK_DIAGNOSTIC_EVENT("settings.hotkey_capture", std::format(
            L"state=start_failed error={}", error));
        return false;
    }
    hotkey_recorder_.begin();
    SetFocus(window_);
    SMK_DIAGNOSTIC_EVENT("settings.hotkey_capture", L"state=started");
    return true;
}

void SettingsWindow::cancel_hotkey_recording() {
    const bool was_recording = hotkey_recorder_.recording() || hotkey_capture_.active();
    const auto key_count = hotkey_recorder_.keys().size();
    hotkey_capture_.stop();
    hotkey_recorder_.cancel();
    if (was_recording) SMK_DIAGNOSTIC_EVENT("settings.hotkey_capture", std::format(
        L"state=cancelled key_count={}", key_count));
    (void)key_count;
}

void SettingsWindow::finish_hotkey_recording() {
    if (!hotkey_recorder_.recording()) return;
    const auto key_count = hotkey_recorder_.keys().size();
    auto& slot = settings_.wheel.extended_wheel.slots[static_cast<std::size_t>(selected_slot_)];
    slot.hotkey = hotkey_recorder_.finish();
    hotkey_capture_.stop();
    hotkey_recording_error_.clear();
    SMK_DIAGNOSTIC_EVENT("settings.hotkey_capture", std::format(
        L"state=finished key_count={} committed={}", key_count, !slot.hotkey.empty()));
    (void)key_count;
}

void SettingsWindow::record_hotkey(WPARAM key, LPARAM key_data) {
    if (hotkey_recorder_.add(static_cast<UINT>(key), key_data)) update_slot_editor();
}

void SettingsWindow::choose_shortcut() {
    auto& slot = settings_.wheel.extended_wheel.slots[static_cast<std::size_t>(selected_slot_)];
    if (!slot.shortcut_path.empty()) { slot.shortcut_path.clear(); slot.browser_launch_url.clear(); update_slot_editor(); return; }
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.GetAddressOf())))) return;
    COMDLG_FILTERSPEC filters[]{{L"Windows 快捷方式", L"*.lnk"}};
    dialog->SetFileTypes(1, filters); dialog->SetTitle(L"选择应用快捷方式");
    if (FAILED(dialog->Show(window_))) return;
    Microsoft::WRL::ComPtr<IShellItem> item; PWSTR path = nullptr;
    if (SUCCEEDED(dialog->GetResult(item.GetAddressOf())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        accept_shortcut_path(path); CoTaskMemFree(path);
    }
}

void SettingsWindow::accept_shortcut_path(const std::wstring& path) {
    if (!smk::windows::is_valid_shortcut_drop_path(path)) return;
    auto accepted_path = path;
    const auto source = std::filesystem::path(path);
    const auto extension = source.extension().wstring();
    std::error_code path_error;
    const bool external_shortcut = _wcsicmp(extension.c_str(), L".lnk") == 0
        && std::filesystem::is_regular_file(source, path_error) && !path_error;
    if (!external_shortcut) {
        std::wstring error;
        const auto candidate = managed_shortcuts_.create_candidate(path, selected_slot_, error);
        if (!candidate) {
            MessageBoxW(window_, error.c_str(), L"超级中键", MB_OK | MB_ICONWARNING);
            shortcut_drop_state_ = smk::windows::ShortcutDropVisualState::reject;
            InvalidateRect(slot_value_, nullptr, FALSE);
            return;
        }
        accepted_path = *candidate;
        managed_shortcut_candidates_.push_back(accepted_path);
    }
    settings_.wheel.extended_wheel.slots[static_cast<std::size_t>(selected_slot_)].shortcut_path = accepted_path;
    update_slot_editor();
}

void SettingsWindow::discard_managed_shortcut_candidates() noexcept {
    for (const auto& path : managed_shortcut_candidates_) managed_shortcuts_.discard(path);
    managed_shortcut_candidates_.clear();
}

void SettingsWindow::start_shortcut_drop_handoff() {
    const auto& slot = settings_.wheel.extended_wheel.slots[static_cast<std::size_t>(selected_slot_)];
    if (!smk::windows::is_administrator() || SendMessageW(slot_mode_, CB_GETCURSEL, 0, 0) != 1 || !slot.shortcut_path.empty()) return;
    GUID guid{}; if (FAILED(CoCreateGuid(&guid))) return;
    wchar_t value[40]{};
    if (StringFromGUID2(guid, value, static_cast<int>(std::size(value))) == 0) return;
    pending_handoff_id_ = value;
    std::erase_if(pending_handoff_id_, [](wchar_t ch) { return ch == L'{' || ch == L'}' || ch == L'-'; });
    std::wstring error;
    if (!smk::windows::launch_shortcut_drop_helper(pending_handoff_id_, error)) {
        pending_handoff_id_.clear(); MessageBoxW(window_, error.c_str(), L"超级中键", MB_OK | MB_ICONWARNING); return;
    }
    handoff_deadline_ = GetTickCount64() + 180'000;
    SetWindowTextW(slot_value_, L"等待普通权限拖放窗口…"); SetTimer(window_, kHandoffTimer, 200, nullptr);
}

void SettingsWindow::poll_shortcut_drop_handoff() {
    if (pending_handoff_id_.empty()) { KillTimer(window_, kHandoffTimer); return; }
    if (const auto result = smk::windows::read_shortcut_drop_result(pending_handoff_id_)) {
        smk::windows::delete_shortcut_drop_result(pending_handoff_id_);
        pending_handoff_id_.clear(); KillTimer(window_, kHandoffTimer);
        if (!result->empty()) accept_shortcut_path(*result); else update_slot_editor(); return;
    }
    if (GetTickCount64() >= handoff_deadline_) {
        smk::windows::delete_shortcut_drop_result(pending_handoff_id_);
        pending_handoff_id_.clear(); KillTimer(window_, kHandoffTimer); update_slot_editor();
    }
}

void SettingsWindow::paint_window() {
    PAINTSTRUCT paint{}; BeginPaint(window_, &paint);
    RECT bounds{}; GetClientRect(window_, &bounds);
    ID2D1DCRenderTarget* target = nullptr;
    if (begin_dc_draw(paint.hdc, bounds, &target)) {
        target->PushAxisAlignedClip(paint_clip_rect(paint.rcPaint, dpi_), D2D1_ANTIALIAS_MODE_ALIASED);
        const double width = dip(bounds.right, dpi_), height = dip(bounds.bottom, dpi_);
        target->Clear(color(kBackground));
        target->FillRectangle(rect_f(chrome_.title_bar), top_brush_.Get());
        target->FillRectangle(rect_f(chrome_.tab_bar), top_brush_.Get());
        target->FillRectangle(rect_f(chrome_.footer), background_brush_.Get());
        target->DrawLine(D2D1::Point2F(0, 44), D2D1::Point2F(static_cast<float>(width), 44), border_brush_.Get(), 1.0f);
        target->DrawLine(D2D1::Point2F(0, 96), D2D1::Point2F(static_cast<float>(width), 96), border_brush_.Get(), 1.0f);
        target->DrawLine(D2D1::Point2F(0, static_cast<float>(chrome_.footer.y)),
            D2D1::Point2F(static_cast<float>(width), static_cast<float>(chrome_.footer.y)), border_brush_.Get(), 1.0f);
        draw_text(target, title_format_.Get(), text_brush_.Get(), L"超级中键设置", {48, 0, 300, 44});
        const UiRect caption_buttons[]{chrome_.minimize_button, chrome_.maximize_button, chrome_.close_caption_button};
        for (int index = 0; index < 3; ++index) {
            if (hovered_chrome_item_ == index) {
                auto* brush = index == 2 ? red_brush_.Get() : control_brush_.Get();
                target->FillRectangle(rect_f(caption_buttons[index]), brush);
            }
        }
        const float min_cx = static_cast<float>(chrome_.minimize_button.x + 23.0);
        target->DrawLine(D2D1::Point2F(min_cx - 6, 23), D2D1::Point2F(min_cx + 6, 23), text_brush_.Get(), 1.2f);
        const float max_cx = static_cast<float>(chrome_.maximize_button.x + 23.0);
        if (IsZoomed(window_)) {
            target->DrawRectangle(D2D1::RectF(max_cx - 5, 16, max_cx + 5, 26), text_brush_.Get(), 1.1f);
            target->DrawRectangle(D2D1::RectF(max_cx - 3, 14, max_cx + 7, 24), text_brush_.Get(), 1.1f);
        } else target->DrawRectangle(D2D1::RectF(max_cx - 6, 15, max_cx + 6, 27), text_brush_.Get(), 1.2f);
        const float close_cx = static_cast<float>(chrome_.close_caption_button.x + 23.0);
        target->DrawLine(D2D1::Point2F(close_cx - 6, 16), D2D1::Point2F(close_cx + 6, 28), text_brush_.Get(), 1.3f);
        target->DrawLine(D2D1::Point2F(close_cx + 6, 16), D2D1::Point2F(close_cx - 6, 28), text_brush_.Get(), 1.3f);
        (void)height;
        target->PopAxisAlignedClip();
        end_dc_draw();
        paint_caption_icon(paint.hdc);
    }
    EndPaint(window_, &paint);
}

void SettingsWindow::paint_page(HWND page, int page_index) {
    PAINTSTRUCT paint{}; BeginPaint(page, &paint);
    RECT bounds{}; GetClientRect(page, &bounds);
    ID2D1DCRenderTarget* target = nullptr;
    if (begin_dc_draw(paint.hdc, bounds, &target)) {
        target->PushAxisAlignedClip(paint_clip_rect(paint.rcPaint, dpi_), D2D1_ANTIALIAS_MODE_ALIASED);
        const double width = dip(bounds.right, dpi_), height = dip(bounds.bottom, dpi_);
        target->Clear(color(kBackground));
        const double scroll = page_scroll_[static_cast<std::size_t>(page_index)];
        if (page_index == 0) paint_basic_page(target, make_basic_page_layout(width), scroll);
        else if (page_index == 1) paint_wheel_page(target, make_wheel_page_layout(width, height), scroll);
        else paint_about_page(target, make_about_page_layout(width, height,
            about_update_presentation()), scroll);
        paint_scrollbar(target, page_index, width, height);
        target->PopAxisAlignedClip();
        end_dc_draw();
    }
    EndPaint(page, &paint);
}

void SettingsWindow::paint_basic_page(ID2D1RenderTarget* target, const BasicPageLayout& layout, double scroll) {
    target->FillRoundedRectangle(rounded(layout.card, 8, scroll), card_brush_.Get());
    target->DrawRoundedRectangle(rounded(layout.card, 8, scroll), border_brush_.Get(), 1.0f);
    for (double y : {96.0, 164.0, 360.0})
        target->DrawLine(D2D1::Point2F(0, static_cast<float>(y - scroll)),
            D2D1::Point2F(static_cast<float>(layout.card.width), static_cast<float>(y - scroll)), border_brush_.Get(), 1.0f);
    draw_text(target, section_format_.Get(), text_brush_.Get(), L"布局形状", shifted({28, 10, 240, 30}, scroll));
    draw_text(target, small_format_.Get(), secondary_text_brush_.Get(),
        L"圆形 4/6/8（默认6），矩形 4/8（默认8），矩形8区为九宫格镂空中心",
        shifted({28, 70, layout.card.width - 56, 24}, scroll));
    draw_text(target, body_format_.Get(), text_brush_.Get(), L"扇区数量（等级制）", shifted({28, 96, 260, 24}, scroll));
    const wchar_t* slider_labels[]{L"轮盘大小", L"中心死区半径", L"透明度"};
    for (std::size_t index = 0; index < 3; ++index)
        draw_text(target, body_format_.Get(), text_brush_.Get(), slider_labels[index],
            shifted({28, 178.0 + index * 62.0, 118, 42}, scroll));
    const wchar_t* toggle_texts[]{
        L"快捷复制：最后一个扇区执行 Ctrl+C",
        L"捕获图片：复制图片后在轮盘中显示缩略图",
        L"表格文本清洗：以文本方式粘贴时去除空格和引号",
        L"开机自启动：登录 Windows 后自动运行",
        L"管理员模式运行",
    };
    const IconKind icons[]{IconKind::link, IconKind::image, IconKind::refresh, IconKind::power, IconKind::user};
    for (std::size_t index = 0; index < 5; ++index) {
        const double y = 366.0 + index * 34.0 - scroll;
        if (index > 0) target->DrawLine(D2D1::Point2F(20, static_cast<float>(y - 5)),
            D2D1::Point2F(static_cast<float>(layout.card.width - 20), static_cast<float>(y - 5)), border_brush_.Get(), 0.7f);
        target->DrawRoundedRectangle(rounded({24, y + 1, 32, 32}, 7), border_brush_.Get(), 1.0f);
        draw_line_icon(target, icons[index], {31, y + 8, 18, 18}, index >= 3 ? red_brush_.Get() : glow_brush_.Get());
        draw_text(target, body_format_.Get(), text_brush_.Get(), toggle_texts[index], {68, y, layout.card.width - 174, 34});
    }
}

void SettingsWindow::paint_wheel_page(ID2D1RenderTarget* target, const WheelPageLayout& layout, double scroll) {
    target->FillRoundedRectangle(rounded(layout.top_card, 8, scroll), card_brush_.Get());
    target->DrawRoundedRectangle(rounded(layout.top_card, 8, scroll), border_brush_.Get(), 1.0f);
    draw_text(target, section_format_.Get(), text_brush_.Get(), L"突破轮盘", shifted({28, 0, 110, 64}, scroll));
    draw_text(target, body_format_.Get(), text_brush_.Get(), L"边界缓冲区", shifted({layout.breakout_slider.x - 116, 0, 112, 64}, scroll), DWRITE_TEXT_ALIGNMENT_TRAILING);

    target->FillRoundedRectangle(rounded(layout.preview_card, 8, scroll), card_brush_.Get());
    target->DrawRoundedRectangle(rounded(layout.preview_card, 8, scroll), border_brush_.Get(), 1.0f);
    draw_text(target, section_format_.Get(), text_brush_.Get(), L"扩展动作槽位", shifted({28, layout.preview_card.y + 6, 220, 28}, scroll));
    draw_text(target, small_format_.Get(), secondary_text_brush_.Get(),
        L"预览始终展示12槽；运行时仅显示鼠标当前方向的3槽。",
        shifted({20, layout.preview_card.bottom() - 31, layout.preview_card.width - 40, 24}, scroll), DWRITE_TEXT_ALIGNMENT_CENTER);

    UiRect editor = layout.editor_card;
    const bool shortcut = SendMessageW(slot_mode_, CB_GETCURSEL, 0, 0) == 1;
    editor.height = shortcut ? 264.0 : 166.0;
    target->FillRoundedRectangle(rounded(editor, 8, scroll), card_brush_.Get());
    target->DrawRoundedRectangle(rounded(editor, 8, scroll), border_brush_.Get(), 1.0f);
    const auto draw_edit_frame = [&](const UiRect& bounds, HWND edit) {
        target->FillRoundedRectangle(rounded(bounds, 6, scroll), control_brush_.Get());
        target->DrawRoundedRectangle(rounded(bounds, 6, scroll),
            GetFocus() == edit ? glow_brush_.Get() : border_brush_.Get(),
            GetFocus() == edit ? 1.35f : 1.0f);
    };
    draw_edit_frame(layout.slot_name, slot_name_);
    if (shortcut) {
        draw_text(target, body_format_.Get(), text_brush_.Get(), L"应用第二次触发", shifted({28, editor.y + 164, 180, 40}, scroll));
        if (IsWindowVisible(browser_url_)) {
            draw_edit_frame(layout.browser_url, browser_url_);
            draw_text(target, body_format_.Get(), text_brush_.Get(), L"浏览器启动网址", shifted({28, editor.y + 216, 180, 40}, scroll));
        }
    }
}

void SettingsWindow::paint_about_page(ID2D1RenderTarget* target, const AboutPageLayout& layout, double scroll) {
    target->FillRoundedRectangle(rounded(layout.update_card, 8, scroll), card_brush_.Get());
    target->DrawRoundedRectangle(rounded(layout.update_card, 8, scroll), border_brush_.Get(), 1.0f);
    draw_line_icon(target, IconKind::refresh, shifted({28, 18, 24, 24}, scroll), glow_brush_.Get());
    draw_text(target, section_format_.Get(), text_brush_.Get(), L"更新与下载",
        shifted({62, 8, 240, 44}, scroll));
    if (!update_state_.version.empty()) {
        draw_text(target, small_format_.Get(), secondary_text_brush_.Get(), L"目标版本  " + update_state_.version,
            shifted({layout.update_card.width - 248, 10, 220, 40}, scroll), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    small_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    draw_text(target, small_format_.Get(), secondary_text_brush_.Get(), update_state_.status,
        shifted(layout.update_status, scroll), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    small_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    if (IsWindowVisible(about_acceleration_)) {
        draw_text(target, small_format_.Get(), secondary_text_brush_.Get(), L"使用加速节点",
            shifted({layout.acceleration.x - 116.0, layout.acceleration.y - 4.0, 106.0, 34.0}, scroll),
            DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    if (update_state_.total > 0 && (update_state_.state == smk::updater::UpdateState::downloading ||
            update_state_.state == smk::updater::UpdateState::paused || update_state_.state == smk::updater::UpdateState::completed)) {
        target->FillRoundedRectangle(rounded(layout.progress, 5, scroll), border_brush_.Get());
        auto fill = layout.progress;
        const double ratio = std::clamp(static_cast<double>(update_state_.received) / update_state_.total, 0.0, 1.0);
        fill.width *= ratio;
        target->FillRoundedRectangle(rounded(fill, 5, scroll), accent_brush_.Get());
        const auto left = std::format(L"{:.0f}%  ·  {} / {}", ratio * 100.0,
            format_byte_count(static_cast<double>(update_state_.received)),
            format_byte_count(static_cast<double>(update_state_.total)));
        const auto right = std::format(L"{}/s  ·  {}  ·  {} 路{}",
            format_byte_count(update_state_.bytes_per_second),
            update_state_.node.empty() ? L"正在选择节点" : update_state_.node,
            update_state_.connections,
            update_state_.parallel_fallback ? L"  ·  已回退单路" : L"");
        auto summary_left = layout.progress_summary;
        summary_left.width *= 0.46;
        auto summary_right = layout.progress_summary;
        summary_right.x = summary_left.right();
        summary_right.width -= summary_left.width;
        draw_text(target, small_format_.Get(), secondary_text_brush_.Get(), left,
            shifted(summary_left, scroll));
        draw_text(target, small_format_.Get(), secondary_text_brush_.Get(), right,
            shifted(summary_right, scroll), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    if (layout.release_notes.width > 0.0) {
        update_release_notes_metrics(layout);
        target->FillRoundedRectangle(rounded(layout.release_notes_card, 7, scroll), control_brush_.Get());
        target->DrawRoundedRectangle(rounded(layout.release_notes_card, 7, scroll), border_brush_.Get(), 1.0f);
        draw_text(target, body_format_.Get(), text_brush_.Get(), L"更新日志",
            shifted(layout.release_notes_title, scroll));
        const auto viewport = shifted(layout.release_notes, scroll);
        target->PushAxisAlignedClip(rect_f(viewport), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const auto notes = release_notes_text();
        Microsoft::WRL::ComPtr<IDWriteTextLayout> text_layout;
        if (SUCCEEDED(dwrite_factory_->CreateTextLayout(notes.c_str(), static_cast<UINT32>(notes.size()),
                small_format_.Get(), static_cast<float>(layout.release_notes.width),
                static_cast<float>(std::max(layout.release_notes.height, release_notes_extent_)),
                text_layout.GetAddressOf()))) {
            text_layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            text_layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            text_layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            target->DrawTextLayout(D2D1::Point2F(static_cast<float>(viewport.x),
                static_cast<float>(viewport.y - release_notes_scroll_)), text_layout.Get(),
                secondary_text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        target->PopAxisAlignedClip();

        const double maximum = std::max(0.0, release_notes_extent_ - layout.release_notes.height);
        if (maximum > 0.5) {
            const auto track = shifted(layout.release_notes_scroll_track, scroll);
            const double thumb_height = std::max(28.0,
                track.height * layout.release_notes.height / release_notes_extent_);
            const double thumb_y = track.y + (track.height - thumb_height) * release_notes_scroll_ / maximum;
            border_brush_->SetOpacity(0.55f);
            target->FillRoundedRectangle(rounded(track, 2), border_brush_.Get());
            border_brush_->SetOpacity(1.0f);
            target->FillRoundedRectangle(rounded({track.x - 1.0, thumb_y, 6.0, thumb_height}, 3),
                secondary_text_brush_.Get());
        }
    }

    target->FillRoundedRectangle(rounded(layout.product_card, 8, scroll), card_brush_.Get());
    target->DrawRoundedRectangle(rounded(layout.product_card, 8, scroll), border_brush_.Get(), 1.0f);
    const double product_y = layout.product_card.y;
    glow_brush_->SetOpacity(0.20f);
    draw_text(target, hero_format_.Get(), glow_brush_.Get(), L"超级中键", shifted({30, product_y + 14, 360, 54}, scroll));
    glow_brush_->SetOpacity(1.0f);
    draw_text(target, hero_format_.Get(), text_brush_.Get(), L"超级中键", shifted({28, product_y + 12, 360, 54}, scroll));
    draw_text(target, section_format_.Get(), secondary_text_brush_.Get(),
        L"轻量级 Windows 剪贴板轮盘与效率工具。", shifted({30, product_y + 60, layout.product_card.width - 60, 30}, scroll));
    target->DrawLine(D2D1::Point2F(28, static_cast<float>(product_y + 94 - scroll)),
        D2D1::Point2F(static_cast<float>(layout.product_card.width - 28), static_cast<float>(product_y + 94 - scroll)),
        border_brush_.Get(), 1.0f);
    draw_line_icon(target, IconKind::user, shifted({32, product_y + 106, 22, 22}, scroll), glow_brush_.Get());
    draw_text(target, body_format_.Get(), text_brush_.Get(), L"开发者", shifted({68, product_y + 96, 104, 42}, scroll));
    draw_text(target, body_format_.Get(), secondary_text_brush_.Get(), L"wet86y", shifted({184, product_y + 96, 240, 42}, scroll));
    draw_line_icon(target, IconKind::repository, shifted({32, product_y + 150, 22, 22}, scroll), glow_brush_.Get());
    draw_text(target, body_format_.Get(), text_brush_.Get(), L"GitHub 历史", shifted({68, product_y + 138, 110, 46}, scroll));
    draw_line_icon(target, IconKind::info, shifted({32, product_y + 194, 22, 22}, scroll), glow_brush_.Get());
    draw_text(target, body_format_.Get(), text_brush_.Get(), L"当前版本", shifted({68, product_y + 182, 110, 46}, scroll));
    draw_text(target, body_format_.Get(), secondary_text_brush_.Get(), version_text_, shifted({184, product_y + 182, 420, 46}, scroll));
}

AboutUpdatePresentation SettingsWindow::about_update_presentation() const noexcept {
    using smk::updater::UpdateState;
    switch (update_state_.state) {
    case UpdateState::available: return AboutUpdatePresentation::release;
    case UpdateState::downloading:
    case UpdateState::paused: return AboutUpdatePresentation::transfer;
    case UpdateState::completed: return AboutUpdatePresentation::completed;
    case UpdateState::launching: return AboutUpdatePresentation::launching;
    case UpdateState::failed:
    case UpdateState::cancelled:
        return update_state_.version.empty() ? AboutUpdatePresentation::compact
                                             : AboutUpdatePresentation::release;
    default: return AboutUpdatePresentation::compact;
    }
}

std::wstring SettingsWindow::release_notes_text() const {
    if (!update_state_.release_notes.empty()) return update_state_.release_notes;
    return update_state_.version.empty() ? std::wstring{} : L"此版本没有附加更新说明。";
}

void SettingsWindow::update_release_notes_metrics(const AboutPageLayout& layout) {
    if (layout.release_notes.width <= 0.0 || !dwrite_factory_ || !ensure_render_resources()) {
        release_notes_extent_ = 0.0;
        release_notes_scroll_ = 0.0;
        return;
    }
    const auto notes = release_notes_text();
    Microsoft::WRL::ComPtr<IDWriteTextLayout> text_layout;
    double extent = layout.release_notes.height;
    if (!notes.empty() && SUCCEEDED(dwrite_factory_->CreateTextLayout(notes.c_str(),
            static_cast<UINT32>(notes.size()), small_format_.Get(),
            static_cast<float>(layout.release_notes.width), 32768.0f, text_layout.GetAddressOf()))) {
        text_layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        text_layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(text_layout->GetMetrics(&metrics)))
            extent = std::max(extent, std::ceil(static_cast<double>(metrics.height)));
    }
    release_notes_extent_ = extent;
    release_notes_scroll_ = std::clamp(release_notes_scroll_, 0.0,
        std::max(0.0, release_notes_extent_ - layout.release_notes.height));
}

bool SettingsWindow::scroll_release_notes(double delta) {
    const auto layout = make_about_page_layout(chrome_.page_viewport.width,
        chrome_.page_viewport.height, about_update_presentation());
    update_release_notes_metrics(layout);
    const double maximum = std::max(0.0, release_notes_extent_ - layout.release_notes.height);
    const double next = std::clamp(release_notes_scroll_ + delta, 0.0, maximum);
    if (std::abs(next - release_notes_scroll_) < 0.1) return false;
    release_notes_scroll_ = next;
    InvalidateRect(pages_[2], nullptr, FALSE);
    return true;
}

void SettingsWindow::apply_update_state(const smk::updater::UpdateViewState& state) {
    if (release_notes_dragging_) {
        release_notes_dragging_ = false;
        if (GetCapture() == pages_[2]) ReleaseCapture();
    }
    if (state.version != update_state_.version || state.release_notes != update_state_.release_notes)
        release_notes_scroll_ = 0.0;
    update_state_ = state;
    Button_SetCheck(about_acceleration_, state.acceleration ? BST_CHECKED : BST_UNCHECKED);
    set_switch_value(about_acceleration_, state.acceleration);
    refresh_update_controls();
    reposition_page_controls(2);
}

void SettingsWindow::refresh_update_controls() {
    if (!about_check_update_) return;
    using smk::updater::UpdateState;
    const bool enabled = update_state_.state != UpdateState::disabled;
    const bool busy = update_state_.state == UpdateState::downloading || update_state_.state == UpdateState::paused;
    const bool has_release = !update_state_.version.empty();
    const bool available = update_state_.state == UpdateState::available
        || ((update_state_.state == UpdateState::failed || update_state_.state == UpdateState::cancelled) && has_release);
    const bool completed = update_state_.state == UpdateState::completed;
    const bool checking = update_state_.state == UpdateState::checking;
    const bool launching = update_state_.state == UpdateState::launching;
    ShowWindow(about_check_update_, busy || launching ? SW_HIDE : SW_SHOW);
    EnableWindow(about_check_update_, enabled && !checking);
    SetWindowTextW(about_check_update_, checking ? L"正在检查…" : L"检查更新");
    ShowWindow(about_install_update_, available || completed ? SW_SHOW : SW_HIDE);
    EnableWindow(about_install_update_, enabled && (available || completed));
    SetWindowTextW(about_install_update_, completed ? L"立即安装"
        : (update_state_.state == UpdateState::failed || update_state_.state == UpdateState::cancelled)
            ? L"重新下载" : L"下载更新");
    for (auto control : {about_pause_resume_, about_background_, about_cancel_}) ShowWindow(control, busy ? SW_SHOW : SW_HIDE);
    ShowWindow(about_switch_node_, busy && update_state_.acceleration ? SW_SHOW : SW_HIDE);
    SetWindowTextW(about_pause_resume_, update_state_.state == UpdateState::paused ? L"继续下载" : L"暂停下载");
    EnableWindow(about_pause_resume_, enabled && busy);
    EnableWindow(about_background_, enabled && busy);
    EnableWindow(about_switch_node_, enabled && busy && update_state_.acceleration);
    EnableWindow(about_cancel_, enabled && busy);
    ShowWindow(about_acceleration_, available ? SW_SHOW : SW_HIDE);
    EnableWindow(about_acceleration_, enabled && !checking);
}

void SettingsWindow::paint_scrollbar(ID2D1RenderTarget* target, int page_index, double width, double height) {
    const double content = page_content_height(page_index);
    if (content <= height + 0.5) return;
    const double thumb_height = std::max(36.0, height * height / content);
    const double maximum = content - height;
    const double y = (height - thumb_height) * page_scroll_[static_cast<std::size_t>(page_index)] / maximum;
    border_brush_->SetOpacity(0.55f);
    target->FillRoundedRectangle(rounded({width - 7, 4, 4, height - 8}, 2), border_brush_.Get());
    border_brush_->SetOpacity(1.0f);
    target->FillRoundedRectangle(rounded({width - 8, y, 6, thumb_height}, 3), secondary_text_brush_.Get());
}

void SettingsWindow::update_preview_visual_layout() {
    if (!preview_) return;
    RECT bounds{};
    GetClientRect(preview_, &bounds);
    const double width = dip(bounds.right, dpi_), height = dip(bounds.bottom, dpi_);
    if (width <= 1.0 || height <= 1.0) return;
    const double configured_radius = std::max(100.0,
        static_cast<double>(SendMessageW(radius_, TBM_GETPOS, 0, 0)));
    const double ring_thickness = smk::core::extended_ring_thickness(configured_radius);
    preview_surface_center_ = {width / 2.0, height / 2.0};
    // Reuse runtime geometry, but fit the managed settings preview against the
    // unselected outer radius. Content is budgeted after this geometry scale.
    preview_visual_layout_ = smk::core::make_extended_wheel_visual_layout({0.0, 0.0},
        configured_radius, smk::core::kExtendedRingGap, ring_thickness,
        settings_.wheel.sector_gap_degrees, kCircleCornerRadius, 1.0, 1.0, 0.0);
    preview_scale_ = std::max(0.01, (std::min(width, height) / 2.0 - 16.0)
        / std::max(1.0, preview_visual_layout_.ring_outer_radius));
}

void SettingsWindow::paint_preview() {
    PAINTSTRUCT paint{}; BeginPaint(preview_, &paint);
    RECT bounds{}; GetClientRect(preview_, &bounds);
    ID2D1DCRenderTarget* target = nullptr;
    std::vector<std::tuple<HICON, double, double, double>> icons;
    if (begin_dc_draw(paint.hdc, bounds, &target)) {
        const auto dpi_transform = D2D1::Matrix3x2F::Scale(
            D2D1::SizeF(dpi_ / 96.0f, dpi_ / 96.0f), D2D1::Point2F(0, 0));
        target->Clear(color(0x0A1629));
        update_preview_visual_layout();
        const smk::core::VisualPoint center = preview_surface_center_;
        const double configured_radius = std::max(80.0, static_cast<double>(SendMessageW(radius_, TBM_GETPOS, 0, 0)));
        const double configured_dead = std::clamp(static_cast<double>(SendMessageW(dead_zone_, TBM_GETPOS, 0, 0)), 0.0, configured_radius - 20.0);
        const auto logical_to_preview = D2D1::Matrix3x2F::Scale(
            D2D1::SizeF(static_cast<float>(preview_scale_), static_cast<float>(preview_scale_)))
            * D2D1::Matrix3x2F::Translation(static_cast<float>(center.x), static_cast<float>(center.y))
            * dpi_transform;
        const double main_inner = configured_dead;
        const double main_outer = configured_radius;
        const bool rectangle = Button_GetCheck(shape_rectangle_) == BST_CHECKED;
        wchar_t sector_text[8]{}; GetWindowTextW(sectors_, sector_text, static_cast<int>(std::size(sector_text)));
        const int count = rectangle ? 6 : std::clamp(_wtoi(sector_text), 4, 8);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> main_brush, main_border_brush;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> configured_brush, empty_brush, center_brush;
        target->CreateSolidColorBrush(color(0x17233D, rectangle ? .46f : .96f), main_brush.GetAddressOf());
        target->CreateSolidColorBrush(color(0x496785, rectangle ? .28f : .62f), main_border_brush.GetAddressOf());
        target->CreateSolidColorBrush(color(0x233A56, .94f), configured_brush.GetAddressOf());
        target->CreateSolidColorBrush(color(0x17263A, .68f), empty_brush.GetAddressOf());
        target->CreateSolidColorBrush(color(0x06101F, .98f), center_brush.GetAddressOf());
        const double step = 360.0 / count;
        for (int index = 0; index < count; ++index) {
            const auto points = smk::core::make_circle_sector({0.0, 0.0}, main_inner, main_outer,
                -90.0 + index * step, -90.0 + (index + 1) * step, kCircleCornerRadius);
            const auto geometry = create_sector_path(d2d_factory_.Get(), points);
            if (!geometry) continue;
            target->SetTransform(D2D1::Matrix3x2F::Scale(
                D2D1::SizeF(.9f, .9f), d2d_point(points.centroid)) * logical_to_preview);
            target->FillGeometry(geometry.Get(), main_brush.Get());
            target->DrawGeometry(geometry.Get(), main_border_brush.Get(), .75f);
        }
        target->SetTransform(logical_to_preview);

        D2D1_GRADIENT_STOP gradient_stops[2]{{0, color(0x0B4FEA)}, {1, color(0x106CFF)}};
        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stops;
        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> selected_brush;
        if (SUCCEEDED(target->CreateGradientStopCollection(gradient_stops, 2, stops.GetAddressOf())))
            target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(static_cast<float>(-main_outer), static_cast<float>(-main_outer)),
                D2D1::Point2F(static_cast<float>(main_outer), static_cast<float>(main_outer))),
                stops.Get(), selected_brush.GetAddressOf());
        for (int index = 0; index < smk::core::kExtendedSlotCount; ++index) {
            const auto& visual = preview_visual_layout_.slots[static_cast<std::size_t>(index)];
            const auto geometry = create_sector_path(d2d_factory_.Get(), visual.sector);
            if (!geometry) continue;
            const auto& slot = settings_.wheel.extended_wheel.slots[static_cast<std::size_t>(index)];
            const bool selected = index == selected_slot_;
            target->SetTransform(logical_to_preview);
            ID2D1Brush* fill = selected && selected_brush ? static_cast<ID2D1Brush*>(selected_brush.Get())
                : slot.configured() ? static_cast<ID2D1Brush*>(configured_brush.Get()) : static_cast<ID2D1Brush*>(empty_brush.Get());
            target->FillGeometry(geometry.Get(), fill);
            if (!slot.configured()) continue;

            D2D1_RECT_F content_bounds{};
            const auto preview_only_scale = D2D1::Matrix3x2F::Scale(
                D2D1::SizeF(static_cast<float>(preview_scale_), static_cast<float>(preview_scale_)));
            if (FAILED(geometry->GetBounds(&preview_only_scale, &content_bounds))) continue;
            const double bounds_width = content_bounds.right - content_bounds.left;
            const double bounds_height = content_bounds.bottom - content_bounds.top;
            if (bounds_width < 28.0 || bounds_height < 18.0) continue;

            const double content_x = center.x + visual.content_center.x * preview_scale_;
            const double content_y = center.y + visual.content_center.y * preview_scale_;
            const bool shortcut = _wcsicmp(slot.mode.c_str(), L"shortcut") == 0;
            const double text_width = std::min(58.0, std::max(30.0, bounds_width * 0.64));
            const bool show_icon = shortcut && bounds_width >= 44.0 && bounds_height >= 36.0;
            const double font_size = show_icon
                ? std::clamp(bounds_height * 0.15, 6.5, 9.0)
                : std::clamp(bounds_height * 0.16, 7.0, 9.5);
            const double text_height = show_icon
                ? std::clamp(font_size + 6.0, 12.0, 17.0)
                : std::min(30.0, std::max(15.0, bounds_height * 0.36));
            double text_top = content_y - text_height / 2.0;
            if (show_icon) {
                if (preview_icon_paths_[static_cast<std::size_t>(index)] != slot.shortcut_path) {
                    if (preview_slot_icons_[static_cast<std::size_t>(index)]) DestroyIcon(preview_slot_icons_[static_cast<std::size_t>(index)]);
                    preview_slot_icons_[static_cast<std::size_t>(index)] = nullptr;
                    preview_icon_paths_[static_cast<std::size_t>(index)] = slot.shortcut_path;
                    preview_slot_icons_[static_cast<std::size_t>(index)] =
                        smk::windows::resolve_shortcut_icon(slot.shortcut_path, 40);
                }
                const double icon_size = std::clamp(
                    std::min(bounds_height * 0.34, bounds_width * 0.28), 12.0, 24.0);
                const double total_height = icon_size + text_height + 2.0;
                const double icon_top = content_y - total_height / 2.0;
                if (preview_slot_icons_[static_cast<std::size_t>(index)])
                    icons.emplace_back(preview_slot_icons_[static_cast<std::size_t>(index)],
                        content_x - icon_size / 2.0, icon_top, icon_size);
                text_top = icon_top + icon_size + 2.0;
            }
            auto name = slot.display_name();
            if (name.size() > 8) name.resize(8);
            Microsoft::WRL::ComPtr<IDWriteTextLayout> text_layout;
            if (SUCCEEDED(dwrite_factory_->CreateTextLayout(name.c_str(), static_cast<UINT32>(name.size()),
                    preview_format_.Get(), static_cast<float>(text_width),
                    static_cast<float>(text_height), text_layout.GetAddressOf()))) {
                const DWRITE_TEXT_RANGE range{0, static_cast<UINT32>(name.size())};
                text_layout->SetFontSize(static_cast<float>(font_size), range);
                text_layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, range);
                text_layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                text_layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                text_layout->SetWordWrapping(show_icon
                    ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);
                target->SetTransform(dpi_transform);
                const D2D1_POINT_2F origin{static_cast<float>(content_x - text_width / 2.0), static_cast<float>(text_top)};
                target->DrawTextLayout(origin, text_layout.Get(), text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }
        target->SetTransform(logical_to_preview);
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(0, 0), static_cast<float>(std::max(10.0, main_inner)),
            static_cast<float>(std::max(10.0, main_inner))), center_brush.Get());
        end_dc_draw();
        for (const auto& [icon, x, y, size] : icons)
            DrawIconEx(paint.hdc, px(x, dpi_), px(y, dpi_), icon, px(size, dpi_), px(size, dpi_), 0, nullptr, DI_NORMAL);
    }
    EndPaint(preview_, &paint);
}

void SettingsWindow::paint_caption_icon(HDC dc) {
    HICON icon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(101), IMAGE_ICON,
        px(20, dpi_), px(20, dpi_), LR_DEFAULTCOLOR));
    if (icon) {
        DrawIconEx(dc, px(18, dpi_), px(12, dpi_), icon, px(20, dpi_), px(20, dpi_), 0, nullptr, DI_NORMAL);
        DestroyIcon(icon);
    }
}

bool SettingsWindow::ensure_render_resources() {
    if (dc_target_) return true;
    D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    if (FAILED(d2d_factory_->CreateDCRenderTarget(&properties, dc_target_.GetAddressOf()))) return false;
    const auto text_format = [&](float size, DWRITE_FONT_WEIGHT weight, auto& output) {
        return SUCCEEDED(dwrite_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", output.GetAddressOf()));
    };
    if (!body_format_ && (!text_format(14, DWRITE_FONT_WEIGHT_NORMAL, body_format_)
        || !text_format(12, DWRITE_FONT_WEIGHT_NORMAL, small_format_)
        || !text_format(16, DWRITE_FONT_WEIGHT_SEMI_BOLD, section_format_)
        || !text_format(18, DWRITE_FONT_WEIGHT_SEMI_BOLD, title_format_)
        || !text_format(36, DWRITE_FONT_WEIGHT_BOLD, hero_format_)
        || !text_format(10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, preview_format_))) return false;
    for (auto* format : {body_format_.Get(), small_format_.Get(), section_format_.Get(), title_format_.Get(), hero_format_.Get(), preview_format_.Get()}) {
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    preview_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwrite_factory_->CreateEllipsisTrimmingSign(preview_format_.Get(), ellipsis.GetAddressOf())))
        preview_format_->SetTrimming(&trimming, ellipsis.Get());
    const auto brush = [&](unsigned value, auto& output) {
        return SUCCEEDED(dc_target_->CreateSolidColorBrush(color(value), output.GetAddressOf()));
    };
    return brush(kBackground, background_brush_) && brush(kTop, top_brush_)
        && brush(kCard, card_brush_) && brush(kControl, control_brush_)
        && brush(kBorder, border_brush_) && brush(kAccent, accent_brush_)
        && brush(kGlow, glow_brush_) && brush(kText, text_brush_)
        && brush(kSecondaryText, secondary_text_brush_) && brush(kRed, red_brush_)
        && brush(kDisabled, disabled_brush_);
}

bool SettingsWindow::begin_dc_draw(HDC dc, const RECT& bounds, ID2D1DCRenderTarget** target) {
    if (!dc || !target || !ensure_render_resources()) return false;
    if (FAILED(dc_target_->BindDC(dc, &bounds))) return false;
    dc_target_->BeginDraw();
    const float scale = dpi_ / 96.0f;
    dc_target_->SetTransform(D2D1::Matrix3x2F::Scale(D2D1::SizeF(scale, scale), D2D1::Point2F(0, 0)));
    *target = dc_target_.Get();
    return true;
}

void SettingsWindow::end_dc_draw() {
    if (!dc_target_) return;
    dc_target_->SetTransform(D2D1::Matrix3x2F::Identity());
    const HRESULT result = dc_target_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) discard_render_resources();
}

void SettingsWindow::discard_render_resources() {
    background_brush_.Reset(); top_brush_.Reset(); card_brush_.Reset(); control_brush_.Reset();
    border_brush_.Reset(); accent_brush_.Reset(); glow_brush_.Reset(); text_brush_.Reset();
    secondary_text_brush_.Reset(); red_brush_.Reset(); disabled_brush_.Reset(); dc_target_.Reset();
}

} // namespace smk::ui
