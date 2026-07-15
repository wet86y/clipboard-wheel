#include "ui/SettingsWindow.h"
#include "platform/windows/ShortcutDropTarget.h"

#include <windows.h>
#include <commctrl.h>
#include <ole2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}
HWND child_with_id(HWND parent, int id) {
    HWND result = nullptr;
    struct Search { int id; HWND* result; } search{id, &result};
    EnumChildWindows(parent, [](HWND child, LPARAM context) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(context);
        if (GetDlgCtrlID(child) == search.id) { *search.result = child; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return result;
}

bool client_has_drawn_pixels(HWND window) {
    if (!window || !IsWindowVisible(window)) return false;
    RECT bounds{};
    GetClientRect(window, &bounds);
    if (bounds.right <= 0 || bounds.bottom <= 0) return false;
    HDC dc = GetDC(window);
    if (!dc) return false;
    bool found = false;
    constexpr int samples = 8;
    const int width = static_cast<int>(bounds.right);
    const int height = static_cast<int>(bounds.bottom);
    for (int row = 0; row < samples && !found; ++row) {
        const int y = std::clamp((row * 2 + 1) * height / (samples * 2), 0, height - 1);
        for (int column = 0; column < samples; ++column) {
            const int x = std::clamp((column * 2 + 1) * width / (samples * 2), 0, width - 1);
            const COLORREF pixel = GetPixel(dc, x, y);
            if (pixel != CLR_INVALID && pixel != RGB(0, 0, 0)
                && pixel != RGB(0x06, 0x10, 0x1F)
                && pixel != RGB(0x0E, 0x1D, 0x31)) {
                found = true;
                break;
            }
        }
    }
    ReleaseDC(window, dc);
    return found;
}

bool client_corners_avoid_system_white(HWND window) {
    if (!window) return false;
    RECT bounds{};
    GetClientRect(window, &bounds);
    if (bounds.right < 2 || bounds.bottom < 2) return false;
    HDC screen = GetDC(window);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, bounds.right, bounds.bottom);
    const HGDIOBJ previous = SelectObject(memory, bitmap);
    PatBlt(memory, 0, 0, bounds.right, bounds.bottom, WHITENESS);
    SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT);
    const std::array<POINT, 4> points{{
        {0, 0}, {bounds.right - 1, 0}, {0, bounds.bottom - 1},
        {bounds.right - 1, bounds.bottom - 1},
    }};
    bool valid = true;
    for (const auto point : points) {
        const COLORREF pixel = GetPixel(memory, point.x, point.y);
        if (pixel == CLR_INVALID || (GetRValue(pixel) > 245
            && GetGValue(pixel) > 245 && GetBValue(pixel) > 245)) {
            valid = false;
            break;
        }
    }
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(window, screen);
    return valid;
}

bool inside(const smk::ui::UiRect& outer, const smk::ui::UiRect& inner) {
    constexpr double epsilon = 0.01;
    return inner.x + epsilon >= outer.x && inner.y + epsilon >= outer.y
        && inner.right() <= outer.right() + epsilon && inner.bottom() <= outer.bottom() + epsilon;
}

