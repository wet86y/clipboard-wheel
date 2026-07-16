#include "ui/WheelWindow.h"

#include "core/ExtendedWheel.h"
#include "core/WheelLayout.h"
#include "core/WheelVisualGeometry.h"
#include "platform/windows/DiagnosticLog.h"
#include "platform/windows/ShortcutIconResolver.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <mmsystem.h>
#include <numeric>
#include <shellapi.h>
#include <utility>

namespace smk::ui {
namespace {
constexpr wchar_t kClassName[] = L"SuperMiddleKeyNativeWheel";
constexpr double kShowMs = 58.0;
constexpr double kHideMs = 60.0;
constexpr double kSelectionMs = 75.0;
constexpr double kExtendedVisibilityMs = 50.0;
constexpr double kExtendedMarqueeLeadInMs = 350.0;
constexpr double kExtendedMarqueeMinimumMs = 1800.0;
constexpr double kExtendedMarqueePixelsPerSecond = 28.0;
constexpr float kCircleCornerRadius = 10.0f;
constexpr float kRectangleCornerRadius = 8.0f;
constexpr float kImageClipInset = 10.0f;
constexpr float kRectangleCutoutGap = 5.0f;

D2D1_POINT_2F d2d_point(const smk::core::VisualPoint& point) {
    return D2D1::Point2F(static_cast<float>(point.x), static_cast<float>(point.y));
}

double normalize_angle(double angle) noexcept {
    angle = std::fmod(angle, 360.0);
    return angle < 0.0 ? angle + 360.0 : angle;
}

bool angle_in_sweep(double angle, double start, double end, double margin) noexcept {
    const double sweep = normalize_angle(end - start);
    if (sweep <= margin * 2.0) return true;
    const double relative = normalize_angle(angle - start);
    return relative >= margin && relative <= sweep - margin;
}

std::vector<smk::core::TextLineSlot> build_sector_text_slots(
    ID2D1Geometry* geometry, D2D1_POINT_2F center, double inner, double outer,
    double start, double end, double edge_margin, double line_height, double width_scale) {
    std::vector<smk::core::TextLineSlot> slots;
    if (!geometry || outer <= inner || line_height <= 0.0) return slots;
    D2D1_RECT_F bounds{};
    if (FAILED(geometry->GetBounds(nullptr, &bounds))) return slots;
    for (double y = bounds.top + line_height / 2.0; y <= bounds.bottom - line_height / 2.0; y += line_height) {
        double run_start = 0.0, best_start = 0.0, best_end = 0.0;
        bool in_run = false;
        constexpr double step = 2.0;
        for (double x = bounds.left; x <= bounds.right; x += step) {
            const double dx = x - center.x, dy = y - center.y;
            const double radius = std::hypot(dx, dy);
            const double angle = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
            const bool inside = radius >= inner && radius <= outer
                && angle_in_sweep(angle, start, end, edge_margin);
            if (inside && !in_run) { run_start = x; in_run = true; }
            if (!inside && in_run) {
                if (x - run_start > best_end - best_start) { best_start = run_start; best_end = x - step; }
                in_run = false;
            }
        }
        if (in_run && bounds.right - run_start > best_end - best_start) {
            best_start = run_start; best_end = bounds.right;
        }
        const double width = (best_end - best_start) * 0.88 * width_scale;
        if (width >= 24.0) slots.push_back({{(best_start + best_end) / 2.0, y}, width});
    }
    return slots;
}
}

WheelWindow::~WheelWindow() {
    stop_animation_timer();
    if (animation_shutdown_event_) SetEvent(animation_shutdown_event_);
    if (animation_timer_thread_.joinable()) animation_timer_thread_.join();
    if (animation_waitable_timer_) CloseHandle(animation_waitable_timer_);
    if (animation_shutdown_event_) CloseHandle(animation_shutdown_event_);
    if (window_) DestroyWindow(window_);
    discard_surface();
}

bool WheelWindow::create(HINSTANCE instance) {
    instance_ = instance;
    QueryPerformanceFrequency(&performance_frequency_);
    creation_error_ = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_factory_.GetAddressOf());
    if (FAILED(creation_error_)) return false;
    creation_error_ = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwrite_factory_.GetAddressOf()));
    if (FAILED(creation_error_)) return false;
    creation_error_ = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(wic_factory_.GetAddressOf()));
    if (FAILED(creation_error_)) creation_error_ = CoCreateInstance(CLSID_WICImagingFactory1, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_factory_.GetAddressOf()));
    if (FAILED(creation_error_)) return false;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.lpfnWndProc = window_proc;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
        WS_EX_LAYERED | WS_EX_TRANSPARENT, kClassName, L"", WS_POPUP, 0, 0, 1, 1,
        nullptr, nullptr, instance, this);
    if (!window_) {
        creation_error_ = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }
    animation_shutdown_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    animation_waitable_timer_ = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    waitable_timer_high_resolution_ = animation_waitable_timer_ != nullptr;
    if (!animation_waitable_timer_)
        animation_waitable_timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (animation_shutdown_event_ && animation_waitable_timer_) {
        animation_timer_thread_ = std::thread([this] {
            const HANDLE handles[]{animation_shutdown_event_, animation_waitable_timer_};
            for (;;) {
                const DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (result == WAIT_OBJECT_0) break;
                if (result == WAIT_OBJECT_0 + 1 && animation_timer_running_.load(std::memory_order_relaxed) && window_)
                    PostMessageW(window_, kAnimationTickMessage, 0, 0);
            }
        });
    }
    creation_error_ = S_OK;
    return true;
}

bool WheelWindow::show(POINT center, std::vector<smk::core::WheelSlot> slots,
    const smk::core::AppSettings& settings) {
    ++visual_generation_;
    center_screen_ = center;
    settings_ = settings;
    slots_ = std::move(slots);
    selected_index_ = -1;
    selected_extended_slot_ = -1;
    active_extended_direction_ = smk::core::ExtendedDirection::none;
    hiding_ = false;
    rendered_frames_ = 0;
    const UINT dpi = dpi_for_point(center);
    coordinates_ = {static_cast<double>(center.x), static_cast<double>(center.y), dpi / 96.0};
    text_format_.Reset();
    quick_copy_text_format_.Reset();
    extended_text_format_.Reset();
    ellipsis_sign_.Reset();
    creation_error_ = dwrite_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        static_cast<float>(12.0 * coordinates_.dpi_scale), L"zh-CN", text_format_.GetAddressOf());
    if (SUCCEEDED(creation_error_)) creation_error_ = dwrite_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        static_cast<float>(14.0 * coordinates_.dpi_scale), L"zh-CN", quick_copy_text_format_.GetAddressOf());
    if (SUCCEEDED(creation_error_)) creation_error_ = dwrite_factory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        static_cast<float>(10.5 * coordinates_.dpi_scale), L"zh-CN", extended_text_format_.GetAddressOf());
    if (FAILED(creation_error_)) return false;
    for (auto* format : {text_format_.Get(), quick_copy_text_format_.Get(), extended_text_format_.Get()}) {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    (void)dwrite_factory_->CreateEllipsisTrimmingSign(extended_text_format_.Get(), ellipsis_sign_.GetAddressOf());
    if (ellipsis_sign_) (void)extended_text_format_->SetTrimming(&trimming, ellipsis_sign_.Get());
    const double padding = settings_.wheel.radius * 0.06;
    double visual_radius = settings_.wheel.radius + padding;
    if (smk::core::extended_wheel_available(settings_)) {
        const auto layout = smk::core::make_extended_wheel_visual_layout(
            {0.0, 0.0}, settings_.wheel.radius, smk::core::kExtendedRingGap,
            smk::core::extended_ring_thickness(settings_.wheel.radius),
            settings_.wheel.sector_gap_degrees, kCircleCornerRadius, 1.0, 1.04, 0.0);
        visual_radius = std::max(visual_radius, layout.bounds.right);
    }
    const UINT extent = static_cast<UINT>(std::ceil(coordinates_.physical(visual_radius * 2.0)));
    SMK_DIAGNOSTIC_EVENT("wheel.show", std::format(L"generation={} shape={} slots={} dpi={} radius={} dead={} extent={} center_x={} center_y={}",
        visual_generation_, settings_.wheel.shape, slots_.size(), dpi, settings_.wheel.radius,
        settings_.wheel.inner_dead_zone_radius, extent, center.x, center.y));
    window_origin_ = {center.x - static_cast<LONG>(extent / 2), center.y - static_cast<LONG>(extent / 2)};
    if (!ensure_surface(extent, extent)) return false;
    if (!SetWindowPos(window_, HWND_TOPMOST, window_origin_.x, window_origin_.y,
        static_cast<int>(extent), static_cast<int>(extent), SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        creation_error_ = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }
    visible_ = true;
    selection_scales_.assign(slots_.size(), 1.0);
    selection_tracks_.assign(slots_.size(), {});
    for (auto& track : selection_tracks_) track.set(1.0);
    extended_visibilities_.assign(smk::core::kExtendedSlotCount, 0.0);
    extended_visibility_scales_.assign(smk::core::kExtendedSlotCount, 0.88);
    extended_selection_scales_.assign(smk::core::kExtendedSlotCount, 1.0);
    extended_visibility_tracks_.assign(smk::core::kExtendedSlotCount, {});
    extended_visibility_scale_tracks_.assign(smk::core::kExtendedSlotCount, {});
    extended_selection_tracks_.assign(smk::core::kExtendedSlotCount, {});
    for (int index = 0; index < smk::core::kExtendedSlotCount; ++index) {
        extended_visibility_tracks_[index].set(0.0);
        extended_visibility_scale_tracks_[index].set(0.88);
        extended_selection_tracks_[index].set(1.0);
    }
    SMK_DIAGNOSTIC_EVENT("wheel.track_init", std::format(L"generation={} count={} minimum=1 maximum=1", visual_generation_, selection_tracks_.size()));
#if defined(SMK_DIAGNOSTICS)
    if (settings_.wheel.shape == L"rectangle") {
        const auto rectangles = smk::core::make_rectangle_slots(
            {extent / 2.0, extent / 2.0}, settings_.wheel.radius * coordinates_.dpi_scale,
            static_cast<int>(slots_.size()), settings_.wheel.sector_gap_pixels * coordinates_.dpi_scale);
        for (std::size_t index = 0; index < rectangles.size(); ++index) {
            const auto& rect = rectangles[index];
            SMK_DIAGNOSTIC_EVENT("wheel.geometry", std::format(
                L"generation={} index={} shape=rectangle left={:.2f} top={:.2f} right={:.2f} bottom={:.2f} scale_origin_x={:.2f} scale_origin_y={:.2f}",
                visual_generation_, index, rect.left, rect.top, rect.right, rect.bottom,
                slots_.size() <= 4 ? extent / 2.0 : rect.center().x,
                slots_.size() <= 4 ? extent / 2.0 : rect.center().y));
        }
    } else if (!slots_.empty()) {
        const double step = 360.0 / slots_.size();
        const auto metrics = smk::core::make_concentric_sector_metrics(
            settings_.wheel.inner_dead_zone_radius * coordinates_.dpi_scale,
            settings_.wheel.radius * coordinates_.dpi_scale, step);
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            const auto geometry = smk::core::make_circle_sector({extent / 2.0, extent / 2.0},
                metrics.inner_radius, metrics.outer_radius,
                -90.0 + index * step, -90.0 + (index + 1) * step,
                10.0 * coordinates_.dpi_scale);
            SMK_DIAGNOSTIC_EVENT("wheel.geometry", std::format(
                L"generation={} index={} shape=circle start={:.2f} end={:.2f} inner={:.2f} outer={:.2f} side_inset={:.2f} centroid_x={:.2f} centroid_y={:.2f} base_scale=baked_parallel selection_origin_x={:.2f} selection_origin_y={:.2f}",
                visual_generation_, index, geometry.start_degrees, geometry.end_degrees,
                metrics.inner_radius, metrics.outer_radius, metrics.side_inset,
                geometry.centroid.x, geometry.centroid.y, extent / 2.0, extent / 2.0));
        }
    }
#endif
    rebuild_image_cache();
    rebuild_visual_cache();
    show_started_ms_ = -1.0;
    hide_started_ms_ = selection_started_ms_ = -1.0;
    animation_milestones_ = 0;
    if (settings_.wheel.animation_enabled) {
        global_scale_ = 0.88;
        global_opacity_ = 0.0;
        global_scale_track_.set(global_scale_);
        global_opacity_track_.set(global_opacity_);
    } else {
        global_scale_ = 1.0;
        global_opacity_ = settings_.wheel.opacity;
    }
    if (!render_frame()) {
        hide_immediately();
        return false;
    }
    if (settings_.wheel.animation_enabled) {
        const double animation_start = now_ms();
        show_started_ms_ = animation_start;
        global_scale_track_.begin(global_scale_, 1.0, animation_start, kShowMs, smk::core::Easing::cubic_out);
        global_opacity_track_.begin(global_opacity_, settings_.wheel.opacity, animation_start, kShowMs, smk::core::Easing::cubic_out);
        ensure_animation_timer();
        next_animation_frame_ms_ = animation_start + kAnimationFrameMs;
    }
    return true;
}

