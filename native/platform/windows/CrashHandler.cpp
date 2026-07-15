#include "platform/windows/CrashHandler.h"

#include <dbghelp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace smk::windows {
namespace {
std::array<std::atomic<wchar_t>, 64> phase{};
std::atomic_bool hook_available{false};
std::atomic_bool capture_ready{false};
std::atomic_uint32_t input_generation{0};
std::atomic_flag writing = ATOMIC_FLAG_INIT;
LPTOP_LEVEL_EXCEPTION_FILTER previous_filter = nullptr;
std::terminate_handler previous_terminate = nullptr;

std::wstring crash_directory() {
    wchar_t local[32'768]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, static_cast<DWORD>(std::size(local)));
    return (std::filesystem::path(local) / L"超级中键" / L"crash").wstring();
}

void rotate_dumps() noexcept {
#if defined(SMK_DIAGNOSTICS)
    try {
        const auto directory = std::filesystem::path(crash_directory());
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        std::vector<std::filesystem::directory_entry> dumps;
        for (const auto& item : std::filesystem::directory_iterator(directory, error))
            if (item.is_regular_file() && item.path().extension() == L".dmp") dumps.push_back(item);
        std::sort(dumps.begin(), dumps.end(), [](const auto& left, const auto& right) {
            return left.last_write_time() > right.last_write_time();
        });
        for (std::size_t index = 2; index < dumps.size(); ++index)
            std::filesystem::remove(dumps[index].path(), error);
    } catch (...) {}
#endif
}

void ensure_crash_directory(wchar_t* output, std::size_t capacity) noexcept {
    wchar_t local[32'768]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, static_cast<DWORD>(std::size(local)));
    wchar_t app[32'768]{};
    _snwprintf_s(app, _TRUNCATE, L"%s\\超级中键", local);
    CreateDirectoryW(app, nullptr);
    _snwprintf_s(output, capacity, _TRUNCATE, L"%s\\crash", app);
    CreateDirectoryW(output, nullptr);
}

void write_record(DWORD exception_code, EXCEPTION_POINTERS* pointers) noexcept {
    if (writing.test_and_set(std::memory_order_acq_rel)) return;
    wchar_t directory[32'768]{};
    ensure_crash_directory(directory, std::size(directory));
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stem[256]{};
    _snwprintf_s(stem, _TRUNCATE,
        L"native-crash-%04u%02u%02u-%02u%02u%02u-%lu-%lu",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        GetCurrentProcessId(), GetCurrentThreadId());
    wchar_t log_path[32'768]{};
    _snwprintf_s(log_path, _TRUNCATE, L"%s\\%s.log", directory, stem);
    HANDLE file = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        wchar_t phase_text[64]{};
        for (std::size_t index = 0; index + 1 < std::size(phase_text); ++index) {
            phase_text[index] = phase[index].load(std::memory_order_relaxed);
            if (!phase_text[index]) break;
        }
        wchar_t wide[1024]{};
        const int count = _snwprintf_s(wide, _TRUNCATE,
            L"exception=0x%08lX\r\npid=%lu\r\ntid=%lu\r\nphase=%s\r\nhook_available=%u\r\ncapture_ready=%u\r\ngeneration=%u\r\n",
            exception_code, GetCurrentProcessId(), GetCurrentThreadId(), phase_text,
            hook_available.load(std::memory_order_relaxed) ? 1u : 0u,
            capture_ready.load(std::memory_order_relaxed) ? 1u : 0u,
            input_generation.load(std::memory_order_relaxed));
        char utf8[4096]{};
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, count,
            utf8, static_cast<int>(std::size(utf8)), nullptr, nullptr);
        DWORD written = 0;
        if (bytes > 0) WriteFile(file, utf8, static_cast<DWORD>(bytes), &written, nullptr);
        FlushFileBuffers(file);
        CloseHandle(file);
    }
#if defined(SMK_DIAGNOSTICS)
    wchar_t dump_path[32'768]{};
    _snwprintf_s(dump_path, _TRUNCATE, L"%s\\%s.dmp", directory, stem);
    HANDLE dump = CreateFileW(dump_path, GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION information{};
        information.ThreadId = GetCurrentThreadId();
        information.ExceptionPointers = pointers;
        information.ClientPointers = FALSE;
        (void)MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
            MiniDumpWithThreadInfo, pointers ? &information : nullptr, nullptr, nullptr);
        FlushFileBuffers(dump);
        CloseHandle(dump);
    }
#else
    (void)pointers;
#endif
}

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* pointers) noexcept {
    const DWORD code = pointers && pointers->ExceptionRecord
        ? pointers->ExceptionRecord->ExceptionCode : 0xE0000001;
    write_record(code, pointers);
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void terminate_handler() noexcept {
    write_record(0xE0000002, nullptr);
    TerminateProcess(GetCurrentProcess(), 0xE0000002);
    std::abort();
}
}

void crash_handler_initialize() noexcept {
    crash_set_phase(L"startup");
    rotate_dumps();
    previous_filter = SetUnhandledExceptionFilter(unhandled_filter);
    previous_terminate = std::set_terminate(terminate_handler);
}

void crash_handler_shutdown() noexcept {
    SetUnhandledExceptionFilter(previous_filter);
    if (previous_terminate) std::set_terminate(previous_terminate);
}

void crash_set_phase(std::wstring_view value) noexcept {
    const auto count = std::min(value.size(), phase.size() - 1);
    for (std::size_t index = 0; index < count; ++index)
        phase[index].store(value[index], std::memory_order_relaxed);
    phase[count].store(L'\0', std::memory_order_release);
}

void crash_set_input_state(bool hook, bool ready, std::uint32_t generation) noexcept {
    hook_available.store(hook, std::memory_order_relaxed);
    capture_ready.store(ready, std::memory_order_relaxed);
    input_generation.store(generation, std::memory_order_relaxed);
}

void crash_write_emergency(DWORD exception_code) noexcept {
    write_record(exception_code, nullptr);
}

} // namespace smk::windows
