#include "platform/windows/SettingsStore.h"

#include <windows.h>
#include <shlobj.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <system_error>

namespace smk::windows {
namespace {

using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;
using winrt::Windows::Data::Json::IJsonValue;

std::filesystem::path roaming_app_data() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &raw))) return {};
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    std::string_view input(text);
    if (input.size() >= 3 && static_cast<unsigned char>(input[0]) == 0xEF
        && static_cast<unsigned char>(input[1]) == 0xBB
        && static_cast<unsigned char>(input[2]) == 0xBF) input.remove_prefix(3);
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("Invalid UTF-8 settings file.");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), result.data(), count);
    return result;
}

std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring lower_camel(const wchar_t* name) {
    std::wstring result(name);
    if (!result.empty()) result.front() = static_cast<wchar_t>(std::towlower(result.front()));
    return result;
}

std::optional<IJsonValue> find_value(const JsonObject& object, const wchar_t* pascal_name) {
    const auto camel_name = lower_camel(pascal_name);
    if (object.HasKey(camel_name)) return object.Lookup(camel_name);
    if (object.HasKey(pascal_name)) return object.Lookup(pascal_name);
    return std::nullopt;
}

JsonObject object_or_empty(const JsonObject& root, const wchar_t* name) {
    const auto value = find_value(root, name);
    return value && value->ValueType() == JsonValueType::Object ? value->GetObject() : JsonObject{};
}

std::wstring string_value(const JsonObject& object, const wchar_t* name, const std::wstring& fallback) {
    const auto value = find_value(object, name);
    return value && value->ValueType() == JsonValueType::String ? std::wstring(value->GetString()) : fallback;
}

bool bool_value(const JsonObject& object, const wchar_t* name, bool fallback) {
    const auto value = find_value(object, name);
    return value && value->ValueType() == JsonValueType::Boolean ? value->GetBoolean() : fallback;
}

double number_value(const JsonObject& object, const wchar_t* name, double fallback) {
    const auto value = find_value(object, name);
    if (!value || value->ValueType() != JsonValueType::Number) return fallback;
    const double number = value->GetNumber();
    return std::isfinite(number) ? number : fallback;
}

int integer_value(const JsonObject& object, const wchar_t* name, int fallback) {
    const double number = number_value(object, name, static_cast<double>(fallback));
    if (number < static_cast<double>(std::numeric_limits<int>::min())
        || number > static_cast<double>(std::numeric_limits<int>::max())) return fallback;
    return static_cast<int>(number);
}

std::wstring relaxed_json(std::wstring_view input) {
    std::wstring without_comments;
    without_comments.reserve(input.size());
    bool in_string = false;
    bool escaped = false;
    for (std::size_t index = 0; index < input.size(); ++index) {
        const wchar_t current = input[index];
        if (in_string) {
            without_comments.push_back(current);
            if (escaped) escaped = false;
            else if (current == L'\\') escaped = true;
            else if (current == L'"') in_string = false;
            continue;
        }
        if (current == L'"') {
            in_string = true;
            without_comments.push_back(current);
            continue;
        }
        if (current == L'/' && index + 1 < input.size() && input[index + 1] == L'/') {
            index += 2;
            while (index < input.size() && input[index] != L'\r' && input[index] != L'\n') ++index;
            if (index < input.size()) without_comments.push_back(input[index]);
            continue;
        }
        if (current == L'/' && index + 1 < input.size() && input[index + 1] == L'*') {
            index += 2;
            while (index + 1 < input.size() && !(input[index] == L'*' && input[index + 1] == L'/')) ++index;
            if (index + 1 >= input.size()) throw std::runtime_error("Unterminated JSON comment.");
            ++index;
            continue;
        }
        without_comments.push_back(current);
    }
    if (in_string) throw std::runtime_error("Unterminated JSON string.");

    std::wstring result;
    result.reserve(without_comments.size());
    in_string = false;
    escaped = false;
    for (std::size_t index = 0; index < without_comments.size(); ++index) {
        const wchar_t current = without_comments[index];
        if (in_string) {
            result.push_back(current);
            if (escaped) escaped = false;
            else if (current == L'\\') escaped = true;
            else if (current == L'"') in_string = false;
            continue;
        }
        if (current == L'"') { in_string = true; result.push_back(current); continue; }
        if (current == L',') {
            std::size_t next = index + 1;
            while (next < without_comments.size() && std::iswspace(without_comments[next])) ++next;
            if (next < without_comments.size() && (without_comments[next] == L'}' || without_comments[next] == L']')) continue;
        }
        result.push_back(current);
    }
    return result;
}

