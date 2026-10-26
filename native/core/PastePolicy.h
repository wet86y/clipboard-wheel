#pragma once

#include "core/ClipboardEntry.h"

#include <string>
#include <string_view>
#include <optional>

namespace smk::core {

enum class PasteMode { smart, plain_text, formatted };

[[nodiscard]] PasteMode resolve_paste_mode(const ClipboardEntry& entry, PasteMode requested) noexcept;
[[nodiscard]] bool requires_formatted_clipboard_payload(
    const ClipboardEntry& entry, PasteMode effective) noexcept;
[[nodiscard]] bool can_skip_clipboard_write(
    const ClipboardEntry& entry, PasteMode effective, std::wstring_view current_unicode_text,
    std::optional<std::wstring_view> paste_text = std::nullopt) noexcept;
// 清理表格文本；子选项关闭时仅移除首尾换行，保留正文内部换行。
[[nodiscard]] std::wstring clean_spreadsheet_plain_text(std::wstring_view text, bool remove_line_breaks = false);
[[nodiscard]] std::wstring paste_plain_text(
    const ClipboardEntry& entry, bool clean_spreadsheet_text, bool remove_line_breaks = false);

} // namespace smk::core
