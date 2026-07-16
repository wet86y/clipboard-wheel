#pragma once

#include <windows.h>

#include <cstdint>
#include <string_view>

namespace smk::windows {

#if defined(SMK_DIAGNOSTICS)
void crash_handler_initialize() noexcept;
void crash_handler_shutdown() noexcept;
void crash_set_phase(std::wstring_view phase) noexcept;
void crash_set_input_state(bool hook_available, bool capture_ready,
    std::uint32_t generation) noexcept;
void crash_write_emergency(DWORD exception_code) noexcept;
#define SMK_CRASH_INPUT_STATE(hook_available, capture_ready, generation) \
    ::smk::windows::crash_set_input_state((hook_available), (capture_ready), (generation))
#else
inline void crash_handler_initialize() noexcept {}
inline void crash_handler_shutdown() noexcept {}
inline void crash_set_phase(std::wstring_view) noexcept {}
inline void crash_set_input_state(bool, bool, std::uint32_t) noexcept {}
inline void crash_write_emergency(DWORD) noexcept {}
#define SMK_CRASH_INPUT_STATE(hook_available, capture_ready, generation) ((void)0)
#endif

} // namespace smk::windows