void WheelWindow::update_pointer(POINT screen_point) {
    if (!visible_ || hiding_) return;
    const double dx = coordinates_.logical_dx(screen_point.x);
    const double dy = coordinates_.logical_dy(screen_point.y);
    const double distance = std::hypot(dx, dy);
    auto extended_direction = smk::core::ExtendedDirection::none;
    int next_extended = -1;
    if (smk::core::extended_wheel_available(settings_)) {
        extended_direction = smk::core::resolve_extended_direction(
            settings_, distance, dx, dy, active_extended_direction_);
        set_active_extended_direction(extended_direction);
        if (extended_direction != smk::core::ExtendedDirection::none)
            next_extended = smk::core::hit_test_extended_slot(
                settings_, extended_direction, dx, dy, selected_extended_slot_);
    } else {
        set_active_extended_direction(smk::core::ExtendedDirection::none);
    }
    int next = next_extended >= 0 ? -1 : smk::core::sector_index_from_point(settings_.wheel.shape,
        settings_.wheel.sector_count, settings_.wheel.radius,
        settings_.wheel.inner_dead_zone_radius, dx, dy, selected_index_);
    if (next >= 0 && (static_cast<std::size_t>(next) >= slots_.size() || !slots_[next].selectable())) next = -1;
    update_selection(next, next_extended, screen_point, dx, dy);
}

void WheelWindow::update_selection(int next, int next_extended, [[maybe_unused]] POINT screen_point,
    [[maybe_unused]] double dx, [[maybe_unused]] double dy) {
    if (next == selected_index_ && next_extended == selected_extended_slot_) return;
    const double now = now_ms();
    [[maybe_unused]] const int previous = selected_index_;
    const int previous_extended = selected_extended_slot_;
    selected_index_ = next;
    selected_extended_slot_ = next_extended;
    selection_started_ms_ = now;
    animation_milestones_ &= ~8U;
    SMK_DIAGNOSTIC_EVENT("wheel.selection", std::format(
        L"generation={} previous={} next={} extended_previous={} extended_next={} physical_x={} physical_y={} logical_dx={:.2f} logical_dy={:.2f}",
        visual_generation_, previous, next, previous_extended, next_extended,
        screen_point.x, screen_point.y, dx, dy));
    for (std::size_t index = 0; index < selection_tracks_.size(); ++index) {
        const double target = static_cast<int>(index) == selected_index_ ? 1.04 : 1.0;
        if (static_cast<int>(index) == previous || static_cast<int>(index) == selected_index_) {
            selection_tracks_[index].begin(selection_scales_[index], target, now,
                settings_.wheel.animation_enabled ? kSelectionMs : 0.0,
                smk::core::Easing::quadratic_out);
        }
    }
    extended_selection_started_ms_ = now;
    for (std::size_t index = 0; index < extended_selection_tracks_.size(); ++index) {
        const double target = static_cast<int>(index) == selected_extended_slot_ ? 1.04 : 1.0;
        if (static_cast<int>(index) == previous_extended || static_cast<int>(index) == selected_extended_slot_)
            extended_selection_tracks_[index].begin(extended_selection_scales_[index], target, now,
                settings_.wheel.animation_enabled ? kSelectionMs : 0.0,
                smk::core::Easing::quadratic_out);
    }
    if (settings_.wheel.animation_enabled) {
        ensure_animation_timer();
        next_animation_frame_ms_ = std::min(next_animation_frame_ms_, now);
    } else {
        for (std::size_t index = 0; index < selection_tracks_.size(); ++index) {
            const double target = static_cast<int>(index) == selected_index_ ? 1.04 : 1.0;
            selection_tracks_[index].set(target);
            selection_scales_[index] = target;
        }
        for (std::size_t index = 0; index < extended_selection_tracks_.size(); ++index) {
            const double target = static_cast<int>(index) == selected_extended_slot_ ? 1.04 : 1.0;
            extended_selection_tracks_[index].set(target);
            extended_selection_scales_[index] = target;
        }
        (void)render_or_fail();
    }
}

void WheelWindow::set_active_extended_direction(smk::core::ExtendedDirection direction) {
    if (direction == active_extended_direction_) return;
    [[maybe_unused]] const auto previous = active_extended_direction_;
    active_extended_direction_ = direction;
    const double now = now_ms();
    for (int index = 0; index < smk::core::kExtendedSlotCount; ++index) {
        const bool visible = static_cast<int>(direction) == index / 3;
        extended_visibility_tracks_[index].begin(extended_visibilities_[index], visible ? 1.0 : 0.0,
            now, settings_.wheel.animation_enabled ? kExtendedVisibilityMs : 0.0,
            visible ? smk::core::Easing::quadratic_out : smk::core::Easing::quadratic_in);
        extended_visibility_scale_tracks_[index].begin(extended_visibility_scales_[index], visible ? 1.0 : 0.88,
            now, settings_.wheel.animation_enabled ? kExtendedVisibilityMs : 0.0,
            visible ? smk::core::Easing::quadratic_out : smk::core::Easing::quadratic_in);
        if (!settings_.wheel.animation_enabled) {
            extended_visibilities_[index] = visible ? 1.0 : 0.0;
            extended_visibility_scales_[index] = visible ? 1.0 : 0.88;
        }
    }
    SMK_DIAGNOSTIC_EVENT("wheel.extended_direction", std::format(
        L"previous={} next={}", static_cast<int>(previous), static_cast<int>(direction)));
    if (settings_.wheel.animation_enabled) ensure_animation_timer(); else (void)render_or_fail();
}

