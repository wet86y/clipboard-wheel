#pragma once

#include "core/Animation.h"
#include "core/ExtendedWheel.h"
#include "core/Settings.h"
#include "core/WheelInteraction.h"
#include "core/WheelSelection.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace smk::ui {

class WheelWindow final {
public:
    WheelWindow() = default;
    ~WheelWindow();
    bool create(HINSTANCE instance);
    [[nodiscard]] bool show(POINT center, std::vector<smk::core::WheelSlot> slots,
        const smk::core::AppSettings& settings);
    void update_pointer(POINT screen_point);
    void hide();
    void hide_immediately();
    void set_failure_callback(std::function<void()> callback) {
        failure_callback_ = std::move(callback);
    }
    [[nodiscard]] bool visible() const noexcept { return visible_ && !hiding_; }
    [[nodiscard]] HRESULT creation_error() const noexcept { return creation_error_; }
    [[nodiscard]] int selected_index() const noexcept { return selected_index_; }
    [[nodiscard]] int selected_extended_slot() const noexcept { return selected_extended_slot_; }
    [[nodiscard]] smk::core::ExtendedDirection active_extended_direction() const noexcept {
        return active_extended_direction_;
    }
    [[nodiscard]] const smk::core::ClipboardEntry* selected_entry() const noexcept;
    [[nodiscard]] smk::core::WheelSelection selection() const;
    bool refresh_selected_lock(bool locked);
    [[nodiscard]] std::size_t nontransparent_slot_count_for_testing() const noexcept;
    [[nodiscard]] std::size_t cached_text_line_count_for_testing() const noexcept;
    [[nodiscard]] std::size_t cached_image_preview_count_for_testing() const noexcept;
    [[nodiscard]] std::size_t multiline_text_layout_count_for_testing() const noexcept;
    [[nodiscard]] std::size_t visible_extended_slot_count_for_testing() const noexcept;
    [[nodiscard]] std::size_t cached_extended_icon_count_for_testing() const noexcept;
    [[nodiscard]] bool extended_slot_has_marquee_for_testing(int slot_index) const noexcept;
    [[nodiscard]] std::uint32_t rendered_frame_count_for_testing() const noexcept { return rendered_frames_; }

private:
    static constexpr UINT_PTR kAnimationTimer = 1;
    static constexpr UINT kAnimationWakeMs = 4;
    static constexpr double kAnimationFrameMs = 1000.0 / 60.0;
    static constexpr UINT kRenderMessage = WM_APP + 72;
    static constexpr UINT kAnimationTickMessage = WM_APP + 73;

    struct CachedTextLine {
        D2D1_POINT_2F origin{};
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    };

    struct SlotVisualCache {
        Microsoft::WRL::ComPtr<ID2D1Geometry> geometry;
        Microsoft::WRL::ComPtr<ID2D1Geometry> image_clip;
        Microsoft::WRL::ComPtr<ID2D1Layer> content_layer;
        Microsoft::WRL::ComPtr<ID2D1Layer> image_layer;
        std::vector<CachedTextLine> text_lines;
        D2D1_RECT_F image_destination{};
        D2D1_POINT_2F selection_origin{};
        bool has_image_destination = false;
    };

    struct ExtendedVisualCache {
        Microsoft::WRL::ComPtr<ID2D1Geometry> geometry;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> icon;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> text_layout;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> marquee_layout;
        D2D1_RECT_F icon_destination{};
        D2D1_RECT_F text_viewport{};
        D2D1_POINT_2F content_center{};
        float marquee_cycle_width = 0.0f;
        bool configured = false;
    };

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
    bool ensure_surface(UINT width, UINT height);
    void discard_surface();
    void rebuild_image_cache();
    void rebuild_visual_cache();
    void rebuild_extended_visual_cache();
    bool rebuild_brush_cache();
    [[nodiscard]] bool render_frame();
    [[nodiscard]] bool render_or_fail();
    void update_animation();
    void ensure_animation_timer();
    void stop_animation_timer();
    [[nodiscard]] bool animation_active() const noexcept;
    [[nodiscard]] double now_ms() const noexcept;
    [[nodiscard]] UINT dpi_for_point(POINT point) const noexcept;

