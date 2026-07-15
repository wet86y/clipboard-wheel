#include "core/Settings.h"

#include "core/WheelLayout.h"

#include <algorithm>
#include <cwctype>

namespace smk::core {
namespace {

bool equals_ignore_case(const std::wstring& left, const wchar_t* right) {
    const std::wstring rhs(right);
    return left.size() == rhs.size() && std::equal(left.begin(), left.end(), rhs.begin(), [](wchar_t a, wchar_t b) {
        return std::towlower(a) == std::towlower(b);
    });
}

} // namespace

bool ExtendedWheelActionSlot::configured() const noexcept {
    if (!enabled) return false;
    if (equals_ignore_case(mode, L"hotkey")) return !hotkey.empty();
    if (equals_ignore_case(mode, L"shortcut")) return !shortcut_path.empty();
    return false;
}

std::wstring ExtendedWheelActionSlot::display_name() const {
    if (!name.empty()) return name;
    if (equals_ignore_case(mode, L"hotkey") && !hotkey.empty()) return hotkey;
    if (equals_ignore_case(mode, L"shortcut") && !shortcut_path.empty()) {
        const auto slash = shortcut_path.find_last_of(L"\\/");
        auto value = slash == std::wstring::npos ? shortcut_path : shortcut_path.substr(slash + 1);
        const auto extension = value.find_last_of(L'.');
        if (extension != std::wstring::npos) value.resize(extension);
        return value;
    }
    return L"未设置";
}

void initialize_slot_indices(AppSettings& settings) noexcept {
    for (int index = 0; index < kExtendedSlotCount; ++index) {
        settings.wheel.extended_wheel.slots[static_cast<std::size_t>(index)].slot_index = index;
    }
}

void normalize_settings(AppSettings& settings) noexcept {
    settings.settings_version = kCurrentSettingsVersion;
    settings.wheel.shape = normalize_shape(settings.wheel.shape);
    settings.wheel.sector_count = normalize_sector_count(settings.wheel.shape, settings.wheel.sector_count);
    settings.wheel.radius = std::clamp(settings.wheel.radius, 80.0, 360.0);
    settings.wheel.inner_dead_zone_radius = std::clamp(settings.wheel.inner_dead_zone_radius, 0.0, settings.wheel.radius - 20.0);
    settings.wheel.opacity = std::clamp(settings.wheel.opacity, 0.2, 1.0);
    settings.wheel.max_preview_chars = std::clamp(settings.wheel.max_preview_chars, 8, 120);
    settings.wheel.sector_gap_degrees = std::clamp(settings.wheel.sector_gap_degrees, 0.0, 12.0);
    settings.wheel.sector_gap_pixels = std::clamp(settings.wheel.sector_gap_pixels, 0.0, 16.0);
    settings.wheel.selected_sector_scale = std::clamp(settings.wheel.selected_sector_scale, 1.0, 1.25);
    settings.wheel.extended_wheel.breakout_buffer_pixels =
        std::clamp(settings.wheel.extended_wheel.breakout_buffer_pixels, 0.0, 80.0);
    settings.mouse.long_press_threshold_ms = std::clamp(settings.mouse.long_press_threshold_ms, 0, 2000);
    settings.clipboard.max_history_items = 8;
    settings.paste.restore_delay_ms = std::clamp(settings.paste.restore_delay_ms, 0, 5000);

    // These policies are intentionally fixed in the managed application and
    // remain fixed during the native migration.
    settings.clipboard.load_windows_clipboard_history_on_startup = true;
    settings.clipboard.capture_plain_text = true;
    settings.clipboard.capture_html = true;
    settings.clipboard.capture_rtf = true;
    settings.clipboard.capture_csv = true;
    settings.clipboard.ignore_password_like_text = false;
    settings.paste.default_mode = L"smart";
    settings.paste.restore_clipboard_after_paste = false;
    settings.paste.add_paste_to_clipboard_history = false;

    initialize_slot_indices(settings);
}

} // namespace smk::core
