#include "platform/windows/HotkeyCaptureHook.h"
#include "platform/windows/KeyboardInput.h"
#include "platform/windows/MouseHook.h"
#include "platform/windows/TrayIcon.h"

#include <hidusage.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

struct FakeKeyboard {
    std::array<bool, 256> down{};
    std::vector<std::pair<WORD, bool>> events;
    std::function<bool(WORD, bool)> allow;
    std::function<void(WORD, bool)> after_send;
    bool send(WORD key, bool up) {
        events.emplace_back(key, up);
        if (allow && !allow(key, up)) return false;
        down[key] = !up;
        if (after_send) after_send(key, up);
        return true;
    }
};

void raw_key(smk::windows::KeyboardInput& input, WORD key, bool down, DWORD time, std::uintptr_t device = 1) {
    RAWINPUT event{};
    event.header.dwType = RIM_TYPEKEYBOARD;
    event.header.hDevice = reinterpret_cast<HANDLE>(device);
    event.data.keyboard.VKey = key;
    event.data.keyboard.Flags = down ? RI_KEY_MAKE : RI_KEY_BREAK;
    input.observe_raw_key(event, time);
}

void test_keyboard_releases() {
    using namespace smk::windows;
    constexpr std::array<WORD, 3> chord{VK_CONTROL, VK_SHIFT, VK_F12};
    FakeKeyboard sink;
    KeyboardInput input([&](WORD key, bool up) { return sink.send(key, up); },
        [&](WORD key) { return sink.down[key]; });
    expect(input.send_chord(chord, input.generation()), "complete chord succeeds");
    expect(sink.events == std::vector<std::pair<WORD, bool>>{
        {WORD{VK_CONTROL}, false}, {WORD{VK_SHIFT}, false}, {WORD{VK_F12}, false},
        {WORD{VK_F12}, true}, {WORD{VK_SHIFT}, true}, {WORD{VK_CONTROL}, true}},
        "all injected downs have reverse-order releases");

    for (const WORD failed_key : chord) {
        sink.events.clear();
        sink.allow = [=](WORD key, bool up) { return up || key != failed_key; };
        expect(!input.send_chord(chord, input.generation()), "a failed down reports failure");
        expect(std::none_of(sink.down.begin(), sink.down.end(), [](bool down) { return down; })
            && input.pending_release_count() == 0, "failure at every down position releases preceding keys");
    }

    unsigned release_failures = 1;
    sink.allow = [&](WORD key, bool up) {
        if (key == VK_CONTROL && up && release_failures) { --release_failures; return false; }
        return true;
    };
    expect(input.send_chord(chord, input.generation()) && !sink.down[VK_CONTROL],
        "a transient failed Ctrl-up is retried immediately");

    bool block_release = true;
    sink.allow = [&](WORD key, bool up) { return !(block_release && key == VK_CONTROL && up); };
    expect(!input.send_chord(chord, input.generation()) && input.pending_release_count() == 1,
        "persistent Ctrl-up failure stays in the release ledger");
    const auto downs_before = std::count_if(sink.events.begin(), sink.events.end(), [](auto event) { return !event.second; });
    expect(!input.send_chord(chord, input.generation()), "unreleased keys prevent another chord");
    expect(downs_before == std::count_if(sink.events.begin(), sink.events.end(), [](auto event) { return !event.second; }),
        "recovery never inserts a new down before outstanding releases succeed");
    block_release = false;
    input.retry_releases();
    expect(!sink.down[VK_CONTROL] && input.pending_release_count() == 0,
        "watchdog retry clears an orphaned Ctrl after input becomes available");

    sink.allow = {};
    sink.events.clear();
    sink.down[VK_CONTROL] = true;
    expect(input.send_chord(chord, input.generation()), "a pre-held modifier can be reused");
    expect(sink.down[VK_CONTROL]
        && std::none_of(sink.events.begin(), sink.events.end(), [](auto event) { return event.first == VK_CONTROL; }),
        "copy does not release a modifier that was held before injection");

    sink.down.fill(false);
    input.observe_suppressed_key(VK_LCONTROL, true);
    raw_key(input, VK_LCONTROL, true, 100);
    expect(input.send_chord(chord, input.generation()) && !sink.down[VK_CONTROL],
        "a Ctrl physically held but suppressed by recording cannot leave the injected Ctrl pressed");

    sink.down[VK_RCONTROL] = true;
    input.reset_physical_keys();
    input.emergency_reset(input.generation());
    input.retry_releases();
    expect(sink.down[VK_RCONTROL], "a modifier held before hook installation is preserved");
}

