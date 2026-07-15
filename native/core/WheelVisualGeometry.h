#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace smk::core {

struct VisualPoint { double x = 0.0; double y = 0.0; };

struct VisualRect {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    [[nodiscard]] bool contains(double x, double y) const noexcept;
    [[nodiscard]] VisualPoint center() const noexcept;
};

struct CircleSectorGeometry {
    double start_degrees = 0.0;
    double end_degrees = 0.0;
    double outer_radius = 0.0;
    double inner_radius = 0.0;
    bool large_outer_arc = false;
    bool large_inner_arc = false;
    VisualPoint outer_start;
    VisualPoint outer_end;
    VisualPoint outer_end_control;
    VisualPoint end_outer_radial;
    VisualPoint end_inner_radial;
    VisualPoint inner_end_control;
    VisualPoint inner_end;
    VisualPoint inner_start;
    VisualPoint inner_start_control;
    VisualPoint start_inner_radial;
    VisualPoint start_outer_radial;
    VisualPoint outer_start_control;
    VisualPoint centroid;
};

struct ConcentricSectorMetrics {
    double inner_radius = 0.0;
    double outer_radius = 0.0;
    double side_inset = 0.0;
};

struct TextLineSlot {
    VisualPoint center;
    double width = 0.0;
};

struct WrappedPreviewText {
    std::size_t first_slot = 0;
    std::vector<std::wstring> lines;
};

struct ImagePreviewLayout {
    VisualPoint center;
    double width = 0.0;
    double height = 0.0;
};

[[nodiscard]] VisualPoint point_on_circle(VisualPoint center, double radius, double angle_degrees) noexcept;
[[nodiscard]] CircleSectorGeometry make_circle_sector(
    VisualPoint center, double inner_radius, double outer_radius,
    double start_degrees, double end_degrees, double corner_radius) noexcept;
[[nodiscard]] ConcentricSectorMetrics make_concentric_sector_metrics(
    double inner_radius, double outer_radius, double sweep_degrees,
    double visual_scale = 0.9) noexcept;
[[nodiscard]] std::wstring normalize_preview_text(
    const std::wstring& plain_text, const std::wstring& display_text);
[[nodiscard]] WrappedPreviewText wrap_preview_text_centered(
    const std::wstring& text, const std::vector<double>& widths, double font_size);
[[nodiscard]] ImagePreviewLayout compute_sector_image_layout(
    double image_width, double image_height, const std::vector<TextLineSlot>& slots,
    double line_height, double inner_radius, double outer_radius,
    double start_degrees, double end_degrees, double image_inset = 10.0) noexcept;
[[nodiscard]] std::vector<VisualRect> make_rectangle_slots(
    VisualPoint center, double radius, int sector_count, double gap_pixels) noexcept;
[[nodiscard]] int rectangle_slot_from_point(
    double dx, double dy, double radius, double dead_zone,
    int sector_count, double gap_pixels = 0.0) noexcept;

} // namespace smk::core
