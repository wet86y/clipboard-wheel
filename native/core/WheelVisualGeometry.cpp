#include "core/WheelVisualGeometry.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <limits>
#include <utility>

namespace smk::core {
namespace { constexpr double kPi = 3.14159265358979323846; }

namespace {

double estimate_character_width(wchar_t character, double font_size) noexcept {
    if (character == L' ') return font_size * 0.35;
    if (character < 128) return font_size * 0.58;
    return font_size * 0.96;
}

double estimate_text_width(const std::wstring& text, double font_size) noexcept {
    double width = 0.0;
    for (const wchar_t character : text) width += estimate_character_width(character, font_size);
    return width;
}

std::wstring take_preview_line(
    const std::wstring& text, std::size_t& index, double maximum_width, double font_size) {
    const std::size_t start = index;
    double width = 0.0;
    while (index < text.size()) {
        const wchar_t character = text[index];
        if (character == L'\n') { ++index; break; }
        const double next_width = width + estimate_character_width(character, font_size);
        if (next_width > maximum_width && index > start) break;
        width = next_width;
        ++index;
        if (next_width > maximum_width) break;
    }
    std::wstring line = text.substr(start, index - start);
    while (!line.empty() && std::iswspace(line.front())) line.erase(line.begin());
    while (!line.empty() && std::iswspace(line.back())) line.pop_back();
    return line;
}

std::wstring add_ellipsis_to_fit(std::wstring text, double maximum_width, double font_size) {
    constexpr wchar_t ellipsis = L'\u2026';
    const double text_width = std::max(0.0, maximum_width - estimate_character_width(ellipsis, font_size));
    while (!text.empty() && estimate_text_width(text, font_size) > text_width) text.pop_back();
    while (!text.empty() && std::iswspace(text.back())) text.pop_back();
    text.push_back(ellipsis);
    return text;
}

std::vector<std::wstring> wrap_preview_text(
    const std::wstring& text, const std::vector<double>& widths, double font_size) {
    std::vector<std::wstring> lines;
    std::size_t index = 0;
    for (std::size_t line_index = 0; line_index < widths.size() && index < text.size(); ++line_index) {
        while (index < text.size() && (text[index] == L' ' || text[index] == L'\n')) ++index;
        if (index >= text.size()) break;
        auto line = take_preview_line(text, index, widths[line_index], font_size);
        if (line_index + 1 == widths.size() && index < text.size())
            line = add_ellipsis_to_fit(std::move(line), widths[line_index], font_size);
        if (!line.empty()) lines.push_back(std::move(line));
    }
    return lines;
}

VisualPoint weighted_slot_center(const std::vector<TextLineSlot>& slots) noexcept {
    double weighted_x = 0.0, weighted_y = 0.0, total = 0.0;
    for (const auto& slot : slots) {
        const double weight = std::max(1.0, slot.width);
        weighted_x += slot.center.x * weight;
        weighted_y += slot.center.y * weight;
        total += weight;
    }
    return total <= 0.0 ? slots[slots.size() / 2].center : VisualPoint{weighted_x / total, weighted_y / total};
}

VisualPoint clamp_image_center(
    VisualPoint desired, double width, double height, const std::vector<TextLineSlot>& slots,
    double line_height, double available_top, double available_bottom) noexcept {
    const double minimum_y = available_top + height / 2.0;
    const double maximum_y = available_bottom - height / 2.0;
    const double y = minimum_y <= maximum_y
        ? std::clamp(desired.y, minimum_y, maximum_y)
        : (available_top + available_bottom) / 2.0;
    const double half = std::max(line_height / 2.0, height / 2.0 + line_height / 2.0);
    double minimum_x = -std::numeric_limits<double>::infinity();
    double maximum_x = std::numeric_limits<double>::infinity();
    double weighted_x = 0.0, total = 0.0;
    for (const auto& slot : slots) {
        if (std::abs(slot.center.y - y) > half) continue;
        minimum_x = std::max(minimum_x, slot.center.x - slot.width / 2.0 + width / 2.0);
        maximum_x = std::min(maximum_x, slot.center.x + slot.width / 2.0 - width / 2.0);
        const double weight = std::max(1.0, slot.width);
        weighted_x += slot.center.x * weight;
        total += weight;
    }
    double x = total > 0.0 ? weighted_x / total : desired.x;
    if (minimum_x <= maximum_x) x = std::clamp(x, minimum_x, maximum_x);
    return {x, y};
}

std::pair<double, double> fit_image_inside(
    double image_width, double image_height, double maximum_width, double maximum_height) noexcept {
    if (image_width <= 0.0 || image_height <= 0.0 || maximum_width <= 0.0 || maximum_height <= 0.0)
        return {std::max(1.0, maximum_width), std::max(1.0, maximum_height)};
    const double image_aspect = image_width / image_height;
    const double box_aspect = maximum_width / maximum_height;
    return image_aspect >= box_aspect
        ? std::pair{maximum_width, std::max(1.0, maximum_width / image_aspect)}
        : std::pair{std::max(1.0, maximum_height * image_aspect), maximum_height};
}

} // namespace

bool VisualRect::contains(double x, double y) const noexcept {
    return x >= left && x <= right && y >= top && y <= bottom;
}

VisualPoint VisualRect::center() const noexcept { return {(left + right) / 2.0, (top + bottom) / 2.0}; }

VisualPoint point_on_circle(VisualPoint center, double radius, double angle_degrees) noexcept {
    const double angle = angle_degrees * kPi / 180.0;
    return {center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)};
}

