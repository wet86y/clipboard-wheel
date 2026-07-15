#pragma once

#include <windows.h>

#include <cstdint>
#include <string_view>

namespace smk::windows {

void crash_handler_initialize() noexcept;
void crash_handler_shutdown() noexcept;
void crash_set_phase(std::wstring_view phase) noexcept;
void crash_set_input_state(bool hook_available, bool capture_ready,
    std::uint32_t generation) noexcept;
void crash_write_emergency(DWORD exception_code) noexcept;

} // namespace smk::windows
