#include "platform/windows/DiagnosticLog.h"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main() {
#if defined(SMK_DIAGNOSTICS)
    wchar_t original_local_app_data[32768]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", original_local_app_data, static_cast<DWORD>(std::size(original_local_app_data)));
    const auto temporary_root = std::filesystem::temp_directory_path() /
        (L"SuperMiddleKeyDiagnosticTests-" + std::to_wstring(GetCurrentProcessId()));
    const auto log_directory = temporary_root / L"超级中键" / L"logs";
    std::filesystem::create_directories(log_directory);
    for (int index = 0; index < 7; ++index)
        std::ofstream(log_directory / (L"native-diagnostic-old-" + std::to_wstring(index) + L".log")) << "old\n";
    SetEnvironmentVariableW(L"LOCALAPPDATA", temporary_root.c_str());
    smk::windows::diagnostic_initialize();
    std::vector<std::thread> writers;
    for (int thread_index = 0; thread_index < 6; ++thread_index) {
        writers.emplace_back([thread_index] {
            for (int index = 0; index < 150; ++index)
                smk::windows::diagnostic_event("concurrency.test", L"thread=" + std::to_wstring(thread_index) + L" index=" + std::to_wstring(index));
        });
    }
    for (auto& writer : writers) writer.join();
    smk::windows::diagnostic_event("privacy.test", L"clipboard_text=SECRET path=C:\\private url=https://secret.invalid safe=ok");
    const auto path = smk::windows::diagnostic_log_path();
    smk::windows::diagnostic_shutdown();
    std::ifstream input(path, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (contents.find("privacy.test") == std::string::npos
        || contents.find("[redacted]") == std::string::npos
        || contents.find("SECRET") != std::string::npos
        || contents.find("secret.invalid") != std::string::npos
        || contents.find("concurrency.test") == std::string::npos) {
        std::cerr << "Diagnostic privacy filtering failed.\n";
        return EXIT_FAILURE;
    }
    std::size_t log_count = 0;
    for (const auto& item : std::filesystem::directory_iterator(log_directory))
        if (item.path().filename().wstring().starts_with(L"native-diagnostic-")) ++log_count;
    if (log_count > 5) {
        std::cerr << "Diagnostic log retention exceeded five files.\n";
        return EXIT_FAILURE;
    }
    SetEnvironmentVariableW(L"LOCALAPPDATA", original_local_app_data);
    std::error_code cleanup_error;
    std::filesystem::remove_all(temporary_root, cleanup_error);
#endif
    std::cout << "Native diagnostic logging tests passed.\n";
    return EXIT_SUCCESS;
}
