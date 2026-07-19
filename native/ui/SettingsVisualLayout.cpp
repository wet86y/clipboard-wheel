#include "ui/SettingsVisualLayout.h"

#include <algorithm>

namespace smk::ui {

SettingsChromeLayout make_settings_chrome_layout(double width, double height) noexcept {
    width = std::max(1.0, width);
    height = std::max(1.0, height);
    SettingsChromeLayout result{};
    result.title_bar = {0.0, 0.0, width, 44.0};
    result.tab_bar = {0.0, 44.0, width, 52.0};
    result.footer = {0.0, std::max(96.0, height - 64.0), width, 64.0};
    result.page_viewport = {16.0, 108.0, std::max(1.0, width - 32.0),
        std::max(1.0, result.footer.y - 120.0)};
    result.close_caption_button = {width - 46.0, 0.0, 46.0, 44.0};
    result.maximize_button = {width - 92.0, 0.0, 46.0, 44.0};
    result.minimize_button = {width - 138.0, 0.0, 46.0, 44.0};
    for (std::size_t index = 0; index < result.tabs.size(); ++index)
        result.tabs[index] = {16.0 + index * 112.0, 50.0, 112.0, 46.0};
    result.close_button = {width - 112.0, result.footer.y + 12.0, 96.0, 40.0};
    result.save_button = {width - 220.0, result.footer.y + 12.0, 96.0, 40.0};
    return result;
}

BasicPageLayout make_basic_page_layout(double width) noexcept {
    width = std::max(600.0, width);
    BasicPageLayout result{};
    result.content_height = 542.0;
    result.card = {0.0, 0.0, width, result.content_height};
    result.circle_radio = {28.0, 42.0, 92.0, 32.0};
    result.rectangle_radio = {128.0, 42.0, 92.0, 32.0};
    result.sector_combo = {28.0, 116.0, 220.0, 40.0};
    for (std::size_t index = 0; index < result.sliders.size(); ++index) {
        const double y = 178.0 + index * 62.0;
        result.sliders[index] = {154.0, y + 7.0, std::max(210.0, width - 326.0), 36.0};
        result.values[index] = {width - 142.0, y + 5.0, 114.0, 40.0};
    }
    for (std::size_t index = 0; index < result.switches.size(); ++index)
        result.switches[index] = {width - 84.0, 374.0 + index * 34.0, 56.0, 26.0};
    return result;
}

WheelPageLayout make_wheel_page_layout(double width, double viewport_height) noexcept {
    width = std::max(600.0, width);
    (void)viewport_height;
    WheelPageLayout result{};
    result.top_card = {0.0, 0.0, width, 64.0};
    result.extended_switch = {132.0, 19.0, 56.0, 26.0};
    result.breakout_slider = {width * 0.55, 14.0, std::max(160.0, width * 0.27), 36.0};
    result.breakout_value = {width - 118.0, 12.0, 94.0, 40.0};

    constexpr double preview_size = 360.0;
    // Match the managed settings preview: title, eight-DIP gap, fixed canvas,
    // hint and card padding. Small windows scroll instead of shrinking content.
    result.preview_card = {0.0, 76.0, width, 436.0};
    result.preview = {(width - preview_size) / 2.0, 116.0, preview_size, preview_size};

    const double editor_y = result.preview_card.bottom() + 12.0;
    result.editor_card = {0.0, editor_y, width, 206.0};
    result.slot_mode = {28.0, editor_y + 48.0, 180.0, 40.0};
    result.slot_name = {224.0, editor_y + 48.0, std::max(220.0, width - 252.0), 40.0};
    result.slot_action = {28.0, editor_y + 100.0, 180.0, 42.0};
    result.slot_value = {224.0, editor_y + 100.0, std::max(220.0, width - 252.0), 52.0};
    result.slot_behavior = {224.0, editor_y + 164.0, 190.0, 40.0};
    result.browser_url = {224.0, editor_y + 216.0, std::max(220.0, width - 252.0), 40.0};
    result.hotkey_content_height = editor_y + 166.0;
    result.shortcut_content_height = editor_y + 276.0;
    return result;
}

AboutPageLayout make_about_page_layout(double width, double viewport_height,
    AboutUpdatePresentation presentation) noexcept {
    width = std::max(600.0, width);
    AboutPageLayout result{};
    const bool release = presentation == AboutUpdatePresentation::release;
    const bool transfer = presentation == AboutUpdatePresentation::transfer;
    const bool completed = presentation == AboutUpdatePresentation::completed;
    const bool has_details = release || transfer || completed;

    constexpr double padding = 28.0;
    constexpr double notes_height = 176.0;
    result.update_status = {padding, 58.0, width - padding * 2.0, 42.0};
    result.check_update = {padding, 112.0, 144.0, 40.0};
    result.install_update = {padding + 156.0, 112.0, 176.0, 40.0};
    result.pause_resume = {padding, 154.0, 118.0, 40.0};
    result.background = {padding + 130.0, 154.0, 142.0, 40.0};
    result.switch_node = {padding + 284.0, 154.0, 112.0, 40.0};
    result.cancel = {padding + 408.0, 154.0, 88.0, 40.0};
    result.acceleration = {width - padding - 56.0, 119.0, 56.0, 26.0};

    double notes_y = 0.0;
    if (transfer || completed) {
        result.progress = {padding, 110.0, width - padding * 2.0, 10.0};
        result.progress_summary = {padding, 123.0, width - padding * 2.0, 24.0};
        if (completed) {
            result.check_update.y = 154.0;
            result.install_update.y = 154.0;
        }
        notes_y = 212.0;
    } else if (release) {
        notes_y = 168.0;
    }

    if (has_details) {
        result.release_notes_card = {padding, notes_y, width - padding * 2.0, notes_height + 48.0};
        result.release_notes_title = {padding + 14.0, notes_y + 8.0, result.release_notes_card.width - 28.0, 28.0};
        result.release_notes = {padding + 14.0, notes_y + 36.0,
            result.release_notes_card.width - 38.0, notes_height};
        result.release_notes_scroll_track = {result.release_notes.right() + 6.0,
            result.release_notes.y, 4.0, result.release_notes.height};
    }

    const double update_height = has_details ? result.release_notes_card.bottom() + 20.0 : 176.0;
    result.update_card = {0.0, 0.0, width, update_height};
    result.product_card = {0.0, result.update_card.bottom() + 16.0, width, 236.0};
    result.repository_link = {184.0, result.product_card.y + 141.0, 280.0, 36.0};
    result.content_height = std::max(viewport_height, result.product_card.bottom() + 16.0);
    return result;
}

} // namespace smk::ui
