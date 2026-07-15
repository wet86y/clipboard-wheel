#include "core/PastePolicy.h"

namespace smk::core {

PasteMode resolve_paste_mode(const ClipboardEntry& entry, PasteMode requested) noexcept {
    if (requested != PasteMode::smart) return requested;
    if (entry.is_image_content) return PasteMode::formatted;
    if (entry.looks_like_single_cell) return PasteMode::plain_text;
    if (entry.looks_like_spreadsheet && entry.has_formatted_payload()) return PasteMode::formatted;
    return PasteMode::plain_text;
}

} // namespace smk::core
