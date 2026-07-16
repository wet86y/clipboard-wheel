#pragma once

#include <string_view>

namespace smk::windows {

#if defined(SMK_DIAGNOSTICS)
void startup_trace(std::wstring_view message) noexcept;
#define SMK_STARTUP_TRACE(message) ::smk::windows::startup_trace((message))
#else
#define SMK_STARTUP_TRACE(message) ((void)0)
#endif

} // namespace smk::windows
