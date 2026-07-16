#include "platform/windows/MouseHook.h"

#include "platform/windows/DiagnosticLog.h"
#include "platform/windows/CrashHandler.h"
#include "platform/windows/StartupTrace.h"

#include <algorithm>

namespace smk::windows {
namespace {
constexpr std::uint64_t kHeartbeatTimeoutMs = 1000;
constexpr std::uint64_t kPhysicalReleaseMinimumMs = 100;
constexpr DWORD kWatchdogIntervalMs = 50;
constexpr std::array<std::uint64_t, 4> kRetryDelays{1000, 5000, 30'000, 30'000};

std::size_t event_index(MouseHookEvent event) noexcept {
    const auto value = static_cast<std::size_t>(event);
    return value > 0 && value <= 6 ? value - 1 : 0;
}
}

MouseButtonSuppression::Decision MouseButtonSuppression::handle(
    WPARAM message, bool wheel_active) noexcept {
    switch (message) {
    case WM_LBUTTONDOWN:
        if (wheel_active) { left_down_suppressed_ = true; late_left_up_ = false; return {.suppress = true}; }
        late_left_up_ = false;
        break;
    case WM_LBUTTONUP:
        if (left_down_suppressed_) { left_down_suppressed_ = false; return {.suppress = true}; }
        if (late_left_up_) { late_left_up_ = false; return {.suppress = true}; }
        break;
    case WM_RBUTTONDOWN:
        if (wheel_active) {
            right_down_suppressed_ = true;
            late_right_up_ = false;
            return {.suppress = true, .toggle_lock = true};
        }
        late_right_up_ = false;
        break;
    case WM_RBUTTONUP:
        if (right_down_suppressed_) { right_down_suppressed_ = false; return {.suppress = true}; }
        if (late_right_up_) { late_right_up_ = false; return {.suppress = true}; }
        break;
    default: break;
    }
    return {};
}

void MouseButtonSuppression::reset() noexcept {
    left_down_suppressed_ = false;
    right_down_suppressed_ = false;
    late_left_up_ = false;
    late_right_up_ = false;
}

void MouseButtonSuppression::confirm_physical_release() noexcept {
    late_left_up_ = late_left_up_ || left_down_suppressed_;
    late_right_up_ = late_right_up_ || right_down_suppressed_;
    left_down_suppressed_ = false;
    right_down_suppressed_ = false;
}

void MouseInputSafetyState::set_enabled(bool enabled) noexcept {
    enabled_ = enabled;
}

void MouseInputSafetyState::note_heartbeat(std::uint64_t tick) noexcept {
    if (!tick || tick == observed_heartbeat_tick_) return;
    last_heartbeat_tick_ = tick;
    observed_heartbeat_tick_ = tick;
    if (recovering_) ++recovery_heartbeats_;
}

void MouseInputSafetyState::note_hook_available(bool available) noexcept {
    hook_available_ = available;
    if (!available) {
        ui_healthy_ = false;
        recovering_ = true;
        recovery_heartbeats_ = 0;
        (void)cancel_active();
    }
}

bool MouseInputSafetyState::capture_ready() const noexcept {
    return enabled_ && hook_available_ && !suspended_ && ui_healthy_ &&
        !middle_down_suppressed_ && !buttons_.pending_release();
}

MouseInputSafetyState::Decision MouseInputSafetyState::handle(
    WPARAM message, bool dispatch_succeeded, std::uint64_t tick) noexcept {
    const auto button = buttons_.handle(message, wheel_active_);
    if (button.suppress) {
        if (button.toggle_lock && dispatch_succeeded)
            return {.suppress = true, .dispatch = true,
                .event = MouseHookEvent::right_down, .generation = generation_};
        if (button.toggle_lock && !dispatch_succeeded) return cancel_active();
        return {.suppress = true};
    }

    switch (message) {
    case WM_MBUTTONDOWN:
        if (!capture_ready() || !dispatch_succeeded) return {};
        ++generation_;
        wheel_active_ = true;
        middle_down_suppressed_ = true;
        middle_down_tick_ = tick;
        physical_up_samples_ = 0;
        physical_down_observed_ = false;
        return {.suppress = true, .dispatch = true,
            .event = MouseHookEvent::middle_down, .generation = generation_};
    case WM_MOUSEMOVE:
        if (wheel_active_ && dispatch_succeeded)
            return {.dispatch = true, .event = MouseHookEvent::mouse_move,
                .generation = generation_};
        if (wheel_active_ && !dispatch_succeeded) return cancel_active();
        break;
    case WM_MBUTTONUP:
        if (middle_down_suppressed_) {
            const bool dispatch = wheel_active_ && dispatch_succeeded && ui_healthy_;
            middle_down_suppressed_ = false;
            wheel_active_ = false;
            physical_up_samples_ = 0;
            physical_down_observed_ = false;
            return {.suppress = true, .dispatch = dispatch,
                .event = MouseHookEvent::middle_up, .generation = generation_};
        }
        break;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        if (wheel_active_) return {.suppress = true};
        break;
    default: break;
    }
    return {};
}

MouseInputSafetyState::Decision MouseInputSafetyState::poll(
    bool physical_middle_down, bool all_buttons_released, std::uint64_t tick) noexcept {
    if (last_heartbeat_tick_ && tick - last_heartbeat_tick_ > kHeartbeatTimeoutMs) {
        if (ui_healthy_ || wheel_active_) {
            ui_healthy_ = false;
            recovering_ = true;
            recovery_heartbeats_ = 0;
            return cancel_active();
        }
    }
    if (recovering_ && all_buttons_released && recovery_heartbeats_ >= 2 &&
        !middle_down_suppressed_ && !buttons_.pending_release()) {
        recovering_ = false;
        ui_healthy_ = true;
    }
    if (buttons_.pending_release()) {
        button_up_samples_ = all_buttons_released ? button_up_samples_ + 1 : 0;
        if (button_up_samples_ >= 2) {
            buttons_.confirm_physical_release();
            button_up_samples_ = 0;
        }
    } else button_up_samples_ = 0;
    if (middle_down_suppressed_ && tick - middle_down_tick_ >= kPhysicalReleaseMinimumMs) {
        if (physical_middle_down) {
            physical_down_observed_ = true;
            physical_up_samples_ = 0;
        } else if (physical_down_observed_) {
            ++physical_up_samples_;
        }
        if (physical_down_observed_ && physical_up_samples_ >= 2) {
            middle_down_suppressed_ = false;
            physical_up_samples_ = 0;
            physical_down_observed_ = false;
            return cancel_active();
        }
    }
    return {};
}

MouseInputSafetyState::Decision MouseInputSafetyState::suspend() noexcept {
    suspended_ = true;
    ui_healthy_ = false;
    recovering_ = true;
    recovery_heartbeats_ = 0;
    return cancel_active();
}

void MouseInputSafetyState::resume() noexcept {
    suspended_ = false;
    ui_healthy_ = false;
    recovering_ = true;
    recovery_heartbeats_ = 0;
}

void MouseInputSafetyState::acknowledge_show(std::uint32_t generation, bool success) noexcept {
    if (generation != generation_ || !wheel_active_) return;
    if (!success) (void)cancel_active();
}

void MouseInputSafetyState::cancel(std::uint32_t generation) noexcept {
    if (generation == 0 || generation == generation_) (void)cancel_active();
}

MouseInputSafetyState::Decision MouseInputSafetyState::dispatch_failed(WPARAM message) noexcept {
    if (message == WM_MBUTTONDOWN) {
        wheel_active_ = false;
        middle_down_suppressed_ = false;
        physical_up_samples_ = 0;
        physical_down_observed_ = false;
        return {};
    }
    return cancel_active();
}

MouseInputSafetyState::Decision MouseInputSafetyState::cancel_active() noexcept {
    if (!wheel_active_) return {};
    wheel_active_ = false;
    return {.dispatch = true, .event = MouseHookEvent::cancel, .generation = generation_};
}

thread_local MouseHook* MouseHook::active_ = nullptr;

MouseHook::MouseHook(HWND event_target) : event_target_(event_target) {}
MouseHook::~MouseHook() {
    (void)stop(5000);
}

bool MouseHook::start() {
    if (thread_) return true;
    ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    stopped_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready_event_ || !stopped_event_) {
        if (ready_event_) CloseHandle(ready_event_);
        if (stopped_event_) CloseHandle(stopped_event_);
        ready_event_ = stopped_event_ = nullptr;
        return false;
    }
    thread_ = CreateThread(nullptr, 0, thread_entry, this, 0, &thread_id_);
    if (!thread_) {
        CloseHandle(ready_event_);
        CloseHandle(stopped_event_);
        ready_event_ = stopped_event_ = nullptr;
        return false;
    }
    if (WaitForSingleObject(ready_event_, 1500) != WAIT_OBJECT_0) {
        available_.store(false, std::memory_order_release);
        (void)PostThreadMessageW(thread_id_, kStopMessage, 0, 0);
        return true;
    }
    return true;
}

