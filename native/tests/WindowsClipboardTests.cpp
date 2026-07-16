#include "platform/windows/ImageClipboardPayload.h"
#include "platform/windows/ClipboardService.h"
#include "platform/windows/ExtendedActionExecutor.h"
#include "platform/windows/HotkeyCaptureHook.h"
#include "platform/windows/MouseHook.h"
#include "platform/windows/ShortcutDropTarget.h"
#include "platform/windows/ShortcutIconResolver.h"
#include "platform/windows/TrayIcon.h"
#include "platform/windows/WindowsClipboardHistory.h"

#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

namespace {
int failures = 0;
constexpr wchar_t kActionHelperClass[] = L"SuperMiddleKeyExtendedActionTestWindow";

void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

bool offers(IDataObject* data, CLIPFORMAT format, DWORD medium) {
    FORMATETC request{format, nullptr, DVASPECT_CONTENT, -1, medium};
    return SUCCEEDED(data->QueryGetData(&request));
}

HGLOBAL copy_test_global(const void* bytes, std::size_t size) {
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!global) return nullptr;
    void* target = GlobalLock(global);
    if (!target) { GlobalFree(global); return nullptr; }
    std::memcpy(target, bytes, size);
    GlobalUnlock(global);
    return global;
}

class BrowserImageDataObject final : public IDataObject {
public:
    BrowserImageDataObject(std::vector<std::uint8_t> image, const wchar_t* image_format, bool unicode)
        : image_(std::move(image)), image_format_(static_cast<CLIPFORMAT>(RegisterClipboardFormatW(image_format))),
          unicode_(unicode), html_(static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"HTML Format"))) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER; *object = nullptr;
        if (id == IID_IUnknown || id == IID_IDataObject) { *object = this; AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG value = --references_; if (!value) delete this; return value; }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!format || !medium || FAILED(QueryGetData(format))) return DV_E_FORMATETC;
        std::memset(medium, 0, sizeof(*medium)); medium->tymed = TYMED_HGLOBAL;
        if (format->cfFormat == image_format_) {
            medium->hGlobal = copy_test_global(image_.data(), image_.size());
        } else if (format->cfFormat == CF_UNICODETEXT) {
            constexpr wchar_t text[] = L"https://example.test/browser-image";
            medium->hGlobal = copy_test_global(text, sizeof(text));
        } else if (format->cfFormat == CF_TEXT) {
            constexpr char text[] = "legacy browser image";
            medium->hGlobal = copy_test_global(text, sizeof(text));
        } else {
            constexpr char html[] = "<img src=\"https://example.test/browser-image\">";
            medium->hGlobal = copy_test_global(html, sizeof(html));
        }
        return medium->hGlobal ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (!format || !(format->tymed & TYMED_HGLOBAL)) return DV_E_FORMATETC;
        if (format->cfFormat == image_format_ || format->cfFormat == html_
            || format->cfFormat == (unicode_ ? CF_UNICODETEXT : CF_TEXT)) return S_OK;
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override { if (output) output->ptd = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
private:
    std::atomic<ULONG> references_{1};
    std::vector<std::uint8_t> image_;
    CLIPFORMAT image_format_{};
    bool unicode_ = true;
    CLIPFORMAT html_{};
};

class ShortcutDropDataObject final : public IDataObject {
public:
    explicit ShortcutDropDataObject(const std::vector<std::wstring>& paths) {
        std::size_t character_count = 1;
        for (const auto& path : paths) character_count += path.size() + 1;
        data_ = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT,
            sizeof(DROPFILES) + character_count * sizeof(wchar_t));
        if (!data_) return;
        auto* bytes = static_cast<std::byte*>(GlobalLock(data_));
        if (!bytes) return;
        auto* header = reinterpret_cast<DROPFILES*>(bytes);
        header->pFiles = sizeof(DROPFILES);
        header->fWide = TRUE;
        auto* cursor = reinterpret_cast<wchar_t*>(bytes + sizeof(DROPFILES));
        for (const auto& path : paths) {
            std::copy(path.begin(), path.end(), cursor);
            cursor += path.size();
            *cursor++ = L'\0';
        }
        *cursor = L'\0';
        GlobalUnlock(data_);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (id == IID_IUnknown || id == IID_IDataObject) {
            *object = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!medium) return E_POINTER;
        if (FAILED(QueryGetData(format))) return DV_E_FORMATETC;
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = static_cast<HGLOBAL>(OleDuplicateData(data_, CF_HDROP, 0));
        medium->pUnkForRelease = nullptr;
        return medium->hGlobal ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        return format && format->cfFormat == CF_HDROP && (format->tymed & TYMED_HGLOBAL)
            ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override {
        if (output) output->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    ~ShortcutDropDataObject() { if (data_) GlobalFree(data_); }
    std::atomic<ULONG> references_{1};
    HGLOBAL data_ = nullptr;
};

LRESULT CALLBACK helper_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_TIMER || message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}

