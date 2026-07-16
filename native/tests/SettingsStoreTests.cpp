#include "platform/windows/SettingsStore.h"

#include <windows.h>
#include <winrt/base.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::filesystem::path temporary_root() {
    wchar_t buffer[MAX_PATH]{};
    GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
    return std::filesystem::path(buffer) /
        (L"super-middle-key-settings-tests-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(GetTickCount64()));
}

void write_bytes(const std::filesystem::path& path, std::string_view value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream result;
    result << stream.rdbuf();
    return result.str();
}

void legacy_upgrade_test(const std::filesystem::path& root) {
    const auto current = root / L"current";
    const auto legacy = root / L"legacy";
    const std::string json =
        "\xEF\xBB\xBF{\n"
        " // managed v1.1 settings\n"
        " \"settingsVersion\": 2,\n"
        " \"autoStartEnabled\": true,\n"
        " \"runAsAdministratorEnabled\": true,\n"
        " \"wheel\": {\"shape\": \"rectangle\", \"sectorCount\": 8, \"radius\": 340,"
        " \"quickCopy\": false, \"extendedWheel\": {\"enabled\": true, \"slots\": ["
        " {\"slotIndex\": 4, \"enabled\": true, \"name\": \"Editor\", \"mode\": \"hotkey\","
        " \"hotkey\": \"Ctrl+Alt+E\",},],},},\n"
        " \"clipboard\": {\"captureImages\": true, \"maxHistoryItems\": \"bad\",},\n"
        " \"mouse\": {\"defaultCaptureMode\": \"always\", \"triggerButton\": \"middle\"},\n"
        " \"paste\": {\"defaultMode\": \"smart\", \"restoreDelayMs\": 150},\n"
        " \"processRules\": {\"default\": \"always\"},\n"
        " \"update\": {\"useAccelerationNodes\": false,},\n"
        "}\n";
    write_bytes(legacy / L"settings.json", json);

    smk::windows::SettingsStore store(current, legacy);
    const auto settings = store.load();
    expect(settings.settings_version == 3, "legacy settings upgrade to version 3");
    expect(settings.auto_start_enabled && settings.run_as_administrator_enabled,
        "legacy top-level booleans survive upgrade");
    expect(settings.wheel.shape == L"rectangle" && settings.wheel.sector_count == 8
        && settings.wheel.radius == 340.0, "legacy wheel settings survive upgrade");
    expect(settings.wheel.quick_copy, "pre-v3 quick copy migration remains enabled");
    expect(settings.wheel.extended_wheel.enabled
        && settings.wheel.extended_wheel.slots[4].hotkey == L"Ctrl+Alt+E",
        "legacy extended wheel slots survive upgrade");
    expect(settings.clipboard.capture_images && settings.clipboard.max_history_items == 8,
        "bad individual fields fall back without discarding valid settings");
    expect(!settings.update.use_acceleration_nodes, "legacy update preference survives upgrade");
    expect(std::filesystem::exists(current / L"settings.json"), "upgraded settings are written to current directory");
    expect(!std::filesystem::exists(legacy / L"settings.json"), "valid legacy source is removed after conversion");
    const auto converted = read_bytes(current / L"settings.json");
    expect(converted.find("\"settingsVersion\"") != std::string::npos
        && converted.find("\"SettingsVersion\"") == std::string::npos,
        "converted settings use canonical lower camel case");
    expect(converted.find("\"defaultCaptureMode\"") == std::string::npos
        && converted.find("\"paste\"") == std::string::npos
        && converted.find("\"processRules\"") == std::string::npos,
        "deprecated fixed-policy fields are accepted but not retained for downgrade");
}

void native_pascal_case_test(const std::filesystem::path& root) {
    const auto current = root / L"pascal";
    write_bytes(current / L"settings.json",
        R"({"SettingsVersion":3,"Wheel":{"Shape":"circle","Radius":88},"Clipboard":{"CaptureImages":true}})");
    smk::windows::SettingsStore store(current);
    const auto settings = store.load();
    expect(settings.wheel.radius == 88.0 && settings.clipboard.capture_images,
        "released native PascalCase settings remain readable");
    const auto converted = read_bytes(current / L"settings.json");
    expect(converted.find("\"settingsVersion\"") != std::string::npos,
        "released native settings are canonicalized after load");
}

void future_version_test(const std::filesystem::path& root) {
    const auto current = root / L"future";
    const std::string json = R"({"settingsVersion":4,"wheel":{"radius":321}})";
    write_bytes(current / L"settings.json", json);
    smk::windows::SettingsStore store(current);
    const auto settings = store.load();
    expect(settings.wheel.radius == 180.0, "future settings versions are not imported during downgrade");
    expect(read_bytes(current / L"settings.json") == json, "future settings files are not overwritten");
}

void failed_write_test(const std::filesystem::path& root) {
    const auto blocker = root / L"blocker";
    write_bytes(blocker, "not-a-directory");
    smk::windows::SettingsStore store(blocker / L"child");
    smk::core::AppSettings settings;
    std::wstring error;
    expect(!store.save(settings, error) && !error.empty(), "failed atomic writes return an error");
}

} // namespace

int main() {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    const auto root = temporary_root();
    std::filesystem::create_directories(root);
    legacy_upgrade_test(root);
    native_pascal_case_test(root);
    future_version_test(root);
    failed_write_test(root);
    std::error_code ignored;
    if (!failures) std::filesystem::remove_all(root, ignored);
    else std::wcerr << L"Settings test files retained at: " << root << L'\n';
    winrt::uninit_apartment();
    if (failures) return 1;
    std::cout << "Settings store tests passed.\n";
    return 0;
}