void set(JsonObject& object, const wchar_t* name, const std::wstring& value) { object.SetNamedValue(lower_camel(name), winrt::Windows::Data::Json::JsonValue::CreateStringValue(value)); }
void set(JsonObject& object, const wchar_t* name, bool value) { object.SetNamedValue(lower_camel(name), winrt::Windows::Data::Json::JsonValue::CreateBooleanValue(value)); }
void set(JsonObject& object, const wchar_t* name, int value) { object.SetNamedValue(lower_camel(name), winrt::Windows::Data::Json::JsonValue::CreateNumberValue(value)); }
void set(JsonObject& object, const wchar_t* name, double value) { object.SetNamedValue(lower_camel(name), winrt::Windows::Data::Json::JsonValue::CreateNumberValue(value)); }

void set_object(JsonObject& object, const wchar_t* name, const JsonObject& value) {
    object.SetNamedValue(lower_camel(name), value);
}

bool write_all(HANDLE file, std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1U << 20));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, requested, &written, nullptr) || written == 0) return false;
        offset += written;
    }
    return FlushFileBuffers(file) != FALSE;
}

} // namespace

SettingsStore::SettingsStore() {
    directory_ = roaming_app_data() / L"超级中键";
    path_ = directory_ / L"settings.json";
    legacy_path_ = roaming_app_data() / L"ClipboardWheel" / L"settings.json";
}

SettingsStore::SettingsStore(std::filesystem::path directory, std::filesystem::path legacy_directory)
    : directory_(std::move(directory)), path_(directory_ / L"settings.json") {
    if (!legacy_directory.empty()) legacy_path_ = std::move(legacy_directory) / L"settings.json";
}

