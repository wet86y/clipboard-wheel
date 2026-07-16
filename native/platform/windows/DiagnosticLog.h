#pragma once

#include <string>
#include <string_view>

namespace smk::windows {

#if defined(SMK_DIAGNOSTICS)
void diagnostic_initialize() noexcept;
void diagnostic_shutdown() noexcept;
void diagnostic_event(std::string_view event, std::wstring_view details = {}) noexcept;
[[nodiscard]] std::wstring diagnostic_log_path();
#else
inline void diagnostic_initialize() noexcept {}
inline void diagnostic_shutdown() noexcept {}
inline void diagnostic_event(std::string_view, std::wstring_view = {}) noexcept {}
[[nodiscard]] inline std::wstring diagnostic_log_path() { return {}; }
#endif

} // namespace smk::windows

#if defined(SMK_DIAGNOSTICS)
#define SMK_DIAGNOSTIC_EVENT(event_name, event_details) ::smk::windows::diagnostic_event((event_name), (event_details))
#else
#define SMK_DIAGNOSTIC_EVENT(event_name, event_details) ((void)0)
#endif
