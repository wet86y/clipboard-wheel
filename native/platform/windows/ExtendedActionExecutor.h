#pragma once

#include "core/Settings.h"
#include "platform/windows/HotkeyCodec.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace smk::windows {

struct ShortcutLaunchInfo {
    std::wstring target_path;
    std::wstring arguments;
    std::wstring working_directory;
};

[[nodiscard]] std::optional<std::wstring> normalize_browser_launch_url(std::wstring_view value);
[[nodiscard]] std::optional<ShortcutLaunchInfo> resolve_shortcut(const std::wstring& shortcut_path);
[[nodiscard]] std::wstring build_browser_launch_arguments(
    const ShortcutLaunchInfo& shortcut,
    std::wstring_view configured_url,
    bool& url_applied);
using ExtendedKeyEventSender = std::function<bool(WORD key, bool key_up)>;
[[nodiscard]] bool dispatch_extended_hotkey(
    const std::vector<WORD>& keys,
    const ExtendedKeyEventSender& sender);
[[nodiscard]] bool send_extended_hotkey(const std::vector<WORD>& keys);
[[nodiscard]] int run_shell_open_helper(const std::wstring& target);
#if defined(SMK_DIAGNOSTICS)
[[nodiscard]] int run_shell_hang_helper_test();
[[nodiscard]] int run_shell_timeout_self_test();
#endif

class ExtendedActionExecutor final {
public:
    ExtendedActionExecutor();
    ~ExtendedActionExecutor();
    ExtendedActionExecutor(const ExtendedActionExecutor&) = delete;
    ExtendedActionExecutor& operator=(const ExtendedActionExecutor&) = delete;

    bool start();
    void enqueue(smk::core::ExtendedWheelActionSlot action);
    bool shutdown(DWORD timeout_ms = 5000) noexcept;

private:
    struct Implementation;
    void worker_main();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<smk::core::ExtendedWheelActionSlot> queue_;
    std::thread worker_;
    HANDLE stopped_event_ = nullptr;
    std::unique_ptr<Implementation> implementation_;
    std::atomic_bool stop_requested_ = false;
    bool stopping_ = false;
};

} // namespace smk::windows
