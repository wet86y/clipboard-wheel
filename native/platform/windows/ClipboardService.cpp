#include "platform/windows/ClipboardService.h"

#include "core/PastePolicy.h"
#include "platform/windows/ImageClipboardPayload.h"
#include "platform/windows/DiagnosticLog.h"

#include <objidl.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <memory>
#include <format>
#include <vector>

namespace smk::windows {
namespace {

constexpr wchar_t kWindowClass[] = L"SuperMiddleKeyNativeClipboardListener";
constexpr UINT kAttemptPasteMessage = WM_APP + 41;
constexpr UINT_PTR kRetryTimer = 1;
constexpr UINT_PTR kSendPasteTimer = 2;
constexpr UINT_PTR kCaptureTimer = 3;
constexpr UINT_PTR kCaptureRetryTimer = 4;
constexpr UINT kRetryIntervalMs = 20;
constexpr UINT kCaptureRetryIntervalMs = 50;
constexpr DWORD kClipboardFlushRetryIntervalMs = 50;
constexpr ULONGLONG kRetryTimeoutMs = 1500;

bool contains_ignore_case(std::wstring_view value, std::wstring_view token) noexcept {
    if (token.empty()) return true;
    for (std::size_t index = 0; index + token.size() <= value.size(); ++index) {
        bool match = true;
        for (std::size_t offset = 0; offset < token.size(); ++offset) {
            if (std::towlower(value[index + offset]) != std::towlower(token[offset])) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

bool clipboard_has_image_format() {
    if (IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_DIB)
        || IsClipboardFormatAvailable(CF_BITMAP)) return true;
    constexpr std::array<const wchar_t*, 12> encoded_names{L"PNG", L"image/png", L"JFIF", L"JPEG", L"JPG",
        L"image/jpeg", L"image/jpg", L"GIF", L"image/gif", L"BMP", L"image/bmp", L"Bitmap"};
    return std::any_of(encoded_names.begin(), encoded_names.end(), [](const wchar_t* name) {
        return IsClipboardFormatAvailable(RegisterClipboardFormatW(name)) != FALSE;
    });
}

std::wstring decode_text_bytes(const void* memory, std::size_t byte_count, bool unicode, UINT code_page) {
    if (!memory || !byte_count) return {};
    if (unicode) {
        const auto count = byte_count / sizeof(wchar_t);
        const auto* text = static_cast<const wchar_t*>(memory);
        return std::wstring(text, std::find(text, text + count, L'\0'));
    }
    const auto* bytes = static_cast<const char*>(memory);
    const auto* end = std::find(bytes, bytes + byte_count, '\0');
    const auto length = static_cast<int>(end - bytes);
    if (!length) return {};
    int chars = MultiByteToWideChar(code_page, code_page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
        bytes, length, nullptr, 0);
    UINT effective_page = code_page;
    if (chars <= 0 && code_page == CP_UTF8) {
        effective_page = CP_ACP;
        chars = MultiByteToWideChar(effective_page, 0, bytes, length, nullptr, 0);
    }
    std::wstring result(static_cast<std::size_t>(std::max(0, chars)), L'\0');
    if (chars > 0) MultiByteToWideChar(effective_page, 0, bytes, length, result.data(), chars);
    return result;
}

std::wstring read_hglobal_text(HGLOBAL handle, bool unicode, UINT code_page) {
    if (!handle) return {};
    const void* memory = GlobalLock(handle);
    if (!memory) return {};
    const auto result = decode_text_bytes(memory, GlobalSize(handle), unicode, code_page);
    GlobalUnlock(handle);
    return result;
}

std::wstring read_stream_text(IStream* stream, bool unicode, UINT code_page) {
    if (!stream) return {};
    constexpr std::size_t kMaximumBytes = 32U * 1024U * 1024U;
    LARGE_INTEGER zero{};
    (void)stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    std::vector<char> bytes;
    std::array<char, 4096> buffer{};
    for (;;) {
        ULONG read = 0;
        const HRESULT result = stream->Read(buffer.data(), static_cast<ULONG>(buffer.size()), &read);
        if (FAILED(result) || bytes.size() + read > kMaximumBytes) return {};
        bytes.insert(bytes.end(), buffer.data(), buffer.data() + read);
        if (result == S_FALSE || read == 0) break;
    }
    return decode_text_bytes(bytes.data(), bytes.size(), unicode, code_page);
}

bool send_ctrl_v() {
    INPUT inputs[4]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'V';
    inputs[2] = inputs[1];
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3] = inputs[0];
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    const UINT sent = SendInput(4, inputs, sizeof(INPUT));
    if (sent == 4) return true;
    INPUT releases[2]{};
    releases[0].type = INPUT_KEYBOARD;
    releases[0].ki.wVk = 'V';
    releases[0].ki.dwFlags = KEYEVENTF_KEYUP;
    releases[1].type = INPUT_KEYBOARD;
    releases[1].ki.wVk = VK_CONTROL;
    releases[1].ki.dwFlags = KEYEVENTF_KEYUP;
    (void)SendInput(2, releases, sizeof(INPUT));
    return false;
}

} // namespace

ClipboardService::ClipboardService(smk::core::ClipboardHistory& history, ChangedCallback changed, bool capture_images)
    : history_(history), changed_(std::move(changed)), capture_images_(capture_images) {}

ClipboardService::~ClipboardService() { stop(); }

bool ClipboardService::start(HINSTANCE instance) {
    instance_ = instance;
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = kWindowClass;
    RegisterClassExW(&window_class);
    window_ = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
    if (!window_) return false;
    return AddClipboardFormatListener(window_) != FALSE;
}

void ClipboardService::stop() {
    if (window_) {
        KillTimer(window_, kRetryTimer);
        KillTimer(window_, kSendPasteTimer);
        KillTimer(window_, kCaptureTimer);
        KillTimer(window_, kCaptureRetryTimer);
        capture_retry_state_.cancel();
        capture_coalescer_.cancel();
        RemoveClipboardFormatListener(window_);
        if (pending_entry_ || clipboard_replaced_) fail_pending_paste(L"shutdown");
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (owned_clipboard_) {
        HRESULT flush_result = S_FALSE;
        for (unsigned attempt = 1; attempt <= 6; ++attempt) {
            if (OleIsCurrentClipboard(owned_clipboard_.Get()) != S_OK) break;
            flush_result = OleFlushClipboard();
            if (SUCCEEDED(flush_result) || !should_retry_clipboard_flush(flush_result, attempt)) break;
            Sleep(kClipboardFlushRetryIntervalMs);
        }
        if (FAILED(flush_result)) {
            SMK_DIAGNOSTIC_EVENT("clipboard.flush_failed",
                std::format(L"hr=0x{:08X}", static_cast<unsigned>(flush_result)));
        }
    }
    owned_clipboard_.Reset();
}

LRESULT CALLBACK ClipboardService::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<ClipboardService*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<ClipboardService*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle_message(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT ClipboardService::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLIPBOARDUPDATE) {
        KillTimer(window_, kCaptureTimer);
        KillTimer(window_, kCaptureRetryTimer);
        scheduled_capture_generation_ = capture_retry_state_.begin();
        const ULONGLONG now = GetTickCount64();
        if (now >= suppress_capture_until_) {
            const ULONGLONG quiet_period = capture_images_ && clipboard_has_image_format()
                ? ClipboardUpdateCoalescer::kImageQuietPeriodMs
                : ClipboardUpdateCoalescer::kQuietPeriodMs;
            capture_coalescer_.note_update(now, quiet_period);
            if (!SetTimer(window_, kCaptureTimer,
                    static_cast<UINT>(quiet_period), nullptr)) {
                capture_coalescer_.cancel();
                if (capture_current_once() == CaptureAttempt::busy) schedule_capture_retry();
            }
        } else {
            capture_coalescer_.cancel();
        }
        return 0;
    }
    if (message == kAttemptPasteMessage) { (void)attempt_pending_paste(); return 0; }
    if (message == WM_TIMER && wparam == kRetryTimer) { (void)attempt_pending_paste(); return 0; }
    if (message == WM_TIMER && wparam == kSendPasteTimer) {
        KillTimer(window_, kSendPasteTimer);
        if (!paste_target_ || !IsWindow(paste_target_) || GetForegroundWindow() != paste_target_) {
            fail_pending_paste(L"target_changed", ERROR_INVALID_WINDOW_HANDLE);
        } else if (!send_ctrl_v()) {
            fail_pending_paste(L"send_input", GetLastError());
        } else {
            original_clipboard_.Reset();
            clipboard_replaced_ = false;
            paste_target_ = nullptr;
        }
        return 0;
    }
    if (message == WM_TIMER && wparam == kCaptureTimer) {
        KillTimer(window_, kCaptureTimer);
        const ULONGLONG now = GetTickCount64();
        if (capture_coalescer_.consume_if_ready(now) && now >= suppress_capture_until_) {
            SMK_DIAGNOSTIC_EVENT("clipboard.capture_coalesced",
                std::format(L"quiet_ms={}", capture_coalescer_.quiet_period_ms()));
            if (capture_current_once() == CaptureAttempt::busy) schedule_capture_retry();
        } else if (capture_coalescer_.pending()) {
            SetTimer(window_, kCaptureTimer, capture_coalescer_.remaining_ms(now), nullptr);
        }
        return 0;
    }
    if (message == WM_TIMER && wparam == kCaptureRetryTimer) {
        KillTimer(window_, kCaptureRetryTimer);
        if (!capture_retry_state_.advance(scheduled_capture_generation_)
            || GetTickCount64() < suppress_capture_until_) return 0;
        if (capture_current_once() == CaptureAttempt::busy) schedule_capture_retry();
        return 0;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

std::wstring ClipboardService::read_text_format(IDataObject* data, CLIPFORMAT format, bool unicode, UINT code_page) {
    FORMATETC request{format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL | TYMED_ISTREAM};
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&request, &medium))) return {};
    const auto result = medium.tymed == TYMED_ISTREAM
        ? read_stream_text(medium.pstm, unicode, code_page)
        : read_hglobal_text(medium.hGlobal, unicode, code_page);
    ReleaseStgMedium(&medium);
    return result;
}

std::wstring ClipboardService::make_id() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return std::to_wstring(GetTickCount64());
    wchar_t text[40]{};
    if (StringFromGUID2(guid, text, static_cast<int>(std::size(text))) == 0)
        return std::to_wstring(GetTickCount64());
    std::wstring result(text);
    std::erase_if(result, [](wchar_t value) { return value == L'{' || value == L'}' || value == L'-'; });
    return result;
}

