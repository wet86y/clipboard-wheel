#include "platform/windows/KeyboardInput.h"

#include "platform/windows/DiagnosticLog.h"

#include <ntddkbd.h>

#include <algorithm>
#include <utility>

namespace smk::windows {
namespace {
constexpr DWORD kResetDoubleTapMs = 500;
constexpr std::array<WORD, 8> kModifierKeys{
    VK_LCONTROL, VK_RCONTROL, VK_LSHIFT, VK_RSHIFT, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN};
constexpr ULONG_PTR kInputMarker = 0x534D4B49; // SMKI：本程序的模拟键盘输入标记。
constexpr InputGeneration kActiveDispatch = InputGeneration{1} << 48;
constexpr InputGeneration kGenerationMask = kActiveDispatch - 1;

UINT physical_virtual_key(const RAWKEYBOARD& key) noexcept {
    switch (key.VKey) {
    case VK_SHIFT: return MapVirtualKeyW(key.MakeCode, MAPVK_VSC_TO_VK_EX);
    case VK_CONTROL: return (key.Flags & RI_KEY_E0) ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU: return (key.Flags & RI_KEY_E0) ? VK_RMENU : VK_LMENU;
    default: return key.VKey;
    }
}

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

void KeyboardInput::emergency_reset(InputGeneration expected) noexcept {
    if ((expected & 1) != 0) return;
    auto previous = generation_.load(std::memory_order_acquire);
    while ((previous & kGenerationMask) == expected) {
        const auto paused = (previous & ~kGenerationMask) | (((expected + 2) | 1) & kGenerationMask);
        if (generation_.compare_exchange_weak(previous, paused,
            std::memory_order_acq_rel, std::memory_order_acquire)) return;
    }
}

void KeyboardInput::resume() noexcept {
    auto previous = generation_.load(std::memory_order_acquire);
    while (!generation_.compare_exchange_weak(previous,
        (previous & ~kGenerationMask) | (((previous + 2) & ~InputGeneration{1}) & kGenerationMask),
        std::memory_order_acq_rel, std::memory_order_acquire)) {}
}

void KeyboardInput::observe_raw_key(const RAWINPUT& input, DWORD message_time) noexcept {
    if (input.header.dwType != RIM_TYPEKEYBOARD || !input.header.hDevice) return;
    const auto& event = input.data.keyboard;
    const UINT key = physical_virtual_key(event);
    if (!key || key >= 0xff || event.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE) return;

    const bool down = (event.Flags & RI_KEY_BREAK) == 0;
    const bool was_down = physical_down_[key].exchange(down, std::memory_order_acq_rel);
    if (!down) physical_suppressed_[key].store(false, std::memory_order_release);

    const auto input_generation = generation();
    auto& gesture = reset_gesture_;
    if ((input_generation & 1) != 0 || !is_emergency_reset_key(key)) {
        gesture = {};
        return;
    }
    const bool matches = gesture.key == key && gesture.device == input.header.hDevice
        && gesture.generation == input_generation && message_time - gesture.first_down_time <= kResetDoubleTapMs;
    if (down) {
        const bool modifier_held = std::any_of(kModifierKeys.begin(), kModifierKeys.end(), [&](WORD modifier) {
            return modifier != key && physical_down_[modifier].load(std::memory_order_acquire);
        });
        if (was_down || modifier_held) gesture = {};
        else if (!matches) gesture = {input_generation, input.header.hDevice, key, message_time};
    } else if (!was_down || !matches) {
        gesture = {};
    } else if (gesture.first_tap_completed) {
        gesture = {};
        emergency_reset(input_generation);
    } else {
        gesture.first_tap_completed = true;
    }
}

void KeyboardInput::observe_suppressed_key(UINT key, bool down) noexcept {
    if (key >= physical_suppressed_.size()) return;
    if (!down) physical_down_[key].store(false, std::memory_order_release);
    physical_suppressed_[key].store(down, std::memory_order_release);
}

void KeyboardInput::reset_physical_keys() noexcept {
    reset_gesture_ = {};
    for (WORD key = 1; key < physical_down_.size(); ++key) {
        const WORD generic = key == VK_LCONTROL || key == VK_RCONTROL ? VK_CONTROL
            : key == VK_LSHIFT || key == VK_RSHIFT ? VK_SHIFT
            : key == VK_LMENU || key == VK_RMENU ? VK_MENU : key;
        if (pending_[key].load(std::memory_order_acquire) || pending_[generic].load(std::memory_order_acquire)) {
            continue;
        }
        // 只给修饰键和急停键建立初始状态，已按住的急停键必须先松开。
        const bool needs_initial_state = key == VK_ESCAPE ||
            std::find(kModifierKeys.begin(), kModifierKeys.end(), key) != kModifierKeys.end();
        physical_down_[key].store(needs_initial_state && query_down(key), std::memory_order_release);
        physical_suppressed_[key].store(false, std::memory_order_release);
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
