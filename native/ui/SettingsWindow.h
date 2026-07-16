#pragma once

#include "core/ExtendedWheel.h"
#include "core/Settings.h"
#include "platform/windows/HotkeyCaptureHook.h"
#include "platform/windows/HotkeyCodec.h"
#include "platform/windows/ShortcutDropTarget.h"
#include "ui/SettingsVisualLayout.h"
#include "updater/UpdateTypes.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <array>
#include <functional>
#include <optional>
#include <string>

namespace smk::ui {

class SettingsWindow final {
public:
    using SaveCallback = std::function<std::optional<smk::core::AppSettings>(const smk::core::AppSettings&)>;

    ~SettingsWindow();
    bool create(HINSTANCE instance, SaveCallback save, smk::updater::UpdateController* update_controller,
        std::wstring version_text);
    void show(const smk::core::AppSettings& settings);

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK page_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK preview_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK page_subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
        UINT_PTR subclass_id, DWORD_PTR context);
    static LRESULT CALLBACK switch_subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
        UINT_PTR subclass_id, DWORD_PTR context);
    static LRESULT CALLBACK radio_subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
        UINT_PTR subclass_id, DWORD_PTR context);
    static LRESULT CALLBACK slider_subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
        UINT_PTR subclass_id, DWORD_PTR context);
    static LRESULT CALLBACK edit_subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
        UINT_PTR subclass_id, DWORD_PTR context);
    static LRESULT CALLBACK combo_subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
        UINT_PTR subclass_id, DWORD_PTR context);
    static LRESULT CALLBACK button_subclass_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
        UINT_PTR subclass_id, DWORD_PTR context);

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_page_message(HWND page, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_preview_message(UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT hit_test_nonclient(POINT screen_point) const;

    void create_controls();
    void create_basic_page();
    void create_wheel_page();
    void create_about_page();
    void recreate_font();
    void refresh_dpi_metrics();
    void apply_font(HWND parent);
    void layout();
    void layout_basic_page();
    void layout_wheel_page();
    void layout_about_page();
    void reposition_page_controls(int page_index);
    void redraw_page_controls(int page_index, bool update_now = false);
    void redraw_visible_controls();
    void invalidate_chrome_item(int item);
    void select_tab(int index);
    void scroll_page(int index, double delta);
    [[nodiscard]] double page_content_height(int index) const;

    void load_controls();
    void save_controls();
    void refresh_sector_items(bool reset_to_default);
    void update_value_labels();
    void set_switch_value(HWND toggle, bool checked);
    void animate_switch(HWND toggle);
    void advance_switch_animations();
    void draw_owner_control(const DRAWITEMSTRUCT& item);
    void draw_switch(const DRAWITEMSTRUCT& item);
    void draw_button(const DRAWITEMSTRUCT& item);
    void draw_radio(const DRAWITEMSTRUCT& item);
    void draw_combo(const DRAWITEMSTRUCT& item);
    void draw_combo_box(HWND combo, HDC dc, const RECT& bounds);
    void draw_value_panel(const DRAWITEMSTRUCT& item);
    void draw_slider(HWND slider, HDC dc, const RECT& bounds);

    void save_current_slot();
    void load_slot(int index);
    void update_slot_editor();
    void update_shortcut_icon(const std::wstring& path);
    void begin_or_clear_hotkey();
    bool start_hotkey_recording();
    void cancel_hotkey_recording();
    void finish_hotkey_recording();
    void record_hotkey(WPARAM key, LPARAM key_data);
    void choose_shortcut();
    void accept_shortcut_path(const std::wstring& path);
    void start_shortcut_drop_handoff();
    void poll_shortcut_drop_handoff();
    void apply_update_state(const smk::updater::UpdateViewState& state);
    void refresh_update_controls();
    [[nodiscard]] AboutUpdatePresentation about_update_presentation() const noexcept;
    [[nodiscard]] std::wstring release_notes_text() const;
    void update_release_notes_metrics(const AboutPageLayout& layout);
    bool scroll_release_notes(double delta);

    void paint_window();
    void paint_page(HWND page, int page_index);
    void paint_basic_page(ID2D1RenderTarget* target, const BasicPageLayout& layout, double scroll);
    void paint_wheel_page(ID2D1RenderTarget* target, const WheelPageLayout& layout, double scroll);
    void paint_about_page(ID2D1RenderTarget* target, const AboutPageLayout& layout, double scroll);
    void update_preview_visual_layout();
    void paint_preview();
    void paint_scrollbar(ID2D1RenderTarget* target, int page_index, double width, double height);
    void paint_caption_icon(HDC dc);
    bool ensure_render_resources();
    bool begin_dc_draw(HDC dc, const RECT& bounds, ID2D1DCRenderTarget** target);
    void end_dc_draw();
    void discard_render_resources();

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND tab_ = nullptr;
    std::array<HWND, 3> tab_buttons_{};
    std::array<HWND, 3> pages_{};
    HWND save_button_ = nullptr;
    HWND close_button_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH background_brush_gdi_ = nullptr;
    HBRUSH card_brush_gdi_ = nullptr;
    HBRUSH control_brush_gdi_ = nullptr;
    UINT dpi_ = 96;
    SettingsChromeLayout chrome_{};
    std::array<double, 3> page_scroll_{};
    int active_tab_ = 0;
    int hovered_chrome_item_ = -1;
    bool tracking_mouse_ = false;
    bool circle_shape_selected_ = true;
    bool rectangle_shape_selected_ = false;

    HWND shape_circle_ = nullptr;
    HWND shape_rectangle_ = nullptr;
    HWND sectors_ = nullptr;
    HWND radius_ = nullptr;
    HWND radius_value_ = nullptr;
    HWND dead_zone_ = nullptr;
    HWND dead_zone_value_ = nullptr;
    HWND opacity_ = nullptr;
    HWND opacity_value_ = nullptr;
    HWND quick_copy_ = nullptr;
    HWND capture_images_ = nullptr;
    HWND auto_start_ = nullptr;
    HWND administrator_ = nullptr;
    HWND admin_status_ = nullptr;

    HWND extended_enabled_ = nullptr;
    HWND breakout_ = nullptr;
    HWND breakout_value_ = nullptr;
    HWND preview_ = nullptr;
    HWND slot_title_ = nullptr;
    HWND slot_mode_ = nullptr;
    HWND slot_name_ = nullptr;
    HWND slot_action_ = nullptr;
    HWND slot_value_ = nullptr;
    HWND slot_behavior_label_ = nullptr;
    HWND slot_behavior_ = nullptr;
    HWND browser_url_label_ = nullptr;
    HWND browser_url_ = nullptr;

    HWND about_check_update_ = nullptr;
    HWND about_install_update_ = nullptr;
    HWND about_pause_resume_ = nullptr;
    HWND about_background_ = nullptr;
    HWND about_switch_node_ = nullptr;
    HWND about_cancel_ = nullptr;
    HWND about_acceleration_ = nullptr;
    HWND repository_link_ = nullptr;

    int selected_slot_ = 0;
    bool loading_ = false;
    smk::windows::HotkeyRecordingSession hotkey_recorder_;
    smk::windows::HotkeyCaptureHook hotkey_capture_;
    std::wstring hotkey_recording_error_;
    std::wstring pending_handoff_id_;
    ULONGLONG handoff_deadline_ = 0;
    smk::windows::ShortcutDropRegistration shortcut_drop_;
    smk::windows::ShortcutDropVisualState shortcut_drop_state_ =
        smk::windows::ShortcutDropVisualState::idle;
    HICON slot_icon_handle_ = nullptr;
    std::array<HICON, smk::core::kExtendedSlotCount> preview_slot_icons_{};
    std::array<std::wstring, smk::core::kExtendedSlotCount> preview_icon_paths_{};
    struct SwitchAnimation {
        HWND control = nullptr;
        double value = 0.0;
        double from = 0.0;
        double to = 0.0;
        ULONGLONG started_at = 0;
        bool active = false;
    };
    std::array<SwitchAnimation, 6> switch_animations_{};
    smk::core::AppSettings settings_{};
    smk::core::AppSettings committed_settings_{};
    smk::core::ExtendedWheelVisualLayout preview_visual_layout_{};
    smk::core::VisualPoint preview_surface_center_{};
    double preview_scale_ = 1.0;
    SaveCallback save_{};
    smk::updater::UpdateController* update_controller_ = nullptr;
    smk::updater::UpdateViewState update_state_{};
    std::wstring version_text_ = L"2.0.0";
    double release_notes_scroll_ = 0.0;
    double release_notes_extent_ = 0.0;
    bool release_notes_dragging_ = false;
    double release_notes_drag_offset_ = 0.0;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_factory_;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dc_target_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> body_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> small_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> section_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> title_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> hero_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> preview_format_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> background_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> top_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> card_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> control_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accent_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> glow_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> secondary_text_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> red_brush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> disabled_brush_;
};

} // namespace smk::ui