void WheelWindow::hide() {
    if (!visible_ || hiding_) return;
    ++visual_generation_;
    selected_index_ = -1;
    selected_extended_slot_ = -1;
    active_extended_direction_ = smk::core::ExtendedDirection::none;
    if (!settings_.wheel.animation_enabled) {
        hide_immediately();
        return;
    }
    hiding_ = true;
    const double now = now_ms();
    hide_started_ms_ = now;
    animation_milestones_ &= ~4U;
    global_scale_track_.begin(global_scale_, 0.94, now, kHideMs, smk::core::Easing::cubic_in);
    global_opacity_track_.begin(global_opacity_, 0.0, now, kHideMs, smk::core::Easing::cubic_in);
    ensure_animation_timer();
}

const smk::core::ClipboardEntry* WheelWindow::selected_entry() const noexcept {
    if (selected_index_ < 0 || static_cast<std::size_t>(selected_index_) >= slots_.size()) return nullptr;
    const auto& slot = slots_[static_cast<std::size_t>(selected_index_)];
    return slot.entry ? &*slot.entry : nullptr;
}

smk::core::WheelSelection WheelWindow::selection() const {
    if (selected_extended_slot_ >= 0 && selected_extended_slot_ < smk::core::kExtendedSlotCount) {
        const auto& slot = settings_.wheel.extended_wheel.slots[static_cast<std::size_t>(selected_extended_slot_)];
        if (slot.configured()) return slot;
    }
    const auto* entry = selected_entry();
    return entry ? smk::core::WheelSelection{*entry} : smk::core::WheelSelection{};
}

bool WheelWindow::refresh_selected_lock(bool locked) {
    if (selected_index_ < 0 || static_cast<std::size_t>(selected_index_) >= slots_.size()) return false;
    auto& slot = slots_[static_cast<std::size_t>(selected_index_)];
    if (!slot.entry || slot.entry->is_quick_copy) return false;
    slot.entry->is_locked = locked;
    (void)render_or_fail();
    return true;
}

std::size_t WheelWindow::nontransparent_slot_count_for_testing() const noexcept {
    if (!dib_pixels_ || surface_width_ == 0 || surface_height_ == 0 || slots_.empty()) return 0;
    const auto* pixels = static_cast<const std::uint8_t*>(dib_pixels_);
    const smk::core::VisualPoint center{surface_width_ / 2.0, surface_height_ / 2.0};
    std::vector<smk::core::VisualPoint> samples;
    if (settings_.wheel.shape == L"rectangle") {
        const auto rectangles = smk::core::make_rectangle_slots(center,
            settings_.wheel.radius * coordinates_.dpi_scale, static_cast<int>(slots_.size()),
            settings_.wheel.sector_gap_pixels * coordinates_.dpi_scale);
        for (const auto& rectangle : rectangles) samples.push_back(rectangle.center());
    } else {
        const double step = 360.0 / slots_.size();
        for (std::size_t index = 0; index < slots_.size(); ++index) {
            const auto geometry = smk::core::make_circle_sector(center,
                settings_.wheel.inner_dead_zone_radius * coordinates_.dpi_scale,
                settings_.wheel.radius * coordinates_.dpi_scale,
                -90.0 + index * step, -90.0 + (index + 1) * step, 10.0 * coordinates_.dpi_scale);
            samples.push_back(geometry.centroid);
        }
    }
    std::size_t visible = 0;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        double selection_scale = index < selection_scales_.size() ? selection_scales_[index] : 1.0;
        if (settings_.wheel.shape == L"rectangle" && slots_.size() > 4) selection_scale = 1.0;
        const double x = center.x + (samples[index].x - center.x) * selection_scale * global_scale_;
        const double y = center.y + (samples[index].y - center.y) * selection_scale * global_scale_;
        const auto pixel_x = static_cast<LONG>(std::lround(x));
        const auto pixel_y = static_cast<LONG>(std::lround(y));
        if (pixel_x < 0 || pixel_y < 0 || pixel_x >= static_cast<LONG>(surface_width_) || pixel_y >= static_cast<LONG>(surface_height_)) continue;
        const auto offset = (static_cast<std::size_t>(pixel_y) * surface_width_ + static_cast<std::size_t>(pixel_x)) * 4 + 3;
        if (pixels[offset] != 0) ++visible;
    }
    return visible;
}

std::size_t WheelWindow::cached_text_line_count_for_testing() const noexcept {
    return std::accumulate(visual_cache_.begin(), visual_cache_.end(), std::size_t{0},
        [](std::size_t total, const auto& slot) { return total + slot.text_lines.size(); });
}

std::size_t WheelWindow::cached_image_preview_count_for_testing() const noexcept {
    return static_cast<std::size_t>(std::count_if(visual_cache_.begin(), visual_cache_.end(),
        [](const auto& slot) { return slot.has_image_destination; }));
}

std::size_t WheelWindow::multiline_text_layout_count_for_testing() const noexcept {
    std::size_t count = 0;
    for (const auto& slot : visual_cache_) {
        for (const auto& line : slot.text_lines) {
            DWRITE_TEXT_METRICS metrics{};
            if (line.layout && SUCCEEDED(line.layout->GetMetrics(&metrics)) && metrics.lineCount > 1) ++count;
        }
    }
    return count;
}

std::size_t WheelWindow::visible_extended_slot_count_for_testing() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        extended_visibilities_.begin(), extended_visibilities_.end(),
        [](double value) { return value > 0.01; }));
}

std::size_t WheelWindow::cached_extended_icon_count_for_testing() const noexcept {
    return static_cast<std::size_t>(std::count_if(extended_visual_cache_.begin(),
        extended_visual_cache_.end(), [](const auto& slot) { return slot.icon != nullptr; }));
}

bool WheelWindow::extended_slot_has_marquee_for_testing(int slot_index) const noexcept {
    return slot_index >= 0 && static_cast<std::size_t>(slot_index) < extended_visual_cache_.size()
        && extended_visual_cache_[static_cast<std::size_t>(slot_index)].marquee_layout != nullptr;
}

LRESULT CALLBACK WheelWindow::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<WheelWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<WheelWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle_message(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT WheelWindow::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case kRenderMessage:
        if (visible_ && static_cast<std::uint64_t>(wparam) == visual_generation_) (void)render_or_fail();
        return 0;
    case kAnimationTickMessage:
        if (animation_timer_running_.load(std::memory_order_relaxed)) update_animation();
        return 0;
    case WM_TIMER:
        if (wparam == kAnimationTimer) { update_animation(); return 0; }
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window_, &paint);
        EndPaint(window_, &paint);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: return HTTRANSPARENT;
    default: break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

bool WheelWindow::ensure_surface(UINT width, UINT height) {
    if (render_target_ && width == surface_width_ && height == surface_height_) return true;
    discard_surface();
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    memory_dc_ = CreateCompatibleDC(nullptr);
    dib_ = CreateDIBSection(memory_dc_, &info, DIB_RGB_COLORS, &dib_pixels_, nullptr, 0);
    if (!memory_dc_ || !dib_) { discard_surface(); return false; }
    previous_bitmap_ = SelectObject(memory_dc_, dib_);
    D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    if (FAILED(d2d_factory_->CreateDCRenderTarget(&properties, render_target_.GetAddressOf()))) {
        discard_surface(); return false;
    }
    surface_width_ = width;
    surface_height_ = height;
    return true;
}

void WheelWindow::discard_surface() {
    visual_cache_.clear();
    extended_visual_cache_.clear();
    image_cache_.clear();
    normal_brush_.Reset(); hover_brush_.Reset(); locked_brush_.Reset(); locked_hover_brush_.Reset();
    quick_copy_brush_.Reset(); empty_brush_.Reset(); center_brush_.Reset(); text_brush_.Reset();
    quick_copy_text_brush_.Reset();
    extended_normal_brush_.Reset(); extended_configured_brush_.Reset();
    extended_selected_brush_.Reset();
    render_target_.Reset();
    if (memory_dc_ && previous_bitmap_) SelectObject(memory_dc_, previous_bitmap_);
    if (dib_) DeleteObject(dib_);
    if (memory_dc_) DeleteDC(memory_dc_);
    memory_dc_ = nullptr; dib_ = nullptr; previous_bitmap_ = nullptr; dib_pixels_ = nullptr;
    surface_width_ = surface_height_ = 0;
}

void WheelWindow::rebuild_image_cache() {
    image_cache_.assign(slots_.size(), {});
    if (!render_target_ || !wic_factory_) return;
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        if (!slots_[index].entry || !slots_[index].entry->is_image_content) continue;
        auto& entry = *slots_[index].entry;
        if (!entry.has_image_preview()) continue;
        const auto& png = entry.preview_image_bytes();
        Microsoft::WRL::ComPtr<IWICStream> stream;
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(wic_factory_->CreateStream(stream.GetAddressOf()))
            && SUCCEEDED(stream->InitializeFromMemory(
                const_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size())))
            && SUCCEEDED(wic_factory_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()))
            && SUCCEEDED(decoder->GetFrame(0, frame.GetAddressOf()))
            && SUCCEEDED(wic_factory_->CreateFormatConverter(converter.GetAddressOf()))
            && SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
            render_target_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, image_cache_[index].GetAddressOf());
        }
    }
}

