#pragma once

#include "core/ClipboardEntry.h"

#include <optional>
#include <string>
#include <vector>

namespace smk::core {

inline constexpr double kMainSelectionHysteresisDegrees = 2.5;

struct WheelSlot {
    std::optional<ClipboardEntry> entry;

    [[nodiscard]] bool selectable() const noexcept { return entry.has_value(); }
};

struct WheelCoordinateSpace {
    double center_physical_x = 0.0;
    double center_physical_y = 0.0;
    double dpi_scale = 1.0;

    [[nodiscard]] double logical_dx(double physical_x) const noexcept;
    [[nodiscard]] double logical_dy(double physical_y) const noexcept;
    [[nodiscard]] double physical(double logical_value) const noexcept;
};

[[nodiscard]] std::vector<WheelSlot> build_wheel_slots(
    const std::vector<ClipboardEntry>& history_entries,
    int resolved_sector_count,
    bool quick_copy);

[[nodiscard]] int hit_test_circle(
    int sector_count,
    double dead_zone_radius,
    double dx,
    double dy,
    int previous_index = -1,
    double hysteresis_degrees = kMainSelectionHysteresisDegrees) noexcept;

[[nodiscard]] bool angle_in_range(double angle, double start, double end) noexcept;

} // namespace smk::core