bool ClipboardService::capture_current() {
    scheduled_capture_generation_ = capture_retry_state_.begin();
    const auto result = capture_current_once();
    if (result == CaptureAttempt::busy) schedule_capture_retry();
    return result == CaptureAttempt::captured;
}

ClipboardService::CaptureAttempt ClipboardService::capture_current_once() {
    IDataObject* raw = nullptr;
    const HRESULT clipboard_result = OleGetClipboard(&raw);
    if (clipboard_result == CLIPBRD_E_CANT_OPEN) return CaptureAttempt::busy;
    if (FAILED(clipboard_result) || !raw) return CaptureAttempt::unavailable;
    std::unique_ptr<IDataObject, void (*)(IDataObject*)> data(raw, [](IDataObject* value) { value->Release(); });

    auto entry = create_entry_from_data_object(data.get(), capture_images_);
    if (!entry) return CaptureAttempt::unavailable;
    entry->id = make_id();
    history_.add_or_promote(std::move(*entry));
    if (changed_) changed_();
    return CaptureAttempt::captured;
}

std::optional<smk::core::ClipboardEntry> ClipboardService::create_entry_from_data_object(
    IDataObject* data, bool capture_images) {
    if (!data) return std::nullopt;
    smk::core::ClipboardEntry entry;
    entry.plain_text = read_text_format(data, CF_UNICODETEXT, true);
    if (entry.plain_text.empty()) entry.plain_text = read_text_format(data, CF_TEXT, false, CP_ACP);
    entry.html_text = read_text_format(data, static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"HTML Format")), false);
    entry.rtf_text = read_text_format(data, static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Rich Text Format")), false);
    entry.csv_text = read_text_format(data, static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Csv")), false);
    entry.tsv_text = entry.plain_text.find(L'\t') != std::wstring::npos ? entry.plain_text : L"";
    entry.looks_like_spreadsheet = !entry.csv_text.empty() || !entry.tsv_text.empty()
        || contains_ignore_case(entry.html_text, L"<table");
    entry.looks_like_single_cell = !entry.plain_text.empty() && entry.plain_text.find_first_of(L"\t\r\n") == std::wstring::npos;
    if (capture_images && !entry.looks_like_spreadsheet) {
        if (auto image = read_image_payload(data)) {
            entry.image_png_bytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(image->png));
            entry.preview_image_png_bytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(image->preview_png));
            entry.image_hash = image->sha256;
            entry.image_width = image->width;
            entry.image_height = image->height;
            SMK_DIAGNOSTIC_EVENT("clipboard.image_capture", std::format(L"width={} height={} png_bytes={} preview_bytes={}",
                image->width, image->height, entry.image_png_bytes->size(),
                entry.preview_image_png_bytes->size()));
        }
    }
    entry.is_image_content = should_capture_as_image(entry);
    entry.display_text = entry.is_image_content ? L"[图片]" : entry.plain_text.substr(0, std::min<std::size_t>(entry.plain_text.size(), 80));
    if (entry.plain_text.empty() && entry.html_text.empty() && entry.rtf_text.empty()
        && entry.csv_text.empty() && !entry.has_image()) return std::nullopt;
    return entry;
}