bool WheelWindow::rebuild_brush_cache() {
    if (!render_target_) return false;
    const auto create = [&](const D2D1_COLOR_F& color, auto& brush, float opacity = 1.0f) {
        brush.Reset();
        const HRESULT result = render_target_->CreateSolidColorBrush(color, brush.GetAddressOf());
        if (SUCCEEDED(result)) brush->SetOpacity(opacity);
        return SUCCEEDED(result);
    };
    const bool solids_ready = create(parse_color(settings_.theme.sector_color), normal_brush_)
        && create(parse_color(settings_.theme.sector_hover_color), hover_brush_)
        && create(parse_color(L"#4E6E9E"), locked_brush_)
        && create(parse_color(L"#5F83BA"), locked_hover_brush_)
        && create(parse_color(L"#2E7D32"), quick_copy_brush_)
        && create(parse_color(settings_.theme.muted_text_color), empty_brush_, 0.35f)
        && create(parse_color(settings_.theme.center_color), center_brush_, 0.9f)
        && create(parse_color(settings_.theme.text_color), text_brush_)
        && create(parse_color(L"#3F6AFF"), quick_copy_text_brush_)
        && create(parse_color(L"#17263A"), extended_normal_brush_)
        && create(parse_color(L"#233A56"), extended_configured_brush_);
    if (!solids_ready) return false;
    D2D1_GRADIENT_STOP stops[2]{
        {0.0f, parse_color(L"#0B4FEA")},
        {1.0f, parse_color(L"#106CFF")},
    };
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stop_collection;
    if (FAILED(render_target_->CreateGradientStopCollection(stops, 2, stop_collection.GetAddressOf()))) return false;
    extended_selected_brush_.Reset();
    return SUCCEEDED(render_target_->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(surface_width_ * 0.34f, surface_height_ * 0.28f),
            D2D1::Point2F(surface_width_ * 0.68f, surface_height_ * 0.72f)),
        stop_collection.Get(), extended_selected_brush_.GetAddressOf()));
}

void WheelWindow::rebuild_visual_cache() {
    visual_cache_.assign(slots_.size(), {});
    if (!render_target_ || !d2d_factory_ || !dwrite_factory_ || !rebuild_brush_cache()) return;
    const float dpi = static_cast<float>(coordinates_.dpi_scale);
    const D2D1_POINT_2F center{surface_width_ / 2.0f, surface_height_ / 2.0f};

    const auto cache_text = [&](SlotVisualCache& cache, const smk::core::ClipboardEntry& entry,
                                const std::vector<smk::core::TextLineSlot>& line_slots,
                                double font_size, double line_height) {
        const auto text = smk::core::normalize_preview_text(entry.plain_text, entry.display_text);
        if (text.empty() || line_slots.empty()) return;
        std::vector<double> widths;
        widths.reserve(line_slots.size());
        for (const auto& slot : line_slots) widths.push_back(slot.width);
        const auto wrapped = smk::core::wrap_preview_text_centered(text, widths, font_size);
        IDWriteTextFormat* format = entry.is_quick_copy ? quick_copy_text_format_.Get() : text_format_.Get();
        for (std::size_t line_index = 0; line_index < wrapped.lines.size(); ++line_index) {
            const std::size_t slot_index = std::min(
                wrapped.first_slot + line_index, line_slots.size() - 1);
            const auto& slot = line_slots[slot_index];
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwrite_factory_->CreateTextLayout(
                    wrapped.lines[line_index].c_str(), static_cast<UINT32>(wrapped.lines[line_index].size()),
                    format, static_cast<float>(slot.width), static_cast<float>(line_height),
                    layout.GetAddressOf()))) {
                layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                    static_cast<float>(line_height), static_cast<float>(font_size));
                cache.text_lines.push_back({
                    D2D1::Point2F(static_cast<float>(slot.center.x - slot.width / 2.0),
                        static_cast<float>(slot.center.y - line_height / 2.0)),
                    std::move(layout),
                });
            }
        }
        if (!cache.text_lines.empty()) render_target_->CreateLayer(nullptr, cache.content_layer.GetAddressOf());
    };

    if (settings_.wheel.shape == L"rectangle") {
        const float radius = static_cast<float>(settings_.wheel.radius) * dpi;
        const float dead = effective_dead_zone_radius();
        const auto rectangles = smk::core::make_rectangle_slots(
            {center.x, center.y}, radius, static_cast<int>(slots_.size()), 8.0 * dpi);
        for (std::size_t index = 0; index < visual_cache_.size() && index < rectangles.size(); ++index) {
            auto& cache = visual_cache_[index];
            const auto& rectangle = rectangles[index];
            const D2D1_RECT_F bounds = D2D1::RectF(
                static_cast<float>(rectangle.left), static_cast<float>(rectangle.top),
                static_cast<float>(rectangle.right), static_cast<float>(rectangle.bottom));
            cache.geometry = create_rectangle_sector_geometry(
                bounds, center, dead + kRectangleCutoutGap * dpi, kRectangleCornerRadius * dpi);
            cache.selection_origin = slots_.size() <= 4 ? center : d2d_point(rectangle.center());
            if (!slots_[index].entry || !cache.geometry) continue;
            const auto& entry = *slots_[index].entry;
            const double cell_width = rectangle.right - rectangle.left;
            const double cell_height = rectangle.bottom - rectangle.top;
            const double text_width = std::max(44.0 * dpi, cell_width - 44.0 * dpi);
            const double text_height = std::max(32.0 * dpi, cell_height - 48.0 * dpi);
            if (entry.is_image_content && index < image_cache_.size() && image_cache_[index]) {
                const float inset = kImageClipInset * dpi;
                D2D1_RECT_F inset_bounds{bounds.left + inset, bounds.top + inset,
                    bounds.right - inset, bounds.bottom - inset};
                if (inset_bounds.right <= inset_bounds.left || inset_bounds.bottom <= inset_bounds.top)
                    inset_bounds = bounds;
                cache.image_clip = create_rectangle_sector_geometry(
                    inset_bounds, center, dead + (kRectangleCutoutGap + kImageClipInset) * dpi,
                    std::max(0.0f, (kRectangleCornerRadius - kImageClipInset / 2.0f) * dpi));
                const auto image_size = image_cache_[index]->GetSize();
                const double maximum_width = text_width * 1.02;
                const double maximum_height = text_height * 1.02;
                const double scale = std::min(maximum_width / std::max(1.0f, image_size.width),
                    maximum_height / std::max(1.0f, image_size.height));
                const float width = static_cast<float>(image_size.width * scale);
                const float height = static_cast<float>(image_size.height * scale);
                const auto cell_center = d2d_point(rectangle.center());
                cache.image_destination = D2D1::RectF(cell_center.x - width / 2.0f, cell_center.y - height / 2.0f,
                    cell_center.x + width / 2.0f, cell_center.y + height / 2.0f);
                cache.has_image_destination = true;
                render_target_->CreateLayer(nullptr, cache.image_layer.GetAddressOf());
            } else {
                const double font_size = (entry.is_quick_copy ? 14.0 : 12.0) * dpi;
                const double line_height = font_size * 1.25;
                const std::size_t line_count = std::max<std::size_t>(1,
                    static_cast<std::size_t>(text_height / line_height));
                const double width = entry.is_quick_copy ? text_width * 1.18 : text_width;
                std::vector<smk::core::TextLineSlot> line_slots;
                line_slots.reserve(line_count);
                const double top = rectangle.center().y - text_height / 2.0;
                for (std::size_t line = 0; line < line_count; ++line)
                    line_slots.push_back({{rectangle.center().x, top + line * line_height + line_height / 2.0}, width});
                cache_text(cache, entry, line_slots, font_size, line_height);
            }
        }
    } else if (!slots_.empty()) {
        const double step = 360.0 / slots_.size();
        const double raw_inner = settings_.wheel.inner_dead_zone_radius * dpi;
        const double raw_outer = settings_.wheel.radius * dpi;
        const auto metrics = smk::core::make_concentric_sector_metrics(raw_inner, raw_outer, step);
        for (std::size_t index = 0; index < visual_cache_.size(); ++index) {
            auto& cache = visual_cache_[index];
            const double raw_start = -90.0 + index * step;
            const double raw_end = raw_start + step;
            const double start = raw_start;
            const double end = raw_end;
            cache.geometry = create_parallel_rounded_sector_geometry(center,
                static_cast<float>(metrics.inner_radius), static_cast<float>(metrics.outer_radius),
                static_cast<float>(start), static_cast<float>(end), static_cast<float>(metrics.side_inset),
                kCircleCornerRadius * dpi);
            cache.selection_origin = center;
            if (!slots_[index].entry || !cache.geometry) continue;
            const auto& entry = *slots_[index].entry;
            const double font_size = (entry.is_quick_copy ? 14.0 : 12.0) * dpi;
            const double line_height = font_size * 1.25;
            const double inner_margin = std::max(22.0 * dpi, font_size * 1.8);
            const double outer_margin = std::max(30.0 * dpi, font_size * 2.4);
            const double edge_margin = std::max(4.0, step * 0.10);
            auto line_slots = build_sector_text_slots(cache.geometry.Get(), center,
                metrics.inner_radius + inner_margin, metrics.outer_radius - outer_margin,
                start, end, edge_margin, line_height, entry.is_quick_copy ? 1.10 : 1.0);
            if (entry.is_image_content && index < image_cache_.size() && image_cache_[index] && !line_slots.empty()) {
                cache.image_clip = create_parallel_inset_sector_geometry(center,
                    static_cast<float>(metrics.inner_radius + kImageClipInset * dpi),
                    static_cast<float>(metrics.outer_radius - kImageClipInset * dpi),
                    static_cast<float>(start), static_cast<float>(end),
                    static_cast<float>(metrics.side_inset + kImageClipInset * dpi));
                const auto image_size = image_cache_[index]->GetSize();
                const auto image_layout = smk::core::compute_sector_image_layout(
                    image_size.width, image_size.height, line_slots, line_height,
                    metrics.inner_radius, metrics.outer_radius, start, end, kImageClipInset * dpi);
                cache.image_destination = D2D1::RectF(
                    static_cast<float>(image_layout.center.x - image_layout.width / 2.0),
                    static_cast<float>(image_layout.center.y - image_layout.height / 2.0),
                    static_cast<float>(image_layout.center.x + image_layout.width / 2.0),
                    static_cast<float>(image_layout.center.y + image_layout.height / 2.0));
                cache.has_image_destination = image_layout.width > 0.0 && image_layout.height > 0.0;
                render_target_->CreateLayer(nullptr, cache.image_layer.GetAddressOf());
            } else {
                cache_text(cache, entry, line_slots, font_size, line_height);
            }
        }
    }
    rebuild_extended_visual_cache();
    SMK_DIAGNOSTIC_EVENT("wheel.visual_cache", std::format(
        L"generation={} slots={} shape={} cached_images={} cached_text_lines={}", visual_generation_,
        visual_cache_.size(), settings_.wheel.shape,
        std::count_if(visual_cache_.begin(), visual_cache_.end(), [](const auto& slot) { return slot.has_image_destination; }),
        std::accumulate(visual_cache_.begin(), visual_cache_.end(), std::size_t{0},
            [](std::size_t total, const auto& slot) { return total + slot.text_lines.size(); })));
}