void layout_contract_tests() {
    constexpr std::array<std::pair<double, double>, 3> sizes{{
        {760.0, 640.0}, {960.0, 720.0}, {1536.0, 864.0},
    }};
    for (const auto [width, height] : sizes) {
        const auto chrome = smk::ui::make_settings_chrome_layout(width, height);
        expect(std::abs(chrome.title_bar.height - 44.0) < 0.01
            && std::abs(chrome.tab_bar.height - 52.0) < 0.01,
            "settings chrome keeps the 44/52 DIP title and tab bands");
        expect(std::abs(chrome.footer.bottom() - height) < 0.01
            && chrome.page_viewport.bottom() < chrome.footer.y,
            "responsive settings layout always keeps the footer below page content");
        expect(inside(chrome.footer, chrome.save_button) && inside(chrome.footer, chrome.close_button),
            "save and close remain inside the fixed footer at every supported size");
        for (const auto& tab : chrome.tabs)
            expect(inside(chrome.tab_bar, tab), "all three tabs remain inside the tab band");

        const auto basic = smk::ui::make_basic_page_layout(chrome.page_viewport.width);
        expect(inside(basic.card, basic.circle_radio) && inside(basic.card, basic.rectangle_radio)
            && inside(basic.card, basic.sector_combo),
            "basic shape controls remain within their card");
        for (std::size_t index = 0; index < basic.sliders.size(); ++index)
            expect(inside(basic.card, basic.sliders[index]) && inside(basic.card, basic.values[index]),
                "basic slider and value panels remain within their card");

        const auto wheel = smk::ui::make_wheel_page_layout(
            chrome.page_viewport.width, chrome.page_viewport.height);
        expect(inside(wheel.preview_card, wheel.preview),
            "twelve-slot preview remains within its card without DPI clipping");
        expect(std::abs(wheel.preview.width - 360.0) < 0.01
            && std::abs(wheel.preview.height - 360.0) < 0.01,
            "settings preview keeps the managed fixed 360-DIP canvas at every window size");
        expect(std::abs(wheel.preview.x - (chrome.page_viewport.width - 360.0) / 2.0) < 0.01,
            "settings preview remains horizontally centered instead of shrinking responsively");
        expect(wheel.editor_card.y >= wheel.preview_card.bottom()
            && wheel.hotkey_content_height <= wheel.shortcut_content_height,
            "slot editor follows the preview and shortcut mode owns the longer scroll extent");
    }

    for (const unsigned dpi : {96u, 120u, 144u, 192u}) {
        const auto chrome = smk::ui::make_settings_chrome_layout(960.0, 720.0);
        const double scale = dpi / 96.0;
        const auto physical_footer_bottom = std::lround(chrome.footer.bottom() * scale);
        expect(physical_footer_bottom == std::lround(720.0 * scale),
            "DIP layout converts exactly once at 100/125/150/200 percent DPI");
    }
}
}

