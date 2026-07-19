#pragma once

#include "core/Settings.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace smk::windows {

class ManagedShortcutStore final {
public:
    ManagedShortcutStore();
    explicit ManagedShortcutStore(std::filesystem::path root);

    [[nodiscard]] std::optional<std::wstring> create_candidate(
        const std::wstring& source_path, int slot_index, std::wstring& error) const;
    [[nodiscard]] bool is_managed(const std::wstring& path) const noexcept;
    void discard(const std::wstring& path) const noexcept;
    void reconcile(const smk::core::AppSettings& previous,
        const smk::core::AppSettings& accepted,
        const std::vector<std::wstring>& candidates) const noexcept;
    void cleanup_unreferenced(const smk::core::AppSettings& settings) const noexcept;
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    std::filesystem::path root_;
};

} // namespace smk::windows
