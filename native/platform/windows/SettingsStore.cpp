#include "platform/windows/SettingsStore.h"

#include <windows.h>
#include <shlobj.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

#include <fstream>
#include <sstream>

namespace smk::windows {
namespace {

using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;

std::filesystem::path roaming_app_data() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &raw))) return {};
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("Invalid UTF-8 settings file.");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}

JsonObject object_or_empty(const JsonObject& root, const wchar_t* name) {
    return root.HasKey(name) ? root.GetNamedObject(name) : JsonObject{};
}

std::wstring string_value(const JsonObject& object, const wchar_t* name, const std::wstring& fallback) {
    return object.HasKey(name) ? std::wstring(object.GetNamedString(name)) : fallback;
}

bool bool_value(const JsonObject& object, const wchar_t* name, bool fallback) {
    return object.HasKey(name) ? object.GetNamedBoolean(name) : fallback;
}

double number_value(const JsonObject& object, const wchar_t* name, double fallback) {
    return object.HasKey(name) ? object.GetNamedNumber(name) : fallback;
}

void set(JsonObject& object, const wchar_t* name, const std::wstring& value) { object.SetNamedValue(name, winrt::Windows::Data::Json::JsonValue::CreateStringValue(value)); }
void set(JsonObject& object, const wchar_t* name, bool value) { object.SetNamedValue(name, winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(value)); }
void set(JsonObject& object, const wchar_t* name, int value) { object.SetNamedValue(name, winrt::Windows::Data::Json::JsonValue::CreateNumberValue(value)); }
void set(JsonObject& object, const wchar_t* name, double value) { object.SetNamedValue(name, winrt::Windows::Data::Json::JsonValue::CreateNumberValue(value)); }

} // namespace

SettingsStore::SettingsStore() {
    directory_ = roaming_app_data() / L"超级中键";
    path_ = directory_ / L"settings.json";
}

void SettingsStore::migrate_legacy_if_needed() const {
    if (std::filesystem::exists(path_)) return;
    const auto legacy = roaming_app_data() / L"ClipboardWheel" / L"settings.json";
    if (!std::filesystem::exists(legacy)) return;
    std::error_code ignored;
    std::filesystem::create_directories(directory_, ignored);
    std::filesystem::copy_file(legacy, path_, std::filesystem::copy_options::skip_existing, ignored);
}

