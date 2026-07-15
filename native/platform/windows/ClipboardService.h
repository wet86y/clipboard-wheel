#pragma once

#include "core/ClipboardHistory.h"
#include "core/PastePolicy.h"

#include <windows.h>
#include <objidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <functional>
#include <optional>

namespace smk::windows {

class ClipboardUpdateCoalescer final {
public:
    static constexpr ULONGLONG kQuietPeriodMs = 120;

    void note_update(ULONGLONG now) noexcept {
        pending_ = true;
        due_at_ = now + kQuietPeriodMs;
    }
    [[nodiscard]] bool consume_if_ready(ULONGLONG now) noexcept {
        if (!pending_ || now < due_at_) return false;
        pending_ = false;
        return true;
    }
    [[nodiscard]] UINT remaining_ms(ULONGLONG now) const noexcept {
        if (!pending_ || now >= due_at_) return 1;
        return static_cast<UINT>(std::min<ULONGLONG>(due_at_ - now, USER_TIMER_MAXIMUM));
    }
    void cancel() noexcept { pending_ = false; due_at_ = 0; }
    [[nodiscard]] bool pending() const noexcept { return pending_; }

private:
    bool pending_ = false;
    ULONGLONG due_at_ = 0;
};

class ClipboardService final {
public:
    using ChangedCallback = std::function<void()>;

    ClipboardService(smk::core::ClipboardHistory& history, ChangedCallback changed, bool capture_images);
    ~ClipboardService();
    ClipboardService(const ClipboardService&) = delete;
    ClipboardService& operator=(const ClipboardService&) = delete;

    bool start(HINSTANCE instance);
    void stop();
    void set_capture_images(bool enabled) noexcept { capture_images_ = enabled; }
    bool capture_current();
    bool paste_entry(const smk::core::ClipboardEntry& entry, smk::core::PasteMode mode);

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
    static std::wstring read_unicode_format(IDataObject* data, CLIPFORMAT format);
    static std::wstring make_id();
    bool attempt_pending_paste();
    bool write_text_entry(const smk::core::ClipboardEntry& entry, smk::core::PasteMode mode);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    smk::core::ClipboardHistory& history_;
    ChangedCallback changed_;
    bool capture_images_ = false;
    std::optional<smk::core::ClipboardEntry> pending_entry_;
    smk::core::PasteMode pending_mode_ = smk::core::PasteMode::smart;
    ULONGLONG pending_started_ = 0;
    ULONGLONG suppress_capture_until_ = 0;
    ClipboardUpdateCoalescer capture_coalescer_;
    Microsoft::WRL::ComPtr<IDataObject> owned_clipboard_;
};

} // namespace smk::windows
