#include "platform/windows/DiagnosticLog.h"
#include "Version.h"

#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <format>
#include <mutex>
#include <thread>

namespace smk::windows {
#if defined(SMK_DIAGNOSTICS)
namespace {
constexpr std::size_t kMaximumQueuedLines = 4096;
constexpr std::uint64_t kMaximumFileBytes = 2ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaximumFiles = 12;

struct Logger {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::string> lines;
    std::thread writer;
    std::filesystem::path directory;
    std::wstring current_path;
    HANDLE file = INVALID_HANDLE_VALUE;
    bool stopping = false;
    std::uint64_t bytes = 0;
    unsigned part = 0;
};
Logger logger;

using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
struct DisplayContext {
    std::wstring summary;
    GetDpiForMonitorFn get_dpi = nullptr;
    unsigned index = 0;
};

BOOL CALLBACK append_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM value) {
    auto& context = *reinterpret_cast<DisplayContext*>(value);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    UINT dpi_x = GetDpiForSystem(), dpi_y = dpi_x;
    if (context.get_dpi) context.get_dpi(monitor, 0, &dpi_x, &dpi_y);
    if (!context.summary.empty()) context.summary += L";";
    context.summary += std::format(L"{}:[{},{},{},{}],dpi={}x{},primary={}", context.index++,
        info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right, info.rcMonitor.bottom,
        dpi_x, dpi_y, (info.dwFlags & MONITORINFOF_PRIMARY) != 0);
    return TRUE;
}

std::wstring display_summary() {
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    DisplayContext context;
    if (shcore) context.get_dpi = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"));
    EnumDisplayMonitors(nullptr, nullptr, append_monitor, reinterpret_cast<LPARAM>(&context));
    if (shcore) FreeLibrary(shcore);
    return context.summary;
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(0, count)), '\0');
    if (count > 0) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

std::string escape_json(std::string value) {
    std::string result; result.reserve(value.size() + 16);
    for (const char ch : value) {
        switch (ch) { case '\\': result += "\\\\"; break; case '"': result += "\\\""; break;
        case '\r': result += "\\r"; break; case '\n': result += "\\n"; break; case '\t': result += "\\t"; break;
        default: if (static_cast<unsigned char>(ch) >= 0x20) result += ch; break; }
    }
    return result;
}

void redact(std::string& value, std::string_view key) {
    std::string lower = value; std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::size_t position = 0;
    while ((position = lower.find(key, position)) != std::string::npos) {
        const auto equals = lower.find('=', position + key.size()); if (equals == std::string::npos) break;
        const auto end = value.find(' ', equals + 1); const auto length = (end == std::string::npos ? value.size() : end) - equals - 1;
        value.replace(equals + 1, length, "[redacted]"); lower = value;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        position = equals + 11;
    }
}

std::string sanitize(std::wstring_view details) {
    auto value = utf8(details);
    for (const auto key : {"clipboard_text", "plain_text", "image_content", "path", "url", "window_title", "user_input"}) redact(value, key);
    return escape_json(std::move(value));
}

std::filesystem::path local_log_directory() {
    std::wstring local(32768, L'\0');
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
    if (!count || count >= local.size()) return {};
    local.resize(count);
    return std::filesystem::path(local) / L"超级中键" / L"logs";
}

std::wstring timestamp_name() {
    SYSTEMTIME now{}; GetLocalTime(&now);
    return std::format(L"native-diagnostic-{:04}{:02}{:02}-{:02}{:02}{:02}-{}",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
}

bool open_part() {
    if (logger.file != INVALID_HANDLE_VALUE) CloseHandle(logger.file);
    const auto suffix = logger.part == 0 ? L"" : std::format(L"-part{}", logger.part);
    const auto path = logger.directory / (timestamp_name() + suffix + L".log");
    logger.file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    logger.current_path = path.wstring(); logger.bytes = logger.file == INVALID_HANDLE_VALUE ? 0 : GetFileSize(logger.file, nullptr);
    return logger.file != INVALID_HANDLE_VALUE;
}

void rotate_old_files() {
    std::error_code error; std::filesystem::create_directories(logger.directory, error);
    std::vector<std::filesystem::directory_entry> files;
    for (const auto& item : std::filesystem::directory_iterator(logger.directory, error))
        if (item.is_regular_file() && item.path().filename().wstring().starts_with(L"native-diagnostic-")) files.push_back(item);
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) { return left.last_write_time() > right.last_write_time(); });
    for (std::size_t index = kMaximumFiles - 1; index < files.size(); ++index) std::filesystem::remove(files[index].path(), error);
}

void writer_loop() {
    for (;;) {
        std::string line;
        {
            std::unique_lock lock(logger.mutex);
            logger.changed.wait(lock, [] { return logger.stopping || !logger.lines.empty(); });
            if (logger.lines.empty() && logger.stopping) break;
            line = std::move(logger.lines.front()); logger.lines.pop_front();
        }
        if (logger.bytes + line.size() > kMaximumFileBytes) { ++logger.part; if (!open_part()) continue; }
        DWORD written = 0; if (logger.file != INVALID_HANDLE_VALUE
            && WriteFile(logger.file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) {
            logger.bytes += written;
            // Diagnostic phase markers must survive a helper process being terminated after a hang timeout.
            FlushFileBuffers(logger.file);
        }
    }
    if (logger.file != INVALID_HANDLE_VALUE) { FlushFileBuffers(logger.file); CloseHandle(logger.file); logger.file = INVALID_HANDLE_VALUE; }
}
}

void diagnostic_initialize() noexcept {
    try {
        logger.directory = local_log_directory();
        if (logger.directory.empty()) return;
        rotate_old_files(); if (!open_part()) return;
        logger.stopping = false; logger.writer = std::thread(writer_loop);
        wchar_t module[MAX_PATH]{};
        GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
        const auto process_name = std::filesystem::path(module).filename().wstring();
        diagnostic_event("session.start", std::format(
            L"version={} diagnostics=RelWithDebInfo diagnostic_revision=r2 process={} pid={} system_dpi={} virtual_screen=[{},{},{},{}] displays={}",
            smk::app::kVersionText,
            process_name, GetCurrentProcessId(), GetDpiForSystem(),
            GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN), display_summary()));
    } catch (...) {}
}

void diagnostic_shutdown() noexcept {
    try {
        diagnostic_event("session.stop");
        { std::scoped_lock lock(logger.mutex); logger.stopping = true; }
        logger.changed.notify_all(); if (logger.writer.joinable()) logger.writer.join();
    } catch (...) {}
}

void diagnostic_event(std::string_view event, std::wstring_view details) noexcept {
    try {
        LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter); SYSTEMTIME now{}; GetLocalTime(&now);
        const auto line = std::format("{{\"time\":\"{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}\",\"qpc\":{},\"pid\":{},\"tid\":{},\"event\":\"{}\",\"details\":\"{}\"}}\r\n",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
            counter.QuadPart, GetCurrentProcessId(), GetCurrentThreadId(), escape_json(std::string(event)), sanitize(details));
        { std::scoped_lock lock(logger.mutex); if (logger.lines.size() >= kMaximumQueuedLines) logger.lines.pop_front(); logger.lines.push_back(line); }
        logger.changed.notify_one();
    } catch (...) {}
}

std::wstring diagnostic_log_path() { std::scoped_lock lock(logger.mutex); return logger.current_path; }
#else
void diagnostic_initialize() noexcept {}
void diagnostic_shutdown() noexcept {}
void diagnostic_event(std::string_view, std::wstring_view) noexcept {}
std::wstring diagnostic_log_path() { return {}; }
#endif
} // namespace smk::windows
