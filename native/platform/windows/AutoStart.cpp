#include "platform/windows/AutoStart.h"

#include <windows.h>

namespace smk::windows {

bool apply_auto_start(bool enabled, const std::wstring& executable_path, std::wstring& error) {
    error.clear();
    HKEY key = nullptr;
    const auto open = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0,
        nullptr,
        0,
        KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr);
    if (open != ERROR_SUCCESS) {
        error = L"无法打开当前用户的开机启动设置。";
        return false;
    }

    const auto close_key = [&] { RegCloseKey(key); };
    if (!enabled) {
        const auto result = RegDeleteValueW(key, L"SuperMiddleKey");
        close_key();
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    }

    const std::wstring command = L"\"" + executable_path + L"\"";
    const auto result = RegSetValueExW(
        key,
        L"SuperMiddleKey",
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    close_key();
    if (result != ERROR_SUCCESS) error = L"写入开机启动设置失败。";
    return result == ERROR_SUCCESS;
}

} // namespace smk::windows
