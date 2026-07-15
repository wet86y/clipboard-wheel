#include "platform/windows/ClipboardService.h"

#include "core/PastePolicy.h"
#include "platform/windows/ImageClipboardPayload.h"
#include "platform/windows/DiagnosticLog.h"

#include <objidl.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <format>

namespace smk::windows {
namespace {

constexpr wchar_t kWindowClass[] = L"SuperMiddleKeyNativeClipboardListener";
constexpr UINT kAttemptPasteMessage = WM_APP + 41;
constexpr UINT_PTR kRetryTimer = 1;
constexpr UINT_PTR kSendPasteTimer = 2;
constexpr UINT_PTR kCaptureTimer = 3;
constexpr UINT kRetryIntervalMs = 20;
constexpr ULONGLONG kRetryTimeoutMs = 1500;

std::wstring read_hglobal_text(HGLOBAL handle, bool unicode) {
    if (!handle) return {};
    const void* memory = GlobalLock(handle);
    if (!memory) return {};
    std::wstring result;
    if (unicode) {
        result = static_cast<const wchar_t*>(memory);
    } else {
        const auto* bytes = static_cast<const char*>(memory);
        const auto length = std::strlen(bytes);
        const int chars = MultiByteToWideChar(CP_UTF8, 0, bytes, static_cast<int>(length), nullptr, 0);
        if (chars > 0) {
            result.resize(static_cast<std::size_t>(chars));
            MultiByteToWideChar(CP_UTF8, 0, bytes, static_cast<int>(length), result.data(), chars);
        } else {
            const int ansi_chars = MultiByteToWideChar(CP_ACP, 0, bytes, static_cast<int>(length), nullptr, 0);
            result.resize(static_cast<std::size_t>(std::max(0, ansi_chars)));
            if (ansi_chars > 0) MultiByteToWideChar(CP_ACP, 0, bytes, static_cast<int>(length), result.data(), ansi_chars);
        }
    }
    GlobalUnlock(handle);
    return result;
}

HGLOBAL make_global_text(const std::wstring& text, bool unicode) {
    if (unicode) {
        const auto bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!handle) return nullptr;
        void* memory = GlobalLock(handle);
        std::memcpy(memory, text.c_str(), bytes);
        GlobalUnlock(handle);
        return handle;
    }

    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(count) + 1);
    if (!handle) return nullptr;
    auto* memory = static_cast<char*>(GlobalLock(handle));
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), memory, count, nullptr, nullptr);
    memory[count] = '\0';
    GlobalUnlock(handle);
    return handle;
}

bool set_clipboard_text(CLIPFORMAT format, const std::wstring& text, bool unicode) {
    if (text.empty()) return true;
    HGLOBAL handle = make_global_text(text, unicode);
    if (!handle) return false;
    if (!SetClipboardData(format, handle)) {
        GlobalFree(handle);
        return false;
    }
    return true;
}

