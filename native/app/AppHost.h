#pragma once

#include "core/ClipboardHistory.h"
#include "core/Settings.h"
#include "platform/windows/ClipboardService.h"
#include "platform/windows/ExtendedActionExecutor.h"
#include "platform/windows/MouseHook.h"
#include "platform/windows/SettingsStore.h"
#include "platform/windows/SingleInstance.h"
#include "platform/windows/TrayIcon.h"
#include "platform/windows/WindowsClipboardHistory.h"
#include "ui/SettingsWindow.h"
#include "ui/WheelWindow.h"
#include "updater/NativeUpdateCoordinator.h"

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace smk::app {

class AppHost final {
public:
    ~AppHost();
    int run(HINSTANCE instance, const std::vector<std::wstring>& arguments);

private:
    bool initialize(HINSTANCE instance, const std::vector<std::wstring>& arguments);
    void shutdown();
    bool create_input_safety_window();
    void destroy_input_safety_window() noexcept;
    static LRESULT CALLBACK input_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_input_message(UINT message, WPARAM wparam, LPARAM lparam);
    bool show_wheel(POINT point);
    void update_wheel(POINT point);
    void finish_wheel(POINT point);
    void cancel_wheel_no_action();
    void toggle_selected_lock();
    void toggle_enabled();
    void show_settings();
    std::optional<smk::core::AppSettings> save_settings(const smk::core::AppSettings& settings);
    static std::wstring executable_path();
    static bool write_update_health_marker(const std::vector<std::wstring>& arguments);
    static int verify_release_bundle(HINSTANCE instance);
    static void send_ctrl_c();

    HINSTANCE instance_ = nullptr;
    HWND input_window_ = nullptr;
    smk::windows::SingleInstance single_instance_;
    smk::windows::SettingsStore settings_store_;
    smk::core::AppSettings settings_{};
    smk::core::ClipboardHistory history_{8};
    smk::ui::WheelWindow wheel_;
    smk::ui::SettingsWindow settings_window_;
    smk::windows::TrayIcon tray_;
    std::unique_ptr<smk::windows::ClipboardService> clipboard_;
    std::unique_ptr<smk::windows::WindowsClipboardHistory> windows_history_;
    std::unique_ptr<smk::windows::MouseHook> mouse_hook_;
    smk::windows::ExtendedActionExecutor extended_actions_;
    std::unique_ptr<smk::updater::NativeUpdateCoordinator> updater_;
    bool enabled_ = true;
    bool lock_changed_ = false;
    bool shutting_down_ = false;
    bool startup_exit_requested_ = false;
    bool hook_unavailable_notified_ = false;
};

} // namespace smk::app