void ClipboardService::schedule_capture_retry() {
    if (!window_ || capture_retry_state_.attempt() >= ClipboardCaptureRetryState::kMaximumAttempts
        || scheduled_capture_generation_ != capture_retry_state_.generation()) return;
    SetTimer(window_, kCaptureRetryTimer, kCaptureRetryIntervalMs, nullptr);
}

bool ClipboardService::paste_entry(const smk::core::ClipboardEntry& entry, smk::core::PasteMode requested) {
    if (!window_) return false;
    if (pending_entry_ || clipboard_replaced_) fail_pending_paste(L"superseded");
    const HWND target = GetForegroundWindow();
    if (!target || !IsWindow(target)) {
        SMK_DIAGNOSTIC_EVENT("clipboard.paste_failed", L"stage=target_invalid");
        return false;
    }
    pending_entry_ = entry;
    pending_mode_ = smk::core::resolve_paste_mode(entry, requested);
    pending_plain_text_ = smk::core::paste_plain_text(entry, clean_spreadsheet_plain_text_);
    pending_started_ = GetTickCount64();
    suppress_capture_until_ = pending_started_ + kRetryTimeoutMs + 250;
    capture_coalescer_.cancel();
    capture_retry_state_.cancel();
    paste_target_ = target;
    clipboard_replaced_ = false;
    original_clipboard_.Reset();
    KillTimer(window_, kCaptureTimer);
    KillTimer(window_, kCaptureRetryTimer);
    if (!PostMessageW(window_, kAttemptPasteMessage, 0, 0)) {
        fail_pending_paste(L"post_attempt", GetLastError());
        return false;
    }
    return true;
}

