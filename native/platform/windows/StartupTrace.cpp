#include "platform/windows/StartupTrace.h"
#include "platform/windows/DiagnosticLog.h"

namespace smk::windows {

void startup_trace(std::wstring_view message) noexcept {
    diagnostic_event("startup", message);
}

} // namespace smk::windows
