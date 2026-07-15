#include "core/WheelInteraction.h"

#include <algorithm>
#include <cmath>

namespace smk::core {
namespace {

constexpr double kPi = 3.14159265358979323846;

double normalize_degrees(double value) noexcept {
    value = std::fmod(value, 360.0);
    if (value < 0.0) value += 360.0;
    return value;
}

} // namespace

double WheelCoordinateSpace::logical_dx(double physical_x) const noexcept {
    return (physical_x - center_physical_x) / std::max(0.01, dpi_scale);
}

double WheelCoordinateSpace::logical_dy(double physical_y) const noexcept {
    return (physical_y - center_physical_y) / std::max(0.01, dpi_scale);
}

double WheelCoordinateSpace::physical(double logical_value) const noexcept {
    return logical_value * std::max(0.01, dpi_scale);
}

std::vector<WheelSlot> build_wheel_slots(
    const std::vector<ClipboardEntry>& history_entries,
    int resolved_sector_count,
    bool quick_copy) {
    const auto count = static_cast<std::size_t>(std::max(1, resolved_sector_count));
    std::vector<WheelSlot> slots(count);
    const std::size_t history_capacity = count - (quick_copy ? 1U : 0U);
    for (std::size_t index = 0; index < history_entries.size() && index < history_capacity; ++index) {
        slots[index].entry = history_entries[index];
    }
    if (quick_copy) {
        ClipboardEntry entry;
        entry.id = L"quick-copy";
        entry.display_text = L"复制";
        entry.is_quick_copy = true;
        slots.back().entry = std::move(entry);
    }
    return slots;
}

bool angle_in_range(double angle, double start, double end) noexcept {
    angle = normalize_degrees(angle);
    start = normalize_degrees(start);
    end = normalize_degrees(end);
    if (start <= end) return angle >= start && angle <= end;
    return angle >= start || angle <= end;
}

int hit_test_circle(
    int sector_count,
    double dead_zone_radius,
    double dx,
    double dy,
    int previous_index,
    double hysteresis_degrees) noexcept {
    if (sector_count <= 0 || std::hypot(dx, dy) < dead_zone_radius) return -1;
    double angle = std::atan2(dy, dx) * 180.0 / kPi;
    angle = normalize_degrees(angle + 450.0);
    const double sector_angle = 360.0 / sector_count;
    int index = std::clamp(static_cast<int>(std::floor(angle / sector_angle)), 0, sector_count - 1);
    if (previous_index >= 0 && previous_index < sector_count && previous_index != index
        && angle_in_range(
            angle,
            previous_index * sector_angle - hysteresis_degrees,
            (previous_index + 1) * sector_angle + hysteresis_degrees)) {
        return previous_index;
    }
    return index;
}

} // namespace smk::core