    Microsoft::WRL::ComPtr<ID2D1PathGeometry> create_sector_geometry(
        D2D1_POINT_2F center,
        float inner_radius,
        float outer_radius,
        float start_angle,
        float end_angle,
        float corner_radius) const;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> create_parallel_inset_sector_geometry(
        D2D1_POINT_2F center, float inner_radius, float outer_radius,
        float start_angle, float end_angle, float side_inset) const;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> create_parallel_rounded_sector_geometry(
        D2D1_POINT_2F center, float inner_radius, float outer_radius,
        float start_angle, float end_angle, float side_inset, float corner_radius) const;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> create_rectangle_sector_geometry(
        const D2D1_RECT_F& rectangle, D2D1_POINT_2F center,
        float cutout_radius, float corner_radius) const;
    void draw_circle();
    void draw_extended_actions();
    void draw_rectangle();
    void draw_center_disk();
    void draw_cached_content(std::size_t index);
    [[nodiscard]] ID2D1SolidColorBrush* brush_for_slot(
        const smk::core::WheelSlot& slot, bool selected) const noexcept;
    [[nodiscard]] float effective_dead_zone_radius() const noexcept;
    void update_selection(int main_index, int extended_index, POINT screen_point, double dx, double dy);
    void set_active_extended_direction(smk::core::ExtendedDirection direction);
    [[nodiscard]] D2D1_COLOR_F parse_color(const std::wstring& value, float alpha = 1.0f) const noexcept;

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HDC memory_dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ previous_bitmap_ = nullptr;
    void* dib_pixels_ = nullptr;
    UINT surface_width_ = 0;
    UINT surface_height_ = 0;
    POINT center_screen_{};
    POINT window_origin_{};
    smk::core::WheelCoordinateSpace coordinates_{};
    smk::core::AppSettings settings_{};
    std::vector<smk::core::WheelSlot> slots_;
    std::vector<Microsoft::WRL::ComPtr<ID2D1Bitmap>> image_cache_;
    std::vector<SlotVisualCache> visual_cache_;
    std::vector<ExtendedVisualCache> extended_visual_cache_;
    smk::core::ExtendedWheelVisualLayout extended_visual_layout_{};
    std::vector<smk::core::AnimationTrack> selection_tracks_;
    std::vector<double> selection_scales_;
    std::vector<smk::core::AnimationTrack> extended_visibility_tracks_;
    std::vector<smk::core::AnimationTrack> extended_visibility_scale_tracks_;
    std::vector<smk::core::AnimationTrack> extended_selection_tracks_;
    std::vector<double> extended_visibilities_;
    std::vector<double> extended_visibility_scales_;
    std::vector<double> extended_selection_scales_;
    int selected_index_ = -1;
    int selected_extended_slot_ = -1;
    smk::core::ExtendedDirection active_extended_direction_ = smk::core::ExtendedDirection::none;
    double extended_selection_started_ms_ = -1.0;
    bool visible_ = false;
    bool hiding_ = false;
    std::uint64_t visual_generation_ = 0;
    std::uint32_t rendered_frames_ = 0;
    double global_scale_ = 1.0;
    double global_opacity_ = 1.0;
    double show_started_ms_ = -1.0;
    double hide_started_ms_ = -1.0;
    double selection_started_ms_ = -1.0;
    unsigned animation_milestones_ = 0;
    smk::core::AnimationTrack global_scale_track_{};
    smk::core::AnimationTrack global_opacity_track_{};
    LARGE_INTEGER performance_frequency_{};
    double next_animation_frame_ms_ = -1.0;
    double last_animation_frame_ms_ = -1.0;
    std::uint32_t skipped_animation_frames_ = 0;
    std::atomic_bool animation_timer_running_ = false;
    bool fallback_window_timer_active_ = false;
    bool high_resolution_timer_active_ = false;
    bool waitable_timer_high_resolution_ = false;
    HANDLE animation_waitable_timer_ = nullptr;
    HANDLE animation_shutdown_event_ = nullptr;
    std::thread animation_timer_thread_;
    HRESULT creation_error_ = S_OK;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_factory_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> render_target_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> text_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> quick_copy_text_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> extended_text_format_;
    Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis_sign_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> normal_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> locked_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> locked_hover_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> quick_copy_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> empty_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> center_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> quick_copy_text_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> extended_normal_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> extended_configured_brush_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> extended_selected_brush_;
    std::function<void()> failure_callback_;
};

} // namespace smk::ui