smk::core::AppSettings SettingsStore::load() {
    smk::core::AppSettings settings;
    smk::core::initialize_slot_indices(settings);
    migrate_legacy_if_needed();
    if (!std::filesystem::exists(path_)) return settings;

    try {
        std::ifstream stream(path_, std::ios::binary);
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        const auto root = JsonObject::Parse(utf8_to_wide(buffer.str()));
        settings.settings_version = static_cast<int>(number_value(root, L"SettingsVersion", settings.settings_version));
        settings.is_first_run = bool_value(root, L"IsFirstRun", settings.is_first_run);
        settings.auto_start_enabled = bool_value(root, L"AutoStartEnabled", settings.auto_start_enabled);
        settings.run_as_administrator_enabled = bool_value(root, L"RunAsAdministratorEnabled", settings.run_as_administrator_enabled);

        const auto wheel = object_or_empty(root, L"Wheel");
        settings.wheel.shape = string_value(wheel, L"Shape", settings.wheel.shape);
        settings.wheel.sector_count = static_cast<int>(number_value(wheel, L"SectorCount", settings.wheel.sector_count));
        settings.wheel.radius = number_value(wheel, L"Radius", settings.wheel.radius);
        settings.wheel.inner_dead_zone_radius = number_value(wheel, L"InnerDeadZoneRadius", settings.wheel.inner_dead_zone_radius);
        settings.wheel.opacity = number_value(wheel, L"Opacity", settings.wheel.opacity);
        settings.wheel.show_at_cursor = bool_value(wheel, L"ShowAtCursor", settings.wheel.show_at_cursor);
        settings.wheel.animation_enabled = bool_value(wheel, L"AnimationEnabled", settings.wheel.animation_enabled);
        settings.wheel.max_preview_chars = static_cast<int>(number_value(wheel, L"MaxPreviewChars", settings.wheel.max_preview_chars));
        settings.wheel.sector_gap_degrees = number_value(wheel, L"SectorGapDegrees", settings.wheel.sector_gap_degrees);
        settings.wheel.sector_gap_pixels = number_value(wheel, L"SectorGapPixels", settings.wheel.sector_gap_pixels);
        settings.wheel.selected_sector_scale = number_value(wheel, L"SelectedSectorScale", settings.wheel.selected_sector_scale);
        settings.wheel.quick_copy = bool_value(wheel, L"QuickCopy", settings.wheel.quick_copy);

        const auto theme = object_or_empty(root, L"Theme");
        settings.theme.background_color = string_value(theme, L"BackgroundColor", settings.theme.background_color);
        settings.theme.sector_color = string_value(theme, L"SectorColor", settings.theme.sector_color);
        settings.theme.sector_hover_color = string_value(theme, L"SectorHoverColor", settings.theme.sector_hover_color);
        settings.theme.sector_border_color = string_value(theme, L"SectorBorderColor", settings.theme.sector_border_color);
        settings.theme.text_color = string_value(theme, L"TextColor", settings.theme.text_color);
        settings.theme.muted_text_color = string_value(theme, L"MutedTextColor", settings.theme.muted_text_color);
        settings.theme.center_color = string_value(theme, L"CenterColor", settings.theme.center_color);

        const auto clipboard = object_or_empty(root, L"Clipboard");
        settings.clipboard.max_history_items = static_cast<int>(number_value(clipboard, L"MaxHistoryItems", settings.clipboard.max_history_items));
        settings.clipboard.load_windows_clipboard_history_on_startup = bool_value(clipboard, L"LoadWindowsClipboardHistoryOnStartup", settings.clipboard.load_windows_clipboard_history_on_startup);
        settings.clipboard.capture_plain_text = bool_value(clipboard, L"CapturePlainText", settings.clipboard.capture_plain_text);
        settings.clipboard.capture_html = bool_value(clipboard, L"CaptureHtml", settings.clipboard.capture_html);
        settings.clipboard.capture_rtf = bool_value(clipboard, L"CaptureRtf", settings.clipboard.capture_rtf);
        settings.clipboard.capture_csv = bool_value(clipboard, L"CaptureCsv", settings.clipboard.capture_csv);
        settings.clipboard.capture_images = bool_value(clipboard, L"CaptureImages", settings.clipboard.capture_images);
        settings.clipboard.ignore_password_like_text = bool_value(clipboard, L"IgnorePasswordLikeText", settings.clipboard.ignore_password_like_text);
        const auto mouse = object_or_empty(root, L"Mouse");
        settings.mouse.default_capture_mode = string_value(mouse, L"DefaultCaptureMode", settings.mouse.default_capture_mode);
        settings.mouse.trigger_button = string_value(mouse, L"TriggerButton", settings.mouse.trigger_button);
        settings.mouse.long_press_threshold_ms = static_cast<int>(number_value(mouse, L"LongPressThresholdMs", settings.mouse.long_press_threshold_ms));
        settings.mouse.middle_button_capture_enabled = bool_value(mouse, L"MiddleButtonCaptureEnabled", settings.mouse.middle_button_capture_enabled);
        settings.mouse.suppress_original_middle_click = bool_value(mouse, L"SuppressOriginalMiddleClick", settings.mouse.suppress_original_middle_click);
        settings.mouse.cancel_when_release_in_dead_zone = bool_value(mouse, L"CancelWhenReleaseInDeadZone", settings.mouse.cancel_when_release_in_dead_zone);
        const auto paste = object_or_empty(root, L"Paste");
        settings.paste.default_mode = string_value(paste, L"DefaultMode", settings.paste.default_mode);
        settings.paste.restore_clipboard_after_paste = bool_value(paste, L"RestoreClipboardAfterPaste", settings.paste.restore_clipboard_after_paste);
        settings.paste.restore_delay_ms = static_cast<int>(number_value(paste, L"RestoreDelayMs", settings.paste.restore_delay_ms));
        settings.paste.ctrl_modifier_mode = string_value(paste, L"CtrlModifierMode", settings.paste.ctrl_modifier_mode);
        settings.paste.shift_modifier_mode = string_value(paste, L"ShiftModifierMode", settings.paste.shift_modifier_mode);
        settings.paste.add_paste_to_clipboard_history = bool_value(paste, L"AddPasteToClipboardHistory", settings.paste.add_paste_to_clipboard_history);
        const auto update = object_or_empty(root, L"Update");
        settings.update.use_acceleration_nodes = bool_value(update, L"UseAccelerationNodes", settings.update.use_acceleration_nodes);

        const auto extended = object_or_empty(wheel, L"ExtendedWheel");
        settings.wheel.extended_wheel.enabled = bool_value(extended, L"Enabled", settings.wheel.extended_wheel.enabled);
        settings.wheel.extended_wheel.breakout_buffer_pixels = number_value(extended, L"BreakoutBufferPixels", settings.wheel.extended_wheel.breakout_buffer_pixels);
        if (extended.HasKey(L"Slots")) {
            const auto slots = extended.GetNamedArray(L"Slots");
            for (std::uint32_t index = 0; index < slots.Size() && index < smk::core::kExtendedSlotCount; ++index) {
                const auto item = slots.GetObjectAt(index);
                const auto slot_index = static_cast<int>(number_value(item, L"SlotIndex", index));
                if (slot_index < 0 || slot_index >= smk::core::kExtendedSlotCount) continue;
                auto& slot = settings.wheel.extended_wheel.slots[static_cast<std::size_t>(slot_index)];
                slot.enabled = bool_value(item, L"Enabled", slot.enabled);
                slot.name = string_value(item, L"Name", slot.name);
                slot.mode = string_value(item, L"Mode", slot.mode);
                slot.hotkey = string_value(item, L"Hotkey", slot.hotkey);
                slot.shortcut_path = string_value(item, L"ShortcutPath", slot.shortcut_path);
                slot.browser_launch_url = string_value(item, L"BrowserLaunchUrl", slot.browser_launch_url);
                slot.second_trigger_behavior = string_value(item, L"SecondTriggerBehavior", slot.second_trigger_behavior);
            }
        }

        if (root.HasKey(L"ProcessRules")) {
            const auto rules = root.GetNamedObject(L"ProcessRules");
            settings.process_rules.clear();
            for (const auto& pair : rules) {
                if (pair.Value().ValueType() == winrt::Windows::Data::Json::JsonValueType::String) {
                    settings.process_rules.emplace(std::wstring(pair.Key()), std::wstring(pair.Value().GetString()));
                }
            }
            if (settings.process_rules.empty()) settings.process_rules.emplace(L"default", L"always");
        }
    } catch (...) {
        std::error_code ignored;
        const auto backup = path_.wstring() + L".corrupt";
        std::filesystem::rename(path_, backup, ignored);
        settings = {};
    }
    smk::core::normalize_settings(settings);
    return settings;
}

