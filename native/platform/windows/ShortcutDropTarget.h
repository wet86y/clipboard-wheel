#pragma once

#include <windows.h>
#include <oleidl.h>

#include <functional>
#include <string>

namespace smk::windows {

enum class ShortcutDropVisualState { idle, accept, reject, success };

struct ShortcutDropCallbacks {
    std::function<bool()> enabled;
    std::function<void(ShortcutDropVisualState)> state_changed;
    std::function<void(const std::wstring&)> shortcut_dropped;
};

[[nodiscard]] bool is_valid_shortcut_drop_path(const std::wstring& path) noexcept;
// Returned with one reference owned by the caller.
[[nodiscard]] IDropTarget* create_shortcut_drop_target(
    ShortcutDropCallbacks callbacks) noexcept;

class ShortcutDropRegistration final {
public:
    ShortcutDropRegistration() = default;
    ~ShortcutDropRegistration();
    ShortcutDropRegistration(const ShortcutDropRegistration&) = delete;
    ShortcutDropRegistration& operator=(const ShortcutDropRegistration&) = delete;
    ShortcutDropRegistration(ShortcutDropRegistration&& other) noexcept;
    ShortcutDropRegistration& operator=(ShortcutDropRegistration&& other) noexcept;

    [[nodiscard]] bool register_window(
        HWND window,
        ShortcutDropCallbacks callbacks,
        HRESULT* error = nullptr) noexcept;
    void reset() noexcept;
    [[nodiscard]] bool registered() const noexcept { return window_ != nullptr; }

private:
    HWND window_ = nullptr;
    IUnknown* target_ = nullptr;
};

} // namespace smk::windows