void WheelWindow::rebuild_extended_visual_cache() {
    extended_visual_cache_.assign(smk::core::kExtendedSlotCount, {});
    if (!smk::core::extended_wheel_available(settings_) || !render_target_ || !extended_text_format_) return;
    const float dpi = static_cast<float>(coordinates_.dpi_scale);
    const D2D1_POINT_2F center{surface_width_ / 2.0f, surface_height_ / 2.0f};
    extended_visual_layout_ = smk::core::make_extended_wheel_visual_layout(
        {center.x, center.y}, settings_.wheel.radius * dpi,
        smk::core::kExtendedRingGap * dpi,
        smk::core::extended_ring_thickness(settings_.wheel.radius) * dpi,
        settings_.wheel.sector_gap_degrees, kCircleCornerRadius * dpi, dpi,
        1.04, 0.0);

    const auto load_icon = [&](const std::wstring& path) -> Microsoft::WRL::ComPtr<ID2D1Bitmap> {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> result;
        if (path.empty()) return result;
        HICON icon = smk::windows::resolve_shortcut_icon(path,
            static_cast<int>(std::lround(40.0 * coordinates_.dpi_scale)));
        if (!icon) {
            SMK_DIAGNOSTIC_EVENT("wheel.extended_icon", L"result=resolve_failed");
            return result;
        }
        Microsoft::WRL::ComPtr<IWICBitmap> source;
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        const HRESULT source_result = wic_factory_->CreateBitmapFromHICON(icon, source.GetAddressOf());
        const HRESULT converter_result = SUCCEEDED(source_result)
            ? wic_factory_->CreateFormatConverter(converter.GetAddressOf()) : E_FAIL;
        const HRESULT initialize_result = SUCCEEDED(converter_result)
            ? converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom) : E_FAIL;
        [[maybe_unused]] const HRESULT bitmap_result = SUCCEEDED(initialize_result)
            ? render_target_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, result.GetAddressOf()) : E_FAIL;
        SMK_DIAGNOSTIC_EVENT("wheel.extended_icon", std::format(
            L"result={} source={:#x} converter={:#x} initialize={:#x} bitmap={:#x}",
            result ? L"success" : L"failed", static_cast<unsigned>(source_result),
            static_cast<unsigned>(converter_result), static_cast<unsigned>(initialize_result),
            static_cast<unsigned>(bitmap_result)));
        DestroyIcon(icon);
        return result;
    };

    for (int index = 0; index < smk::core::kExtendedSlotCount; ++index) {
        auto& cache = extended_visual_cache_[static_cast<std::size_t>(index)];
        const auto& slot = settings_.wheel.extended_wheel.slots[static_cast<std::size_t>(index)];
        const auto& visual = extended_visual_layout_.slots[static_cast<std::size_t>(index)];
        cache.geometry = create_sector_geometry(center,
            static_cast<float>(extended_visual_layout_.ring_inner_radius),
            static_cast<float>(extended_visual_layout_.ring_outer_radius),
            static_cast<float>(visual.logical.start_degrees), static_cast<float>(visual.logical.end_degrees),
            kCircleCornerRadius * dpi);
        cache.configured = slot.configured();
        if (!cache.configured) continue;
        cache.content_center = D2D1::Point2F(
            static_cast<float>(visual.content_center.x), static_cast<float>(visual.content_center.y));
        const float viewport_width = static_cast<float>(visual.text_viewport_width);
        const float text_height = static_cast<float>(visual.text_height);
        const bool shortcut = _wcsicmp(slot.mode.c_str(), L"shortcut") == 0;
        const bool can_show_icon = visual.can_show_icon;
        float text_top = cache.content_center.y - text_height * 0.5f;
        if (shortcut && can_show_icon) {
            const float icon_size = static_cast<float>(visual.icon_size);
            const float content_height = icon_size + 21.0f * dpi;
            const float top = cache.content_center.y - content_height * 0.5f;
            cache.icon_destination = D2D1::RectF(cache.content_center.x - icon_size * 0.5f, top,
                cache.content_center.x + icon_size * 0.5f, top + icon_size);
            text_top = top + icon_size + 2.0f * dpi;
            cache.icon = load_icon(slot.shortcut_path);
        }
        cache.text_viewport = D2D1::RectF(cache.content_center.x - viewport_width * 0.5f, text_top,
            cache.content_center.x + viewport_width * 0.5f, text_top + text_height);

        const auto name = slot.display_name();
        (void)dwrite_factory_->CreateTextLayout(name.c_str(), static_cast<UINT32>(name.size()),
            extended_text_format_.Get(), viewport_width, text_height, cache.text_layout.GetAddressOf());
        Microsoft::WRL::ComPtr<IDWriteTextLayout> measured;
        DWRITE_TEXT_METRICS full_metrics{};
        if (SUCCEEDED(dwrite_factory_->CreateTextLayout(name.c_str(), static_cast<UINT32>(name.size()),
                extended_text_format_.Get(), 4096.0f * dpi, text_height, measured.GetAddressOf()))) {
            measured->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            (void)measured->GetMetrics(&full_metrics);
        }
        const bool overflows = full_metrics.widthIncludingTrailingWhitespace > viewport_width + 0.5f * dpi;
        if (settings_.wheel.animation_enabled && overflows) {
            const std::wstring spacer = L"   ";
            const std::wstring repeated = name + spacer + name;
            const std::wstring cycle = name + spacer;
            Microsoft::WRL::ComPtr<IDWriteTextLayout> cycle_layout;
            DWRITE_TEXT_METRICS cycle_metrics{};
            (void)dwrite_factory_->CreateTextLayout(cycle.c_str(), static_cast<UINT32>(cycle.size()),
                extended_text_format_.Get(), 4096.0f * dpi, text_height, cycle_layout.GetAddressOf());
            if (cycle_layout) (void)cycle_layout->GetMetrics(&cycle_metrics);
            cache.marquee_cycle_width = cycle_metrics.widthIncludingTrailingWhitespace;
            (void)dwrite_factory_->CreateTextLayout(repeated.c_str(), static_cast<UINT32>(repeated.size()),
                extended_text_format_.Get(), 8192.0f * dpi, text_height, cache.marquee_layout.GetAddressOf());
            if (cache.marquee_layout) cache.marquee_layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        SMK_DIAGNOSTIC_EVENT("wheel.extended_text", std::format(
            L"slot={} source={} chars={} width={:.2f} viewport={:.2f} overflow={} marquee={}",
            index, slot.name.empty() ? L"fallback" : L"custom", name.size(),
            full_metrics.widthIncludingTrailingWhitespace, viewport_width, overflows,
            cache.marquee_layout != nullptr));
    }
}