CircleSectorGeometry make_circle_sector(
    VisualPoint center, double inner_radius, double outer_radius,
    double start_degrees, double end_degrees, double corner_radius) noexcept {
    CircleSectorGeometry result{};
    result.start_degrees = start_degrees;
    result.end_degrees = end_degrees;
    result.outer_radius = outer_radius;
    result.inner_radius = inner_radius;
    const double sweep = std::abs(end_degrees - start_degrees);
    const double thickness = std::max(0.0, outer_radius - inner_radius);
    const double corner = std::min(corner_radius, thickness / 3.0);
    const double maximum_offset = sweep * 0.22;
    const double outer_offset = std::min(maximum_offset, corner / std::max(1.0, outer_radius) * 180.0 / kPi);
    const double inner_offset = std::min(maximum_offset, corner / std::max(1.0, inner_radius) * 180.0 / kPi);
    result.outer_start = point_on_circle(center, outer_radius, start_degrees + outer_offset);
    result.outer_end = point_on_circle(center, outer_radius, end_degrees - outer_offset);
    result.inner_end = point_on_circle(center, inner_radius, end_degrees - inner_offset);
    result.inner_start = point_on_circle(center, inner_radius, start_degrees + inner_offset);
    result.outer_end_control = point_on_circle(center, outer_radius, end_degrees);
    result.outer_start_control = point_on_circle(center, outer_radius, start_degrees);
    result.inner_end_control = point_on_circle(center, inner_radius, end_degrees);
    result.inner_start_control = point_on_circle(center, inner_radius, start_degrees);
    result.end_outer_radial = point_on_circle(center, outer_radius - corner, end_degrees);
    result.end_inner_radial = point_on_circle(center, inner_radius + corner, end_degrees);
    result.start_inner_radial = point_on_circle(center, inner_radius + corner, start_degrees);
    result.start_outer_radial = point_on_circle(center, outer_radius - corner, start_degrees);
    result.large_outer_arc = std::abs((end_degrees - outer_offset) - (start_degrees + outer_offset)) > 180.0;
    result.large_inner_arc = std::abs((end_degrees - inner_offset) - (start_degrees + inner_offset)) > 180.0;
    result.centroid = point_on_circle(center, (inner_radius + outer_radius) / 2.0, (start_degrees + end_degrees) / 2.0);
    return result;
}

ConcentricSectorMetrics make_concentric_sector_metrics(
    double inner_radius, double outer_radius, double sweep_degrees, double visual_scale) noexcept {
    visual_scale = std::clamp(visual_scale, 0.01, 1.0);
    const double thickness_inset = std::max(0.0, outer_radius - inner_radius) * (1.0 - visual_scale) / 2.0;
    const double half_sweep = std::clamp(std::abs(sweep_degrees) / 2.0, 0.0, 89.9) * kPi / 180.0;
    const double middle_radius = (inner_radius + outer_radius) / 2.0;
    return {
        inner_radius + thickness_inset,
        std::max(inner_radius + thickness_inset + 1.0, outer_radius - thickness_inset),
        std::max(0.0, (1.0 - visual_scale) * middle_radius * std::sin(half_sweep)),
    };
}