bool MouseHook::stop(DWORD timeout_ms) noexcept {
    if (!thread_) return true;
    if (thread_id_ && !PostThreadMessageW(thread_id_, kStopMessage, 0, 0))
        (void)PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
    const DWORD wait = WaitForSingleObject(stopped_event_, timeout_ms);
    const bool stopped = wait == WAIT_OBJECT_0;
    if (stopped) {
        CloseHandle(thread_);
        thread_ = nullptr;
        thread_id_ = 0;
        CloseHandle(ready_event_);
        CloseHandle(stopped_event_);
        ready_event_ = stopped_event_ = nullptr;
    }
    return stopped;
}

void MouseHook::set_enabled(bool enabled) noexcept {
    desired_enabled_.store(enabled, std::memory_order_release);
    if (thread_id_) PostThreadMessageW(thread_id_, kStateChangedMessage, enabled, 0);
}

void MouseHook::heartbeat() noexcept {
    heartbeat_tick_.store(GetTickCount64(), std::memory_order_release);
}

void MouseHook::suspend() noexcept {
    if (thread_id_) PostThreadMessageW(thread_id_, kSuspendMessage, 0, 0);
}

void MouseHook::resume() noexcept {
    if (thread_id_) PostThreadMessageW(thread_id_, kResumeMessage, 0, 0);
}

