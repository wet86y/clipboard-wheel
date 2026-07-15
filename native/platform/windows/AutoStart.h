#pragma once

#include <string>

namespace smk::windows {

[[nodiscard]] bool apply_auto_start(bool enabled, const std::wstring& executable_path, std::wstring& error);

} // namespace smk::windows