smk::core::AppSettings SettingsStore::load() {
    smk::core::AppSettings settings;
    smk::core::initialize_slot_indices(settings);
    const bool from_legacy = !std::filesystem::exists(path_) && !legacy_path_.empty()
        && std::filesystem::exists(legacy_path_);
    const auto source_path = from_legacy ? legacy_path_ : path_;
    if (!std::filesystem::exists(source_path)) return settings;

    try {
        std::ifstream stream(source_path, std::ios::binary);
        if (!stream) throw std::runtime_error("Unable to open settings file.");
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        if (!stream.eof() && stream.fail()) throw std::runtime_error("Unable to read settings file.");
        stream.close();
        const auto root = JsonObject::Parse(relaxed_json(utf8_to_wide(buffer.str())));
        settings.settings_version = integer_value(root, L"SettingsVersion", settings.settings_version);
        if (settings.settings_version > smk::core::kCurrentSettingsVersion) {
            smk::core::AppSettings future_defaults;
            smk::core::initialize_slot_indices(future_defaults);
            return future_defaults;
        }
        const int loaded_version = settings.settings_version;
        settings.is_first_run = bool_value(root, L"IsFirstRun", settings.is_first_run);
        settings.auto_start_enabled = bool_value(root, L"AutoStartEnabled", settings.auto_start_enabled);
        settings.run_as_administrator_enabled = bool_value(root, L"RunAsAdministratorEnabled", settings.run_as_administrator_enabled);

        const auto wheel = object_or_empty(root, L"Wheel");
        settings.wheel.shape = string_value(wheel, L"Shape", settings.wheel.shape);
        settings.wheel.sector_count = integer_value(wheel, L"SectorCount", settings.wheel.sector_count);
        settings.wheel.radius = number_value(wheel, L"Radius", settings.wheel.radius);
        settings.wheel.inner_dead_zone_radius = number_value(wheel, L"InnerDeadZoneRadius", settings.wheel.inner_dead_zone_radius);
        settings.wheel.opacity = number_value(wheel, L"Opacity", settings.wheel.opacity);
        settings.wheel.show_at_cursor = bool_value(wheel, L"ShowAtCursor", settings.wheel.show_at_cursor);
        settings.wheel.animation_enabled = bool_value(wheel, L"AnimationEnabled", settings.wheel.animation_enabled);
        settings.wheel.max_preview_chars = integer_value(wheel, L"MaxPreviewChars", settings.wheel.max_preview_chars);
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
        settings.clipboard.max_history_items = integer_value(clipboard, L"MaxHistoryItems", settings.clipboard.max_history_items);
        settings.clipboard.capture_images = bool_value(clipboard, L"CaptureImages", settings.clipboard.capture_images);
        settings.clipboard.clean_spreadsheet_plain_text = bool_value(
            clipboard, L"CleanSpreadsheetPlainText", settings.clipboard.clean_spreadsheet_plain_text);
        const auto mouse = object_or_empty(root, L"Mouse");
        settings.mouse.middle_button_capture_enabled = bool_value(mouse, L"MiddleButtonCaptureEnabled", settings.mouse.middle_button_capture_enabled);
        const auto update = object_or_empty(root, L"Update");
        settings.update.use_acceleration_nodes = bool_value(update, L"UseAccelerationNodes", settings.update.use_acceleration_nodes);

        const auto extended = object_or_empty(wheel, L"ExtendedWheel");
        settings.wheel.extended_wheel.enabled = bool_value(extended, L"Enabled", settings.wheel.extended_wheel.enabled);
        settings.wheel.extended_wheel.breakout_buffer_pixels = number_value(extended, L"BreakoutBufferPixels", settings.wheel.extended_wheel.breakout_buffer_pixels);
        const auto slot_value = find_value(extended, L"Slots");
        if (slot_value && slot_value->ValueType() == JsonValueType::Array) {
            const auto slots = slot_value->GetArray();
            for (std::uint32_t index = 0; index < slots.Size() && index < smk::core::kExtendedSlotCount; ++index) {
                const auto candidate = slots.GetAt(index);
                if (candidate.ValueType() != JsonValueType::Object) continue;
                const auto item = candidate.GetObject();
                const auto slot_index = integer_value(item, L"SlotIndex", static_cast<int>(index));
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

        if (loaded_version < 3) settings.wheel.quick_copy = true;
        smk::core::normalize_settings(settings);
        std::wstring ignored;
        const bool needs_rewrite = from_legacy || loaded_version < smk::core::kCurrentSettingsVersion
            || root.HasKey(L"SettingsVersion");
        if ((!needs_rewrite || save(settings, ignored)) && from_legacy) {
            std::error_code remove_error;
            std::filesystem::remove(legacy_path_, remove_error);
            if (!remove_error) {
                const auto parent = legacy_path_.parent_path();
                if (std::filesystem::is_empty(parent, remove_error)) std::filesystem::remove(parent, remove_error);
            }
        }
        return settings;
    } catch (...) {
        std::error_code ignored;
        const auto backup = source_path.wstring() + L".corrupt." + std::to_wstring(GetTickCount64());
        std::filesystem::rename(source_path, backup, ignored);
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
        extended.SetNamedValue(lower_camel(L"Slots"), slots);
        set_object(wheel, L"ExtendedWheel", extended);
        set_object(root, L"Wheel", wheel);

        JsonObject theme;
        set(theme, L"BackgroundColor", settings.theme.background_color);
        set(theme, L"SectorColor", settings.theme.sector_color);
        set(theme, L"SectorHoverColor", settings.theme.sector_hover_color);
        set(theme, L"SectorBorderColor", settings.theme.sector_border_color);
        set(theme, L"TextColor", settings.theme.text_color);
        set(theme, L"MutedTextColor", settings.theme.muted_text_color);
        set(theme, L"CenterColor", settings.theme.center_color);
        set_object(root, L"Theme", theme);

        JsonObject clipboard;
        set(clipboard, L"MaxHistoryItems", settings.clipboard.max_history_items);
        set(clipboard, L"CaptureImages", settings.clipboard.capture_images);
        set(clipboard, L"CleanSpreadsheetPlainText", settings.clipboard.clean_spreadsheet_plain_text);
        set_object(root, L"Clipboard", clipboard);

        JsonObject mouse;
        set(mouse, L"MiddleButtonCaptureEnabled", settings.mouse.middle_button_capture_enabled);
        set_object(root, L"Mouse", mouse);

        JsonObject update;
        set(update, L"UseAccelerationNodes", settings.update.use_acceleration_nodes);
        set_object(root, L"Update", update);

        std::error_code directory_error;
        std::filesystem::create_directories(directory_, directory_error);
        if (directory_error) throw std::system_error(directory_error);
        const auto temp = path_.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId())
            + L"." + std::to_wstring(GetTickCount64());
        const std::string json = wide_to_utf8(root.Stringify().c_str());
        HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::system_error(GetLastError(), std::system_category());
        const bool write_succeeded = write_all(file, json);
        const DWORD write_error = write_succeeded ? ERROR_SUCCESS : GetLastError();
        CloseHandle(file);
        if (!write_succeeded) {
            DeleteFileW(temp.c_str());
            throw std::system_error(write_error ? write_error : ERROR_WRITE_FAULT, std::system_category());
        }
        if (!MoveFileExW(temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD move_error = GetLastError();
            DeleteFileW(temp.c_str());
            throw std::system_error(move_error, std::system_category());
        }
        return true;
    } catch (...) {
        error = L"保存设置失败。";
        return false;
    }
}

} // namespace smk::windows
