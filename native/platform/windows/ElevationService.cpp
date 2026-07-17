#include "platform/windows/ElevationService.h"
#include "platform/windows/DiagnosticLog.h"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <userenv.h>

#include <format>
#include <memory>

namespace smk::windows {
namespace {
struct HandleCloser { void operator()(void* value) const noexcept { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
using unique_handle = std::unique_ptr<void, HandleCloser>;

std::wstring executable_path() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length); return path;
}

std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    for (const wchar_t ch : value) result += ch == L'\"' ? L"\\\"" : std::wstring(1, ch);
    return result + L"\"";
}

DWORD explorer_process_id() {
    DWORD current_session = 0; ProcessIdToSessionId(GetCurrentProcessId(), &current_session);
    unique_handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W process{sizeof(process)};
    if (!Process32FirstW(snapshot.get(), &process)) return 0;
    do {
        DWORD session = 0;
        if (_wcsicmp(process.szExeFile, L"explorer.exe") == 0
            && ProcessIdToSessionId(process.th32ProcessID, &session) && session == current_session) return process.th32ProcessID;
    } while (Process32NextW(snapshot.get(), &process));
    return 0;
}
}

bool is_administrator() noexcept {
    HANDLE token_raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_raw)) return false;
    unique_handle token(token_raw);
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    return GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &returned)
        && elevation.TokenIsElevated != 0;
}

bool start_unelevated(const std::vector<std::wstring>& arguments, std::wstring& error) {
    const DWORD explorer = explorer_process_id();
    if (!explorer) { error = L"找不到当前用户的 Windows Shell。"; return false; }
    unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, explorer));
    HANDLE token_raw = nullptr;
    if (!process || !OpenProcessToken(process.get(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY, &token_raw)) {
        error = L"无法读取 Windows Shell 权限令牌。"; return false;
    }
    unique_handle token(token_raw); HANDLE primary_raw = nullptr;
    if (!DuplicateTokenEx(token.get(), TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &primary_raw)) {
        error = L"无法复制普通权限令牌。"; return false;
    }
    unique_handle primary(primary_raw); void* environment = nullptr;
    const BOOL environment_ok = CreateEnvironmentBlock(&environment, primary.get(), FALSE);
    const auto executable = executable_path();
    std::wstring command = quote(executable); for (const auto& argument : arguments) command += L" " + quote(argument);
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION created{};
    const auto directory = executable.substr(0, executable.find_last_of(L"\\/"));
    const BOOL started = CreateProcessAsUserW(primary.get(), executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        environment_ok ? CREATE_UNICODE_ENVIRONMENT : 0, environment_ok ? environment : nullptr,
        directory.c_str(), &startup, &created);
    if (environment_ok) DestroyEnvironmentBlock(environment);
    if (!started) { error = L"无法启动普通权限进程，错误码：" + std::to_wstring(GetLastError()); return false; }
    CloseHandle(created.hThread); CloseHandle(created.hProcess); return true;
}

bool restart_with_privilege(bool elevated, std::wstring& error) {
    SMK_DIAGNOSTIC_EVENT("elevation.restart.request", std::wstring(L"requested=")
        + (elevated ? L"elevated" : L"standard") + L" current_elevated="
        + (is_administrator() ? L"true" : L"false"));
    if (!elevated) {
        const bool started = start_unelevated({L"--admin-restart"}, error);
        SMK_DIAGNOSTIC_EVENT("elevation.restart.result", std::wstring(L"requested=standard success=")
            + (started ? L"true" : L"false") + (started ? L"" : L" error_present=true"));
        return started;
    }
    const auto executable = executable_path();
    SHELLEXECUTEINFOW info{sizeof(info)}; info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.hwnd = nullptr; info.lpVerb = L"runas"; info.lpFile = executable.c_str(); info.lpParameters = L"--admin-restart";
    info.lpDirectory = nullptr; info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        error = code == ERROR_CANCELLED ? L"已取消管理员权限请求。" : L"管理员模式进程启动失败。";
        SMK_DIAGNOSTIC_EVENT("elevation.restart.result", std::format(
            L"requested=elevated success=false error={}", code));
        return false;
    }
    [[maybe_unused]] const DWORD child_pid = info.hProcess ? GetProcessId(info.hProcess) : 0;
    SMK_DIAGNOSTIC_EVENT("elevation.restart.result", std::format(
        L"requested=elevated success=true child_pid={}", child_pid));
    if (info.hProcess) CloseHandle(info.hProcess); return true;
}

} // namespace smk::windows
