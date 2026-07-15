#pragma once

#include "core/ClipboardEntry.h"

namespace smk::core {

enum class PasteMode { smart, plain_text, formatted };

[[nodiscard]] PasteMode resolve_paste_mode(const ClipboardEntry& entry, PasteMode requested) noexcept;

} // namespace smk::core
