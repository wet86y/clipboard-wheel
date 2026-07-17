#include "platform/windows/SingleInstance.h"
#include "platform/windows/DiagnosticLog.h"

#include <format>

namespace smk::windows {

SingleInstance::~SingleInstance() { release(); }

void SingleInstance::release() noexcept {
    if (!handle_) return;
    const BOOL released = ReleaseMutex(handle_);
    [[maybe_unused]] const DWORD release_error = released ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle_);
    handle_ = nullptr;
    SMK_DIAGNOSTIC_EVENT("single_instance.release", std::format(
        L"released={} error={}", released != FALSE, release_error));
}

bool SingleInstance::acquire(const wchar_t* name, bool wait_for_takeover) {
    if (handle_) return true;
    handle_ = CreateMutexW(nullptr, TRUE, name);
    const DWORD create_error = GetLastError();
    if (!handle_) {
        SMK_DIAGNOSTIC_EVENT("single_instance.acquire", std::format(
            L"created=false error={} takeover={}", create_error, wait_for_takeover));
        return false;
    }
    if (create_error != ERROR_ALREADY_EXISTS) {
        SMK_DIAGNOSTIC_EVENT("single_instance.acquire", L"created=true existing=false");
        return true;
    }

    if (!wait_for_takeover) {
        SMK_DIAGNOSTIC_EVENT("single_instance.acquire", L"created=true existing=true takeover=false result=rejected");
        CloseHandle(handle_);
        handle_ = nullptr;
        return false;
    }

    const DWORD result = WaitForSingleObject(handle_, 15'000);
    SMK_DIAGNOSTIC_EVENT("single_instance.takeover", std::format(
        L"wait_result={} last_error={}", result, GetLastError()));
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) return true;
    CloseHandle(handle_);
    handle_ = nullptr;
    return false;
}

} // namespace smk::windows
