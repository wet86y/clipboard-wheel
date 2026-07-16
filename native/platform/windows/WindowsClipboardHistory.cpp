#include "platform/windows/WindowsClipboardHistory.h"
#include "platform/windows/ImageClipboardPayload.h"

#include <roapi.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <algorithm>
#include <cwctype>

namespace smk::windows {
namespace {

constexpr wchar_t kWindowClass[] = L"SuperMiddleKeyNativeWinRtHistory";

std::wstring make_id() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return std::to_wstring(GetTickCount64());
    wchar_t text[40]{};
    StringFromGUID2(guid, text, static_cast<int>(std::size(text)));
    std::wstring result(text);
    std::erase_if(result, [](wchar_t value) { return value == L'{' || value == L'}' || value == L'-'; });
    return result;
}

bool contains_table_html(std::wstring_view html) noexcept {
    constexpr std::wstring_view token = L"<table";
    for (std::size_t index = 0; index + token.size() <= html.size(); ++index) {
        bool match = true;
        for (std::size_t offset = 0; offset < token.size(); ++offset) {
            if (std::towlower(html[index + offset]) != token[offset]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

} // namespace

WindowsClipboardHistory::WindowsClipboardHistory(smk::core::ClipboardHistory& history, ImportedCallback imported)
    : history_(history), imported_(std::move(imported)) {}

WindowsClipboardHistory::~WindowsClipboardHistory() { stop(); }

bool WindowsClipboardHistory::start(HINSTANCE instance, std::size_t limit, bool capture_images) {
    instance_ = instance;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.lpfnWndProc = window_proc;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    window_ = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
    if (!window_) return false;
    worker_ = std::jthread([this, limit, capture_images](std::stop_token stop_token) {
        load_on_worker(stop_token, limit, capture_images);
    });
    return true;
}

void WindowsClipboardHistory::stop() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    if (window_) {
        MSG pending{};
        while (PeekMessageW(&pending, window_, kImportMessage, kImportMessage, PM_REMOVE)) {
            delete reinterpret_cast<std::vector<smk::core::ClipboardEntry>*>(pending.lParam);
        }
        DestroyWindow(window_);
        window_ = nullptr;
    }
}

LRESULT CALLBACK WindowsClipboardHistory::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<WindowsClipboardHistory*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<WindowsClipboardHistory*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle_message(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT WindowsClipboardHistory::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kImportMessage) {
        std::unique_ptr<std::vector<smk::core::ClipboardEntry>> entries(
            reinterpret_cast<std::vector<smk::core::ClipboardEntry>*>(lparam));
        for (auto& entry : *entries) history_.import_backfill(std::move(entry));
        if (imported_) imported_();
        return 0;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

void WindowsClipboardHistory::load_on_worker(std::stop_token stop_token, std::size_t limit, bool capture_images) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        using Clipboard = winrt::Windows::ApplicationModel::DataTransfer::Clipboard;
        using StandardDataFormats = winrt::Windows::ApplicationModel::DataTransfer::StandardDataFormats;
        using Status = winrt::Windows::ApplicationModel::DataTransfer::ClipboardHistoryItemsResultStatus;
        if (stop_token.stop_requested() || !Clipboard::IsHistoryEnabled()) return;
        const auto result = Clipboard::GetHistoryItemsAsync().get();
        if (result.Status() != Status::Success) return;
        const auto items = result.Items();
        std::vector<smk::core::ClipboardEntry> imported;
        const auto count = std::min<std::size_t>(limit, items.Size());
        imported.reserve(count);
        for (std::size_t offset = count; offset > 0 && !stop_token.stop_requested(); --offset) {
            const auto view = items.GetAt(static_cast<std::uint32_t>(offset - 1)).Content();
            smk::core::ClipboardEntry entry;
            entry.id = make_id();
            entry.source_process_name = L"WindowsClipboardHistory";
            if (view.Contains(StandardDataFormats::Text())) entry.plain_text = view.GetTextAsync().get().c_str();
            if (view.Contains(StandardDataFormats::Html())) entry.html_text = view.GetHtmlFormatAsync().get().c_str();
            if (view.Contains(StandardDataFormats::Rtf())) entry.rtf_text = view.GetRtfAsync().get().c_str();
            entry.looks_like_spreadsheet = entry.plain_text.find(L'\t') != std::wstring::npos
                || entry.plain_text.find(L'\n') != std::wstring::npos
                || contains_table_html(entry.html_text);
            entry.looks_like_single_cell = !entry.plain_text.empty()
                && entry.plain_text.find_first_of(L"\t\r\n") == std::wstring::npos;
            if (should_import_history_image(entry, capture_images,
                    view.Contains(StandardDataFormats::Bitmap()))) {
                const auto reference = view.GetBitmapAsync().get();
                const auto stream = reference ? reference.OpenReadAsync().get() : nullptr;
                if (stream && history_image_stream_size_allowed(stream.Size())) {
                    const auto size = static_cast<std::uint32_t>(stream.Size());
                    winrt::Windows::Storage::Streams::DataReader reader(stream);
                    if (reader.LoadAsync(size).get() == size) {
                        std::vector<std::uint8_t> bytes(size);
                        reader.ReadBytes(winrt::array_view<std::uint8_t>(bytes));
                        if (const auto image = normalize_png_payload(bytes)) {
                            entry.image_png_bytes = image->png;
                            entry.preview_image_png_bytes = image->preview_png;
                            entry.image_hash = image->sha256;
                            entry.image_width = image->width;
                            entry.image_height = image->height;
                            entry.is_image_content = true;
                        }
                    }
                }
            }
            entry.display_text = entry.is_image_content
                ? L"[图片]"
                : entry.plain_text.substr(0, std::min<std::size_t>(entry.plain_text.size(), 80));
            if (!entry.plain_text.empty() || !entry.html_text.empty() || !entry.rtf_text.empty()
                || entry.has_image()) imported.push_back(std::move(entry));
        }
        if (imported.empty() || stop_token.stop_requested() || !window_) return;
        auto* payload = new std::vector<smk::core::ClipboardEntry>(std::move(imported));
        if (!PostMessageW(window_, kImportMessage, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
    } catch (...) {
        // Clipboard history is an optional enhancement. Runtime clipboard
        // listening remains active when WinRT is unavailable or restricted.
    }
}

} // namespace smk::windows