void test_emergency_during_chord() {
    using namespace smk::windows;
    constexpr std::array<WORD, 3> chord{VK_CONTROL, VK_SHIFT, VK_F12};
    for (const WORD reset_after : chord) {
        FakeKeyboard sink;
        KeyboardInput input([&](WORD key, bool up) { return sink.send(key, up); },
            [&](WORD key) { return sink.down[key]; });
        const auto old_generation = input.generation();
        sink.after_send = [&](WORD key, bool up) {
            if (up || key != reset_after) return;
            input.emergency_reset(old_generation);
            input.retry_releases(); // 模拟钩子线程与发送线程竞争，不应死锁。
        };
        expect(!input.send_chord(chord, old_generation) && input.paused(),
            "emergency reset cancels a chord at every down position");
        const auto reset_position = std::find(chord.begin(), chord.end(), reset_after);
        const auto downs = std::count_if(sink.events.begin(), sink.events.end(), [](auto event) { return !event.second; });
        expect(downs == std::distance(chord.begin(), reset_position) + 1,
            "no later key is pressed after the reset latch");
        expect(std::none_of(sink.down.begin(), sink.down.end(), [](bool down) { return down; }),
            "reset permits cleanup key-ups while refusing new key-downs");
        input.resume();
        expect(!input.accepts(old_generation), "explicit resume never revives an old action token");
    }

    FakeKeyboard sink;
    KeyboardInput input([&](WORD key, bool up) { return sink.send(key, up); },
        [&](WORD key) { return sink.down[key]; });
    for (const WORD key : {WORD{VK_LCONTROL}, WORD{VK_RCONTROL}, WORD{VK_LWIN}, WORD{VK_RWIN}, WORD{VK_ESCAPE}})
        sink.down[key] = true;
    raw_key(input, VK_RCONTROL, true, 100);
    input.emergency_reset(input.generation());
    input.retry_releases();
    expect(sink.events.empty() && sink.down[VK_LCONTROL] && sink.down[VK_RCONTROL]
        && sink.down[VK_LWIN] && sink.down[VK_RWIN] && sink.down[VK_ESCAPE],
        "reset emits no key-ups for unowned keys even when they are logically down");

    // 发送器报告成功，但另一个钩子吞掉松开事件时，异步状态检查应留下恢复任务。
    raw_key(input, VK_RCONTROL, false, 200);
    sink.down.fill(false);
    input.resume();
    bool swallow_up = true;
    sink.after_send = [&](WORD key, bool up) {
        if (swallow_up && key == VK_CONTROL && up) sink.down[key] = true;
    };
    constexpr std::array<WORD, 2> copy{VK_CONTROL, 'C'};
    expect(input.send_chord(copy, input.generation()) && input.pending_release_count() == 1,
        "reported successful but ineffective key-up remains recoverable");
    swallow_up = false;
    input.retry_releases();
    expect(!sink.down[VK_CONTROL] && input.pending_release_count() == 0,
        "watchdog also retries a swallowed key-up");

    FakeKeyboard boundary_sink;
    KeyboardInput* boundary_input = nullptr;
    KeyboardInput boundary([&](WORD key, bool up) { return boundary_sink.send(key, up); },
        [&](WORD key) {
            if (key == 'C') boundary_input->emergency_reset(boundary_input->generation());
            return boundary_sink.down[key];
        });
    boundary_input = &boundary;
    expect(!boundary.send_chord(copy, boundary.generation())
        && std::none_of(boundary_sink.events.begin(), boundary_sink.events.end(),
            [](auto event) { return event.first == 'C' && !event.second; }),
        "reset during key-state query prevents the subsequent injection commit");

    input.resume();
    const auto committed_generation = input.generation();
    auto committed = input.try_dispatch(committed_generation);
    input.emergency_reset(committed_generation);
    expect(committed && !input.try_dispatch(committed_generation)
        && !input.try_dispatch(input.generation()), "reset refuses all later dispatch permits while an earlier operation finishes");
    committed = {};
    input.resume();
    expect(!input.accepts(committed_generation), "finishing an in-flight operation cannot revert the pause generation");
}

