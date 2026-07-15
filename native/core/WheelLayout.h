#pragma once

#include <string>

namespace smk::core {

[[nodiscard]] std::wstring normalize_shape(const std::wstring& shape);
[[nodiscard]] int normalize_sector_count(const std::wstring& shape, int requested) noexcept;
[[nodiscard]] int sector_index_from_point(
    const std::wstring& shape,
    int sector_count,
    double radius,
    double dead_zone,
    double dx,
    double dy,
    int previous_index = -1) noexcept;

} // namespace smk::core