int run_action_helper() {
    WNDCLASSW window_class{};
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpfnWndProc = helper_window_proc;
    window_class.lpszClassName = kActionHelperClass;
    RegisterClassW(&window_class);
    const HWND window = CreateWindowW(kActionHelperClass, L"Extended action test", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 320, 200, nullptr, nullptr, window_class.hInstance, nullptr);
    if (!window) return 2;
    ShowWindow(window, SW_SHOWNORMAL);
    SetTimer(window, 1, 15'000, nullptr);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return 0;
}

template <typename Predicate>
bool wait_until(Predicate predicate, DWORD timeout = 5000) {
    const auto deadline = GetTickCount64() + timeout;
    while (GetTickCount64() < deadline) {
        if (predicate()) return true;
        Sleep(20);
    }
    return predicate();
}
}

int main() {
    if (wcsstr(GetCommandLineW(), L"--extended-action-helper")) return run_action_helper();
    if (FAILED(OleInitialize(nullptr))) return EXIT_FAILURE;
    constexpr UINT width = 720, height = 360;
    std::vector<std::uint8_t> zero_alpha(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t index = 0; index < zero_alpha.size(); index += 4) {
        zero_alpha[index] = 0x20; zero_alpha[index + 1] = 0x80; zero_alpha[index + 2] = 0xE0;
    }
    const auto payload = smk::windows::normalize_bgra_payload(width, height, zero_alpha);
    expect(payload && payload->valid(), "zero-alpha screenshot pixels normalize to a valid PNG");
    expect(payload && payload->png.size() <= smk::windows::ImageClipboardPayload::kMaxEncodedBytes, "normalized PNG respects 32 MiB limit");
    const auto preview = payload ? smk::windows::normalize_png_payload(payload->preview_png) : std::nullopt;
    expect(preview && preview->width == 360 && preview->height == 180, "preview is constrained to a 360 pixel edge");
    expect(!smk::windows::normalize_bgra_payload(8001, 8000, {}).has_value(), "64 million pixel overflow is rejected");
    expect(smk::windows::history_image_stream_size_allowed(33ULL * 1024ULL * 1024ULL),
        "Win+V accepts a raw 4K bitmap stream larger than the final PNG limit");
    expect(!smk::windows::history_image_stream_size_allowed(
        smk::windows::kMaximumHistoryImageInputBytes + 1),
        "Win+V rejects image streams beyond the pixel-safe raw input budget");
    smk::core::ClipboardEntry history_table;
    history_table.looks_like_spreadsheet = true;
    expect(!smk::windows::should_import_history_image(history_table, true, true),
        "Win+V spreadsheet entries suppress their redundant bitmap representation");
    smk::core::ClipboardEntry history_screenshot;
    expect(smk::windows::should_import_history_image(history_screenshot, true, true),
        "Win+V pure screenshots import when image capture is enabled");
    expect(!smk::windows::should_import_history_image(history_screenshot, false, true),
        "Win+V screenshots respect the image capture toggle");
    expect(smk::windows::should_retry_clipboard_flush(CLIPBRD_E_CANT_OPEN, 1)
        && smk::windows::should_retry_clipboard_flush(CLIPBRD_E_CANT_CLOSE, 5),
        "shutdown retries transient clipboard open and close failures");
    expect(!smk::windows::should_retry_clipboard_flush(CLIPBRD_E_CANT_OPEN, 6)
        && !smk::windows::should_retry_clipboard_flush(E_FAIL, 1)
        && !smk::windows::should_retry_clipboard_flush(S_OK, 1),
        "shutdown stops after six attempts and never retries terminal or successful flush results");

    if (payload) {
        auto* browser_source = new BrowserImageDataObject(payload->png, L"JPG", true);
        const auto browser_entry = smk::windows::ClipboardService::create_entry_from_data_object(browser_source, true);
        browser_source->Release();
        expect(browser_entry && browser_entry->is_image_content && browser_entry->has_image()
            && !browser_entry->plain_text.empty() && !browser_entry->html_text.empty(),
            "browser JPG plus text and HTML is captured and classified as an image");

        auto* legacy_source = new BrowserImageDataObject(payload->png, L"GIF", false);
        const auto legacy_entry = smk::windows::ClipboardService::create_entry_from_data_object(legacy_source, true);
        legacy_source->Release();
        expect(legacy_entry && legacy_entry->plain_text == L"legacy browser image" && legacy_entry->is_image_content,
            "CF_TEXT fallback and GIF alias are captured together");

        auto* disabled_source = new BrowserImageDataObject(payload->png, L"JPG", true);
        const auto disabled_entry = smk::windows::ClipboardService::create_entry_from_data_object(disabled_source, false);
        disabled_source->Release();
        expect(disabled_entry && !disabled_entry->has_image() && !disabled_entry->is_image_content,
            "image capture toggle suppresses browser image bytes while retaining text fallbacks");
    }

    auto data = payload ? smk::windows::create_image_data_object(*payload) : nullptr;
    expect(data != nullptr, "image data object is created");
    if (data) {
        expect(!offers(data.Get(), static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"PNG")), TYMED_HGLOBAL | TYMED_ISTREAM), "custom PNG is not advertised to Office targets");
        expect(!offers(data.Get(), static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"image/png")), TYMED_HGLOBAL | TYMED_ISTREAM), "custom image/png is not advertised to Office targets");
        expect(offers(data.Get(), static_cast<CLIPFORMAT>(CF_DIBV5), TYMED_HGLOBAL), "DIBV5 is advertised");
        expect(offers(data.Get(), static_cast<CLIPFORMAT>(CF_DIB), TYMED_HGLOBAL), "DIB is advertised");
        expect(offers(data.Get(), static_cast<CLIPFORMAT>(CF_BITMAP), TYMED_GDI), "BITMAP is advertised");
        expect(!offers(data.Get(), static_cast<CLIPFORMAT>(CF_UNICODETEXT), TYMED_HGLOBAL), "pure images do not advertise empty Unicode text");
        expect(offers(data.Get(), static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"CanIncludeInClipboardHistory")), TYMED_HGLOBAL), "clipboard history opt-out is advertised");
        for (const auto format : {static_cast<CLIPFORMAT>(CF_DIBV5), static_cast<CLIPFORMAT>(CF_DIB), static_cast<CLIPFORMAT>(CF_BITMAP)}) {
            FORMATETC request{format, nullptr, DVASPECT_CONTENT, -1,
                static_cast<DWORD>(format == CF_BITMAP ? TYMED_GDI : TYMED_HGLOBAL)};
            STGMEDIUM medium{}; const HRESULT result = data->GetData(&request, &medium);
            expect(SUCCEEDED(result), "advertised compatibility format can be materialized");
            if (SUCCEEDED(result)) ReleaseStgMedium(&medium);
        }
    }

    smk::core::ClipboardEntry mixed_image;
    mixed_image.id = L"browser-image";
    mixed_image.plain_text = L"https://example.test/image.png";
    mixed_image.html_text = L"<img src=\"https://example.test/image.png\">";
    if (payload) mixed_image.image_png_bytes = payload->png;
    expect(smk::windows::should_capture_as_image(mixed_image),
        "browser image payload remains an image when text and HTML fallbacks are present");
    mixed_image.is_image_content = smk::windows::should_capture_as_image(mixed_image);
    auto mixed_data = smk::windows::create_clipboard_data_object(
        mixed_image, smk::core::PasteMode::formatted);
    expect(mixed_data && offers(mixed_data.Get(), static_cast<CLIPFORMAT>(CF_DIBV5), TYMED_HGLOBAL)
        && !offers(mixed_data.Get(), static_cast<CLIPFORMAT>(CF_UNICODETEXT), TYMED_HGLOBAL),
        "browser mixed payload is written through the image path, not the fallback text path");
    mixed_image.looks_like_spreadsheet = true;
    expect(!smk::windows::should_capture_as_image(mixed_image),
        "spreadsheet payloads still suppress accidental image capture");

    smk::core::ClipboardEntry rich_text;
    rich_text.id = L"rich";
    rich_text.plain_text = L"中文 text";
    rich_text.html_text = L"<b>中文 text</b>";
    rich_text.rtf_text = L"{\\rtf1 text}";
    rich_text.csv_text = L"a,b";
    auto text_data = smk::windows::create_clipboard_data_object(
        rich_text, smk::core::PasteMode::formatted);
    expect(text_data && offers(text_data.Get(), CF_UNICODETEXT, TYMED_HGLOBAL),
        "text data object advertises Unicode text");
    expect(text_data && offers(text_data.Get(), CF_TEXT, TYMED_HGLOBAL),
        "text data object advertises the managed CF_TEXT compatibility fallback");
    expect(text_data && offers(text_data.Get(), static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"HTML Format")), TYMED_HGLOBAL)
        && offers(text_data.Get(), static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Rich Text Format")), TYMED_HGLOBAL)
        && offers(text_data.Get(), static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Csv")), TYMED_HGLOBAL),
        "formatted data object advertises HTML, RTF, and CSV together");

    smk::windows::ClipboardCaptureRetryState capture_retry;
    const auto first_generation = capture_retry.begin();
    for (unsigned attempt = 2; attempt <= smk::windows::ClipboardCaptureRetryState::kMaximumAttempts; ++attempt) {
        expect(capture_retry.advance(first_generation), "clipboard busy retry advances within the six-attempt budget");
    }
    expect(!capture_retry.advance(first_generation), "clipboard busy retry stops after six attempts");
    const auto second_generation = capture_retry.begin();
    expect(second_generation != first_generation && !capture_retry.advance(first_generation),
        "a new clipboard update invalidates stale capture retries");

    smk::windows::MouseButtonSuppression buttons;
    auto decision = buttons.handle(WM_LBUTTONUP, true);
    expect(!decision.suppress, "a button pressed before the wheel keeps its matching release");
    decision = buttons.handle(WM_LBUTTONDOWN, true);
    expect(decision.suppress && !decision.toggle_lock, "left down is swallowed without an action");
    decision = buttons.handle(WM_LBUTTONUP, false);
    expect(decision.suppress && !buttons.pending_release(),
        "left up remains swallowed after the wheel closes");
    decision = buttons.handle(WM_RBUTTONDOWN, true);
    expect(decision.suppress && decision.toggle_lock, "right down is swallowed and toggles lock once");
    decision = buttons.handle(WM_RBUTTONUP, false);
    expect(decision.suppress && !decision.toggle_lock && !buttons.pending_release(),
        "right up remains swallowed after the wheel closes");
    decision = buttons.handle(WM_RBUTTONDOWN, false);
    expect(!decision.suppress && !decision.toggle_lock, "right clicks outside a wheel pass through");
    decision = buttons.handle(WM_LBUTTONDOWN, true);
    buttons.confirm_physical_release();
    decision = buttons.handle(WM_LBUTTONUP, false);
    expect(decision.suppress, "a physically confirmed late release is still paired");
    decision = buttons.handle(WM_LBUTTONDOWN, true);
    buttons.confirm_physical_release();
    decision = buttons.handle(WM_LBUTTONDOWN, false);
    expect(!decision.suppress && !buttons.handle(WM_LBUTTONUP, false).suppress,
        "a new click supersedes a missing late release without swallowing its up");

    smk::windows::MouseInputSafetyState input_state;
    input_state.note_hook_available(true);
    input_state.note_heartbeat(100);
    (void)input_state.poll(false, true, 100);
    input_state.note_heartbeat(350);
    (void)input_state.poll(false, true, 350);
    expect(input_state.capture_ready(), "capture waits for two UI heartbeats and released buttons");
    auto input = input_state.handle(WM_MBUTTONDOWN, true, 400);
    expect(input.suppress && input.dispatch && input.generation == 1,
        "a healthy middle down is paired and dispatched");
    input_state.acknowledge_show(input.generation, true);
    input_state.note_heartbeat(30'400);
    input = input_state.poll(true, false, 30'400);
    expect(input_state.wheel_active() && input_state.middle_down_suppressed() && !input.dispatch,
        "a physically held middle button remains active for 30 seconds without an absolute timeout");
    input = input_state.handle(WM_MBUTTONUP, true, 30'450);
    expect(input.suppress && input.dispatch && !input_state.middle_down_suppressed(),
        "the matching middle release is swallowed and delivered once");

    smk::windows::MouseInputSafetyState lost_release;
    lost_release.note_hook_available(true);
    lost_release.note_heartbeat(10);
    (void)lost_release.poll(false, true, 10);
    lost_release.note_heartbeat(260);
    (void)lost_release.poll(false, true, 260);
    input = lost_release.handle(WM_MBUTTONDOWN, true, 300);
    lost_release.acknowledge_show(input.generation, true);
    input = lost_release.poll(true, false, 405);
    expect(lost_release.middle_down_suppressed() && !input.dispatch,
        "physical release recovery first establishes an observed down state");
    input = lost_release.poll(false, true, 410);
    expect(lost_release.middle_down_suppressed() && !input.dispatch,
        "one physical-up sample cannot end an input session");
    input = lost_release.poll(false, true, 460);
    expect(!lost_release.middle_down_suppressed() && input.dispatch &&
        input.event == smk::windows::MouseHookEvent::cancel,
        "two physical-up samples recover a missing WM_MBUTTONUP without an action");

    smk::windows::MouseInputSafetyState async_state_unavailable;
    async_state_unavailable.note_hook_available(true);
    async_state_unavailable.note_heartbeat(10);
    (void)async_state_unavailable.poll(false, true, 10);
    async_state_unavailable.note_heartbeat(260);
    (void)async_state_unavailable.poll(false, true, 260);
    input = async_state_unavailable.handle(WM_MBUTTONDOWN, true, 300);
    async_state_unavailable.acknowledge_show(input.generation, true);
    async_state_unavailable.note_heartbeat(30'300);
    input = async_state_unavailable.poll(false, true, 30'300);
    input = async_state_unavailable.poll(false, true, 30'350);
    expect(async_state_unavailable.wheel_active() &&
        async_state_unavailable.middle_down_suppressed() && !input.dispatch,
        "an async-state source that never observed down cannot falsely close a held wheel");

    smk::windows::MouseInputSafetyState stale_ui;
    stale_ui.note_hook_available(true);
    stale_ui.note_heartbeat(100);
    (void)stale_ui.poll(false, true, 100);
    stale_ui.note_heartbeat(350);
    (void)stale_ui.poll(false, true, 350);
    input = stale_ui.handle(WM_MBUTTONDOWN, true, 400);
    stale_ui.acknowledge_show(input.generation, true);
    input = stale_ui.poll(true, false, 1'401);
    expect(input.dispatch && input.event == smk::windows::MouseHookEvent::cancel &&
        !stale_ui.ui_healthy() && stale_ui.middle_down_suppressed(),
        "a stale UI fails open while retaining the paired middle release");
    input = stale_ui.handle(WM_MBUTTONUP, false, 1'450);
    expect(input.suppress && !input.dispatch,
        "a stale UI never receives an action but the paired release remains swallowed");
    stale_ui.note_heartbeat(1'500);
    (void)stale_ui.poll(false, true, 1'500);
    stale_ui.note_heartbeat(1'750);
    (void)stale_ui.poll(false, true, 1'750);
    expect(stale_ui.capture_ready(), "capture automatically recovers after two heartbeats and full release");

    smk::windows::MouseInputSafetyState dispatch_failure;
    dispatch_failure.note_hook_available(true);
    dispatch_failure.note_heartbeat(10);
    (void)dispatch_failure.poll(false, true, 10);
    dispatch_failure.note_heartbeat(260);
    (void)dispatch_failure.poll(false, true, 260);
    input = dispatch_failure.handle(WM_MBUTTONDOWN, false, 300);
    expect(!input.suppress && !dispatch_failure.wheel_active(),
        "a failed UI event post passes the middle down through immediately");
    input = dispatch_failure.handle(WM_MBUTTONDOWN, true, 350);
    dispatch_failure.acknowledge_show(input.generation, false);
    expect(!dispatch_failure.wheel_active() && dispatch_failure.middle_down_suppressed(),
        "a failed first frame cancels the wheel but preserves release pairing");
    input = dispatch_failure.handle(WM_MBUTTONUP, false, 400);
    expect(input.suppress && !input.dispatch,
        "first-frame failure cannot leak an orphaned middle release");

    WNDCLASSW input_window_class{};
    input_window_class.hInstance = GetModuleHandleW(nullptr);
    input_window_class.lpfnWndProc = DefWindowProcW;
    input_window_class.lpszClassName = L"SuperMiddleKeyInputSafetyTestWindow";
    RegisterClassW(&input_window_class);
    const HWND input_window = CreateWindowExW(0, input_window_class.lpszClassName,
        L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, input_window_class.hInstance, nullptr);
    expect(input_window != nullptr, "input safety integration target is created");
    if (input_window) {
        smk::windows::MouseHook threaded_hook(input_window);
        expect(threaded_hook.start(),
            "the dedicated hook message thread starts even when hook installation must degrade");
        expect(threaded_hook.stop(1500), "the dedicated hook thread confirms unhook and shutdown");
        DestroyWindow(input_window);
    }

    smk::windows::ClipboardUpdateCoalescer clipboard_updates;
    clipboard_updates.note_update(1000);
    expect(!clipboard_updates.consume_if_ready(1039),
        "clipboard updates use the managed forty millisecond coalescing window");
    clipboard_updates.note_update(1020);
    expect(!clipboard_updates.consume_if_ready(1059) && clipboard_updates.remaining_ms(1059) == 1,
        "a second format update restarts the forty millisecond quiet window");
    expect(clipboard_updates.consume_if_ready(1060) && !clipboard_updates.pending(),
        "a burst of clipboard updates produces one capture after the final managed quiet period");
    clipboard_updates.note_update(2000, smk::windows::ClipboardUpdateCoalescer::kImageQuietPeriodMs);
    expect(!clipboard_updates.consume_if_ready(2119),
        "image clipboard updates retain the screenshot stabilization window");
    clipboard_updates.note_update(2080, smk::windows::ClipboardUpdateCoalescer::kImageQuietPeriodMs);
    expect(!clipboard_updates.consume_if_ready(2199) && clipboard_updates.remaining_ms(2199) == 1,
        "a screenshot final-format update restarts the image stabilization window");
    expect(clipboard_updates.consume_if_ready(2200) && !clipboard_updates.pending(),
        "two-stage screenshot publication produces one capture after the final image update");

    const auto hotkey = smk::windows::parse_extended_hotkey(L"Ctrl+Shift+F12");
    expect(hotkey.size() == 3 && hotkey[0] == VK_CONTROL && hotkey[2] == VK_F12,
        "extended hotkey parser matches the managed key vocabulary");
    smk::windows::HotkeyRecordingSession recording;
    recording.begin();
    expect(recording.add(VK_RCONTROL), "hotkey recorder accepts a modifier");
    expect(!recording.add(VK_LCONTROL), "hotkey recorder normalizes and deduplicates left/right modifiers");
    expect(recording.add(VK_F12), "hotkey recorder accumulates a later non-modifier key");
    expect(recording.add(VK_LSHIFT), "hotkey recorder accepts keys entered after earlier releases");
    expect(recording.display_text() == L"Ctrl+Shift+F12",
        "hotkey recorder keeps a managed-style persistent chord");
    expect(recording.finish() == L"Ctrl+Shift+F12" && !recording.recording(),
        "finishing a recording commits the canonical chord");

    smk::windows::HotkeyCaptureFilter capture_filter;
    auto capture = capture_filter.handle(WM_KEYDOWN, VK_LCONTROL, 0x1d, 0, true);
    expect(capture.suppress && capture.deliver && capture.virtual_key == VK_LCONTROL,
        "low-level hotkey capture delivers the first physical key and suppresses it before IME handling");
    capture = capture_filter.handle(WM_KEYDOWN, VK_LCONTROL, 0x1d, 0, true);
    expect(capture.suppress && !capture.deliver,
        "low-level hotkey capture removes keyboard auto-repeat");
    capture = capture_filter.handle(WM_KEYUP, VK_LCONTROL, 0x1d, 0, false);
    expect(capture.suppress && !capture.deliver,
        "a captured key release stays suppressed after the settings window loses foreground");
    capture = capture_filter.handle(WM_KEYDOWN, L'A', 0x1e, 0, false);
    expect(!capture.suppress && !capture.deliver,
        "capture does not intercept new keys when the settings window is not foreground");
    capture = capture_filter.handle(WM_KEYDOWN, VK_PROCESSKEY, 0, 0, true);
    expect(capture.suppress && !capture.deliver,
        "IME process-key placeholders are swallowed without entering the recorded chord");
    (void)capture_filter.handle(WM_KEYUP, VK_PROCESSKEY, 0, 0, true);
    capture = capture_filter.handle(WM_SYSKEYDOWN, VK_RMENU, 0x38, LLKHF_EXTENDED, true);
    expect(capture.suppress && capture.deliver
        && (capture.key_data & static_cast<LPARAM>(1u << 24)) != 0,
        "system keys and the extended-key flag are forwarded to the shared hotkey codec");
    capture_filter.reset();
    const std::array<WORD, 5> extended_keys{VK_NUMPAD7, VK_F24, VK_OEM_2, VK_MENU, VK_LWIN};
    const auto encoded = smk::windows::format_extended_hotkey(extended_keys);
    const auto decoded = smk::windows::parse_extended_hotkey(encoded);
    expect(smk::windows::format_extended_hotkey(decoded) == encoded
        && encoded.find(L"Num7") != std::wstring::npos && encoded.find(L"F24") != std::wstring::npos,
        "numpad, F24, punctuation and modifiers round-trip through the shared codec");
    recording.begin(); (void)recording.add(L'A'); recording.cancel();
    expect(!recording.recording() && recording.keys().empty(), "cancelling discards an unfinished recording");
    expect(smk::windows::normalize_browser_launch_url(L"example.com/path") == L"https://example.com/path",
        "browser URL normalization adds https");
    expect(!smk::windows::normalize_browser_launch_url(L"https://bad host.invalid"),
        "browser URL normalization rejects whitespace");
    smk::windows::ShortcutLaunchInfo browser{
        L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", L"--profile-directory=Default", L""};
    bool url_applied = false;
    const auto browser_arguments = smk::windows::build_browser_launch_arguments(
        browser, L"https://example.com", url_applied);
    expect(url_applied && browser_arguments.find(L"--new-window") != std::wstring::npos,
        "browser actions request a separate window and append the configured URL");

    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    wchar_t temporary[MAX_PATH]{};
    GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
    const std::wstring shortcut_path = std::wstring(temporary) + L"smk-native-action-"
        + std::to_wstring(GetCurrentProcessId()) + L".lnk";
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> persist;
    expect(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(link.GetAddressOf()))) && SUCCEEDED(link.As(&persist)), "shortcut fixture COM objects are available");
    if (link && persist) {
        link->SetPath(executable); link->SetArguments(L"--extended-action-helper"); link->SetWorkingDirectory(temporary);
        link->SetIconLocation(executable, 0);
        expect(SUCCEEDED(persist->Save(shortcut_path.c_str(), TRUE)), "shortcut fixture is saved");
        const auto resolved = smk::windows::resolve_shortcut(shortcut_path);
        expect(resolved && resolved->target_path == executable && resolved->arguments == L"--extended-action-helper",
            "native shortcut resolver preserves target and arguments");
        HICON explicit_icon = smk::windows::resolve_shortcut_icon(shortcut_path, 40);
        expect(explicit_icon != nullptr, "shortcut icon-location resource is resolved");
        if (explicit_icon) DestroyIcon(explicit_icon);
        link->SetIconLocation(L"Z:\\missing\\invalid-icon.dll", 7);
        expect(SUCCEEDED(persist->Save(shortcut_path.c_str(), TRUE)), "invalid icon-location fixture is saved");
        smk::windows::clear_shortcut_icon_cache();
        HICON fallback_icon = smk::windows::resolve_shortcut_icon(shortcut_path, 32);
        expect(fallback_icon != nullptr, "invalid icon location falls back to the shortcut target or association");
        if (fallback_icon) DestroyIcon(fallback_icon);
        expect(smk::windows::is_valid_shortcut_drop_path(shortcut_path),
            "drop validation accepts one existing absolute shortcut");
        expect(!smk::windows::is_valid_shortcut_drop_path(L"relative.lnk")
            && !smk::windows::is_valid_shortcut_drop_path(executable),
            "drop validation rejects relative paths and non-shortcut files");

        std::vector<smk::windows::ShortcutDropVisualState> drop_states;
        std::wstring dropped_path;
        IDropTarget* target = smk::windows::create_shortcut_drop_target({
            {},
            [&](smk::windows::ShortcutDropVisualState state) { drop_states.push_back(state); },
            [&](const std::wstring& path) { dropped_path = path; },
        });
        expect(target != nullptr, "native OLE shortcut drop target is created");
        if (target) {
            auto* valid_drop = new ShortcutDropDataObject({shortcut_path});
            DWORD effect = DROPEFFECT_COPY;
            expect(SUCCEEDED(target->DragEnter(valid_drop, 0, {}, &effect))
                && effect == DROPEFFECT_COPY
                && !drop_states.empty()
                && drop_states.back() == smk::windows::ShortcutDropVisualState::accept,
                "one existing shortcut enters the native drop target as valid");
            expect(SUCCEEDED(target->Drop(valid_drop, 0, {}, &effect))
                && dropped_path == shortcut_path
                && drop_states.back() == smk::windows::ShortcutDropVisualState::success,
                "a valid native OLE drop is accepted without a compatibility message handoff");
            valid_drop->Release();

            auto* multiple_drop = new ShortcutDropDataObject({shortcut_path, shortcut_path});
            effect = DROPEFFECT_COPY;
            expect(SUCCEEDED(target->DragEnter(multiple_drop, 0, {}, &effect))
                && effect == DROPEFFECT_NONE
                && drop_states.back() == smk::windows::ShortcutDropVisualState::reject,
                "multiple files are rejected by the native shortcut drop target");
            (void)target->DragLeave();
            multiple_drop->Release();
            target->Release();
        }
    }

    smk::windows::ExtendedActionExecutor executor;
    expect(executor.start(), "extended action worker starts");
    smk::core::ExtendedWheelActionSlot action;
    action.slot_index = 4; action.enabled = true; action.mode = L"shortcut";
    action.shortcut_path = shortcut_path; action.second_trigger_behavior = L"minimize";
    executor.enqueue(action);
    HWND helper = nullptr;
    expect(wait_until([&] { return (helper = FindWindowW(kActionHelperClass, nullptr)) != nullptr; }),
        "shortcut action starts a trackable helper window");
    if (helper) {
        executor.enqueue(action);
        expect(wait_until([&] { return IsIconic(helper) != FALSE; }),
            "second shortcut trigger minimizes the tracked window");
        executor.enqueue(action);
        expect(wait_until([&] { return IsWindow(helper) && IsIconic(helper) == FALSE; }),
            "a hidden tracked window is restored before applying another toggle");
        action.second_trigger_behavior = L"close";
        executor.enqueue(action);
        expect(wait_until([&] { return IsWindow(helper) == FALSE; }),
            "close behavior targets the window launched by this executor");
    }
    std::vector<std::pair<WORD, bool>> key_events;
    expect(smk::windows::dispatch_extended_hotkey(hotkey,
        [&](WORD key, bool key_up) { key_events.emplace_back(key, key_up); return true; }),
        "extended hotkey dispatch accepts a complete key chord");
    const std::vector<std::pair<WORD, bool>> expected_events{
        {static_cast<WORD>(VK_CONTROL), false}, {static_cast<WORD>(VK_SHIFT), false},
        {static_cast<WORD>(VK_F12), false}, {static_cast<WORD>(VK_F12), true},
        {static_cast<WORD>(VK_SHIFT), true}, {static_cast<WORD>(VK_CONTROL), true},
    };
    expect(key_events == expected_events,
        "extended hotkey presses in configured order and releases in reverse order");
    key_events.clear();
    expect(!smk::windows::dispatch_extended_hotkey(hotkey,
        [&](WORD key, bool key_up) {
            key_events.emplace_back(key, key_up);
            return key_up || key != VK_SHIFT;
        }), "a failed key injection reports failure");
    expect(key_events == std::vector<std::pair<WORD, bool>>{
        {static_cast<WORD>(VK_CONTROL), false}, {static_cast<WORD>(VK_SHIFT), false},
        {static_cast<WORD>(VK_CONTROL), true}},
        "a failed key injection releases every key that was already pressed");
    executor.shutdown();
    DeleteFileW(shortcut_path.c_str());

    bool clear_history_called = false;
    smk::windows::TrayIcon tray;
    smk::windows::TrayIcon::Callbacks callbacks;
    callbacks.clear_history = [&] { clear_history_called = true; };
    expect(tray.create(GetModuleHandleW(nullptr), std::move(callbacks)),
        "managed-style enabled tray icon is created");
    HWND tray_window = FindWindowExW(HWND_MESSAGE, nullptr, L"SuperMiddleKeyNativeTrayHost", nullptr);
    expect(tray_window != nullptr, "tray message window is available");
    if (tray_window) SendMessageW(tray_window, WM_COMMAND, 1003, 0);
    expect(clear_history_called, "tray clear-history command reaches the host callback");
    tray.set_enabled(false);
    tray.set_enabled(true);
    tray.destroy();
    OleUninitialize();
    if (!failures) std::cout << "Native Windows clipboard tests passed.\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
