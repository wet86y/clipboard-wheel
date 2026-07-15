#include "platform/windows/HotkeyCaptureHook.h"

namespace smk::windows {

HotkeyCaptureDecision HotkeyCaptureFilter::handle(
    WPARAM message,
    UINT virtual_key,
    UINT scan_code,
    DWORD flags,
    bool target_foreground) noexcept {
    const bool key_down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool key_up = message == WM_KEYUP || message == WM_SYSKEYUP;
    if (!key_down && !key_up) return {};

    const bool in_range = virtual_key < suppressed_keys_.size();
    const bool pending_release = in_range && suppressed_keys_[virtual_key];
    if (!target_foreground && !pending_release) return {};

    if (key_up) {
        if (in_range) suppressed_keys_[virtual_key] = false;
        return {.suppress = target_foreground || pending_release};
    }

    if (in_range && suppressed_keys_[virtual_key]) return {.suppress = true};
    if (in_range) suppressed_keys_[virtual_key] = true;

    const bool placeholder = virtual_key == VK_PROCESSKEY || virtual_key == VK_PACKET;
    LPARAM key_data = static_cast<LPARAM>((scan_code & 0xffu) << 16);
    if ((flags & LLKHF_EXTENDED) != 0) key_data |= static_cast<LPARAM>(1u << 24);
    return {
        .suppress = true,
        .deliver = !placeholder,
        .virtual_key = virtual_key,
        .key_data = key_data,
    };
}

void HotkeyCaptureFilter::reset() noexcept {
    suppressed_keys_.fill(false);
}

HotkeyCaptureHook* HotkeyCaptureHook::active_hook_ = nullptr;

HotkeyCaptureHook::~HotkeyCaptureHook() { stop(); }

bool HotkeyCaptureHook::start(HWND target, UINT delivery_message, DWORD* error) noexcept {
    stop();
    if (!IsWindow(target) || delivery_message < WM_APP) {
        if (error) *error = ERROR_INVALID_PARAMETER;
        return false;
    }
    if (active_hook_ && active_hook_ != this) {
        if (error) *error = ERROR_BUSY;
        return false;
    }

    target_ = target;
    delivery_message_ = delivery_message;
    filter_.reset();
    active_hook_ = this;
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, hook_proc, GetModuleHandleW(nullptr), 0);
    if (!hook_) {
        const DWORD hook_error = GetLastError();
        active_hook_ = nullptr;
        target_ = nullptr;
        delivery_message_ = 0;
        if (error) *error = hook_error;
        return false;
    }
    if (error) *error = ERROR_SUCCESS;
    return true;
}

void HotkeyCaptureHook::stop() noexcept {
    if (hook_) UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
    target_ = nullptr;
    delivery_message_ = 0;
    filter_.reset();
    if (active_hook_ == this) active_hook_ = nullptr;
}

LRESULT CALLBACK HotkeyCaptureHook::hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code < 0 || !active_hook_ || !active_hook_->hook_)
        return CallNextHookEx(nullptr, code, wparam, lparam);

    const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
    const HWND foreground = GetForegroundWindow();
    const HWND target_root = GetAncestor(active_hook_->target_, GA_ROOT);
    const bool target_foreground = foreground && target_root
        && GetAncestor(foreground, GA_ROOT) == target_root;
    const auto decision = active_hook_->filter_.handle(
        wparam, event->vkCode, event->scanCode, event->flags, target_foreground);
    if (decision.deliver && IsWindow(active_hook_->target_)) {
        (void)PostMessageW(active_hook_->target_, active_hook_->delivery_message_,
            decision.virtual_key, decision.key_data);
    }
    if (decision.suppress) return 1;
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

} // namespace smk::windows
