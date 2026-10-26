#pragma once

#include <windows.h>
#include <shellapi.h>

#include <functional>
#include <utility>

namespace smk::windows {

class TrayIcon final {
public:
    using NotifyIconSender = std::function<bool(DWORD, NOTIFYICONDATAW*)>; // 托盘 API（应用编程接口）发送器，可注入故障替身。
    struct Callbacks {
        std::function<void()> toggle;
        std::function<void()> settings;
        std::function<void()> clear_history;
        std::function<void()> exit;
    };

    explicit TrayIcon(NotifyIconSender sender = {}) : notify_icon_(std::move(sender)) {}
    ~TrayIcon();
    bool create(HINSTANCE instance, Callbacks callbacks);
    void destroy();
    void set_enabled(bool enabled);
    void set_capture_available(bool available, bool notify);
    [[nodiscard]] HWND window_handle() const noexcept { return window_; } // 本实例的接收窗口，避免跨进程按类名误定位。

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
    void show_menu();
    void refresh_icon(bool notify = false);
    bool send_icon_message(DWORD message);

    static constexpr UINT kCallbackMessage = WM_APP + 41;
    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    NOTIFYICONDATAW icon_{};
    Callbacks callbacks_{};
    bool enabled_ = true;
    bool capture_available_ = true;
    ULONG_PTR gdiplus_token_ = 0;
    HICON enabled_icon_ = nullptr;
    HICON disabled_icon_ = nullptr;
    NotifyIconSender notify_icon_;
    UINT taskbar_created_message_ = 0; // 任务栏重建广播编号。
    bool registered_ = false; // 仅表示最近一次托盘注册/更新成功。
};

} // namespace smk::windows
