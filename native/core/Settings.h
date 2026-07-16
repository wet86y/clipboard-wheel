#pragma once

#include <array>
#include <string>

namespace smk::core {

inline constexpr int kCurrentSettingsVersion = 3;
inline constexpr int kExtendedSlotCount = 12;

struct ExtendedWheelActionSlot {
    int slot_index = 0;
    bool enabled = false;
    std::wstring name;
    std::wstring mode = L"none";
    std::wstring hotkey;
    std::wstring shortcut_path;
    std::wstring browser_launch_url;
    std::wstring second_trigger_behavior = L"minimize";

    [[nodiscard]] bool configured() const noexcept;
    [[nodiscard]] std::wstring display_name() const;
};

struct ExtendedWheelSettings {
    bool enabled = false;
    double breakout_buffer_pixels = 18.0;
    std::array<ExtendedWheelActionSlot, kExtendedSlotCount> slots{};
};

struct WheelSettings {
    std::wstring shape = L"circle";
    int sector_count = 6;
    double radius = 180.0;
    double inner_dead_zone_radius = 42.0;
    double opacity = 0.88;
    bool show_at_cursor = true;
    bool animation_enabled = true;
    int max_preview_chars = 32;
    double sector_gap_degrees = 3.0;
    double sector_gap_pixels = 4.0;
    double selected_sector_scale = 1.08;
    bool quick_copy = true;
    ExtendedWheelSettings extended_wheel{};
};

struct ThemeSettings {
    std::wstring background_color = L"#202020";
    std::wstring sector_color = L"#2D2D2D";
    std::wstring sector_hover_color = L"#3F6AFF";
    std::wstring sector_border_color = L"#666666";
    std::wstring text_color = L"#FFFFFF";
    std::wstring muted_text_color = L"#BBBBBB";
    std::wstring center_color = L"#111111";
};

struct MouseSettings {
    bool middle_button_capture_enabled = true;
};

struct ClipboardSettings {
    int max_history_items = 8;
    bool capture_images = false;
};

struct UpdateSettings {
    bool use_acceleration_nodes = true;
};

struct AppSettings {
    int settings_version = kCurrentSettingsVersion;
    bool is_first_run = true;
    bool auto_start_enabled = false;
    bool run_as_administrator_enabled = false;
    WheelSettings wheel{};
    ThemeSettings theme{};
    MouseSettings mouse{};
    ClipboardSettings clipboard{};
    UpdateSettings update{};
};

void initialize_slot_indices(AppSettings& settings) noexcept;
void normalize_settings(AppSettings& settings) noexcept;

} // namespace smk::core