bool ClipboardService::clipboard_already_has_plain_text(const smk::core::ClipboardEntry& entry) const {
    IDataObject* raw = nullptr;
    if (FAILED(OleGetClipboard(&raw)) || !raw) return false;
    std::unique_ptr<IDataObject, void (*)(IDataObject*)> data(raw, [](IDataObject* value) { value->Release(); });
    const auto current = read_text_format(data.get(), CF_UNICODETEXT, true);
    return smk::core::can_skip_clipboard_write(
        entry, pending_mode_, current, std::wstring_view(pending_plain_text_));
}

bool ClipboardService::attempt_pending_paste() {
    if (!pending_entry_) { KillTimer(window_, kRetryTimer); return false; }
    if (!paste_target_ || !IsWindow(paste_target_) || GetForegroundWindow() != paste_target_) {
        fail_pending_paste(L"target_changed", ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }
    const bool requires_formatted = smk::core::requires_formatted_clipboard_payload(
        *pending_entry_, pending_mode_);
    bool written = !requires_formatted && clipboard_already_has_plain_text(*pending_entry_);
    if (!written) {
        if (!original_clipboard_) {
            IDataObject* original = nullptr;
            const HRESULT original_result = OleGetClipboard(&original);
            if (FAILED(original_result) || !original) {
                if (GetTickCount64() - pending_started_ >= kRetryTimeoutMs) {
                    fail_pending_paste(L"capture_original", static_cast<DWORD>(original_result));
                    return false;
                }
                if (!SetTimer(window_, kRetryTimer, kRetryIntervalMs, nullptr))
                    fail_pending_paste(L"retry_timer", GetLastError());
                return false;
            }
            original_clipboard_.Attach(original);
        }
        auto data = create_clipboard_data_object(*pending_entry_, pending_mode_, pending_plain_text_);
        const HRESULT result = data ? OleSetClipboard(data.Get()) : E_OUTOFMEMORY;
        if (data && SUCCEEDED(result)) {
            owned_clipboard_ = std::move(data);
            clipboard_replaced_ = true;
            written = true;
        }
        else SMK_DIAGNOSTIC_EVENT("clipboard.paste_retry", std::format(L"elapsed={} hr=0x{:08X}",
            GetTickCount64() - pending_started_, static_cast<unsigned>(result)));
    }
    if (written) {
        suppress_capture_until_ = GetTickCount64() + 300;
        pending_entry_.reset(); KillTimer(window_, kRetryTimer);
        if (!SetTimer(window_, kSendPasteTimer, kRetryIntervalMs, nullptr)) {
            fail_pending_paste(L"send_timer", GetLastError());
            return false;
        }
        return true;
    }
    if (GetTickCount64() - pending_started_ >= kRetryTimeoutMs) {
        fail_pending_paste(L"clipboard_timeout");
        return false;
    }
    if (!SetTimer(window_, kRetryTimer, kRetryIntervalMs, nullptr))
        fail_pending_paste(L"retry_timer", GetLastError());
    return false;
}

void ClipboardService::restore_original_clipboard() noexcept {
    if (clipboard_replaced_ && original_clipboard_) {
        [[maybe_unused]] const HRESULT result = OleSetClipboard(original_clipboard_.Get());
        SMK_DIAGNOSTIC_EVENT("clipboard.paste_restore", std::format(
            L"result=0x{:08X}", static_cast<unsigned>(result)));
    }
    clipboard_replaced_ = false;
    original_clipboard_.Reset();
}

void ClipboardService::fail_pending_paste(std::wstring_view stage, DWORD error) noexcept {
    if (window_) {
        KillTimer(window_, kRetryTimer);
        KillTimer(window_, kSendPasteTimer);
    }
    restore_original_clipboard();
    pending_entry_.reset();
    pending_plain_text_.clear();
    paste_target_ = nullptr;
    (void)stage;
    (void)error;
    SMK_DIAGNOSTIC_EVENT("clipboard.paste_failed", std::format(
        L"stage={} error={}", stage, error));
}

} // namespace smk::windows
