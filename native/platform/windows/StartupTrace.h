#pragma once

#include <string_view>

namespace smk::windows {

void startup_trace(std::wstring_view message) noexcept;

} // namespace smk::windows
