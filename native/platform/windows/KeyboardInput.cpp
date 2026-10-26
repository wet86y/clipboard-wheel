#include "platform/windows/KeyboardInput.h"

#include "platform/windows/DiagnosticLog.h"

#include <utility>

namespace smk::windows {
namespace {
constexpr std::array<WORD, 3> kResetKeys{VK_ESCAPE, VK_LWIN, VK_RWIN};
constexpr std::array<WORD, 8> kModifierKeys{
    VK_LCONTROL, VK_RCONTROL, VK_LSHIFT, VK_RSHIFT, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN};
constexpr ULONG_PTR kInputMarker = 0x534D4B49; // SMKI：本程序的模拟键盘输入标记。
constexpr InputGeneration kActiveDispatch = InputGeneration{1} << 48;
constexpr InputGeneration kGenerationMask = kActiveDispatch - 1;

bool native_send(WORD key, bool key_up) noexcept {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.dwExtraInfo = kInputMarker;
    input.ki.dwFlags = key_up ? KEYEVENTF_KEYUP : 0;
    switch (key) {
    case VK_RCONTROL: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_APPS:
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
    case VK_SNAPSHOT:
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        break;
    default: break;
    }
    return SendInput(1, &input, sizeof(input)) == 1;
}
}

bool is_emergency_reset_key(UINT key) noexcept {
    return key == VK_ESCAPE || key == VK_LWIN || key == VK_RWIN;
}

KeyboardInput::KeyboardInput(KeyboardEventSender sender, KeyboardDownQuery down_query)
    : sender_(std::move(sender)), down_query_(std::move(down_query)) {}

InputGeneration KeyboardInput::generation() const noexcept {
    return generation_.load(std::memory_order_acquire) & kGenerationMask;
}

KeyboardInput::DispatchPermit::~DispatchPermit() {
    if (owner_) owner_->generation_.fetch_sub(kActiveDispatch, std::memory_order_acq_rel);
}

KeyboardInput::DispatchPermit::DispatchPermit(DispatchPermit&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)) {}

KeyboardInput::DispatchPermit& KeyboardInput::DispatchPermit::operator=(DispatchPermit&& other) noexcept {
    if (this == &other) return *this;
    if (owner_) owner_->generation_.fetch_sub(kActiveDispatch, std::memory_order_acq_rel);
    owner_ = std::exchange(other.owner_, nullptr);
    return *this;
}

KeyboardInput::DispatchPermit KeyboardInput::try_dispatch(InputGeneration expected) noexcept {
    if ((expected & 1) != 0) return {};
    auto previous = generation_.load(std::memory_order_acquire);
    while ((previous & kGenerationMask) == expected) {
        if (generation_.compare_exchange_weak(previous, previous + kActiveDispatch,
            std::memory_order_acq_rel, std::memory_order_acquire)) return DispatchPermit(this);
    }
    return {};
}

bool KeyboardInput::paused() const noexcept { return (generation() & 1) != 0; }

bool KeyboardInput::accepts(InputGeneration expected) const noexcept {
    return (expected & 1) == 0 && generation() == expected;
}

void KeyboardInput::pause() noexcept {
    generation_.fetch_or(1, std::memory_order_acq_rel);
}

void KeyboardInput::emergency_reset() noexcept {
    auto previous = generation_.load(std::memory_order_acquire);
    while (!generation_.compare_exchange_weak(previous,
        (previous & ~kGenerationMask) | (((previous + 2) | 1) & kGenerationMask),
        std::memory_order_acq_rel, std::memory_order_acquire)) {}
    force_release_modifiers_.store(true, std::memory_order_release);
}

void KeyboardInput::resume() noexcept {
    auto previous = generation_.load(std::memory_order_acquire);
    while (!generation_.compare_exchange_weak(previous,
        (previous & ~kGenerationMask) | (((previous + 2) & ~InputGeneration{1}) & kGenerationMask),
        std::memory_order_acq_rel, std::memory_order_acquire)) {}
}

void KeyboardInput::observe_key(WPARAM message, UINT key, DWORD flags, bool suppressed) noexcept {
    if (key >= physical_down_.size()) return;
    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
    if (!down && !up) return;
    if ((flags & LLKHF_INJECTED) != 0) {
        injected_down_[key].store(down, std::memory_order_release);
        return;
    }
    physical_suppressed_[key].store(down && suppressed, std::memory_order_release);
    const bool previous = physical_down_[key].exchange(down, std::memory_order_acq_rel);
    if (down && !previous && is_emergency_reset_key(key)) emergency_reset();
}

