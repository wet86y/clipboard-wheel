#pragma once

#include <windows.h>

#include <string>

namespace smk::windows {

class SingleInstance final {
public:
    SingleInstance() = default;
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    bool acquire(const wchar_t* name = L"Local\\SuperMiddleKey.SingleInstance", bool wait_for_takeover = false);
    [[nodiscard]] bool owns_mutex() const noexcept { return handle_ != nullptr; }

private:
    HANDLE handle_ = nullptr;
};

} // namespace smk::windows