bool SettingsStore::save(const smk::core::AppSettings& input, std::wstring& error) const {
    error.clear();
    try {
        auto settings = input;
        smk::core::normalize_settings(settings);
        JsonObject root;
        set(root, L"SettingsVersion", settings.settings_version);
        set(root, L"IsFirstRun", settings.is_first_run);
        set(root, L"AutoStartEnabled", settings.auto_start_enabled);
        set(root, L"RunAsAdministratorEnabled", settings.run_as_administrator_enabled);

        JsonObject wheel;
        set(wheel, L"Shape", settings.wheel.shape);
        set(wheel, L"SectorCount", settings.wheel.sector_count);
        set(wheel, L"Radius", settings.wheel.radius);
        set(wheel, L"InnerDeadZoneRadius", settings.wheel.inner_dead_zone_radius);
        set(wheel, L"Opacity", settings.wheel.opacity);
        set(wheel, L"ShowAtCursor", settings.wheel.show_at_cursor);
        set(wheel, L"AnimationEnabled", settings.wheel.animation_enabled);
        set(wheel, L"MaxPreviewChars", settings.wheel.max_preview_chars);
        set(wheel, L"SectorGapDegrees", settings.wheel.sector_gap_degrees);
        set(wheel, L"SectorGapPixels", settings.wheel.sector_gap_pixels);
        set(wheel, L"SelectedSectorScale", settings.wheel.selected_sector_scale);
        set(wheel, L"QuickCopy", settings.wheel.quick_copy);
        JsonObject extended;
        set(extended, L"Enabled", settings.wheel.extended_wheel.enabled);
        set(extended, L"BreakoutBufferPixels", settings.wheel.extended_wheel.breakout_buffer_pixels);
        JsonArray slots;
        for (const auto& slot : settings.wheel.extended_wheel.slots) {
            JsonObject item;
            set(item, L"SlotIndex", slot.slot_index);
            set(item, L"Enabled", slot.enabled);
            set(item, L"Name", slot.name);
            set(item, L"Mode", slot.mode);
            set(item, L"Hotkey", slot.hotkey);
            set(item, L"ShortcutPath", slot.shortcut_path);
            set(item, L"BrowserLaunchUrl", slot.browser_launch_url);
            set(item, L"SecondTriggerBehavior", slot.second_trigger_behavior);
            slots.Append(item);
        }
        extended.SetNamedValue(L"Slots", slots);
        wheel.SetNamedValue(L"ExtendedWheel", extended);
        root.SetNamedValue(L"Wheel", wheel);

        JsonObject theme;
        set(theme, L"BackgroundColor", settings.theme.background_color);
        set(theme, L"SectorColor", settings.theme.sector_color);
        set(theme, L"SectorHoverColor", settings.theme.sector_hover_color);
        set(theme, L"SectorBorderColor", settings.theme.sector_border_color);
        set(theme, L"TextColor", settings.theme.text_color);
        set(theme, L"MutedTextColor", settings.theme.muted_text_color);
        set(theme, L"CenterColor", settings.theme.center_color);
        root.SetNamedValue(L"Theme", theme);

        JsonObject clipboard;
        set(clipboard, L"MaxHistoryItems", settings.clipboard.max_history_items);
        set(clipboard, L"LoadWindowsClipboardHistoryOnStartup", settings.clipboard.load_windows_clipboard_history_on_startup);
        set(clipboard, L"CapturePlainText", settings.clipboard.capture_plain_text);
        set(clipboard, L"CaptureHtml", settings.clipboard.capture_html);
        set(clipboard, L"CaptureRtf", settings.clipboard.capture_rtf);
        set(clipboard, L"CaptureCsv", settings.clipboard.capture_csv);
        set(clipboard, L"CaptureImages", settings.clipboard.capture_images);
        set(clipboard, L"IgnorePasswordLikeText", settings.clipboard.ignore_password_like_text);
        root.SetNamedValue(L"Clipboard", clipboard);

        JsonObject mouse;
        set(mouse, L"DefaultCaptureMode", settings.mouse.default_capture_mode);
        set(mouse, L"TriggerButton", settings.mouse.trigger_button);
        set(mouse, L"LongPressThresholdMs", settings.mouse.long_press_threshold_ms);
        set(mouse, L"SuppressOriginalMiddleClick", settings.mouse.suppress_original_middle_click);
        set(mouse, L"CancelWhenReleaseInDeadZone", settings.mouse.cancel_when_release_in_dead_zone);
        set(mouse, L"MiddleButtonCaptureEnabled", settings.mouse.middle_button_capture_enabled);
        root.SetNamedValue(L"Mouse", mouse);

        JsonObject paste;
        set(paste, L"DefaultMode", settings.paste.default_mode);
        set(paste, L"RestoreClipboardAfterPaste", settings.paste.restore_clipboard_after_paste);
        set(paste, L"RestoreDelayMs", settings.paste.restore_delay_ms);
        set(paste, L"CtrlModifierMode", settings.paste.ctrl_modifier_mode);
        set(paste, L"ShiftModifierMode", settings.paste.shift_modifier_mode);
        set(paste, L"AddPasteToClipboardHistory", settings.paste.add_paste_to_clipboard_history);
        root.SetNamedValue(L"Paste", paste);

        JsonObject update;
        set(update, L"UseAccelerationNodes", settings.update.use_acceleration_nodes);
        root.SetNamedValue(L"Update", update);

        JsonObject process_rules;
        for (const auto& [process, mode] : settings.process_rules) set(process_rules, process.c_str(), mode);
        root.SetNamedValue(L"ProcessRules", process_rules);

        std::error_code ignored;
        std::filesystem::create_directories(directory_, ignored);
        const auto temp = path_.wstring() + L".tmp";
        std::ofstream stream(std::filesystem::path(temp), std::ios::binary | std::ios::trunc);
        stream << wide_to_utf8(root.Stringify().c_str());
        stream.close();
        if (!MoveFileExW(temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temp.c_str());
            throw std::runtime_error("MoveFileExW failed");
        }
        return true;
    } catch (...) {
        error = L"保存设置失败。";
        return false;
    }
}

} // namespace smk::windows
