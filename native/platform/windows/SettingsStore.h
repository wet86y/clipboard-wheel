#pragma once

#include "core/Settings.h"

#include <filesystem>
#include <string>

namespace smk::windows {

class SettingsStore final {
public:
    SettingsStore();

    [[nodiscard]] smk::core::AppSettings load();
    [[nodiscard]] bool save(const smk::core::AppSettings& settings, std::wstring& error) const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    void migrate_legacy_if_needed() const;
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

} // namespace smk::windows
