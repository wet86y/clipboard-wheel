#pragma once

#include <windows.h>

#include <string>

namespace smk::windows {

// Resolves the icon represented by a shortcut. The returned icon is owned by
// the caller and must be released with DestroyIcon.
[[nodiscard]] HICON resolve_shortcut_icon(
    const std::wstring& shortcut_path,
    int desired_size_pixels = 32) noexcept;

// Primarily used by deterministic tests and device-lifetime boundaries.
void clear_shortcut_icon_cache() noexcept;

} // namespace smk::windows