std::wstring normalize_preview_text(const std::wstring& plain_text, const std::wstring& display_text) {
    std::wstring text = plain_text.find_first_not_of(L" \t\r\n") != std::wstring::npos ? plain_text : display_text;
    std::wstring normalized;
    normalized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const wchar_t character = text[index];
        if (character == L'\r') {
            if (index + 1 < text.size() && text[index + 1] == L'\n') ++index;
            normalized.push_back(L'\n');
        } else if (character == L'\t') {
            normalized.append(4, L' ');
        } else {
            normalized.push_back(character);
        }
    }
    while (!normalized.empty() && std::iswspace(normalized.front())) normalized.erase(normalized.begin());
    while (!normalized.empty() && std::iswspace(normalized.back())) normalized.pop_back();
    return normalized;
}

WrappedPreviewText wrap_preview_text_centered(
    const std::wstring& text, const std::vector<double>& widths, double font_size) {
    WrappedPreviewText result{};
    result.lines = wrap_preview_text(text, widths, font_size);
    if (result.lines.empty()) return result;
    for (int attempt = 0; attempt < 6; ++attempt) {
        const std::size_t next_first = (widths.size() - std::min(widths.size(), result.lines.size())) / 2;
        if (next_first == result.first_slot) break;
        result.first_slot = next_first;
        std::vector<double> sliced(widths.begin() + static_cast<std::ptrdiff_t>(result.first_slot), widths.end());
        result.lines = wrap_preview_text(text, sliced, font_size);
        if (result.lines.empty()) { result.first_slot = 0; break; }
    }
    return result;
}

ImagePreviewLayout compute_sector_image_layout(
    double image_width, double image_height, const std::vector<TextLineSlot>& slots,
    double line_height, double inner_radius, double outer_radius,
    double start_degrees, double end_degrees, double image_inset) noexcept {
    if (slots.empty()) return {};
    image_inset = std::max(0.0, image_inset);
    VisualPoint center = weighted_slot_center(slots);
    const double available_top = slots.front().center.y - line_height / 2.0;
    const double available_bottom = slots.back().center.y + line_height / 2.0;
    const double safe_inner = inner_radius + image_inset;
    const double safe_outer = std::max(safe_inner + 1.0, outer_radius - image_inset);
    const double safe_thickness = std::max(line_height, safe_outer - safe_inner);
    const double middle_radius = (safe_inner + safe_outer) / 2.0;
    const double sweep = std::abs(end_degrees - start_degrees);
    const double tangential_width = std::max(
        line_height, 2.0 * middle_radius * std::sin(sweep * kPi / 360.0) - image_inset * 2.0);
    const double dense_boost = sweep <= 60.1 ? 1.2 : 1.0;
    const double maximum_height = safe_thickness * 0.96 * dense_boost;
    const double maximum_width = tangential_width * 0.92 * dense_boost;
    auto [width, height] = fit_image_inside(image_width, image_height, maximum_width, maximum_height);
    for (int iteration = 0; iteration < 4; ++iteration)
        center = clamp_image_center(center, width, height, slots, line_height, available_top, available_bottom);
    return {center, width, height};
}

std::vector<VisualRect> make_rectangle_slots(
    VisualPoint center, double radius, int sector_count, double gap) noexcept {
    const double left = center.x - radius;
    const double top = center.y - radius;
    std::vector<VisualRect> slots;
    if (sector_count <= 4) {
        const double cell = radius;
        const int order[4][2]{{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        for (const auto& item : order) {
            const double x = left + item[1] * cell;
            const double y = top + item[0] * cell;
            slots.push_back({x + gap / 2.0, y + gap / 2.0, x + cell - gap / 2.0, y + cell - gap / 2.0});
        }
        return slots;
    }
    const double cell = radius * 2.0 / 3.0;
    const int order[8][2]{{0, 1}, {0, 2}, {1, 2}, {2, 2}, {2, 1}, {2, 0}, {1, 0}, {0, 0}};
    for (const auto& item : order) {
        const double x = left + item[1] * cell;
        const double y = top + item[0] * cell;
        slots.push_back({x + gap / 2.0, y + gap / 2.0, x + cell - gap / 2.0, y + cell - gap / 2.0});
    }
    return slots;
}

int rectangle_slot_from_point(
    double dx, double dy, double radius, double dead_zone,
    int sector_count, double gap) noexcept {
    const double effective_dead = sector_count >= 8 ? dead_zone * 1.22 : dead_zone;
    if (std::hypot(dx, dy) < effective_dead || std::abs(dx) > radius || std::abs(dy) > radius) return -1;
    const auto slots = make_rectangle_slots({0.0, 0.0}, radius, sector_count, gap);
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (slots[index].contains(dx, dy)) return static_cast<int>(index);
    }
    return -1;
}

} // namespace smk::core
