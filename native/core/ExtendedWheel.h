#pragma once

#include "core/Settings.h"
#include "core/WheelVisualGeometry.h"

#include <array>
#include <string>

namespace smk::core {

inline constexpr double kExtendedRingGap = 4.0;
inline constexpr double kExtendedRingMinThickness = 52.0;
inline constexpr double kExtendedSelectionHysteresisDegrees = 3.0;

enum class ExtendedDirection : int { none = -1, up = 0, right = 1, down = 2, left = 3 };

struct ExtendedSlotGeometry {
    int slot_index = -1;
    ExtendedDirection direction = ExtendedDirection::none;
    double start_degrees = 0.0;
    double end_degrees = 0.0;
};

struct ExtendedSlotVisualLayout {
    ExtendedSlotGeometry logical{};
    CircleSectorGeometry sector{};
    VisualPoint content_center{};
    double content_width = 0.0;
    double content_height = 0.0;
    double text_viewport_width = 0.0;
    double text_height = 0.0;
    double icon_size = 0.0;
    bool can_show_icon = false;
};

struct ExtendedWheelVisualLayout {
    VisualPoint center{};
    double ring_inner_radius = 0.0;
    double ring_outer_radius = 0.0;
    double selected_outer_radius = 0.0;
    VisualRect bounds{};
    std::array<ExtendedSlotVisualLayout, kExtendedSlotCount> slots{};
};

[[nodiscard]] bool extended_wheel_available(const AppSettings& settings) noexcept;
[[nodiscard]] double extended_ring_thickness(double wheel_radius) noexcept;
[[nodiscard]] ExtendedSlotGeometry extended_slot_geometry(int slot_index, double gap_degrees) noexcept;
[[nodiscard]] ExtendedWheelVisualLayout make_extended_wheel_visual_layout(
    VisualPoint center,
    double wheel_radius,
    double ring_gap,
    double ring_thickness,
    double gap_degrees,
    double corner_radius,
    double unit_scale = 1.0,
    double selected_scale = 1.04,
    double glow_extent = 6.0) noexcept;
[[nodiscard]] int extended_visual_slot_from_point(
    const ExtendedWheelVisualLayout& layout, double x, double y) noexcept;
[[nodiscard]] std::wstring extended_slot_position_label(int slot_index);
[[nodiscard]] ExtendedDirection resolve_extended_direction(
    const AppSettings& settings,
    double distance,
    double dx,
    double dy,
    ExtendedDirection current) noexcept;
[[nodiscard]] int hit_test_extended_slot(
    const AppSettings& settings,
    ExtendedDirection direction,
    double dx,
    double dy,
    int previous_slot = -1) noexcept;

} // namespace smk::core
