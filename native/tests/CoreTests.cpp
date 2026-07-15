#include "core/Animation.h"
#include "core/ClipboardHistory.h"
#include "core/ExtendedWheel.h"
#include "core/PastePolicy.h"
#include "core/Settings.h"
#include "core/WheelLayout.h"
#include "core/WheelInteraction.h"
#include "core/WheelVisualGeometry.h"
#include "updater/UpdatePolicy.h"

#include <cstdlib>
#include <cmath>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

smk::core::ClipboardEntry entry(std::wstring id, std::wstring text) {
    smk::core::ClipboardEntry value;
    value.id = std::move(id);
    value.display_text = text;
    value.plain_text = std::move(text);
    return value;
}

void settings_tests() {
    smk::core::AppSettings settings;
    settings.settings_version = 1;
    settings.wheel.shape = L"invalid";
    settings.wheel.sector_count = 7;
    settings.wheel.radius = 999;
    settings.clipboard.max_history_items = 99;
    settings.paste.restore_clipboard_after_paste = true;
    smk::core::normalize_settings(settings);
    expect(settings.settings_version == 3, "settings version normalizes to v3");
    expect(settings.wheel.shape == L"circle", "unknown wheel shape falls back to circle");
    expect(settings.wheel.sector_count == 6 || settings.wheel.sector_count == 8, "circle sector count snaps to a tier");
    expect(settings.wheel.radius == 360, "wheel radius matches managed normalization range");
    expect(settings.clipboard.max_history_items == 8, "history count remains fixed at eight");
    expect(!settings.paste.restore_clipboard_after_paste, "clipboard restore remains disabled");
    for (int index = 0; index < smk::core::kExtendedSlotCount; ++index) {
        expect(settings.wheel.extended_wheel.slots[static_cast<std::size_t>(index)].slot_index == index, "slot indexes normalize");
    }
}

