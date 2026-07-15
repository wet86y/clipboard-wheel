#include "platform/windows/StartupTrace.h"
#include "platform/windows/DiagnosticLog.h"

#include <windows.h>

#include <string>
#include <algorithm>

namespace smk::windows {

void startup_trace(std::wstring_view message) noexcept {
#if defined(SMK_DIAGNOSTICS)
    diagnostic_event("startup", message);
#else
    wchar_t directory[MAX_PATH]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(directory)), directory);
    if (length == 0 || length >= std::size(directory)) return;
    const std::wstring path = std::wstring(directory) + L"super-middle-key-native-startup.log";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t prefix[64]{};
    swprintf_s(prefix, L"%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu ",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, GetCurrentProcessId());
    std::wstring line(prefix);
    line.append(message);
    line.append(L"\r\n");
    const int bytes_needed = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(std::max(0, bytes_needed)), '\0');
    if (bytes_needed > 0) WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), utf8.data(), bytes_needed, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
#endif
}

} // namespace smk::windows