bool WheelWindow::render_frame() {
    if (!visible_ || !render_target_ || !memory_dc_) return false;
    const double render_started = now_ms();
    RECT bounds{0, 0, static_cast<LONG>(surface_width_), static_cast<LONG>(surface_height_)};
    const HRESULT bind = render_target_->BindDC(memory_dc_, &bounds);
    if (FAILED(bind)) { SMK_DIAGNOSTIC_EVENT("wheel.bind_dc_failed", std::format(L"hr=0x{:08X}", static_cast<unsigned>(bind))); return false; }
    render_target_->BeginDraw();
    render_target_->SetTransform(D2D1::Matrix3x2F::Identity());
    render_target_->Clear(D2D1::ColorF(0, 0.0f));
    const D2D1_POINT_2F center{surface_width_ / 2.0f, surface_height_ / 2.0f};
    render_target_->SetTransform(D2D1::Matrix3x2F::Scale(
        D2D1::SizeF(static_cast<float>(global_scale_), static_cast<float>(global_scale_)), center));
    if (settings_.wheel.shape == L"rectangle") draw_rectangle(); else draw_circle();
    render_target_->SetTransform(D2D1::Matrix3x2F::Identity());
    const HRESULT result = render_target_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) {
        SMK_DIAGNOSTIC_EVENT("wheel.device_rebuild", std::format(L"generation={} frame={}", visual_generation_, rendered_frames_));
        const UINT width = surface_width_, height = surface_height_;
        discard_surface();
        if (ensure_surface(width, height)) {
            rebuild_image_cache();
            rebuild_visual_cache();
            return PostMessageW(window_, kRenderMessage,
                static_cast<WPARAM>(visual_generation_), 0) != FALSE;
        }
        return false;
    }
    if (FAILED(result)) {
        SMK_DIAGNOSTIC_EVENT("wheel.end_draw_failed", std::format(
            L"generation={} hr=0x{:08X}", visual_generation_, static_cast<unsigned>(result)));
        return false;
    }
    POINT source{};
    SIZE size{static_cast<LONG>(surface_width_), static_cast<LONG>(surface_height_)};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(std::clamp(global_opacity_, 0.0, 1.0) * 255.0), AC_SRC_ALPHA};
    HDC screen = GetDC(nullptr);
    [[maybe_unused]] const BOOL updated = UpdateLayeredWindow(window_, screen, &window_origin_, &size, memory_dc_, &source, 0, &blend, ULW_ALPHA);
#if defined(SMK_DIAGNOSTICS)
    const DWORD update_error = updated ? ERROR_SUCCESS : GetLastError();
#endif
    ReleaseDC(nullptr, screen);
#if defined(SMK_DIAGNOSTICS)
    if (rendered_frames_ < 4 || !updated) SMK_DIAGNOSTIC_EVENT("wheel.render_result", std::format(
        L"generation={} frame={} bind_dc=0x{:08X} end_draw=0x{:08X} update_layered={} win32_error={}",
        visual_generation_, rendered_frames_ + 1, static_cast<unsigned>(bind), static_cast<unsigned>(result), updated != FALSE, update_error));
    ++rendered_frames_;
    if (!updated) SMK_DIAGNOSTIC_EVENT("wheel.update_layered_failed", std::format(L"generation={} error={}", visual_generation_, update_error));
#else
    ++rendered_frames_;
#endif
    if (rendered_frames_ <= 4) SMK_DIAGNOSTIC_EVENT("wheel.frame", std::format(L"generation={} frame={} scale={:.4f} opacity={:.4f} selected={}",
        visual_generation_, rendered_frames_, global_scale_, global_opacity_, selected_index_));
    const double render_elapsed = now_ms() - render_started;
    if (rendered_frames_ <= 4 || render_elapsed > 12.0) SMK_DIAGNOSTIC_EVENT("wheel.frame_timing", std::format(
        L"generation={} frame={} render_ms={:.3f} interval_ms={:.3f} skipped={}", visual_generation_,
        rendered_frames_, render_elapsed,
        last_animation_frame_ms_ < 0.0 ? 0.0 : render_started - last_animation_frame_ms_,
        skipped_animation_frames_));
    last_animation_frame_ms_ = render_started;
    return updated != FALSE;
}

bool WheelWindow::render_or_fail() {
    if (render_frame()) return true;
    if (visible_) {
        SMK_DIAGNOSTIC_EVENT("wheel.render_cancel",
            std::format(L"generation={}", visual_generation_));
        hide_immediately();
        if (failure_callback_) failure_callback_();
    }
    return false;
}

void WheelWindow::update_animation() {
    const double now = now_ms();
    double due_time = next_animation_frame_ms_;
    const auto include_deadline = [&](const smk::core::AnimationTrack& track) {
        if (track.active) due_time = due_time < 0.0
            ? track.started_ms + track.duration_ms
            : std::min(due_time, track.started_ms + track.duration_ms);
    };
    include_deadline(global_scale_track_);
    include_deadline(global_opacity_track_);
    for (const auto& track : selection_tracks_) include_deadline(track);
    for (const auto& track : extended_visibility_tracks_) include_deadline(track);
    for (const auto& track : extended_visibility_scale_tracks_) include_deadline(track);
    for (const auto& track : extended_selection_tracks_) include_deadline(track);
    if (due_time >= 0.0 && now + 0.25 < due_time) return;
    if (next_animation_frame_ms_ < 0.0) next_animation_frame_ms_ = now;
    const bool animation_deadline_frame = due_time + 0.25 < next_animation_frame_ms_;
    if (!animation_deadline_frame) {
        const double late_by = now - next_animation_frame_ms_;
        if (late_by >= kAnimationFrameMs) {
            const auto skipped = static_cast<std::uint32_t>(late_by / kAnimationFrameMs);
            skipped_animation_frames_ += skipped;
            next_animation_frame_ms_ += skipped * kAnimationFrameMs;
        }
        next_animation_frame_ms_ += kAnimationFrameMs;
    }
    global_scale_ = global_scale_track_.sample(now);
    global_opacity_ = global_opacity_track_.sample(now);
    for (std::size_t index = 0; index < selection_tracks_.size(); ++index)
        selection_scales_[index] = selection_tracks_[index].sample(now);
    for (std::size_t index = 0; index < extended_visibility_tracks_.size(); ++index) {
        extended_visibilities_[index] = extended_visibility_tracks_[index].sample(now);
        extended_visibility_scales_[index] = extended_visibility_scale_tracks_[index].sample(now);
        extended_selection_scales_[index] = extended_selection_tracks_[index].sample(now);
    }
    if (show_started_ms_ >= 0.0 && !(animation_milestones_ & 1U) && now - show_started_ms_ >= 16.0) {
        animation_milestones_ |= 1U;
        SMK_DIAGNOSTIC_EVENT("wheel.animation", std::format(L"phase=show milestone_ms=16 elapsed_ms={:.2f}", now - show_started_ms_));
    }
    if (show_started_ms_ >= 0.0 && !(animation_milestones_ & 2U) && now - show_started_ms_ >= 58.0) {
        animation_milestones_ |= 2U;
        SMK_DIAGNOSTIC_EVENT("wheel.animation", std::format(L"phase=show milestone_ms=58 elapsed_ms={:.2f}", now - show_started_ms_));
    }
    if (hide_started_ms_ >= 0.0 && !(animation_milestones_ & 4U) && now - hide_started_ms_ >= 60.0) {
        animation_milestones_ |= 4U;
        SMK_DIAGNOSTIC_EVENT("wheel.animation", std::format(L"phase=hide milestone_ms=60 elapsed_ms={:.2f}", now - hide_started_ms_));
    }
    if (selection_started_ms_ >= 0.0 && !(animation_milestones_ & 8U) && now - selection_started_ms_ >= 75.0) {
        animation_milestones_ |= 8U;
        SMK_DIAGNOSTIC_EVENT("wheel.animation", std::format(L"phase=selection milestone_ms=75 elapsed_ms={:.2f}", now - selection_started_ms_));
    }
    if (!render_or_fail()) return;
    if (hiding_ && !global_opacity_track_.active) { hide_immediately(); return; }
    if (!animation_active()) stop_animation_timer();
}

void WheelWindow::ensure_animation_timer() {
    if (!window_ || animation_timer_running_.load(std::memory_order_relaxed)) return;
    high_resolution_timer_active_ = timeBeginPeriod(1) == TIMERR_NOERROR;
    if (animation_waitable_timer_ && animation_timer_thread_.joinable()) {
        LARGE_INTEGER due_time{};
        due_time.QuadPart = -static_cast<LONGLONG>(kAnimationWakeMs) * 10'000LL;
        animation_timer_running_.store(SetWaitableTimer(
            animation_waitable_timer_, &due_time, kAnimationWakeMs, nullptr, nullptr, FALSE) != FALSE,
            std::memory_order_relaxed);
    }
    if (!animation_timer_running_.load(std::memory_order_relaxed)) {
        fallback_window_timer_active_ = SetTimer(window_, kAnimationTimer, kAnimationWakeMs, nullptr) != 0;
        animation_timer_running_.store(fallback_window_timer_active_, std::memory_order_relaxed);
    }
    const double now = now_ms();
    next_animation_frame_ms_ = now + kAnimationFrameMs;
    last_animation_frame_ms_ = -1.0;
    skipped_animation_frames_ = 0;
    SMK_DIAGNOSTIC_EVENT("wheel.frame_clock", std::format(
        L"state=start wake_ms={} target_ms={:.3f} clock_resolution={} waitable_high_resolution={}",
        kAnimationWakeMs, kAnimationFrameMs, high_resolution_timer_active_, waitable_timer_high_resolution_));
    if (!animation_timer_running_.load(std::memory_order_relaxed) && high_resolution_timer_active_) {
        timeEndPeriod(1);
        high_resolution_timer_active_ = false;
    }
}
void WheelWindow::stop_animation_timer() {
    animation_timer_running_.store(false, std::memory_order_relaxed);
    if (animation_waitable_timer_) CancelWaitableTimer(animation_waitable_timer_);
    if (window_ && fallback_window_timer_active_) KillTimer(window_, kAnimationTimer);
    fallback_window_timer_active_ = false;
    if (high_resolution_timer_active_) timeEndPeriod(1);
    high_resolution_timer_active_ = false;
    next_animation_frame_ms_ = -1.0;
    SMK_DIAGNOSTIC_EVENT("wheel.frame_clock", std::format(
        L"state=stop skipped={}", skipped_animation_frames_));
}
void WheelWindow::hide_immediately() {
    stop_animation_timer();
    if (window_) ShowWindow(window_, SW_HIDE);
    visible_ = false; hiding_ = false; selected_index_ = -1;
    slots_.clear(); image_cache_.clear(); visual_cache_.clear();
    selection_tracks_.clear(); selection_scales_.clear();
    selected_extended_slot_ = -1;
    active_extended_direction_ = smk::core::ExtendedDirection::none;
    extended_visual_cache_.clear();
    extended_visibility_tracks_.clear(); extended_visibility_scale_tracks_.clear(); extended_selection_tracks_.clear();
    extended_visibilities_.clear(); extended_visibility_scales_.clear(); extended_selection_scales_.clear();
}
bool WheelWindow::animation_active() const noexcept {
    if (global_scale_track_.active || global_opacity_track_.active) return true;
    const auto any_active = [](const auto& tracks) {
        return std::any_of(tracks.begin(), tracks.end(), [](const auto& track) { return track.active; });
    };
    if (any_active(selection_tracks_) || any_active(extended_visibility_tracks_)
        || any_active(extended_visibility_scale_tracks_) || any_active(extended_selection_tracks_)) return true;
    return settings_.wheel.animation_enabled && selected_extended_slot_ >= 0
        && static_cast<std::size_t>(selected_extended_slot_) < extended_visual_cache_.size()
        && extended_visual_cache_[static_cast<std::size_t>(selected_extended_slot_)].marquee_layout;
}
double WheelWindow::now_ms() const noexcept {
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    return performance_frequency_.QuadPart > 0 ? now.QuadPart * 1000.0 / performance_frequency_.QuadPart : GetTickCount64();
}

