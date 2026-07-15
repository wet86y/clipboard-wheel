#pragma once

#include <windows.h>

#include <array>

namespace smk::windows {

struct HotkeyCaptureDecision {
    bool suppress = false;
    bool deliver = false;
    UINT virtual_key = 0;
    LPARAM key_data = 0;
};

class HotkeyCaptureFilter final {
public:
    [[nodiscard]] HotkeyCaptureDecision handle(
        WPARAM message,
        UINT virtual_key,
        UINT scan_code,
        DWORD flags,
        bool target_foreground) noexcept;
    void reset() noexcept;

private:
    std::array<bool, 256> suppressed_keys_{};
};

class HotkeyCaptureHook final {
public:
    HotkeyCaptureHook() = default;
    ~HotkeyCaptureHook();
    HotkeyCaptureHook(const HotkeyCaptureHook&) = delete;
    HotkeyCaptureHook& operator=(const HotkeyCaptureHook&) = delete;

    [[nodiscard]] bool start(HWND target, UINT delivery_message, DWORD* error = nullptr) noexcept;
    void stop() noexcept;
    [[nodiscard]] bool active() const noexcept { return hook_ != nullptr; }

private:
    static LRESULT CALLBACK hook_proc(int code, WPARAM wparam, LPARAM lparam);
    static HotkeyCaptureHook* active_hook_;

    HHOOK hook_ = nullptr;
    HWND target_ = nullptr;
    UINT delivery_message_ = 0;
    HotkeyCaptureFilter filter_;
};

} // namespace smk::windows
