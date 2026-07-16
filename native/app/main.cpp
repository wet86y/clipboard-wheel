#include "app/AppHost.h"
#include "platform/windows/DiagnosticLog.h"
#include "platform/windows/CrashHandler.h"

#include <windows.h>
#include <shellapi.h>
#include <winrt/base.h>

#include <string>
#include <vector>
#include <algorithm>
#include <memory>

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    smk::windows::diagnostic_initialize();
    smk::windows::crash_handler_initialize();
    smk::windows::crash_set_phase(L"ole_initialize");
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) {
        smk::windows::crash_handler_shutdown();
        smk::windows::diagnostic_shutdown();
        return 1;
    }

    bool apartment_initialized = false;
    int result = 1;
    try {
        smk::windows::crash_set_phase(L"winrt_initialize");
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        apartment_initialized = true;

        int count = 0;
        LPWSTR* raw_arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        std::vector<std::wstring> arguments;
        for (int index = 1; index < count; ++index) arguments.emplace_back(raw_arguments[index]);
        if (raw_arguments) LocalFree(raw_arguments);

#if defined(SMK_DIAGNOSTICS)
        if (std::any_of(arguments.begin(), arguments.end(), [](const std::wstring& value) {
            return _wcsicmp(value.c_str(), L"--diagnostic-crash-test") == 0;
        })) {
            smk::windows::crash_set_phase(L"diagnostic_fault_injection");
            RaiseException(0xE0424242, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        }
#endif

        smk::windows::crash_set_phase(L"app_host_run");
        auto host = std::make_unique<smk::app::AppHost>();
        result = host->run(instance, arguments);
        smk::windows::crash_set_phase(L"app_host_stopped");
    } catch (...) {
        smk::windows::crash_set_phase(L"caught_cpp_exception");
        smk::windows::crash_write_emergency(0xE0000003);
        result = 2;
    }
    if (apartment_initialized) winrt::uninit_apartment();
    OleUninitialize();
    smk::windows::crash_set_phase(L"shutdown_complete");
    smk::windows::crash_handler_shutdown();
    smk::windows::diagnostic_shutdown();
    return result;
}
