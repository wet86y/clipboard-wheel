#pragma once

#include "core/Settings.h"

#include <filesystem>
#include <string>

namespace smk::windows {

class SettingsStore final {
public:
    SettingsStore();
    explicit SettingsStore(std::filesystem::path directory,
        std::filesystem::path legacy_directory = {});

    [[nodiscard]] smk::core::AppSettings load();
    [[nodiscard]] bool save(const smk::core::AppSettings& settings, std::wstring& error) const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
    std::filesystem::path legacy_path_;
};

} // namespace smk::windows