void MouseHook::retry_now() noexcept {
    if (thread_id_) PostThreadMessageW(thread_id_, kRetryMessage, 0, 0);
}

void MouseHook::acknowledge_show(std::uint32_t generation, bool success) noexcept {
    if (thread_id_) PostThreadMessageW(thread_id_, kShowResultMessage, generation, success);
}

void MouseHook::cancel_session(std::uint32_t generation) noexcept {
    if (thread_id_) PostThreadMessageW(thread_id_, kCancelMessage, generation, 0);
}

POINT MouseHook::event_point(MouseHookEvent event) const noexcept {
    const auto index = event_index(event);
    return {event_x_[index].load(std::memory_order_acquire),
        event_y_[index].load(std::memory_order_acquire)};
}

void MouseHook::complete_move(std::uint32_t generation) noexcept {
    move_pending_.store(false, std::memory_order_release);
    const auto current = move_sequence_.load(std::memory_order_acquire);
    if (current != posted_move_sequence_.load(std::memory_order_acquire)
        && !move_pending_.exchange(true, std::memory_order_acq_rel)) {
        if (PostMessageW(event_target_, kEventMessage,
            static_cast<WPARAM>(MouseHookEvent::mouse_move),
            generation)) {
            posted_move_sequence_.store(current, std::memory_order_release);
        } else {
            move_pending_.store(false, std::memory_order_release);
        }
    }
}

DWORD WINAPI MouseHook::thread_entry(void* context) noexcept {
    return static_cast<MouseHook*>(context)->thread_main();
}