UINT WheelWindow::dpi_for_point(POINT point) const noexcept {
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"));
        UINT x = 96, y = 96;
        if (fn && SUCCEEDED(fn(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), 0, &x, &y))) {
            FreeLibrary(shcore); return x;
        }
        FreeLibrary(shcore);
    }
    HDC screen = GetDC(nullptr); const UINT dpi = screen ? static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX)) : 96;
    if (screen) ReleaseDC(nullptr, screen); return dpi;
}

Microsoft::WRL::ComPtr<ID2D1PathGeometry> WheelWindow::create_sector_geometry(
    D2D1_POINT_2F center, float inner, float outer, float start, float end, float corner) const {
    const auto points = smk::core::make_circle_sector(
        {center.x, center.y}, inner, outer, start, end, corner);
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(d2d_factory_->CreatePathGeometry(geometry.GetAddressOf())) || FAILED(geometry->Open(sink.GetAddressOf()))) return {};
    sink->BeginFigure(d2d_point(points.outer_start), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddArc(D2D1::ArcSegment(d2d_point(points.outer_end), D2D1::SizeF(outer, outer), 0,
        D2D1_SWEEP_DIRECTION_CLOCKWISE, points.large_outer_arc ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(d2d_point(points.outer_end_control), d2d_point(points.end_outer_radial)));
    sink->AddLine(d2d_point(points.end_inner_radial));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(d2d_point(points.inner_end_control), d2d_point(points.inner_end)));
    sink->AddArc(D2D1::ArcSegment(d2d_point(points.inner_start), D2D1::SizeF(inner, inner), 0,
        D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, points.large_inner_arc ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(d2d_point(points.inner_start_control), d2d_point(points.start_inner_radial)));
    sink->AddLine(d2d_point(points.start_outer_radial));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(d2d_point(points.outer_start_control), d2d_point(points.outer_start)));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED); sink->Close(); return geometry;
}

