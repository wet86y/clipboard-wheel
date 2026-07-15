#pragma once

#include <array>

namespace smk::ui {

struct UiRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    [[nodiscard]] double right() const noexcept { return x + width; }
    [[nodiscard]] double bottom() const noexcept { return y + height; }
    [[nodiscard]] bool contains(double px, double py) const noexcept {
        return px >= x && px <= right() && py >= y && py <= bottom();
    }
};

struct SettingsChromeLayout {
    UiRect title_bar{};
    UiRect tab_bar{};
    UiRect page_viewport{};
    UiRect footer{};
    UiRect minimize_button{};
    UiRect maximize_button{};
    UiRect close_caption_button{};
    std::array<UiRect, 3> tabs{};
    UiRect save_button{};
    UiRect close_button{};
};

struct BasicPageLayout {
    UiRect card{};
    UiRect circle_radio{};
    UiRect rectangle_radio{};
    UiRect sector_combo{};
    std::array<UiRect, 3> sliders{};
    std::array<UiRect, 3> values{};
    std::array<UiRect, 4> switches{};
    double content_height = 0.0;
};

struct WheelPageLayout {
    UiRect top_card{};
    UiRect extended_switch{};
    UiRect breakout_slider{};
    UiRect breakout_value{};
    UiRect preview_card{};
    UiRect preview{};
    UiRect editor_card{};
    UiRect slot_mode{};
    UiRect slot_name{};
    UiRect slot_action{};
    UiRect slot_value{};
    UiRect slot_behavior{};
    UiRect browser_url{};
    double hotkey_content_height = 0.0;
    double shortcut_content_height = 0.0;
};

enum class AboutUpdatePresentation {
    compact,
    release,
    transfer,
    completed,
    launching,
};

struct AboutPageLayout {
    UiRect update_card{};
    UiRect product_card{};
    UiRect update_status{};
    UiRect check_update{};
    UiRect install_update{};
    UiRect pause_resume{};
    UiRect background{};
    UiRect switch_node{};
    UiRect cancel{};
    UiRect acceleration{};
    UiRect progress{};
    UiRect progress_summary{};
    UiRect release_notes_card{};
    UiRect release_notes_title{};
    UiRect release_notes{};
    UiRect release_notes_scroll_track{};
    UiRect repository_link{};
    double content_height = 0.0;
};

[[nodiscard]] SettingsChromeLayout make_settings_chrome_layout(double width, double height) noexcept;
[[nodiscard]] BasicPageLayout make_basic_page_layout(double width) noexcept;
[[nodiscard]] WheelPageLayout make_wheel_page_layout(double width, double viewport_height) noexcept;
[[nodiscard]] AboutPageLayout make_about_page_layout(double width, double viewport_height,
    AboutUpdatePresentation presentation) noexcept;

} // namespace smk::ui
