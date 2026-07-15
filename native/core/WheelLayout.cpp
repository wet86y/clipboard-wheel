#include "core/WheelLayout.h"
#include "core/WheelInteraction.h"
#include "core/WheelVisualGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>

namespace smk::core {
namespace {

bool is_rectangle(const std::wstring& shape) {
    const std::wstring expected = L"rectangle";
    return shape.size() == expected.size() && std::equal(shape.begin(), shape.end(), expected.begin(), [](wchar_t a, wchar_t b) {
        return std::towlower(a) == std::towlower(b);
    });
}

} // namespace

std::wstring normalize_shape(const std::wstring& shape) {
    return is_rectangle(shape) ? L"rectangle" : L"circle";
}

int normalize_sector_count(const std::wstring& shape, int requested) noexcept {
    if (is_rectangle(shape)) return std::abs(requested - 4) <= std::abs(requested - 8) ? 4 : 8;
    constexpr std::array<int, 3> tiers{4, 6, 8};
    return *std::min_element(tiers.begin(), tiers.end(), [requested](int left, int right) {
        return std::abs(left - requested) < std::abs(right - requested);
    });
}

int sector_index_from_point(
    const std::wstring& shape,
    int sector_count,
    double radius,
    double dead_zone,
    double dx,
    double dy,
    int previous_index) noexcept {
    sector_count = normalize_sector_count(shape, sector_count);
    if (is_rectangle(shape)) {
        return rectangle_slot_from_point(dx, dy, radius, dead_zone, sector_count);
    }

    return hit_test_circle(sector_count, dead_zone, dx, dy, previous_index);
}

} // namespace smk::core
