#pragma once

#include <windows.h>

#include <optional>
#include <string>

namespace smk::windows {

[[nodiscard]] int run_shortcut_drop_helper(HINSTANCE instance, const std::wstring& handoff_id);
[[nodiscard]] bool launch_shortcut_drop_helper(const std::wstring& handoff_id, std::wstring& error);
[[nodiscard]] std::optional<std::wstring> read_shortcut_drop_result(const std::wstring& handoff_id);
void delete_shortcut_drop_result(const std::wstring& handoff_id) noexcept;

} // namespace smk::windows