void layout_tests() {
    expect(smk::core::normalize_sector_count(L"circle", 5) == 4, "circle ties choose first tier like managed OrderBy");
    expect(smk::core::normalize_sector_count(L"rectangle", 7) == 8, "rectangle count snaps to eight");
    expect(smk::core::sector_index_from_point(L"circle", 4, 180, 42, 0, 0) == -1, "dead zone cancels selection");
    expect(smk::core::sector_index_from_point(L"circle", 4, 180, 42, 0, -100) == 0, "top maps to first circle sector");
    expect(smk::core::sector_index_from_point(L"circle", 4, 180, 42, 1000, 0) == 1, "circle keeps direction beyond outer radius");
    expect(smk::core::sector_index_from_point(L"rectangle", 4, 180, 42, 100, -100) == 0, "top right maps to first rectangle cell");
    expect(smk::core::sector_index_from_point(L"rectangle", 4, 180, 42, -100, -100) == 3, "four-cell rectangle proceeds clockwise");
    expect(smk::core::sector_index_from_point(L"rectangle", 4, 180, 42, -180, -180) == 3, "rectangle corners are not clipped by a circular radius");
    expect(smk::core::sector_index_from_point(L"rectangle", 8, 180, 42, 0, -150) == 0, "eight-cell rectangle starts at top center");
    expect(smk::core::sector_index_from_point(L"rectangle", 8, 180, 42, 150, 0) == 2, "eight-cell rectangle proceeds clockwise");
    expect(smk::core::hit_test_circle(8, 42, 0, -100) == 0, "eight-sector top starts sector zero");
    expect(smk::core::hit_test_circle(8, 42, 100, 0) == 2, "eight-sector right maps clockwise");
    const double boundary = 100.0 * 0.41421356237;
    expect(smk::core::hit_test_circle(8, 42, boundary, -100, 0) == 0, "hysteresis retains prior sector near boundary");
    expect(smk::core::hit_test_circle(8, 42, 120, -100, 0) == 1, "hysteresis releases after boundary buffer");

    std::vector<smk::core::ClipboardEntry> history{entry(L"a", L"A"), entry(L"b", L"B")};
    const auto slots = smk::core::build_wheel_slots(history, 6, true);
    expect(slots.size() == 6, "wheel slots are padded to configured count");
    expect(slots[0].entry && slots[1].entry, "history occupies leading slots");
    expect(!slots[2].entry && !slots[4].entry, "unused slots remain empty");
    expect(slots[5].entry && slots[5].entry->is_quick_copy, "quick copy always occupies the final slot");

    for (const double scale : {1.0, 1.25, 1.5, 2.0}) {
        smk::core::WheelCoordinateSpace coordinates{ -1920.0, 540.0, scale };
        expect(std::abs(coordinates.logical_dx(-1920.0 + 100.0 * scale) - 100.0) < 0.0001, "DPI coordinate conversion preserves logical X");
        expect(std::abs(coordinates.logical_dy(540.0 - 80.0 * scale) + 80.0) < 0.0001, "DPI coordinate conversion preserves logical Y");
    }

    smk::core::AppSettings extended;
    extended.wheel.shape = L"circle";
    extended.wheel.radius = 180;
    extended.wheel.extended_wheel.enabled = true;
    extended.wheel.extended_wheel.breakout_buffer_pixels = 18;
    for (int index = 0; index < smk::core::kExtendedSlotCount; ++index) {
        auto& slot = extended.wheel.extended_wheel.slots[static_cast<std::size_t>(index)];
        slot.enabled = true;
        slot.mode = L"hotkey";
        slot.hotkey = L"Ctrl+" + std::to_wstring(index);
        const auto geometry = smk::core::extended_slot_geometry(index, 6.0);
        expect(static_cast<int>(geometry.direction) == index / 3, "extended slots group three actions per direction");
        expect(std::abs((geometry.end_degrees - geometry.start_degrees) - 24.0) < 0.001,
            "extended visual gap is half of the configured gap on each side");
    }
    expect(smk::core::extended_wheel_available(extended), "extended wheel is available for enabled circle settings");
    expect(std::abs(smk::core::extended_ring_thickness(100) - 52.0) < 0.001,
        "extended ring preserves its minimum thickness");
    expect(smk::core::resolve_extended_direction(extended, 190, 0, -190,
        smk::core::ExtendedDirection::none) == smk::core::ExtendedDirection::none,
        "breakout buffer prevents early direction activation");
    expect(smk::core::resolve_extended_direction(extended, 210, 0, -210,
        smk::core::ExtendedDirection::none) == smk::core::ExtendedDirection::up,
        "pointer beyond the buffer activates the upper group");
    expect(smk::core::resolve_extended_direction(extended, 190, 190, 0,
        smk::core::ExtendedDirection::up) == smk::core::ExtendedDirection::up,
        "breakout buffer latches the current direction");
    expect(smk::core::hit_test_extended_slot(extended, smk::core::ExtendedDirection::up,
        0, -240) == 1, "top direction selects the middle upper action");
    expect(smk::core::hit_test_extended_slot(extended, smk::core::ExtendedDirection::up,
        -58.5, -191.3, 1) == 1, "extended selection hysteresis retains the previous slot");
    for (const double dpi : {1.0, 1.25, 1.5, 2.0}) {
        const auto visual = smk::core::make_extended_wheel_visual_layout(
            {-320.0 * dpi, 240.0 * dpi}, 180.0 * dpi, 4.0 * dpi,
            smk::core::extended_ring_thickness(180.0) * dpi, 6.0, 10.0 * dpi, dpi);
        expect(visual.slots.size() == smk::core::kExtendedSlotCount,
            "extended visual layout always exposes twelve slots");
        expect(visual.bounds.left < visual.center.x - visual.ring_outer_radius
            && visual.bounds.right > visual.center.x + visual.ring_outer_radius,
            "extended visual bounds include selected scale and glow");
        for (int index = 0; index < smk::core::kExtendedSlotCount; ++index) {
            const auto& slot = visual.slots[static_cast<std::size_t>(index)];
            expect(smk::core::extended_visual_slot_from_point(
                visual, slot.content_center.x, slot.content_center.y) == index,
                "preview hit testing shares the twelve-slot visual geometry");
            expect(slot.text_viewport_width > 0.0 && slot.content_height > 0.0,
                "extended slot visual layout provides text and icon budgets");
        }
    }
    expect(smk::core::extended_slot_position_label(0) == L"上1"
        && smk::core::extended_slot_position_label(4) == L"右2"
        && smk::core::extended_slot_position_label(11) == L"左3",
        "extended position labels match the managed direction groups");
    extended.wheel.extended_wheel.slots[1].enabled = false;
    expect(smk::core::hit_test_extended_slot(extended, smk::core::ExtendedDirection::up,
        0, -240) == -1, "unconfigured extended slots never execute");
    extended.wheel.shape = L"rectangle";
    expect(!smk::core::extended_wheel_available(extended), "rectangle wheels never expose the extended ring");

    for (const int count : {4, 6, 8}) {
        const double step = 360.0 / count;
        for (int index = 0; index < count; ++index) {
            const auto geometry = smk::core::make_circle_sector(
                {0, 0}, 42, 180, -90.0 + index * step, -90.0 + (index + 1) * step, 10);
            expect(std::abs(std::hypot(geometry.outer_start.x, geometry.outer_start.y) - 180.0) < 0.001,
                "rounded outer start remains on outer circle");
            expect(std::abs(std::hypot(geometry.inner_start.x, geometry.inner_start.y) - 42.0) < 0.001,
                "rounded inner start remains on inner circle");
        }
        const auto visual = smk::core::make_concentric_sector_metrics(80.0, 180.0, step);
        expect(visual.inner_radius > 80.0 && visual.outer_radius < 180.0,
            "visual sector bakes the managed base scale into concentric radii");
        expect(visual.side_inset > 0.0,
            "visual sector preserves the managed parallel side gap without radial wedges");
        expect(std::abs((visual.inner_radius - 80.0) - (180.0 - visual.outer_radius)) < 0.0001,
            "concentric visual sector uses symmetric radial inset");
    }

    const auto normalized = smk::core::normalize_preview_text(
        L"  first\r\nsecond\tvalue  ", L"fallback");
    expect(normalized == L"first\nsecond    value", "preview text normalizes line endings and tabs like managed UI");
    const auto wrapped = smk::core::wrap_preview_text_centered(
        L"alpha beta gamma delta epsilon", {80.0, 80.0, 80.0}, 12.0);
    expect(!wrapped.lines.empty() && wrapped.lines.size() <= 3, "preview text wraps into available visual lines");
    const auto ellipsis = smk::core::wrap_preview_text_centered(
        L"abcdefghijklmnopqrstuvwxyz", {30.0}, 12.0);
    expect(!ellipsis.lines.empty() && ellipsis.lines.front().back() == L'\u2026',
        "preview text adds an ellipsis when the final line overflows");
    const std::vector<smk::core::TextLineSlot> image_slots{
        {{100.0, 80.0}, 90.0}, {{100.0, 95.0}, 110.0}, {{100.0, 110.0}, 90.0},
    };
    const auto landscape = smk::core::compute_sector_image_layout(
        1600, 900, image_slots, 15.0, 40.0, 180.0, -90.0, 0.0);
    expect(landscape.width > landscape.height && landscape.width <= 160.0,
        "sector image layout preserves aspect ratio and stays in its content band");
}

