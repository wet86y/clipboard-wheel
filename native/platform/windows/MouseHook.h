#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace smk::windows {

class MouseButtonSuppression final {
public:
    struct Decision {
        bool suppress = false;
        bool toggle_lock = false;
    };

    [[nodiscard]] Decision handle(WPARAM message, bool wheel_active) noexcept;
    void reset() noexcept;
    void confirm_physical_release() noexcept;
    [[nodiscard]] bool pending_release() const noexcept {
        return left_down_suppressed_ || right_down_suppressed_;
    }

private:
    bool left_down_suppressed_ = false;
    bool right_down_suppressed_ = false;
    bool late_left_up_ = false;
    bool late_right_up_ = false;
};

enum class MouseHookEvent : std::uint32_t {
    middle_down = 1,
    mouse_move,
    middle_up,
    right_down,
    cancel,
    availability_changed,
};

// The state machine is deliberately independent of HHOOK so failure paths can
// be verified without injecting global mouse input into a test process.
class MouseInputSafetyState final {
public:
    struct Decision {
        bool suppress = false;
        bool dispatch = false;
        MouseHookEvent event = MouseHookEvent::mouse_move;
        std::uint32_t generation = 0;
    };

    void set_enabled(bool enabled) noexcept;
    void note_heartbeat(std::uint64_t tick) noexcept;
    void note_hook_available(bool available) noexcept;
    [[nodiscard]] Decision handle(WPARAM message, bool dispatch_succeeded,
        std::uint64_t tick) noexcept;
    [[nodiscard]] Decision poll(bool physical_middle_down, bool all_buttons_released,
        std::uint64_t tick) noexcept;
    [[nodiscard]] Decision suspend() noexcept;
    void resume() noexcept;
    void acknowledge_show(std::uint32_t generation, bool success) noexcept;
    void cancel(std::uint32_t generation) noexcept;
    [[nodiscard]] Decision dispatch_failed(WPARAM message) noexcept;

    [[nodiscard]] bool wheel_active() const noexcept { return wheel_active_; }
    [[nodiscard]] bool middle_down_suppressed() const noexcept { return middle_down_suppressed_; }
    [[nodiscard]] bool ui_healthy() const noexcept { return ui_healthy_; }
    [[nodiscard]] bool capture_ready() const noexcept;
    [[nodiscard]] std::uint32_t generation() const noexcept { return generation_; }

private:
    [[nodiscard]] Decision cancel_active() noexcept;

    MouseButtonSuppression buttons_;
    bool enabled_ = true;
    bool hook_available_ = false;
    bool suspended_ = false;
    bool ui_healthy_ = false;
    bool recovering_ = true;
    bool wheel_active_ = false;
    bool middle_down_suppressed_ = false;
    std::uint32_t generation_ = 0;
    std::uint64_t last_heartbeat_tick_ = 0;
    std::uint64_t observed_heartbeat_tick_ = 0;
    std::uint64_t middle_down_tick_ = 0;
    unsigned recovery_heartbeats_ = 0;
    unsigned physical_up_samples_ = 0;
    bool physical_down_observed_ = false;
    unsigned button_up_samples_ = 0;
};

class MouseHook final {
public:
    static constexpr UINT kEventMessage = WM_APP + 210;

    explicit MouseHook(HWND event_target);
    ~MouseHook();
    MouseHook(const MouseHook&) = delete;
    MouseHook& operator=(const MouseHook&) = delete;

    // A running hook thread is considered a successful start even when the
    // initial SetWindowsHookEx call fails; installation is retried in-place.
    bool start();
    [[nodiscard]] bool stop(DWORD timeout_ms = 1500) noexcept;
    void set_enabled(bool enabled) noexcept;
    void heartbeat() noexcept;
    void suspend() noexcept;
    void resume() noexcept;
    void retry_now() noexcept;
    void acknowledge_show(std::uint32_t generation, bool success) noexcept;
    void cancel_session(std::uint32_t generation) noexcept;
    [[nodiscard]] POINT event_point(MouseHookEvent event) const noexcept;
    void complete_move(std::uint32_t generation) noexcept;
    [[nodiscard]] bool available() const noexcept { return available_.load(std::memory_order_acquire); }
    [[nodiscard]] bool capture_ready() const noexcept { return capture_ready_.load(std::memory_order_acquire); }
    [[nodiscard]] bool session_active() const noexcept { return session_active_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint32_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }

private:
    static constexpr UINT kStopMessage = WM_APP + 211;
    static constexpr UINT kStateChangedMessage = WM_APP + 212;
    static constexpr UINT kSuspendMessage = WM_APP + 213;
    static constexpr UINT kResumeMessage = WM_APP + 214;
    static constexpr UINT kRetryMessage = WM_APP + 215;
    static constexpr UINT kShowResultMessage = WM_APP + 216;
    static constexpr UINT kCancelMessage = WM_APP + 217;

    static DWORD WINAPI thread_entry(void* context) noexcept;
    DWORD thread_main() noexcept;
    static LRESULT CALLBACK hook_proc(int code, WPARAM wparam, LPARAM lparam) noexcept;
    LRESULT process_hook_event(int code, WPARAM wparam, const MSLLHOOKSTRUCT& event);
    bool install_hook() noexcept;
    void uninstall_hook() noexcept;
    void schedule_retry(std::uint64_t now) noexcept;
    void run_watchdog(std::uint64_t now) noexcept;
    bool post_event(MouseHookEvent event, std::uint32_t generation, POINT point) noexcept;
    void publish_state() noexcept;
    void fail_open() noexcept;
    [[nodiscard]] bool all_buttons_released() const noexcept;

    static thread_local MouseHook* active_;
    HWND event_target_ = nullptr;
    HANDLE thread_ = nullptr;
    HANDLE ready_event_ = nullptr;
    HANDLE stopped_event_ = nullptr;
    DWORD thread_id_ = 0;
    HHOOK hook_ = nullptr;
    MouseInputSafetyState state_;
    std::atomic_bool desired_enabled_{true};
    std::atomic_bool available_{false};
    std::atomic_bool capture_ready_{false};
    std::atomic_bool session_active_{false};
    std::atomic_uint32_t generation_{0};
    std::atomic_uint64_t heartbeat_tick_{0};
    std::atomic_uint64_t move_sequence_{0};
    std::atomic_uint64_t posted_move_sequence_{0};
    std::atomic_bool move_pending_{false};
    std::array<std::atomic_long, 6> event_x_{};
    std::array<std::atomic_long, 6> event_y_{};
    std::uint64_t next_retry_tick_ = 0;
    unsigned retry_index_ = 0;
};

} // namespace smk::windows
