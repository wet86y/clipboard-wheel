#pragma once

#include <string>
#include <vector>

namespace smk::windows {

[[nodiscard]] bool is_administrator() noexcept;
[[nodiscard]] bool restart_with_privilege(bool elevated, std::wstring& error);
[[nodiscard]] bool start_unelevated(const std::vector<std::wstring>& arguments, std::wstring& error);

} // namespace smk::windows
