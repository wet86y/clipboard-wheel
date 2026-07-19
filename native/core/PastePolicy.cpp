#include "core/PastePolicy.h"

#include <cwctype>

namespace smk::core {

namespace {

bool is_removed_horizontal_space(wchar_t value) noexcept {
    if (value == L'\r' || value == L'\n') return false;
    if (value == L' ' || value == L'\t' || value == L'\v' || value == L'\f') return true;
    if (value == 0x00A0 || value == 0x1680 || value == 0x180E || value == 0x202F
        || value == 0x205F || value == 0x3000 || value == 0xFEFF) return true;
    return value >= 0x2000 && value <= 0x200A;
}

bool is_removed_quote(wchar_t value) noexcept {
    return value == L'\"' || value == L'\'' || value == 0x2018 || value == 0x2019
        || value == 0x201C || value == 0x201D || value == 0xFF02 || value == 0xFF07;
}

} // namespace

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
    const ClipboardEntry& entry, PasteMode effective, std::wstring_view current_unicode_text,
    std::optional<std::wstring_view> paste_text) noexcept {
    if (requires_formatted_clipboard_payload(entry, effective)) return false;
    const auto& target = entry.plain_text.empty() ? entry.display_text : entry.plain_text;
    const auto expected = paste_text.value_or(std::wstring_view(target));
    return !expected.empty() && current_unicode_text == expected;
}

std::wstring clean_spreadsheet_plain_text(std::wstring_view text) {
    std::wstring result;
    result.reserve(text.size());
    for (const wchar_t value : text) {
        if (!is_removed_horizontal_space(value) && !is_removed_quote(value)) result.push_back(value);
    }
    return result;
}

std::wstring paste_plain_text(const ClipboardEntry& entry, bool clean_spreadsheet_text) {
    const auto& source = entry.plain_text.empty() ? entry.display_text : entry.plain_text;
    return clean_spreadsheet_text && entry.looks_like_spreadsheet
        ? clean_spreadsheet_plain_text(source)
        : source;
}

} // namespace smk::core
