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
    static constexpr ULONGLONG kQuietPeriodMs = 40;
    static constexpr ULONGLONG kImageQuietPeriodMs = 120;

    void note_update(ULONGLONG now, ULONGLONG quiet_period_ms = kQuietPeriodMs) noexcept {
        pending_ = true;
        quiet_period_ms_ = quiet_period_ms;
        due_at_ = now + quiet_period_ms_;
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
    [[nodiscard]] ULONGLONG quiet_period_ms() const noexcept { return quiet_period_ms_; }

private:
    bool pending_ = false;
    ULONGLONG due_at_ = 0;
    ULONGLONG quiet_period_ms_ = kQuietPeriodMs;
};

class ClipboardCaptureRetryState final {
public:
    static constexpr unsigned kMaximumAttempts = 6;

    [[nodiscard]] std::uint64_t begin() noexcept {
        attempt_ = 1;
        return ++generation_;
    }
    [[nodiscard]] bool advance(std::uint64_t generation) noexcept {
        if (generation != generation_ || attempt_ >= kMaximumAttempts) return false;
        ++attempt_;
        return true;
    }
    void cancel() noexcept { ++generation_; attempt_ = 0; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] unsigned attempt() const noexcept { return attempt_; }

private:
    std::uint64_t generation_ = 0;
    unsigned attempt_ = 0;
};

[[nodiscard]] inline bool should_capture_as_image(const smk::core::ClipboardEntry& entry) noexcept {
    return entry.has_image() && !entry.looks_like_spreadsheet;
}

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
    [[nodiscard]] static std::optional<smk::core::ClipboardEntry> create_entry_from_data_object(
        IDataObject* data, bool capture_images);

private:
    enum class CaptureAttempt { captured, unavailable, busy };

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
    static std::wstring read_text_format(IDataObject* data, CLIPFORMAT format, bool unicode,
        UINT code_page = CP_UTF8);
    static std::wstring make_id();
    CaptureAttempt capture_current_once();
    void schedule_capture_retry();
    bool attempt_pending_paste();
    bool clipboard_already_has_plain_text(const smk::core::ClipboardEntry& entry) const;

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
    ClipboardCaptureRetryState capture_retry_state_;
    std::uint64_t scheduled_capture_generation_ = 0;
    Microsoft::WRL::ComPtr<IDataObject> owned_clipboard_;
};

} // namespace smk::windows