void test_reset_key_detection() {
    using namespace smk::windows;
    for (const WORD key : {WORD{VK_ESCAPE}, WORD{VK_LWIN}, WORD{VK_RWIN}}) {
        FakeKeyboard sink;
        KeyboardInput input([&](WORD value, bool up) { return sink.send(value, up); },
            [&](WORD value) { return sink.down[value]; });
        raw_key(input, key, true, 10);
        raw_key(input, key, false, 60);
        expect(!input.paused(), "one physical tap never pauses capture");
        raw_key(input, key, true, 10'050);
        raw_key(input, key, false, 10'100);
        expect(!input.paused(), "a long gap starts a new gesture");
        raw_key(input, key, true, 10'200);
        expect(!input.paused(), "the second tap is checked on release");
        raw_key(input, key, false, 10'250);
        expect(input.paused(), "two completed taps of the same physical key pause capture");
        const auto generation = input.generation();
        raw_key(input, key, true, 10'300);
        raw_key(input, key, false, 10'350);
        raw_key(input, key, true, 10'400);
        raw_key(input, key, false, 10'450);
        expect(input.generation() == generation, "taps while paused do not repeat the reset");
    }

    FakeKeyboard sink;
    KeyboardInput input([&](WORD key, bool up) { return sink.send(key, up); },
        [&](WORD key) { return sink.down[key]; });
    raw_key(input, VK_LWIN, true, 100);
    raw_key(input, 'E', true, 120);
    raw_key(input, VK_LWIN, false, 140);
    raw_key(input, 'E', false, 150);
    raw_key(input, VK_LWIN, true, 200);
    raw_key(input, 'V', true, 220);
    raw_key(input, 'V', false, 240);
    raw_key(input, VK_LWIN, false, 250);
    expect(!input.paused() && sink.events.empty(), "Win+E and Win+V cause neither pause nor keyboard output");

    raw_key(input, VK_ESCAPE, true, 300);
    raw_key(input, VK_ESCAPE, true, 350);
    raw_key(input, VK_ESCAPE, false, 400);
    raw_key(input, VK_ESCAPE, true, 450);
    raw_key(input, VK_ESCAPE, false, 500);
    expect(!input.paused(), "a repeated key-down does not produce a completed tap");

    raw_key(input, VK_LWIN, true, 600);
    raw_key(input, VK_LWIN, false, 650);
    raw_key(input, VK_RWIN, true, 700);
    raw_key(input, VK_RWIN, false, 750);
    raw_key(input, VK_RWIN, true, 800, 2);
    raw_key(input, VK_RWIN, false, 850, 2);
    expect(!input.paused(), "different keys and different keyboards cannot pair taps");

    raw_key(input, VK_RWIN, true, 875, 0);
    raw_key(input, VK_RWIN, false, 925, 0);
    expect(!input.paused(), "events without a keyboard device never complete a gesture");

    input.pause();
    input.resume();
    raw_key(input, VK_RWIN, true, 950, 2);
    raw_key(input, VK_RWIN, false, 980, 2);
    expect(!input.paused(), "a tap before pause and resume cannot complete a gesture afterward");
    input.reset_physical_keys();
    raw_key(input, VK_RWIN, true, 1000, 2);
    raw_key(input, VK_RWIN, false, 1020, 2);
    expect(!input.paused(), "a keyboard observation reset discards earlier taps");

    raw_key(input, VK_LMENU, true, 1060);
    raw_key(input, VK_ESCAPE, true, 1080);
    raw_key(input, VK_ESCAPE, false, 1100);
    raw_key(input, VK_ESCAPE, true, 1120);
    raw_key(input, VK_ESCAPE, false, 1140);
    raw_key(input, VK_LMENU, false, 1160);
    expect(!input.paused(), "taps with another modifier held are system chords");

    raw_key(input, VK_ESCAPE, true, 1200);
    raw_key(input, VK_ESCAPE, false, 1800);
    raw_key(input, VK_ESCAPE, true, 1850);
    raw_key(input, VK_ESCAPE, false, 1900);
    expect(!input.paused(), "a long press with no repeat is not a tap");

    sink.down[VK_ESCAPE] = true;
    input.reset_physical_keys();
    raw_key(input, VK_ESCAPE, true, 2000);
    raw_key(input, VK_ESCAPE, false, 2020);
    raw_key(input, VK_ESCAPE, true, 2040);
    raw_key(input, VK_ESCAPE, false, 2060);
    expect(!input.paused(), "an Escape held when listening starts must be released before a new tap");

    HotkeyCaptureFilter filter;
    expect(!filter.handle(WM_KEYUP, VK_LCONTROL, 0, 0, true).suppress,
        "recording does not swallow the release of a key pressed before recording");
    for (const UINT key : {VK_ESCAPE, VK_LWIN, VK_RWIN}) {
        const auto decision = filter.handle(WM_KEYDOWN, key, 0, 0, true);
        expect(!decision.suppress && !decision.deliver,
            "the recording hook passes emergency keys through");
    }
    expect(!filter.handle(WM_KEYDOWN, 'C', 0, LLKHF_INJECTED, true).suppress,
        "recording cannot swallow cleanup or copy events injected by the input service");
}

void test_raw_keyboard_registration() {
    smk::windows::MouseHook listener(nullptr);
    listener.set_enabled(false);
    expect(listener.start() && listener.available(), "the input thread starts the background keyboard listener");

    RAWINPUTDEVICE keyboard{};
    UINT count = 1;
    const UINT registered = GetRegisteredRawInputDevices(&keyboard, &count, sizeof(keyboard));
    const bool observes_keyboard = registered == 1 && keyboard.hwndTarget
        && keyboard.usUsagePage == HID_USAGE_PAGE_GENERIC && keyboard.usUsage == HID_USAGE_GENERIC_KEYBOARD
        && (keyboard.dwFlags & RIDEV_INPUTSINK) != 0
        && (keyboard.dwFlags & (RIDEV_NOLEGACY | RIDEV_NOHOTKEYS)) == 0;
    expect(observes_keyboard, "background keyboard registration leaves normal input delivery enabled");
    if (!observes_keyboard) {
        std::cerr << "Registrations: " << registered << ", page: " << keyboard.usUsagePage
                  << ", usage: " << keyboard.usUsage << ", flags: " << keyboard.dwFlags
                  << ", target: " << keyboard.hwndTarget << '\n';
    }

    expect(listener.stop(5000), "the input thread stops cleanly");
    count = 1;
    expect(GetRegisteredRawInputDevices(&keyboard, &count, sizeof(keyboard)) == 0,
        "stopping the thread unregisters the keyboard listener");
}

void test_mouse_reset_pairing() {
    using namespace smk::windows;
    MouseInputSafetyState state;
    state.note_hook_available(true);
    state.note_heartbeat(100);
    state.note_heartbeat(200);
    (void)state.poll(PhysicalButtonState::up, true, 200);
    expect(state.handle(WM_MBUTTONDOWN, true, 210).suppress, "wheel starts before reset");
    (void)state.handle(WM_LBUTTONDOWN, true, 211);
    (void)state.handle(WM_RBUTTONDOWN, true, 212);
    const auto old_generation = state.generation();
    const auto pause = state.emergency_pause();
    expect(pause.dispatch && pause.event == MouseHookEvent::emergency_pause && !state.wheel_active()
        && !state.capture_ready() && state.generation() != old_generation,
        "emergency pause invalidates queued middle-up and disables capture immediately");
    expect(state.handle(WM_MBUTTONUP, true, 220).suppress
        && state.handle(WM_LBUTTONUP, true, 221).suppress
        && state.handle(WM_RBUTTONUP, true, 222).suppress,
        "late releases remain paired with the suppressed mouse downs");
    expect(!state.handle(WM_MBUTTONDOWN, true, 230).suppress
        && !state.handle(WM_LBUTTONDOWN, true, 231).suppress
        && !state.handle(WM_RBUTTONDOWN, true, 232).suppress,
        "subsequent mouse input passes normally while paused");
    state.resume();
    state.note_heartbeat(300);
    state.note_heartbeat(400);
    (void)state.poll(PhysicalButtonState::up, true, 400);
    expect(!state.capture_ready(), "session or power resume cannot clear the emergency pause");
    state.set_enabled(true);
    expect(state.handle(WM_MBUTTONDOWN, true, 410).dispatch,
        "an explicit tray enable can start a fresh wheel session");
    (void)state.emergency_pause();
    expect(!state.handle(WM_MBUTTONDOWN, true, 420).suppress
        && !state.handle(WM_MBUTTONUP, true, 421).suppress,
        "a lost old middle-up cannot swallow the next ordinary click release while paused");
}

void test_tray_recovery() {
    using namespace smk::windows;
    bool shell_ready = false;
    bool present = false;
    unsigned added = 0;
    unsigned versions = 0;
    std::wstring tooltip;
    TrayIcon tray([&](DWORD message, NOTIFYICONDATAW* data) {
        if (message == NIM_DELETE) { present = false; return true; }
        if (!shell_ready) return false;
        if (message == NIM_SETVERSION) { ++versions; return present; }
        if (message == NIM_ADD) {
            if (present) return false;
            present = true;
            ++added;
        } else if (message == NIM_MODIFY && !present) return false;
        tooltip = data->szTip;
        expect(data->hIcon && (data->uFlags & NIF_MESSAGE), "restored icon retains its image and callback");
        return true;
    });
    expect(tray.create(GetModuleHandleW(nullptr), {}), "missing shell at startup schedules recovery instead of exiting");
    const HWND window = tray.window_handle();
    expect(window && !IsWindowVisible(window) && GetParent(window) == nullptr,
        "tray uses an invisible top-level broadcast receiver");
    tray.set_enabled(false);
    shell_ready = true;
    SendMessageW(window, WM_TIMER, 1, 0);
    expect(present && added == 1 && versions == 1 && tooltip.find(L"已暂停") != std::wstring::npos,
        "startup retry restores the paused icon and notification version");
    present = false;
    SendMessageW(window, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
    expect(present && added == 2 && versions == 2 && tooltip.find(L"已暂停") != std::wstring::npos,
        "taskbar recreation re-registers the icon without re-enabling capture");
    present = false;
    SendMessageW(window, WM_TIMER, 1, 0);
    expect(present && added == 3, "maintenance recovers a missing icon even without a broadcast");
    SendMessageW(window, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
    expect(present && added == 3, "duplicate taskbar messages do not duplicate icons");
    tray.destroy();
    expect(!present && !IsWindow(window), "tray shutdown removes both registration and receiver");
}
}

int main() {
    test_keyboard_releases();
    test_emergency_during_chord();
    test_reset_key_detection();
    test_raw_keyboard_registration();
    test_mouse_reset_pairing();
    test_tray_recovery();
    if (!failures) std::cout << "Native input safety tests passed.\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
