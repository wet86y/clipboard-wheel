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

class ExtendedActionExecutor final {
public:
    ExtendedActionExecutor();
    ~ExtendedActionExecutor();
    ExtendedActionExecutor(const ExtendedActionExecutor&) = delete;
    ExtendedActionExecutor& operator=(const ExtendedActionExecutor&) = delete;

    bool start();
    void enqueue(smk::core::ExtendedWheelActionSlot action);
    void shutdown();

private:
    struct Implementation;
    void worker_main();

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<smk::core::ExtendedWheelActionSlot> queue_;
    std::thread worker_;
    std::unique_ptr<Implementation> implementation_;
    std::atomic_bool stop_requested_ = false;
    bool stopping_ = false;
};

} // namespace smk::windows