Microsoft::WRL::ComPtr<ID2D1PathGeometry> WheelWindow::create_parallel_inset_sector_geometry(
    D2D1_POINT_2F center, float inner, float outer, float start, float end, float side_inset) const {
    if (outer <= inner + 1.0f) return create_sector_geometry(center, inner, outer, start, end, 0.0f);
    const auto offset_point = [&](float angle, float radius, bool start_side) {
        constexpr double pi = 3.14159265358979323846;
        const double radians = angle * pi / 180.0;
        const double ux = std::cos(radians), uy = std::sin(radians);
        const double nx = start_side ? -uy : uy;
        const double ny = start_side ? ux : -ux;
        const double inset = std::min<double>(side_inset, std::max(0.0f, radius - 1.0f));
        const double along = std::sqrt(std::max(1.0, radius * radius - inset * inset));
        return D2D1::Point2F(static_cast<float>(center.x + nx * inset + ux * along),
            static_cast<float>(center.y + ny * inset + uy * along));
    };
    const auto outer_start = offset_point(start, outer, true);
    const auto outer_end = offset_point(end, outer, false);
    const auto inner_end = offset_point(end, inner, false);
    const auto inner_start = offset_point(start, inner, true);
    const bool large_arc = std::abs(end - start) > 180.0f;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(d2d_factory_->CreatePathGeometry(geometry.GetAddressOf()))
        || FAILED(geometry->Open(sink.GetAddressOf()))) return {};
    sink->BeginFigure(outer_start, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddArc(D2D1::ArcSegment(outer_end, D2D1::SizeF(outer, outer), 0,
        D2D1_SWEEP_DIRECTION_CLOCKWISE, large_arc ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->AddLine(inner_end);
    sink->AddArc(D2D1::ArcSegment(inner_start, D2D1::SizeF(inner, inner), 0,
        D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, large_arc ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->AddLine(outer_start);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return {};
    return geometry;
}

Microsoft::WRL::ComPtr<ID2D1PathGeometry> WheelWindow::create_parallel_rounded_sector_geometry(
    D2D1_POINT_2F center, float inner, float outer, float start, float end,
    float side_inset, float corner_radius) const {
    if (outer <= inner + 1.0f) return {};
    constexpr double pi = 3.14159265358979323846;
    const float thickness = outer - inner;
    const float corner = std::min(corner_radius, thickness / 3.0f);
    if (corner <= 0.1f || side_inset <= 0.1f)
        return create_parallel_inset_sector_geometry(center, inner, outer, start, end, side_inset);

    const auto circle_point = [&](float radius, double angle_degrees) {
        const double radians = angle_degrees * pi / 180.0;
        return D2D1::Point2F(static_cast<float>(center.x + radius * std::cos(radians)),
            static_cast<float>(center.y + radius * std::sin(radians)));
    };
    const auto offset_point = [&](float angle, float radius, bool start_side) {
        const double radians = angle * pi / 180.0;
        const double ux = std::cos(radians), uy = std::sin(radians);
        const double nx = start_side ? -uy : uy;
        const double ny = start_side ? ux : -ux;
        const double inset = std::min<double>(side_inset, std::max(0.0f, radius - 1.0f));
        const double along = std::sqrt(std::max(1.0, radius * radius - inset * inset));
        return D2D1::Point2F(static_cast<float>(center.x + nx * inset + ux * along),
            static_cast<float>(center.y + ny * inset + uy * along));
    };
    const auto edge_angle_offset = [&](float radius) {
        return std::asin(std::clamp<double>(side_inset / std::max(1.0f, radius), 0.0, 0.99)) * 180.0 / pi;
    };
    const double sweep = std::abs(end - start);
    const double maximum_corner_angle = sweep * 0.22;
    const double outer_corner_angle = std::min(maximum_corner_angle, corner / outer * 180.0 / pi);
    const double inner_corner_angle = std::min(maximum_corner_angle, corner / inner * 180.0 / pi);
    const double outer_start_edge = start + edge_angle_offset(outer);
    const double outer_end_edge = end - edge_angle_offset(outer);
    const double inner_start_edge = start + edge_angle_offset(inner);
    const double inner_end_edge = end - edge_angle_offset(inner);

    const auto outer_start = circle_point(outer, outer_start_edge + outer_corner_angle);
    const auto outer_end = circle_point(outer, outer_end_edge - outer_corner_angle);
    const auto outer_end_control = offset_point(end, outer, false);
    const auto end_outer_radial = offset_point(end, outer - corner, false);
    const auto end_inner_radial = offset_point(end, inner + corner, false);
    const auto inner_end_control = offset_point(end, inner, false);
    const auto inner_end = circle_point(inner, inner_end_edge - inner_corner_angle);
    const auto inner_start = circle_point(inner, inner_start_edge + inner_corner_angle);
    const auto inner_start_control = offset_point(start, inner, true);
    const auto start_inner_radial = offset_point(start, inner + corner, true);
    const auto start_outer_radial = offset_point(start, outer - corner, true);
    const auto outer_start_control = offset_point(start, outer, true);
    const bool large_outer_arc = outer_end_edge - outer_corner_angle
        - (outer_start_edge + outer_corner_angle) > 180.0;
    const bool large_inner_arc = inner_end_edge - inner_corner_angle
        - (inner_start_edge + inner_corner_angle) > 180.0;

    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(d2d_factory_->CreatePathGeometry(geometry.GetAddressOf()))
        || FAILED(geometry->Open(sink.GetAddressOf()))) return {};
    sink->BeginFigure(outer_start, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddArc(D2D1::ArcSegment(outer_end, D2D1::SizeF(outer, outer), 0,
        D2D1_SWEEP_DIRECTION_CLOCKWISE, large_outer_arc ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(outer_end_control, end_outer_radial));
    sink->AddLine(end_inner_radial);
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(inner_end_control, inner_end));
    sink->AddArc(D2D1::ArcSegment(inner_start, D2D1::SizeF(inner, inner), 0,
        D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, large_inner_arc ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(inner_start_control, start_inner_radial));
    sink->AddLine(start_outer_radial);
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(outer_start_control, outer_start));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return {};
    return geometry;
}

Microsoft::WRL::ComPtr<ID2D1PathGeometry> WheelWindow::create_rectangle_sector_geometry(
    const D2D1_RECT_F& rectangle, D2D1_POINT_2F center, float cutout_radius, float corner_radius) const {
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> rounded;
    Microsoft::WRL::ComPtr<ID2D1EllipseGeometry> cutout;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> combined;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(d2d_factory_->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(rectangle, corner_radius, corner_radius), rounded.GetAddressOf()))
        || FAILED(d2d_factory_->CreateEllipseGeometry(
            D2D1::Ellipse(center, cutout_radius, cutout_radius), cutout.GetAddressOf()))
        || FAILED(d2d_factory_->CreatePathGeometry(combined.GetAddressOf()))
        || FAILED(combined->Open(sink.GetAddressOf()))
        || FAILED(rounded->CombineWithGeometry(cutout.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get()))
        || FAILED(sink->Close())) return {};
    return combined;
}

void WheelWindow::draw_circle() {
    const D2D1_POINT_2F center{surface_width_ / 2.0f, surface_height_ / 2.0f};
    const auto global = D2D1::Matrix3x2F::Scale(
        D2D1::SizeF(static_cast<float>(global_scale_), static_cast<float>(global_scale_)), center);
    for (std::size_t index = 0; index < visual_cache_.size(); ++index) {
        const auto& cache = visual_cache_[index];
        if (!cache.geometry) continue;
        const float selected_scale = static_cast<float>(selection_scales_[index]);
        const auto selection = D2D1::Matrix3x2F::Scale(D2D1::SizeF(selected_scale, selected_scale), center);
        render_target_->SetTransform(selection * global);
        render_target_->FillGeometry(cache.geometry.Get(), brush_for_slot(slots_[index], static_cast<int>(index) == selected_index_));
        render_target_->SetTransform(global);
        draw_cached_content(index);
    }
    draw_extended_actions();
    render_target_->SetTransform(global);
    draw_center_disk();
}

void WheelWindow::draw_extended_actions() {
    if (extended_visual_cache_.empty()) return;
    const D2D1_POINT_2F center{surface_width_ / 2.0f, surface_height_ / 2.0f};
    const auto global = D2D1::Matrix3x2F::Scale(
        D2D1::SizeF(static_cast<float>(global_scale_), static_cast<float>(global_scale_)), center);
    const double now = now_ms();
    for (std::size_t index = 0; index < extended_visual_cache_.size(); ++index) {
        const auto& cache = extended_visual_cache_[index];
        const float visibility = index < extended_visibilities_.size()
            ? static_cast<float>(std::clamp(extended_visibilities_[index], 0.0, 1.0)) : 0.0f;
        if (!cache.geometry || visibility <= 0.001f) continue;
        const bool selected = static_cast<int>(index) == selected_extended_slot_ && cache.configured;
        const float visual_scale = static_cast<float>(extended_visibility_scales_[index]
            * extended_selection_scales_[index]);
        render_target_->SetTransform(D2D1::Matrix3x2F::Scale(
            D2D1::SizeF(visual_scale, visual_scale), center) * global);
        ID2D1Brush* fill = selected && extended_selected_brush_
            ? static_cast<ID2D1Brush*>(extended_selected_brush_.Get())
            : cache.configured ? static_cast<ID2D1Brush*>(extended_configured_brush_.Get())
                : static_cast<ID2D1Brush*>(extended_normal_brush_.Get());
        const float fill_opacity = visibility * (selected ? 0.96f : cache.configured ? 0.92f : 0.32f);
        fill->SetOpacity(fill_opacity);
        render_target_->FillGeometry(cache.geometry.Get(), fill);
        fill->SetOpacity(1.0f);
        if (!cache.configured) continue;

        render_target_->SetTransform(global);
        const float content_opacity = visibility * (selected ? 1.0f : 0.95f);
        if (cache.icon) render_target_->DrawBitmap(cache.icon.Get(), cache.icon_destination,
            content_opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        text_brush_->SetOpacity(content_opacity);
        render_target_->PushAxisAlignedClip(cache.text_viewport, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (selected && cache.marquee_layout && cache.marquee_cycle_width > 0.0f) {
            const double elapsed = std::max(0.0, now - extended_selection_started_ms_ - kExtendedMarqueeLeadInMs);
            const double duration = std::max(kExtendedMarqueeMinimumMs,
                cache.marquee_cycle_width / (kExtendedMarqueePixelsPerSecond * coordinates_.dpi_scale) * 1000.0);
            const float offset = elapsed <= 0.0 ? 0.0f
                : static_cast<float>(-cache.marquee_cycle_width * std::fmod(elapsed, duration) / duration);
            render_target_->DrawTextLayout(D2D1::Point2F(cache.text_viewport.left + offset, cache.text_viewport.top),
                cache.marquee_layout.Get(), text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        } else if (cache.text_layout) {
            render_target_->DrawTextLayout(D2D1::Point2F(cache.text_viewport.left, cache.text_viewport.top),
                cache.text_layout.Get(), text_brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        render_target_->PopAxisAlignedClip();
        text_brush_->SetOpacity(1.0f);
    }
}

void WheelWindow::draw_rectangle() {
    const D2D1_POINT_2F center{surface_width_ / 2.0f, surface_height_ / 2.0f};
    const auto global = D2D1::Matrix3x2F::Scale(
        D2D1::SizeF(static_cast<float>(global_scale_), static_cast<float>(global_scale_)), center);
    for (std::size_t index = 0; index < visual_cache_.size(); ++index) {
        const auto& cache = visual_cache_[index];
        if (!cache.geometry) continue;
        const float selected_scale = static_cast<float>(selection_scales_[index]);
        const auto selection = D2D1::Matrix3x2F::Scale(
            D2D1::SizeF(selected_scale, selected_scale), cache.selection_origin);
        render_target_->SetTransform(selection * global);
        render_target_->FillGeometry(cache.geometry.Get(), brush_for_slot(slots_[index], static_cast<int>(index) == selected_index_));
        render_target_->SetTransform(global);
        draw_cached_content(index);
    }
    render_target_->SetTransform(global);
    draw_center_disk();
}

void WheelWindow::draw_center_disk() {
    if (!center_brush_) return;
    const float dead = effective_dead_zone_radius();
    render_target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(surface_width_ / 2.0f, surface_height_ / 2.0f),
        dead, dead), center_brush_.Get());
}

void WheelWindow::draw_cached_content(std::size_t index) {
    if (index >= visual_cache_.size() || index >= slots_.size()) return;
    const auto& cache = visual_cache_[index];
    if (cache.has_image_destination && index < image_cache_.size() && image_cache_[index]) {
        if (cache.image_clip && cache.image_layer)
            render_target_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), cache.image_clip.Get()), cache.image_layer.Get());
        render_target_->DrawBitmap(image_cache_[index].Get(), cache.image_destination, 0.96f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        if (cache.image_clip && cache.image_layer) render_target_->PopLayer();
        return;
    }
    if (cache.text_lines.empty()) return;
    if (cache.geometry && cache.content_layer)
        render_target_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), cache.geometry.Get()), cache.content_layer.Get());
    ID2D1SolidColorBrush* brush = slots_[index].entry && slots_[index].entry->is_quick_copy
        ? quick_copy_text_brush_.Get() : text_brush_.Get();
    for (const auto& line : cache.text_lines)
        render_target_->DrawTextLayout(line.origin, line.layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (cache.geometry && cache.content_layer) render_target_->PopLayer();
}

ID2D1SolidColorBrush* WheelWindow::brush_for_slot(
    const smk::core::WheelSlot& slot, bool selected) const noexcept {
    if (slot.entry && slot.entry->is_quick_copy) return quick_copy_brush_.Get();
    if (slot.entry && slot.entry->is_locked) return selected ? locked_hover_brush_.Get() : locked_brush_.Get();
    if (!slot.entry) return empty_brush_.Get();
    return selected ? hover_brush_.Get() : normal_brush_.Get();
}

float WheelWindow::effective_dead_zone_radius() const noexcept {
    const float dpi = static_cast<float>(coordinates_.dpi_scale);
    const float dead = static_cast<float>(settings_.wheel.inner_dead_zone_radius) * dpi;
    if (settings_.wheel.shape != L"rectangle" || slots_.size() <= 4) return dead;
    const float padding = static_cast<float>(settings_.wheel.radius * 0.06) * dpi;
    const float inner_width = static_cast<float>(surface_width_) - padding * 2.0f - 16.0f * dpi;
    const float cell_size = inner_width / 3.0f;
    const float maximum_dead = std::max(dead, cell_size / 2.0f - kRectangleCutoutGap * dpi);
    return std::min(dead * 1.22f, maximum_dead);
}

D2D1_COLOR_F WheelWindow::parse_color(const std::wstring& value, float alpha) const noexcept {
    if (value.size() != 7 || value[0] != L'#') return D2D1::ColorF(D2D1::ColorF::DarkGray, alpha);
    const auto channel = [&](std::size_t offset) { wchar_t pair[3]{value[offset], value[offset + 1], 0};
        return static_cast<float>(wcstoul(pair, nullptr, 16)) / 255.0f; };
    return D2D1::ColorF(channel(1), channel(3), channel(5), alpha);
}

} // namespace smk::ui