void send_ctrl_v() {
    INPUT inputs[4]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'V';
    inputs[2] = inputs[1];
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3] = inputs[0];
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, inputs, sizeof(INPUT));
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
        KillTimer(window_, kCaptureTimer);
        capture_coalescer_.cancel();
        RemoveClipboardFormatListener(window_);
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (owned_clipboard_) OleFlushClipboard();
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
        const ULONGLONG now = GetTickCount64();
        if (now >= suppress_capture_until_) {
            // Snipping tools commonly publish a provisional bitmap and then
            // replace it 50-85 ms later. Restart the quiet window so only the
            // final multi-format data object reaches history.
            capture_coalescer_.note_update(now);
            if (!SetTimer(window_, kCaptureTimer,
                    static_cast<UINT>(ClipboardUpdateCoalescer::kQuietPeriodMs), nullptr)) {
                capture_coalescer_.cancel();
                capture_current();
            }
        } else {
            capture_coalescer_.cancel();
        }
        return 0;
    }
    if (message == kAttemptPasteMessage) { (void)attempt_pending_paste(); return 0; }
    if (message == WM_TIMER && wparam == kRetryTimer) { (void)attempt_pending_paste(); return 0; }
    if (message == WM_TIMER && wparam == kSendPasteTimer) {
        KillTimer(window_, kSendPasteTimer); send_ctrl_v(); return 0;
    }
    if (message == WM_TIMER && wparam == kCaptureTimer) {
        KillTimer(window_, kCaptureTimer);
        const ULONGLONG now = GetTickCount64();
        if (capture_coalescer_.consume_if_ready(now) && now >= suppress_capture_until_) {
            SMK_DIAGNOSTIC_EVENT("clipboard.capture_coalesced", L"quiet_ms=120");
            capture_current();
        } else if (capture_coalescer_.pending()) {
            SetTimer(window_, kCaptureTimer, capture_coalescer_.remaining_ms(now), nullptr);
        }
        return 0;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

std::wstring ClipboardService::read_unicode_format(IDataObject* data, CLIPFORMAT format) {
    FORMATETC request{format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (FAILED(data->GetData(&request, &medium))) return {};
    const auto result = read_hglobal_text(medium.hGlobal, format == CF_UNICODETEXT);
    ReleaseStgMedium(&medium);
    return result;
}

std::wstring ClipboardService::make_id() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return std::to_wstring(GetTickCount64());
    wchar_t text[40]{};
    StringFromGUID2(guid, text, static_cast<int>(std::size(text)));
    std::wstring result(text);
    std::erase_if(result, [](wchar_t value) { return value == L'{' || value == L'}' || value == L'-'; });
    return result;
}

bool ClipboardService::capture_current() {
    IDataObject* raw = nullptr;
    if (FAILED(OleGetClipboard(&raw)) || !raw) return false;
    std::unique_ptr<IDataObject, void (*)(IDataObject*)> data(raw, [](IDataObject* value) { value->Release(); });

    smk::core::ClipboardEntry entry;
    entry.id = make_id();
    entry.plain_text = read_unicode_format(data.get(), CF_UNICODETEXT);
    entry.html_text = read_unicode_format(data.get(), static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"HTML Format")));
    entry.rtf_text = read_unicode_format(data.get(), static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Rich Text Format")));
    entry.csv_text = read_unicode_format(data.get(), static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Csv")));
    entry.tsv_text = entry.plain_text.find(L'\t') != std::wstring::npos ? entry.plain_text : L"";
    entry.looks_like_spreadsheet = !entry.csv_text.empty() || !entry.tsv_text.empty() || entry.html_text.find(L"<table") != std::wstring::npos;
    entry.looks_like_single_cell = !entry.plain_text.empty() && entry.plain_text.find_first_of(L"\t\r\n") == std::wstring::npos;
    if (capture_images_ && !entry.looks_like_spreadsheet) {
        if (const auto image = read_image_payload(data.get())) {
            entry.image_png_bytes = image->png;
            entry.preview_image_png_bytes = image->preview_png;
            entry.image_hash = image->sha256;
            entry.image_width = image->width;
            entry.image_height = image->height;
            SMK_DIAGNOSTIC_EVENT("clipboard.image_capture", std::format(L"width={} height={} png_bytes={} preview_bytes={}",
                image->width, image->height, image->png.size(), image->preview_png.size()));
        }
    }
    entry.is_image_content = !entry.image_png_bytes.empty() && entry.plain_text.empty() && entry.html_text.empty();
    entry.display_text = entry.is_image_content ? L"图片" : entry.plain_text.substr(0, std::min<std::size_t>(entry.plain_text.size(), 80));
    if (entry.plain_text.empty() && entry.html_text.empty() && entry.rtf_text.empty() && entry.csv_text.empty() && entry.image_png_bytes.empty()) return false;
    history_.add_or_promote(std::move(entry));
    if (changed_) changed_();
    return true;
}

bool ClipboardService::paste_entry(const smk::core::ClipboardEntry& entry, smk::core::PasteMode requested) {
    pending_entry_ = entry;
    pending_mode_ = smk::core::resolve_paste_mode(entry, requested);
    pending_started_ = GetTickCount64();
    suppress_capture_until_ = pending_started_ + kRetryTimeoutMs + 250;
    capture_coalescer_.cancel();
    if (window_) KillTimer(window_, kCaptureTimer);
    PostMessageW(window_, kAttemptPasteMessage, 0, 0);
    return true;
}

bool ClipboardService::write_text_entry(const smk::core::ClipboardEntry& entry, smk::core::PasteMode mode) {
    if (!OpenClipboard(window_)) return false;
    EmptyClipboard();
    bool ok = set_clipboard_text(CF_UNICODETEXT, entry.plain_text, true);
    if (mode == smk::core::PasteMode::formatted) {
        ok = set_clipboard_text(static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"HTML Format")), entry.html_text, false) && ok;
        ok = set_clipboard_text(static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Rich Text Format")), entry.rtf_text, false) && ok;
        ok = set_clipboard_text(static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Csv")), entry.csv_text, false) && ok;
    }
    const BOOL include_in_history = FALSE;
    HGLOBAL opt_out = GlobalAlloc(GMEM_MOVEABLE, sizeof(include_in_history));
    if (opt_out) {
        void* memory = GlobalLock(opt_out);
        std::memcpy(memory, &include_in_history, sizeof(include_in_history));
        GlobalUnlock(opt_out);
        if (!SetClipboardData(RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"), opt_out)) GlobalFree(opt_out);
    }
    CloseClipboard();
    return ok;
}

bool ClipboardService::attempt_pending_paste() {
    if (!pending_entry_) { KillTimer(window_, kRetryTimer); return false; }
    bool written = false;
    if (pending_entry_->has_image() && pending_entry_->is_image_content) {
        if (const auto payload = normalize_png_payload(pending_entry_->image_png_bytes)) {
            auto data = create_image_data_object(*payload);
            const HRESULT result = data ? OleSetClipboard(data.Get()) : E_OUTOFMEMORY;
            if (data && SUCCEEDED(result)) { owned_clipboard_ = std::move(data); written = true; }
            else SMK_DIAGNOSTIC_EVENT("clipboard.image_paste_retry", std::format(L"elapsed={} hr=0x{:08X}", GetTickCount64() - pending_started_, static_cast<unsigned>(result)));
        }
    } else {
        written = write_text_entry(*pending_entry_, pending_mode_);
        if (written) owned_clipboard_.Reset();
    }
    if (written) {
        suppress_capture_until_ = GetTickCount64() + 300;
        pending_entry_.reset(); KillTimer(window_, kRetryTimer);
        SetTimer(window_, kSendPasteTimer, kRetryIntervalMs, nullptr); return true;
    }
    if (GetTickCount64() - pending_started_ >= kRetryTimeoutMs) {
        pending_entry_.reset(); KillTimer(window_, kRetryTimer); return false;
    }
    SetTimer(window_, kRetryTimer, kRetryIntervalMs, nullptr); return false;
}

} // namespace smk::windows
