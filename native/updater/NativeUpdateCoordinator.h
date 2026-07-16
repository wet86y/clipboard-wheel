#pragma once

#include "updater/UpdateTypes.h"

#include <DesktopUpdateKit/UpdateKit.h>
#include <windows.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

namespace smk::updater {

enum class ExecutableNameNormalizationResult {
    unchanged,
    relaunch_started,
    failed,
};

std::optional<std::filesystem::path> canonical_executable_target(
    const std::filesystem::path& executable, const std::wstring& canonical_file_name);
ExecutableNameNormalizationResult normalize_executable_name(
    HINSTANCE instance, const std::filesystem::path& executable,
    const std::wstring& canonical_file_name, std::wstring& error);

class NativeUpdateCoordinator final : public UpdateController {
public:
    using ExitCallback = std::function<void()>;

    NativeUpdateCoordinator(HINSTANCE instance, std::filesystem::path executable,
        std::string repository, desktop_update_kit::Version current_version,
        bool acceleration, bool ui_enabled, ExitCallback exit_callback);
    ~NativeUpdateCoordinator() override;

    void set_observer(Observer observer) override;
    UpdateViewState state() const override;
    void check() override;
    void download_or_resume() override;
    void pause() override;
    void continue_in_background() override;
    void cancel() override;
    void set_acceleration(bool enabled) override;
    void next_node() override;
    bool install() override;
    void settings_closed() override;

    void shutdown();
    [[nodiscard]] bool ui_enabled() const noexcept { return ui_enabled_; }

private:
    struct ObserverSlot {
        std::recursive_mutex mutex;
        Observer observer;
        bool active{true};
    };

    static void deactivate_observer(const std::shared_ptr<ObserverSlot>& slot) noexcept;
    void apply_snapshot(const desktop_update_kit::SessionSnapshot& snapshot);
    void publish(UpdateViewState state);
    std::vector<std::byte> updater_stub() const;
    static std::wstring widen(const std::string& text);

    HINSTANCE instance_{};
    std::filesystem::path executable_;
    bool ui_enabled_{};
    ExitCallback exit_callback_;
    desktop_update_kit::UpdateClient client_;
    desktop_update_kit::DownloadSession session_;
    std::jthread check_thread_;
    mutable std::mutex mutex_;
    UpdateViewState state_;
    std::shared_ptr<ObserverSlot> observer_slot_;
    std::optional<desktop_update_kit::Release> release_;
    bool shutting_down_{};
    bool installation_started_{};
};

} // namespace smk::updater