int main() {
    layout_contract_tests();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(OleInitialize(nullptr))) return EXIT_FAILURE;
    bool saved = false;
    bool allow_save = true;
    smk::core::AppSettings saved_settings;
    {
        smk::ui::SettingsWindow settings;
        expect(settings.create(GetModuleHandleW(nullptr), [&](const smk::core::AppSettings& value) {
            saved = true; saved_settings = value; return allow_save;
        }, nullptr, L"2.0.0（测试）"),
            "settings window creates");
        smk::core::AppSettings model;
        model.wheel.extended_wheel.slots[0].name = L"测试槽位";
        settings.show(model);
        HWND window = FindWindowW(L"SuperMiddleKeyNativeSettings", nullptr);
        expect(window && IsWindowVisible(window), "settings window becomes visible");
        const auto validate_visible_control_pixels = [&] {
            for (const auto [id, message] : std::array{
                std::pair{2010, "circle radio is painted before any interaction"},
                std::pair{2011, "rectangle radio is painted before any interaction"},
                std::pair{2012, "sector combo is painted before any interaction"},
                std::pair{2016, "quick-copy switch is painted before any interaction"},
                std::pair{2019, "administrator switch is painted before any interaction"},
            }) {
                expect(client_has_drawn_pixels(child_with_id(window, id)), message);
            }
            int value_panels = 0;
            EnumChildWindows(window, [](HWND child, LPARAM context) -> BOOL {
                wchar_t class_name[32]{};
                GetClassNameW(child, class_name, static_cast<int>(std::size(class_name)));
                const auto style = static_cast<DWORD>(GetWindowLongPtrW(child, GWL_STYLE));
                if (wcscmp(class_name, L"Static") == 0 && (style & SS_TYPEMASK) == SS_OWNERDRAW
                    && IsWindowVisible(child) && client_has_drawn_pixels(child)) {
                    ++*reinterpret_cast<int*>(context);
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&value_panels));
            expect(value_panels >= 3, "all basic value panels are painted before any interaction");
        };
        const auto validate_combo_popup = [&](HWND combo, int expected_items, const char* message) {
            expect(combo != nullptr, message);
            if (!combo) return;
            SendMessageW(combo, CB_SHOWDROPDOWN, TRUE, 0);
            pump();
            COMBOBOXINFO information{sizeof(information)};
            RECT list_bounds{};
            const bool available = GetComboBoxInfo(combo, &information) != FALSE
                && information.hwndList != nullptr
                && GetWindowRect(information.hwndList, &list_bounds) != FALSE;
            const UINT combo_dpi = GetDpiForWindow(combo);
            const int minimum_height = MulDiv(expected_items * 34, static_cast<int>(combo_dpi), 96);
            expect(available && IsWindowVisible(information.hwndList)
                && list_bounds.bottom - list_bounds.top >= minimum_height, message);
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            pump();
            RECT after_repaint{};
            expect(IsWindowVisible(information.hwndList)
                && GetWindowRect(information.hwndList, &after_repaint)
                && EqualRect(&list_bounds, &after_repaint),
                "root repaint does not cover an open combo popup");
            SendMessageW(combo, CB_SHOWDROPDOWN, FALSE, 0);
            pump();
        };
        validate_visible_control_pixels();
        validate_combo_popup(child_with_id(window, 2012), 3,
            "sector combo exposes all three rows instead of a two-pixel popup");
        expect((GetWindowLongPtrW(window, GWL_STYLE) & WS_CLIPCHILDREN) != 0,
            "settings root excludes child windows from parent painting");
        const auto hover_caption_item = [&](int item) {
            RECT client{};
            GetClientRect(window, &client);
            const UINT dpi = GetDpiForWindow(window);
            const auto chrome = smk::ui::make_settings_chrome_layout(
                client.right * 96.0 / std::max(1u, dpi), client.bottom * 96.0 / std::max(1u, dpi));
            const smk::ui::UiRect items[]{chrome.minimize_button, chrome.maximize_button, chrome.close_caption_button};
            const auto& bounds = items[static_cast<std::size_t>(item)];
            const int x = static_cast<int>(std::lround((bounds.x + bounds.width / 2.0) * dpi / 96.0));
            const int y = static_cast<int>(std::lround((bounds.y + bounds.height / 2.0) * dpi / 96.0));
            SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
            pump();
        };
        for (int item = 0; item < 3; ++item) {
            hover_caption_item(item);
            validate_visible_control_pixels();
        }
        SendMessageW(window, WM_MOUSELEAVE, 0, 0);
        pump();
        validate_visible_control_pixels();
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        pump();
        validate_visible_control_pixels();
        HWND basic_page = GetParent(child_with_id(window, 2010));
        RedrawWindow(basic_page, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        pump();
        validate_visible_control_pixels();
        wchar_t* initial_hold = nullptr;
        std::size_t initial_hold_length = 0;
        if (_wdupenv_s(&initial_hold, &initial_hold_length, L"SMK_SETTINGS_TEST_INITIAL_HOLD_MS") == 0
            && initial_hold) {
            const DWORD duration = static_cast<DWORD>(std::max(0, _wtoi(initial_hold)));
            free(initial_hold);
            std::cout << "settings_hwnd=" << reinterpret_cast<std::uintptr_t>(window) << '\n' << std::flush;
            const ULONGLONG deadline = GetTickCount64() + duration;
            while (GetTickCount64() < deadline) { pump(); Sleep(15); }
        }
        const auto validate_current_footer = [&] {
            RECT client{}; GetClientRect(window, &client);
            for (const int id : {2001, 2002}) {
                HWND button = child_with_id(window, id);
                RECT bounds{}; GetWindowRect(button, &bounds);
                MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2);
                expect(IsWindowVisible(button) && bounds.bottom <= client.bottom,
                    "footer buttons remain visible after resizing");
            }
        };
        const auto validate_footer = [&](int width_dip, int height_dip) {
            const UINT dpi = GetDpiForWindow(window);
            SetWindowPos(window, nullptr, 0, 0, MulDiv(width_dip, static_cast<int>(dpi), 96),
                MulDiv(height_dip, static_cast<int>(dpi), 96),
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            pump();
            validate_current_footer();
        };
        validate_footer(760, 640);
        ShowWindow(window, SW_MAXIMIZE); pump();
        validate_current_footer();
        ShowWindow(window, SW_RESTORE); pump();
        validate_footer(960, 720);
        HWND tab = FindWindowExW(window, nullptr, WC_TABCONTROLW, nullptr);
        expect(tab && TabCtrl_GetItemCount(tab) == 3, "settings window exposes three tabs");
        const auto activate_tab = [&](int selected) {
            TabCtrl_SetCurSel(tab, selected);
            NMHDR notification{tab, static_cast<UINT_PTR>(GetDlgCtrlID(tab)), TCN_SELCHANGE};
            SendMessageW(window, WM_NOTIFY, notification.idFrom, reinterpret_cast<LPARAM>(&notification));
        };
        activate_tab(1);
        const auto validate_wheel_control_pixels = [&] {
            for (const auto [id, message] : std::array{
                std::pair{2030, "extended-wheel switch is painted on the first wheel-page frame"},
                std::pair{2031, "breakout slider is painted on the first wheel-page frame"},
                std::pair{2032, "extended-wheel preview remains painted"},
            }) {
                expect(client_has_drawn_pixels(child_with_id(window, id)), message);
            }
        };
        validate_wheel_control_pixels();
        for (int item = 0; item < 3; ++item) {
            hover_caption_item(item);
            validate_wheel_control_pixels();
        }
        const HWND wheel_page = GetParent(child_with_id(window, 2030));
        SendMessageW(wheel_page, WM_MOUSEWHEEL,
            MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
        SendMessageW(wheel_page, WM_MOUSEWHEEL,
            MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
        pump();
        HWND slot_mode = child_with_id(window, 2033);
        expect(client_has_drawn_pixels(slot_mode),
            "slot mode is painted after scrolling below the fixed-size preview");
        validate_combo_popup(slot_mode, 2,
            "slot-mode combo exposes both rows after the page scrolls");
        if (slot_mode) {
            SendMessageW(slot_mode, CB_SETCURSEL, 1, 0);
            SendMessageW(GetParent(slot_mode), WM_COMMAND,
                MAKEWPARAM(2033, CBN_SELCHANGE), reinterpret_cast<LPARAM>(slot_mode));
            pump();
            validate_combo_popup(child_with_id(window, 2036), 2,
                "second-trigger combo exposes both rows in shortcut mode");
        }
        expect(client_has_drawn_pixels(child_with_id(window, 2035)),
            "slot action is painted after its editor scrolls into view");
        expect(client_corners_avoid_system_white(child_with_id(window, 2035)),
            "slot action paints its corners without a transient system-white button frame");
        expect(client_has_drawn_pixels(child_with_id(window, 2038)),
            "slot value is painted after its editor scrolls into view");
        if (HWND slot_value = child_with_id(window, 2038)) {
            smk::windows::ShortcutDropRegistration duplicate_drop_target;
            HRESULT drop_result = S_OK;
            expect(!duplicate_drop_target.register_window(slot_value, {}, &drop_result)
                && drop_result == DRAGDROP_E_ALREADYREGISTERED,
                "slot value owns a native OLE shortcut drop target");
        }
        activate_tab(2);
        expect(client_has_drawn_pixels(child_with_id(window, 2040)),
            "repository action is painted on the first about-page frame");
        for (int item = 0; item < 3; ++item) {
            hover_caption_item(item);
            expect(client_has_drawn_pixels(child_with_id(window, 2040)),
                "repository action remains painted across caption hover transitions");
        }
        activate_tab(0);
        validate_visible_control_pixels();
        pump();
        bool preview_found = false;
        EnumChildWindows(window, [](HWND child, LPARAM context) -> BOOL {
            wchar_t name[128]{}; GetClassNameW(child, name, static_cast<int>(std::size(name)));
            if (wcscmp(name, L"SuperMiddleKeySettingsPreview") == 0) { *reinterpret_cast<bool*>(context) = true; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&preview_found));
        expect(preview_found, "wheel settings page owns a Direct2D preview window");
        HWND quick_copy = child_with_id(window, 2016);
        expect(quick_copy != nullptr, "basic page exposes the quick-copy switch");
        if (quick_copy) {
            const auto before = SendMessageW(quick_copy, BM_GETCHECK, 0, 0);
            SendMessageW(GetParent(quick_copy), WM_COMMAND, MAKEWPARAM(2016, BN_CLICKED), reinterpret_cast<LPARAM>(quick_copy)); pump();
            expect(SendMessageW(quick_copy, BM_GETCHECK, 0, 0) != before, "page command forwarding toggles owner-drawn switches");
        }
        HWND circle_shape = child_with_id(window, 2010);
        HWND rectangle_shape = child_with_id(window, 2011);
        HWND sectors = child_with_id(window, 2012);
        if (circle_shape && rectangle_shape && sectors) {
            SendMessageW(circle_shape, BM_SETCHECK, BST_UNCHECKED, 0);
            SendMessageW(rectangle_shape, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(GetParent(rectangle_shape), WM_COMMAND, MAKEWPARAM(2011, BN_CLICKED), reinterpret_cast<LPARAM>(rectangle_shape)); pump();
            expect(SendMessageW(sectors, CB_GETCOUNT, 0, 0) == 2 && SendMessageW(sectors, CB_GETCURSEL, 0, 0) == 1,
                "rectangle shape refreshes the 4/8 tiers and defaults to eight");
            SendMessageW(rectangle_shape, BM_SETCHECK, BST_UNCHECKED, 0);
            SendMessageW(circle_shape, BM_SETCHECK, BST_CHECKED, 0);
            SendMessageW(GetParent(circle_shape), WM_COMMAND, MAKEWPARAM(2010, BN_CLICKED), reinterpret_cast<LPARAM>(circle_shape)); pump();
            expect(SendMessageW(sectors, CB_GETCOUNT, 0, 0) == 3 && SendMessageW(sectors, CB_GETCURSEL, 0, 0) == 1,
                "circle shape refreshes the 4/6/8 tiers and defaults to six");
        }
        wchar_t* requested_tab = nullptr;
        std::size_t requested_tab_length = 0;
        if (_wdupenv_s(&requested_tab, &requested_tab_length, L"SMK_SETTINGS_TEST_TAB") == 0 && requested_tab) {
            const int selected = std::clamp(_wtoi(requested_tab), 0, 2);
            free(requested_tab);
            activate_tab(selected);
            pump();
        }
        wchar_t* hold = nullptr;
        std::size_t hold_length = 0;
        if (_wdupenv_s(&hold, &hold_length, L"SMK_SETTINGS_TEST_HOLD_MS") == 0 && hold) {
            const DWORD duration = static_cast<DWORD>(std::max(0, _wtoi(hold)));
            free(hold);
            std::cout << "settings_hwnd=" << reinterpret_cast<std::uintptr_t>(window) << '\n' << std::flush;
            const ULONGLONG deadline = GetTickCount64() + duration;
            while (GetTickCount64() < deadline) { pump(); Sleep(15); }
        }
        HWND slot_name = child_with_id(window, 2034);
        expect(slot_name != nullptr, "slot editor exposes the custom-name field");
        if (slot_name && slot_mode) {
            RECT page_bounds{};
            GetClientRect(wheel_page, &page_bounds);
            const UINT page_dpi = GetDpiForWindow(wheel_page);
            const auto wheel_layout = smk::ui::make_wheel_page_layout(
                page_bounds.right * 96.0 / std::max(1u, page_dpi),
                page_bounds.bottom * 96.0 / std::max(1u, page_dpi));
            RECT mode_bounds{};
            GetWindowRect(slot_mode, &mode_bounds);
            MapWindowPoints(nullptr, wheel_page, reinterpret_cast<POINT*>(&mode_bounds), 2);
            const double scroll = wheel_layout.slot_mode.y
                - mode_bounds.top * 96.0 / std::max(1u, page_dpi);
            const auto validate_edit_center = [&](HWND edit, const smk::ui::UiRect& frame,
                                                   const char* message) {
                RECT edit_bounds{};
                GetWindowRect(edit, &edit_bounds);
                MapWindowPoints(nullptr, wheel_page, reinterpret_cast<POINT*>(&edit_bounds), 2);
                const int expected_center = MulDiv(static_cast<int>(std::lround(
                    (frame.y - scroll + frame.height / 2.0) * 96.0)),
                    static_cast<int>(page_dpi), 96 * 96);
                const int actual_center = (edit_bounds.top + edit_bounds.bottom) / 2;
                expect(std::abs(actual_center - expected_center) <= 1, message);
            };
            validate_edit_center(slot_name, wheel_layout.slot_name,
                "custom-name edit content is vertically centered in its forty-DIP frame");
            HWND browser_url = child_with_id(window, 2037);
            validate_edit_center(browser_url, wheel_layout.browser_url,
                "browser URL edit content is vertically centered in its forty-DIP frame");
            SetFocus(slot_mode);
            const int frame_x = MulDiv(static_cast<int>(std::lround(
                (wheel_layout.slot_name.x + 5.0) * 96.0)), static_cast<int>(page_dpi), 96 * 96);
            const int frame_y = MulDiv(static_cast<int>(std::lround(
                (wheel_layout.slot_name.y - scroll + 2.0) * 96.0)), static_cast<int>(page_dpi), 96 * 96);
            SendMessageW(wheel_page, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(frame_x, frame_y));
            pump();
            expect(GetFocus() == slot_name, "clicking the edit-frame padding focuses the real edit control");

            SendMessageW(slot_mode, CB_SETCURSEL, 0, 0);
            SendMessageW(GetParent(slot_mode), WM_COMMAND,
                MAKEWPARAM(2033, CBN_SELCHANGE), reinterpret_cast<LPARAM>(slot_mode));
            pump();
        }
        if (slot_name) SetWindowTextW(slot_name, L"应放弃的草稿");
        SendMessageW(window, WM_CLOSE, 0, 0); pump();
        expect(!IsWindowVisible(window) && !saved, "close hides without saving");
        settings.show(model);
        validate_visible_control_pixels();
        pump();
        wchar_t restored_name[128]{};
        if (slot_name) GetWindowTextW(slot_name, restored_name, static_cast<int>(std::size(restored_name)));
        expect(std::wstring(restored_name) == model.wheel.extended_wheel.slots[0].name,
            "reopening discards an unsaved slot draft");
        HWND slot_action = child_with_id(window, 2035);
        HWND slot_value = child_with_id(window, 2038);
        if (slot_action && slot_value) {
            SendMessageW(GetParent(slot_action), WM_COMMAND,
                MAKEWPARAM(2035, BN_CLICKED), reinterpret_cast<LPARAM>(slot_action));
            SendMessageW(window, WM_KEYDOWN, VK_PROCESSKEY, 0);
            SendMessageW(window, WM_KEYDOWN, VK_PACKET, 0);
            wchar_t placeholder_text[128]{};
            GetWindowTextW(slot_value, placeholder_text, static_cast<int>(std::size(placeholder_text)));
            expect(std::wstring(placeholder_text).find(L"Process") == std::wstring::npos,
                "IME placeholder keys never appear in the live shortcut display");
            SendMessageW(window, WM_KEYDOWN, VK_LCONTROL, 0);
            SendMessageW(window, WM_KEYUP, VK_LCONTROL, 0);
            SendMessageW(window, WM_KEYDOWN, VK_LSHIFT, 0);
            SendMessageW(window, WM_KEYUP, VK_LSHIFT, 0);
            SendMessageW(window, WM_KEYDOWN, VK_F12, 0);
            SendMessageW(window, WM_KEYUP, VK_F12, 0);
            wchar_t live_hotkey[128]{};
            GetWindowTextW(slot_value, live_hotkey, static_cast<int>(std::size(live_hotkey)));
            expect(std::wstring(live_hotkey) == L"Ctrl+Shift+F12",
                "settings recorder accumulates keys across separate press/release cycles");
            SendMessageW(GetParent(slot_action), WM_COMMAND,
                MAKEWPARAM(2035, BN_CLICKED), reinterpret_cast<LPARAM>(slot_action));
            expect(client_corners_avoid_system_white(slot_action),
                "recording-state text changes remain on one dark owner-drawn surface");
        }
        HWND radius = child_with_id(window, 2013);
        if (radius) SendMessageW(radius, TBM_SETPOS, TRUE, 250);
        if (preview_found && slot_name) {
            HWND preview = nullptr;
            EnumChildWindows(window, [](HWND child, LPARAM context) -> BOOL {
                wchar_t name[128]{}; GetClassNameW(child, name, static_cast<int>(std::size(name)));
                if (wcscmp(name, L"SuperMiddleKeySettingsPreview") == 0) { *reinterpret_cast<HWND*>(context) = child; return FALSE; }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&preview));
            RECT preview_rect{}; GetClientRect(preview, &preview_rect);
            const double center_x = preview_rect.right / 2.0, center_y = preview_rect.bottom / 2.0;
            const double selection_radius = std::min(preview_rect.right, preview_rect.bottom) * 0.39;
            for (int index = 0; index < 12; ++index) {
                const auto geometry = smk::core::extended_slot_geometry(index, 0.0);
                const double angle = ((geometry.start_degrees + geometry.end_degrees) * 0.5)
                    * 3.14159265358979323846 / 180.0;
                const int x = static_cast<int>(std::lround(center_x + selection_radius * std::cos(angle)));
                const int y = static_cast<int>(std::lround(center_y + selection_radius * std::sin(angle)));
                SendMessageW(preview, WM_LBUTTONDOWN, 0, MAKELPARAM(x, y)); pump();
                const auto name = L"槽位草稿 " + std::to_wstring(index + 1);
                SetWindowTextW(slot_name, name.c_str()); pump();
            }
        }
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(2001, BN_CLICKED), 0); pump();
        expect(saved && !IsWindowVisible(window), "save applies the draft and closes");
        expect(saved_settings.wheel.radius == 250, "save applies the current radius slider value");
        expect(saved_settings.wheel.extended_wheel.slots[0].hotkey == L"Ctrl+Shift+F12",
            "ending a persistent recording commits the chord to the selected slot");
        for (int index = 0; index < smk::core::kExtendedSlotCount; ++index)
            expect(saved_settings.wheel.extended_wheel.slots[static_cast<std::size_t>(index)].name == L"槽位草稿 " + std::to_wstring(index + 1),
                "all twelve extended slot drafts survive switching and save");

        const auto persisted = saved_settings;
        allow_save = false;
        settings.show(persisted); pump();
        HWND administrator = child_with_id(window, 2019);
        const auto previous_admin = SendMessageW(administrator, BM_GETCHECK, 0, 0);
        SendMessageW(GetParent(administrator), WM_COMMAND,
            MAKEWPARAM(2019, BN_CLICKED), reinterpret_cast<LPARAM>(administrator));
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(2001, BN_CLICKED), 0); pump();
        expect(IsWindowVisible(window)
            && SendMessageW(administrator, BM_GETCHECK, 0, 0) == previous_admin,
            "failed privilege-mode save keeps the window open and restores the prior switch");
        SendMessageW(window, WM_CLOSE, 0, 0); pump();
    }
    OleUninitialize();
    if (!failures) std::cout << "Native settings window tests passed.\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