void KeyboardInput::seed_physical_modifiers() noexcept {
    for (const WORD key : kModifierKeys) {
        const WORD generic = key == VK_LCONTROL || key == VK_RCONTROL ? VK_CONTROL
            : key == VK_LSHIFT || key == VK_RSHIFT ? VK_SHIFT
            : key == VK_LMENU || key == VK_RMENU ? VK_MENU : key;
        if (pending_[key].load(std::memory_order_acquire) || pending_[generic].load(std::memory_order_acquire)) continue;
        physical_down_[key].store(query_down(key), std::memory_order_release);
        physical_suppressed_[key].store(false, std::memory_order_release);
    }
}

void KeyboardInput::poll_reset_keys() noexcept {
    for (const WORD key : kResetKeys) {
        if (pending_[key].load(std::memory_order_acquire)
            || injected_down_[key].load(std::memory_order_acquire)) continue;
        const bool down = query_down(key);
        const bool previous = physical_down_[key].exchange(down, std::memory_order_acq_rel);
        if (down && !previous) emergency_reset();
    }
}

bool KeyboardInput::physically_held(WORD key) const noexcept {
    if (key == VK_CONTROL) return physically_held(VK_LCONTROL) || physically_held(VK_RCONTROL);
    if (key == VK_SHIFT) return physically_held(VK_LSHIFT) || physically_held(VK_RSHIFT);
    if (key == VK_MENU) return physically_held(VK_LMENU) || physically_held(VK_RMENU);
    return key < physical_down_.size() && physical_down_[key].load(std::memory_order_acquire)
        && !physical_suppressed_[key].load(std::memory_order_acquire);
}

bool KeyboardInput::query_down(WORD key) const noexcept {
    try { return down_query_ && down_query_(key); } catch (...) { return true; }
}

bool KeyboardInput::send_key(WORD key, bool key_up) noexcept {
    try { return sender_ && sender_(key, key_up); } catch (...) { return false; }
}

bool KeyboardInput::release_key(WORD key) noexcept {
    // 用户在注入期间自己按住该键时，交给其真实松开事件收尾，不能抢先释放。
    if (physically_held(key)) {
        pending_[key].store(false, std::memory_order_release);
        return true;
    }
    for (unsigned attempt = 0; attempt != 3; ++attempt) {
        if (!send_key(key, true)) continue;
        // 插入成功也可能被其他钩子吞掉，仍显示按下时交给看门狗再次补偿。
        pending_[key].store(query_down(key), std::memory_order_release);
        return true;
    }
    return false;
}

bool KeyboardInput::release_pending() noexcept {
    if (force_release_modifiers_.exchange(false, std::memory_order_acq_rel)) {
        for (const WORD key : kModifierKeys) {
            if (!physically_held(key) && query_down(key))
                pending_[key].store(true, std::memory_order_release);
        }
    }
    bool released = true;
    for (std::size_t key = pending_.size(); key-- != 0;) {
        if (pending_[key].load(std::memory_order_acquire))
            released = release_key(static_cast<WORD>(key)) && released;
    }
    return released && pending_release_count() == 0;
}

bool KeyboardInput::send_chord(std::span<const WORD> keys, InputGeneration expected) noexcept {
    std::unique_lock lock(send_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || keys.empty() || !accepts(expected)) return false;
    if (!release_pending()) return false;
    std::array<WORD, 256> pressed{};
    std::array<bool, 256> seen{};
    std::size_t pressed_count = 0;
    bool success = true;
    for (const WORD key : keys) {
        if (!accepts(expected)) { success = false; break; }
        if (!key || key >= pending_.size() || seen[key]) continue;
        seen[key] = true;
        if (query_down(key)) continue;
        // 与急停在同一个原子字上竞争提交；已取得许可的在途输入必须继续配对释放。
        auto permit = try_dispatch(expected);
        if (!permit) { success = false; break; }
        pending_[key].store(true, std::memory_order_release);
        if (!send_key(key, false)) {
            pending_[key].store(false, std::memory_order_release);
            success = false;
            break;
        }
        pressed[pressed_count++] = key;
    }
    for (std::size_t index = pressed_count; index-- != 0;)
        success = release_key(pressed[index]) && success;
    success = accepts(expected) && success;
    wchar_t details[160]{};
    swprintf_s(details, L"generation=%llu requested_keys=%zu pressed_keys=%zu pending_releases=%zu success=%u",
        expected, keys.size(), pressed_count, pending_release_count(), success ? 1u : 0u);
    SMK_DIAGNOSTIC_EVENT("input.keyboard.chord", details);
    return success;
}

void KeyboardInput::retry_releases() noexcept {
    std::unique_lock lock(send_mutex_, std::try_to_lock);
    if (lock.owns_lock()) (void)release_pending();
}

std::size_t KeyboardInput::pending_release_count() const noexcept {
    std::size_t count = 0;
    for (const auto& key : pending_) count += key.load(std::memory_order_acquire) ? 1 : 0;
    return count;
}

KeyboardInput& keyboard_input() {
    static KeyboardInput input(native_send, [](WORD key) { return (GetAsyncKeyState(key) & 0x8000) != 0; });
    return input;
}

} // namespace smk::windows
