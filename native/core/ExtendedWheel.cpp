#include "core/ExtendedWheel.h"

#include "core/WheelInteraction.h"

#include <algorithm>
#include <cmath>

namespace smk::core {
namespace {
constexpr double kPi = 3.14159265358979323846;
}

bool extended_wheel_available(const AppSettings& settings) noexcept {
    return settings.wheel.shape == L"circle" && settings.wheel.extended_wheel.enabled;
}

double extended_ring_thickness(double wheel_radius) noexcept {
    return std::max(kExtendedRingMinThickness, wheel_radius * 0.44);
}

ExtendedSlotGeometry extended_slot_geometry(int slot_index, double gap_degrees) noexcept {
    slot_index = std::clamp(slot_index, 0, kExtendedSlotCount - 1);
    const auto direction = static_cast<ExtendedDirection>(slot_index / 3);
    const double start = -135.0 + slot_index * 30.0;
    const double gap = std::clamp(gap_degrees * 0.5, 0.0, 4.0);
    return {slot_index, direction, start + gap, start + 30.0 - gap};
}

ExtendedWheelVisualLayout make_extended_wheel_visual_layout(
    VisualPoint center,
    double wheel_radius,
    double ring_gap,
    double ring_thickness,
    double gap_degrees,
    double corner_radius,
    double unit_scale,
    double selected_scale,
    double glow_extent) noexcept {
    ExtendedWheelVisualLayout result{};
    result.center = center;
    unit_scale = std::max(0.01, unit_scale);
    result.ring_inner_radius = std::max(0.0, wheel_radius + ring_gap);
    result.ring_outer_radius = std::max(result.ring_inner_radius + unit_scale, result.ring_inner_radius + ring_thickness);
    result.selected_outer_radius = result.ring_outer_radius * std::max(1.0, selected_scale);
    const double extent = result.selected_outer_radius + std::max(0.0, glow_extent);
    result.bounds = {center.x - extent, center.y - extent, center.x + extent, center.y + extent};

    constexpr double pi = 3.14159265358979323846;
    for (int index = 0; index < kExtendedSlotCount; ++index) {
        auto& slot = result.slots[static_cast<std::size_t>(index)];
        slot.logical = extended_slot_geometry(index, gap_degrees);
        slot.sector = make_circle_sector(center, result.ring_inner_radius, result.ring_outer_radius,
            slot.logical.start_degrees, slot.logical.end_degrees, corner_radius);
        const double middle_angle = (slot.logical.start_degrees + slot.logical.end_degrees) * 0.5 * pi / 180.0;
        const double middle_radius = (result.ring_inner_radius + result.ring_outer_radius) * 0.5;
        slot.content_center = {
            center.x + std::cos(middle_angle) * middle_radius,
            center.y + std::sin(middle_angle) * middle_radius,
        };
        const double sweep = std::abs(slot.logical.end_degrees - slot.logical.start_degrees) * pi / 180.0;
        const double tangential = std::max(unit_scale, 2.0 * middle_radius * std::sin(sweep * 0.5));
        slot.content_width = std::clamp(tangential * 0.82, 42.0 * unit_scale, 96.0 * unit_scale);
        slot.content_height = std::clamp((result.ring_outer_radius - result.ring_inner_radius) * 0.84,
            34.0 * unit_scale, 78.0 * unit_scale);
        slot.text_viewport_width = std::min(72.0 * unit_scale,
            std::max(42.0 * unit_scale, slot.content_width));
        slot.text_height = 18.0 * unit_scale;
        slot.can_show_icon = slot.content_width >= 62.0 * unit_scale
            && slot.content_height >= 50.0 * unit_scale;
        if (slot.can_show_icon) {
            slot.icon_size = std::clamp(
                std::min(slot.content_height - 23.0 * unit_scale, slot.content_width * 0.52),
                26.0 * unit_scale, 40.0 * unit_scale);
        }
    }
    return result;
}

int extended_visual_slot_from_point(
    const ExtendedWheelVisualLayout& layout, double x, double y) noexcept {
    const double dx = x - layout.center.x;
    const double dy = y - layout.center.y;
    const double distance = std::hypot(dx, dy);
    if (distance < layout.ring_inner_radius || distance > layout.ring_outer_radius) return -1;
    const double angle = std::atan2(dy, dx) * 180.0 / kPi;
    for (int index = 0; index < kExtendedSlotCount; ++index) {
        const auto logical = extended_slot_geometry(index, 0.0);
        if (angle_in_range(angle, logical.start_degrees, logical.end_degrees)) return index;
    }
    return -1;
}

std::wstring extended_slot_position_label(int slot_index) {
    slot_index = std::clamp(slot_index, 0, kExtendedSlotCount - 1);
    const wchar_t* direction = slot_index < 3 ? L"上"
        : slot_index < 6 ? L"右" : slot_index < 9 ? L"下" : L"左";
    return direction + std::to_wstring(slot_index % 3 + 1);
}

ExtendedDirection resolve_extended_direction(
    const AppSettings& settings,
    double distance,
    double dx,
    double dy,
    ExtendedDirection current) noexcept {
    if (!extended_wheel_available(settings) || distance <= settings.wheel.radius)
        return ExtendedDirection::none;
    if (distance <= settings.wheel.radius + settings.wheel.extended_wheel.breakout_buffer_pixels)
        return current;
    const double angle = std::atan2(dy, dx) * 180.0 / kPi;
    if (angle >= -45.0 && angle < 45.0) return ExtendedDirection::right;
    if (angle >= 45.0 && angle < 135.0) return ExtendedDirection::down;
    if (angle >= -135.0 && angle < -45.0) return ExtendedDirection::up;
    return ExtendedDirection::left;
}

int hit_test_extended_slot(
    const AppSettings& settings,
    ExtendedDirection direction,
    double dx,
    double dy,
    int previous_slot) noexcept {
    if (!extended_wheel_available(settings) || direction == ExtendedDirection::none) return -1;
    const double angle = std::atan2(dy, dx) * 180.0 / kPi;
    if (previous_slot >= 0 && previous_slot < kExtendedSlotCount) {
        const auto& previous = settings.wheel.extended_wheel.slots[static_cast<std::size_t>(previous_slot)];
        const auto geometry = extended_slot_geometry(previous_slot, 0.0);
        if (previous.configured() && geometry.direction == direction
            && angle_in_range(angle,
                geometry.start_degrees - kExtendedSelectionHysteresisDegrees,
                geometry.end_degrees + kExtendedSelectionHysteresisDegrees))
            return previous_slot;
    }
    const int first = static_cast<int>(direction) * 3;
    for (int slot = first; slot < first + 3; ++slot) {
        if (!settings.wheel.extended_wheel.slots[static_cast<std::size_t>(slot)].configured()) continue;
        const auto geometry = extended_slot_geometry(slot, 0.0);
        if (angle_in_range(angle, geometry.start_degrees, geometry.end_degrees)) return slot;
    }
    return -1;
}

} // namespace smk::core
