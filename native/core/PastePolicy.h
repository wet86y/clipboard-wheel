#pragma once

#include "core/ClipboardEntry.h"

namespace smk::core {

enum class PasteMode { smart, plain_text, formatted };

[[nodiscard]] PasteMode resolve_paste_mode(const ClipboardEntry& entry, PasteMode requested) noexcept;
[[nodiscard]] bool requires_formatted_clipboard_payload(
    const ClipboardEntry& entry, PasteMode effective) noexcept;
[[nodiscard]] bool can_skip_clipboard_write(
    const ClipboardEntry& entry, PasteMode effective, std::wstring_view current_unicode_text) noexcept;

} // namespace smk::core
