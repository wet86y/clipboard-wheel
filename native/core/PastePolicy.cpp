#include "core/PastePolicy.h"

#include <cwctype>

namespace smk::core {

PasteMode resolve_paste_mode(const ClipboardEntry& entry, PasteMode requested) noexcept {
    if (requested != PasteMode::smart) return requested;
    if (entry.is_image_content) return PasteMode::formatted;
    constexpr std::wstring_view table = L"<table";
    bool has_table_html = false;
    for (std::size_t index = 0; index + table.size() <= entry.html_text.size(); ++index) {
        bool match = true;
        for (std::size_t offset = 0; offset < table.size(); ++offset) {
            if (std::towlower(entry.html_text[index + offset]) != table[offset]) { match = false; break; }
        }
        if (match) { has_table_html = true; break; }
    }
    return entry.looks_like_single_cell && !has_table_html
        ? PasteMode::plain_text
        : PasteMode::formatted;
}

bool requires_formatted_clipboard_payload(const ClipboardEntry& entry, PasteMode effective) noexcept {
    return entry.is_image_content || (effective == PasteMode::formatted
        && (!entry.html_text.empty() || !entry.rtf_text.empty() || !entry.csv_text.empty()));
}

bool can_skip_clipboard_write(
    const ClipboardEntry& entry, PasteMode effective, std::wstring_view current_unicode_text) noexcept {
    if (requires_formatted_clipboard_payload(entry, effective)) return false;
    const auto& target = entry.plain_text.empty() ? entry.display_text : entry.plain_text;
    return !target.empty() && current_unicode_text == target;
}

} // namespace smk::core