DWORD MouseHook::thread_main() noexcept {
    active_ = this;
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    state_.set_enabled(desired_enabled_.load(std::memory_order_acquire));
    (void)install_hook();
    SetEvent(ready_event_);
    bool running = true;
    while (running) {
        const DWORD wait = MsgWaitForMultipleObjectsEx(0, nullptr, kWatchdogIntervalMs,
            QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_FAILED) { fail_open(); break; }
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            switch (message.message) {
            case kStopMessage: running = false; break;
            case WM_QUIT: running = false; break;
            case kStateChangedMessage:
                if (!message.wParam) {
                    const auto cancel = state_.suspend();
                    if (cancel.dispatch) post_event(cancel.event, cancel.generation, {});
                    state_.set_enabled(false);
                } else {
                    state_.set_enabled(true);
                    state_.resume();
                }
                publish_state();
                break;
            case kSuspendMessage: {
                const auto cancel = state_.suspend();
                if (cancel.dispatch) post_event(cancel.event, cancel.generation, {});
                uninstall_hook();
                publish_state();
                break;
            }
            case kResumeMessage:
                state_.resume();
                retry_index_ = 0;
                next_retry_tick_ = 0;
                (void)install_hook();
                publish_state();
                break;
            case kRetryMessage:
                retry_index_ = 0;
                next_retry_tick_ = 0;
                if (!hook_) (void)install_hook();
                break;
            case kShowResultMessage:
                state_.acknowledge_show(static_cast<std::uint32_t>(message.wParam), message.lParam != 0);
                publish_state();
                break;
            case kCancelMessage:
                state_.cancel(static_cast<std::uint32_t>(message.wParam));
                publish_state();
                break;
            default:
                TranslateMessage(&message);
                DispatchMessageW(&message);
                break;
            }
            if (!running) break;
        }
        if (running) run_watchdog(GetTickCount64());
    }
    state_.set_enabled(false);
    uninstall_hook();
    active_ = nullptr;
    SetEvent(stopped_event_);
    return 0;
}

LRESULT CALLBACK MouseHook::hook_proc(int code, WPARAM wparam, LPARAM lparam) noexcept {
    if (code < 0 || !active_) return CallNextHookEx(nullptr, code, wparam, lparam);
    try {
        return active_->process_hook_event(code, wparam,
            *reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam));
    } catch (...) {
        active_->fail_open();
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }
}

LRESULT MouseHook::process_hook_event(int code, WPARAM wparam,
    const MSLLHOOKSTRUCT& event) {
    if (code < 0) return CallNextHookEx(nullptr, code, wparam, reinterpret_cast<LPARAM>(&event));
    auto decision = state_.handle(wparam, true, GetTickCount64());
    generation_.store(state_.generation(), std::memory_order_release);
    if (decision.dispatch && !post_event(decision.event, decision.generation, event.pt)) {
        const auto cancel = state_.dispatch_failed(wparam);
        if (cancel.dispatch) (void)post_event(cancel.event, cancel.generation, event.pt);
        if (wparam == WM_MBUTTONDOWN) decision.suppress = false;
    }
    publish_state();
    return decision.suppress ? 1 : CallNextHookEx(nullptr, code, wparam,
        reinterpret_cast<LPARAM>(&event));
}

bool MouseHook::install_hook() noexcept {
    if (hook_) return true;
    hook_ = SetWindowsHookExW(WH_MOUSE_LL, hook_proc, GetModuleHandleW(nullptr), 0);
    const bool success = hook_ != nullptr;
    available_.store(success, std::memory_order_release);
    state_.note_hook_available(success);
    if (success) {
        retry_index_ = 0;
        next_retry_tick_ = 0;
        startup_trace(L"mouse hook installed on dedicated thread");
        SMK_DIAGNOSTIC_EVENT("input.hook.install", L"success=true");
    } else {
        const DWORD error = GetLastError();
        schedule_retry(GetTickCount64());
        wchar_t details[96]{};
        swprintf_s(details, L"success=false error=%lu", error);
        SMK_DIAGNOSTIC_EVENT("input.hook.install", details);
    }
    publish_state();
    post_event(MouseHookEvent::availability_changed, state_.generation(), {});
    return success;
}

