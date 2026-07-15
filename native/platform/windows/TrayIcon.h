#pragma once

#include <windows.h>
#include <shellapi.h>

#include <functional>

namespace smk::windows {

class TrayIcon final {
public:
    struct Callbacks {
        std::function<void()> toggle;
        std::function<void()> settings;
        std::function<void()> clear_history;
        std::function<void()> exit;
    };

    TrayIcon() = default;
    ~TrayIcon();
    bool create(HINSTANCE instance, Callbacks callbacks);
    void destroy();
    void set_enabled(bool enabled);
    void set_capture_available(bool available, bool notify);

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
    void show_menu();

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
};

} // namespace smk::windows
