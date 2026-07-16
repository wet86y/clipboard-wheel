#include "core/WheelInteraction.h"
#include "platform/windows/DiagnosticLog.h"
#include "platform/windows/ShortcutIconResolver.h"
#include "ui/WheelWindow.h"

#include <windows.h>
#include <ole2.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void pump_for(DWORD milliseconds) {
    const ULONGLONG end = GetTickCount64() + milliseconds;
    while (GetTickCount64() < end) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        Sleep(1);
    }
}
int environment_integer(const wchar_t* name, int fallback) {
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, name) != 0 || !value) return fallback;
    const int result = _wtoi(value); free(value); return result;
}
std::wstring environment_text(const wchar_t* name, const wchar_t* fallback) {
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, name) != 0 || !value) return fallback;
    std::wstring result(value); free(value); return result;
}
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(OleInitialize(nullptr))) return EXIT_FAILURE;
    smk::windows::diagnostic_initialize();
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    wchar_t temporary[MAX_PATH]{};
    GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
    const std::wstring shortcut_path = std::wstring(temporary) + L"smk-wheel-icon-"
        + std::to_wstring(GetCurrentProcessId()) + L".lnk";
    Microsoft::WRL::ComPtr<IShellLinkW> shortcut;
    Microsoft::WRL::ComPtr<IPersistFile> shortcut_file;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(shortcut.GetAddressOf()))) && SUCCEEDED(shortcut.As(&shortcut_file))) {
        shortcut->SetPath(executable);
        shortcut->SetIconLocation(executable, 0);
        expect(SUCCEEDED(shortcut_file->Save(shortcut_path.c_str(), TRUE)),
            "wheel shortcut icon fixture is saved");
    }
    if (std::filesystem::exists(shortcut_path)) {
        HICON resolved_icon = smk::windows::resolve_shortcut_icon(shortcut_path, 40);
        expect(resolved_icon != nullptr, "wheel shortcut icon fixture resolves to an HICON");
        if (resolved_icon) DestroyIcon(resolved_icon);
    }
    {
        const int hold = environment_integer(L"SMK_WHEEL_TEST_HOLD_MS", 0);
        if (hold > 0 && GetConsoleWindow()) ShowWindow(GetConsoleWindow(), SW_HIDE);
        smk::ui::WheelWindow wheel;
        expect(wheel.create(GetModuleHandleW(nullptr)), "wheel window creates");
        smk::core::AppSettings settings;
        settings.wheel.radius = 120;
        settings.wheel.inner_dead_zone_radius = 30;
        settings.wheel.sector_count = environment_integer(L"SMK_WHEEL_TEST_SECTORS", 6);
        settings.wheel.shape = environment_text(L"SMK_WHEEL_TEST_SHAPE", L"circle");
        settings.wheel.quick_copy = true;
        smk::core::ClipboardEntry entry;
        entry.id = L"text";
        entry.display_text = L"A long clipboard preview with multiple lines and 中文内容";
        entry.plain_text = L"A long clipboard preview\r\nwith\ttabs and 中文内容 that must be clipped safely";
        smk::core::ClipboardEntry image_entry;
        image_entry.id = L"image";
        image_entry.is_image_content = true;
        image_entry.plain_text = L"browser fallback text";
        image_entry.html_text = L"<img src=\"browser-image\">";
        image_entry.image_width = image_entry.image_height = 1;
        image_entry.image_png_bytes = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{
            137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,1,0,0,0,1,8,4,0,0,0,
            181,28,12,2,0,0,0,11,73,68,65,84,120,218,99,252,255,31,0,2,235,1,245,143,89,
            148,187,0,0,0,0,73,69,78,68,174,66,96,130,
        });
        image_entry.preview_image_png_bytes = image_entry.image_png_bytes;
        auto missing_preview = image_entry;
        missing_preview.id = L"image-without-preview";
        missing_preview.preview_image_png_bytes.reset();
        const auto slots = smk::core::build_wheel_slots(
            {entry, image_entry, missing_preview}, settings.wheel.sector_count, true);
        POINT center{320, 320};
        (void)wheel.show(center, slots, settings);
        expect(wheel.cached_text_line_count_for_testing() >= 2,
            "managed-style preview text is pre-wrapped into cached lines");
        expect(wheel.multiline_text_layout_count_for_testing() == 0,
            "pre-wrapped preview lines cannot wrap again and overlap near the sector edge");
        expect(wheel.cached_image_preview_count_for_testing() == 1,
            "wheel decodes only explicit low-resolution previews and never falls back to the original image");
        expect(wheel.nontransparent_slot_count_for_testing() == slots.size(), "initial DIB frame contains every wheel sector");
        pump_for(16);
        expect(wheel.nontransparent_slot_count_for_testing() == slots.size(), "16ms DIB frame contains every wheel sector");
        pump_for(42);
        expect(wheel.nontransparent_slot_count_for_testing() == slots.size(), "58ms animation milestone contains every wheel sector");
        pump_for(22);
        HWND window = FindWindowW(L"SuperMiddleKeyNativeWheel", nullptr);
        expect(window && IsWindowVisible(window), "layered wheel becomes visible");
        expect(window && (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED), "wheel uses a layered window");
        if (hold > 0) {
            std::cout << "wheel_hwnd=" << reinterpret_cast<std::uintptr_t>(window) << '\n' << std::flush;
            pump_for(static_cast<DWORD>(hold));
        }
        wheel.update_pointer(POINT{320, 220});
        expect(wheel.selected_index() == 0, "physical point above center selects sector zero");
        expect(wheel.refresh_selected_lock(true), "selected clipboard sector accepts an immediate lock refresh");
        expect(wheel.selected_entry() && wheel.selected_entry()->is_locked,
            "lock refresh updates the visible wheel slot copy");
        wheel.update_pointer(POINT{320, -200});
        expect(wheel.selected_index() == (settings.wheel.shape == L"circle" ? 0 : -1),
            "outer-radius behavior matches the selected wheel shape");
        wheel.update_pointer(center);
        expect(wheel.selected_index() == -1, "center dead zone cancels selection");
        wheel.hide();
        pump_for(20);
        (void)wheel.show(center, slots, settings);
        pump_for(80);
        expect(IsWindowVisible(window), "reopening during hide cancels the stale hide animation");
        wheel.hide();
        pump_for(140);
        expect(!IsWindowVisible(window), "hide animation completes at 60ms");

        settings.wheel.shape = L"circle";
        settings.wheel.radius = 180;
        settings.wheel.extended_wheel.enabled = true;
        settings.wheel.extended_wheel.breakout_buffer_pixels = 18;
        smk::core::initialize_slot_indices(settings);
        for (int index = 0; index < smk::core::kExtendedSlotCount; ++index) {
            auto& action = settings.wheel.extended_wheel.slots[static_cast<std::size_t>(index)];
            action.enabled = true; action.mode = L"hotkey";
            action.hotkey = L"Ctrl+" + std::to_wstring(index);
            action.name = L"Extended action with a marquee " + std::to_wstring(index);
        }
        if (std::filesystem::exists(shortcut_path)) {
            auto& icon_action = settings.wheel.extended_wheel.slots[1];
            icon_action.mode = L"shortcut";
            icon_action.shortcut_path = shortcut_path;
            icon_action.name.clear();
        }
        settings.wheel.extended_wheel.slots[2].name = L"MicrosoftEdge";
        settings.wheel.extended_wheel.slots[3].name = L"Edge";
        (void)wheel.show(center, slots, settings);
        pump_for(80);
        if (std::filesystem::exists(shortcut_path)) {
            const auto icon_count = wheel.cached_extended_icon_count_for_testing();
            if (icon_count != 1) std::cerr << "extended_icon_count=" << icon_count << '\n';
            expect(icon_count == 1,
                "runtime extended wheel resolves and caches the configured shortcut icon");
            expect(wheel.extended_slot_has_marquee_for_testing(1),
                "shortcut filename fallback uses the same measured marquee path as a custom name");
        }
        expect(wheel.extended_slot_has_marquee_for_testing(2),
            "an Edge-length name scrolls as soon as DirectWrite reports visible truncation");
        expect(!wheel.extended_slot_has_marquee_for_testing(3),
            "a short name that fits the runtime viewport does not start a marquee");
        const double wheel_dpi_scale = GetDpiForWindow(window) / 96.0;
        const LONG breakout_offset = static_cast<LONG>(std::lround(
            (settings.wheel.radius + settings.wheel.extended_wheel.breakout_buffer_pixels + 30.0)
            * wheel_dpi_scale));
        const LONG latched_offset = static_cast<LONG>(std::lround(
            (settings.wheel.radius + settings.wheel.extended_wheel.breakout_buffer_pixels / 2.0)
            * wheel_dpi_scale));
        wheel.update_pointer(POINT{center.x, center.y - breakout_offset});
        expect(wheel.active_extended_direction() == smk::core::ExtendedDirection::up,
            "pointer beyond the breakout buffer activates the upper action group");
        expect(wheel.selected_extended_slot() == 1, "top direction selects the middle upper action");
        expect(wheel.visible_extended_slot_count_for_testing() == 0,
            "extended direction animation starts from a transparent zero-millisecond frame");
        pump_for(25);
        expect(wheel.visible_extended_slot_count_for_testing() == 3,
            "all three active-direction slots are present at the 25ms animation midpoint");
        pump_for(30);
        expect(wheel.visible_extended_slot_count_for_testing() == 3,
            "only the active direction's three extended slots remain visible after 50ms");
        const auto extended_selection = wheel.selection();
        expect(std::holds_alternative<smk::core::ExtendedWheelActionSlot>(extended_selection)
            && std::get<smk::core::ExtendedWheelActionSlot>(extended_selection).slot_index == 1,
            "wheel selection returns a discriminated extended action");
        pump_for(420);
        const auto marquee_frames = wheel.rendered_frame_count_for_testing();
        pump_for(80);
        expect(wheel.rendered_frame_count_for_testing() > marquee_frames,
            "selected overflowing shortcut text keeps the frame clock alive after the 350ms lead-in");
        wheel.update_pointer(POINT{center.x + breakout_offset, center.y});
        // The animation ends at 50ms; allow one 60 FPS scheduling quantum so
        // this window-message smoke test is not coupled to the final timer tick.
        pump_for(70);
        expect(wheel.active_extended_direction() == smk::core::ExtendedDirection::right
            && wheel.selected_extended_slot() == 4
            && wheel.visible_extended_slot_count_for_testing() == 3,
            "rapid direction changes cancel the old group and expose only the new three slots");
        wheel.update_pointer(POINT{center.x + latched_offset, center.y});
        expect(wheel.active_extended_direction() == smk::core::ExtendedDirection::right,
            "the breakout buffer keeps the active direction latched while the pointer folds back");
        wheel.update_pointer(POINT{320, 250});
        pump_for(55);
        expect(wheel.active_extended_direction() == smk::core::ExtendedDirection::none,
            "returning inside the main radius hides the extended ring");
        wheel.hide();
        pump_for(80);
    }
    DeleteFileW(shortcut_path.c_str());
    smk::windows::diagnostic_shutdown();
    OleUninitialize();
    if (!failures) std::cout << "Native wheel window smoke tests passed.\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