void MouseHook::uninstall_hook() noexcept {
    if (hook_) {
        [[maybe_unused]] const BOOL result = UnhookWindowsHookEx(hook_);
        const DWORD error = result ? ERROR_SUCCESS : GetLastError();
        wchar_t details[96]{};
        swprintf_s(details, L"success=%u error=%lu", result != FALSE ? 1u : 0u, error);
        SMK_DIAGNOSTIC_EVENT("input.hook.uninstall", details);
    }
    hook_ = nullptr;
    available_.store(false, std::memory_order_release);
    state_.note_hook_available(false);
    publish_state();
}

void MouseHook::schedule_retry(std::uint64_t now) noexcept {
    const auto index = std::min<std::size_t>(retry_index_, kRetryDelays.size() - 1);
    next_retry_tick_ = now + kRetryDelays[index];
    if (retry_index_ + 1 < kRetryDelays.size()) ++retry_index_;
}

void MouseHook::run_watchdog(std::uint64_t now) noexcept {
    const auto heartbeat = heartbeat_tick_.load(std::memory_order_acquire);
    if (heartbeat) state_.note_heartbeat(heartbeat);
    const bool physical_middle = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
    const bool was_ui_healthy = state_.ui_healthy();
    const bool was_middle_suppressed = state_.middle_down_suppressed();
    const bool was_capture_ready = state_.capture_ready();
    const auto decision = state_.poll(physical_middle, all_buttons_released(), now);
    if (decision.dispatch) {
        (void)post_event(decision.event, decision.generation, {});
        wchar_t details[96]{};
        const wchar_t* reason = was_ui_healthy && !state_.ui_healthy()
            ? L"ui_heartbeat_stale"
            : was_middle_suppressed && !state_.middle_down_suppressed()
                ? L"physical_middle_release" : L"watchdog";
        swprintf_s(details, L"generation=%u reason=%s", decision.generation, reason);
        SMK_DIAGNOSTIC_EVENT("input.session.cancel", details);
    }
    if (!was_capture_ready && state_.capture_ready())
        SMK_DIAGNOSTIC_EVENT("input.capture.recovered", L"heartbeats=2 buttons=released");
    if (!hook_ && next_retry_tick_ && now >= next_retry_tick_) (void)install_hook();
    publish_state();
}

bool MouseHook::post_event(MouseHookEvent event, std::uint32_t generation, POINT point) noexcept {
    const auto index = event_index(event);
    event_x_[index].store(point.x, std::memory_order_release);
    event_y_[index].store(point.y, std::memory_order_release);
    if (event == MouseHookEvent::mouse_move) {
        const auto sequence = move_sequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (move_pending_.exchange(true, std::memory_order_acq_rel)) return true;
        posted_move_sequence_.store(sequence, std::memory_order_release);
    }
    const BOOL posted = PostMessageW(event_target_, kEventMessage,
        static_cast<WPARAM>(event), generation);
    if (!posted && event == MouseHookEvent::mouse_move)
        move_pending_.store(false, std::memory_order_release);
    return posted != FALSE;
}

void MouseHook::publish_state() noexcept {
    generation_.store(state_.generation(), std::memory_order_release);
    capture_ready_.store(state_.capture_ready(), std::memory_order_release);
    session_active_.store(state_.wheel_active(), std::memory_order_release);
    crash_set_input_state(available_.load(std::memory_order_acquire),
        state_.capture_ready(), state_.generation());
}

void MouseHook::fail_open() noexcept {
    const auto decision = state_.suspend();
    state_.resume();
    publish_state();
    if (decision.dispatch) (void)post_event(decision.event, decision.generation, {});
    SMK_DIAGNOSTIC_EVENT("input.fail_open", L"reason=callback_or_message_failure");
}

bool MouseHook::all_buttons_released() const noexcept {
    return (GetAsyncKeyState(VK_MBUTTON) & 0x8000) == 0 &&
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0 &&
        (GetAsyncKeyState(VK_RBUTTON) & 0x8000) == 0;
}

} // namespace smk::windows