void animation_tests() {
    smk::core::AnimationTrack show;
    show.begin(0.88, 1.0, 100.0, 58.0, smk::core::Easing::cubic_out);
    expect(std::abs(show.sample(100.0) - 0.88) < 0.0001, "show animation starts at 0.88");
    expect(show.sample(129.0) > 0.94, "show animation uses cubic ease out");
    expect(std::abs(show.sample(158.0) - 1.0) < 0.0001 && !show.active, "show animation completes at 58ms");
    smk::core::AnimationTrack hide;
    hide.begin(1.0, 0.94, 10.0, 60.0, smk::core::Easing::cubic_in);
    expect(hide.sample(40.0) > 0.98, "hide animation uses cubic ease in");
    expect(std::abs(hide.sample(70.0) - 0.94) < 0.0001, "hide animation completes at 60ms");
    smk::core::AnimationTrack selection;
    selection.set(1.0);
    expect(std::abs(selection.sample(0.0) - 1.0) < 0.0001, "static animation track preserves visible first-frame scale");
    selection.begin(1.0, 1.04, 0.0, 75.0, smk::core::Easing::quadratic_out);
    const double current = selection.sample(25.0);
    selection.begin(current, 1.0, 25.0, 75.0, smk::core::Easing::quadratic_out);
    expect(std::abs(selection.sample(25.0) - current) < 0.0001, "selection retargets from current scale without jumping");
    smk::core::AnimationTrack extended_hide;
    extended_hide.begin(1.0, 0.0, 0.0, 50.0, smk::core::Easing::quadratic_in);
    expect(extended_hide.sample(25.0) > 0.7, "extended direction hide uses quadratic ease in");
    expect(std::abs(extended_hide.sample(50.0)) < 0.0001 && !extended_hide.active,
        "extended direction hide reaches an exact transparent endpoint at 50ms");
}

void history_tests() {
    smk::core::ClipboardHistory history(3);
    history.add_or_promote(entry(L"a", L"A"));
    history.add_or_promote(entry(L"b", L"B"));
    history.add_or_promote(entry(L"c", L"C"));
    expect(history.entries().front().plain_text == L"C", "new clipboard content is first");
    history.toggle_lock(L"b");
    history.add_or_promote(entry(L"d", L"D"));
    expect(history.entries().size() == 3, "history remains bounded");
    expect(history.entries()[1].id == L"b" && history.entries()[1].is_locked, "locked entry keeps its slot");
    history.clear_unlocked();
    expect(history.entries().size() == 1 && history.entries()[0].id == L"b", "clear retains locked entries");
    history.clear();
    expect(history.entries().empty(), "tray clear history matches the managed app and removes locked entries too");
}

void paste_tests() {
    auto single = entry(L"single", L"value");
    single.looks_like_single_cell = true;
    expect(smk::core::resolve_paste_mode(single, smk::core::PasteMode::smart) == smk::core::PasteMode::plain_text,
        "single spreadsheet cell uses plain text");
    auto table = entry(L"table", L"a\tb");
    table.looks_like_spreadsheet = true;
    table.html_text = L"<table></table>";
    expect(smk::core::resolve_paste_mode(table, smk::core::PasteMode::smart) == smk::core::PasteMode::formatted,
        "formatted spreadsheet uses formatted paste");
}

} // namespace

int main() {
    expect(smk::updater::production_update_ui_enabled,
        "production native builds expose the updater UI");
    settings_tests();
    layout_tests();
    animation_tests();
    history_tests();
    paste_tests();
    if (failures == 0) std::cout << "Native core tests passed.\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
