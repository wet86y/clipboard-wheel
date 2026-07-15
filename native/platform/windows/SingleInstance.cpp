#include "platform/windows/SingleInstance.h"

namespace smk::windows {

SingleInstance::~SingleInstance() {
    if (handle_) {
        ReleaseMutex(handle_);
        CloseHandle(handle_);
    }
}

bool SingleInstance::acquire(const wchar_t* name, bool wait_for_takeover) {
    handle_ = CreateMutexW(nullptr, TRUE, name);
    if (!handle_) return false;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return true;

    if (!wait_for_takeover) {
        CloseHandle(handle_);
        handle_ = nullptr;
        return false;
    }

    const DWORD result = WaitForSingleObject(handle_, 15'000);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) return true;
    CloseHandle(handle_);
    handle_ = nullptr;
    return false;
}

} // namespace smk::windows
