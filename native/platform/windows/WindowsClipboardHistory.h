#pragma once

#include "core/ClipboardHistory.h"
#include "platform/windows/ImageClipboardPayload.h"

#include <windows.h>

#include <functional>
#include <stop_token>
#include <thread>
#include <vector>

namespace smk::windows {

inline constexpr std::uint64_t kMaximumHistoryImageInputBytes =
    ImageClipboardPayload::kMaxPixels * 4ULL + 1024ULL * 1024ULL;

[[nodiscard]] inline bool history_image_stream_size_allowed(std::uint64_t size) noexcept {
    return size > 0 && size <= kMaximumHistoryImageInputBytes;
}

[[nodiscard]] inline bool should_import_history_image(
    const smk::core::ClipboardEntry& entry, bool capture_images, bool contains_bitmap) noexcept {
    return capture_images && contains_bitmap && !entry.looks_like_spreadsheet;
}

class WindowsClipboardHistory final {
public:
    using ImportedCallback = std::function<void()>;

    WindowsClipboardHistory(smk::core::ClipboardHistory& history, ImportedCallback imported);
    ~WindowsClipboardHistory();
    WindowsClipboardHistory(const WindowsClipboardHistory&) = delete;
    WindowsClipboardHistory& operator=(const WindowsClipboardHistory&) = delete;

    bool start(HINSTANCE instance, std::size_t limit, bool capture_images);
    void stop();

private:
    static constexpr UINT kImportMessage = WM_APP + 73;
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
    void load_on_worker(std::stop_token stop_token, std::size_t limit, bool capture_images);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    smk::core::ClipboardHistory& history_;
    ImportedCallback imported_;
    std::jthread worker_;
};

} // namespace smk::windows
