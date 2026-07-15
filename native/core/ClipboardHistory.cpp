#include "core/ClipboardHistory.h"

#include <algorithm>
#include <optional>

namespace smk::core {

ClipboardHistory::ClipboardHistory(std::size_t capacity) : capacity_(std::max<std::size_t>(1, capacity)) {}

bool ClipboardHistory::same_payload(const ClipboardEntry& left, const ClipboardEntry& right) noexcept {
    if (!left.image_hash.empty() || !right.image_hash.empty()) return left.image_hash == right.image_hash;
    return left.plain_text == right.plain_text && left.html_text == right.html_text && left.rtf_text == right.rtf_text
        && left.csv_text == right.csv_text && left.tsv_text == right.tsv_text;
}

void ClipboardHistory::add_or_promote(ClipboardEntry entry) {
    const auto duplicate = std::find_if(entries_.begin(), entries_.end(), [&](const ClipboardEntry& candidate) {
        return same_payload(candidate, entry);
    });
    if (duplicate != entries_.end()) {
        if (duplicate->is_locked) return;
    }

    std::vector<std::optional<ClipboardEntry>> slots(capacity_);
    for (std::size_t index = 0; index < entries_.size() && index < capacity_; ++index) {
        if (entries_[index].is_locked) slots[index] = entries_[index];
    }

    const auto new_slot = std::find_if(slots.begin(), slots.end(), [](const auto& slot) { return !slot.has_value(); });
    if (new_slot != slots.end()) *new_slot = std::move(entry);

    for (const auto& existing : entries_) {
        if (existing.is_locked || (duplicate != entries_.end() && existing.id == duplicate->id)) continue;
        const auto empty = std::find_if(slots.begin(), slots.end(), [](const auto& slot) { return !slot.has_value(); });
        if (empty == slots.end()) break;
        *empty = existing;
    }

    entries_.clear();
    for (auto& slot : slots) {
        if (slot) entries_.push_back(std::move(*slot));
    }
}

void ClipboardHistory::import_backfill(ClipboardEntry entry) {
    if (entries_.size() >= capacity_) return;
    if (std::any_of(entries_.begin(), entries_.end(), [&](const ClipboardEntry& candidate) {
            return same_payload(candidate, entry);
        })) return;
    entries_.push_back(std::move(entry));
}

bool ClipboardHistory::toggle_lock(const std::wstring& id) {
    const auto item = std::find_if(entries_.begin(), entries_.end(), [&](const ClipboardEntry& candidate) {
        return candidate.id == id;
    });
    if (item == entries_.end()) return false;
    item->is_locked = !item->is_locked;
    return true;
}

void ClipboardHistory::clear() { entries_.clear(); }

void ClipboardHistory::clear_unlocked() {
    std::erase_if(entries_, [](const ClipboardEntry& entry) { return !entry.is_locked; });
}

std::vector<ClipboardEntry> ClipboardHistory::snapshot_for_wheel(std::size_t visible_capacity) const {
    visible_capacity = std::min(visible_capacity, capacity_);
    std::vector<ClipboardEntry> result;
    result.reserve(visible_capacity);
    for (const auto& entry : entries_) {
        if (result.size() == visible_capacity) break;
        result.push_back(entry);
    }
    return result;
}

void ClipboardHistory::trim() {
    while (entries_.size() > capacity_) {
        const auto removable = std::find_if(entries_.rbegin(), entries_.rend(), [](const ClipboardEntry& entry) {
            return !entry.is_locked;
        });
        if (removable == entries_.rend()) break;
        entries_.erase(std::next(removable).base());
    }
}

} // namespace smk::core
