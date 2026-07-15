#pragma once

#include "core/ClipboardEntry.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace smk::core {

class ClipboardHistory final {
public:
    explicit ClipboardHistory(std::size_t capacity = 8);

    void add_or_promote(ClipboardEntry entry);
    void import_backfill(ClipboardEntry entry);
    bool toggle_lock(const std::wstring& id);
    void clear();
    void clear_unlocked();

    [[nodiscard]] const std::vector<ClipboardEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] std::vector<ClipboardEntry> snapshot_for_wheel(std::size_t visible_capacity) const;

private:
    [[nodiscard]] static bool same_payload(const ClipboardEntry& left, const ClipboardEntry& right) noexcept;
    void trim();

    std::size_t capacity_;
    std::vector<ClipboardEntry> entries_;
};

} // namespace smk::core
