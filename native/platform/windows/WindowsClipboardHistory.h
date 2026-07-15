#pragma once

#include "core/ClipboardHistory.h"

#include <windows.h>

#include <functional>
#include <stop_token>
#include <thread>
#include <vector>

namespace smk::windows {

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
