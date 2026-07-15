#include "platform/windows/ShortcutDropHelper.h"

#include "platform/windows/DiagnosticLog.h"
#include "platform/windows/ElevationService.h"
#include "platform/windows/ShortcutDropTarget.h"

#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <format>
#include <utility>

namespace smk::windows {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kClassName[] = L"SuperMiddleKeyShortcutDropHelper";
constexpr UINT_PTR kSuccessTimer = 1;
constexpr UINT_PTR kTimeoutTimer = 2;
constexpr UINT kHandoffTimeoutMs = 180'000;

struct State {
    std::wstring id;
    bool completed = false;
    bool close_hovered = false;
    bool tracking_mouse = false;
    bool paint_logged = false;
    UINT dpi = 96;
    HWND window = nullptr;
    ShortcutDropVisualState visual_state = ShortcutDropVisualState::idle;
    ShortcutDropRegistration drop_target;
    ComPtr<ID2D1Factory> d2d_factory;
    ComPtr<IDWriteFactory> dwrite_factory;
    ComPtr<ID2D1DCRenderTarget> render_target;
    ComPtr<IDWriteTextFormat> title_format;
    ComPtr<IDWriteTextFormat> body_format;
    ComPtr<IDWriteTextFormat> small_format;
};

int px(double value, UINT dpi) {
    return static_cast<int>(std::lround(value * static_cast<double>(dpi) / 96.0));
}

D2D1_COLOR_F color(unsigned rgb, float alpha = 1.0f) {
    return D2D1::ColorF((rgb >> 16 & 0xff) / 255.0f,
        (rgb >> 8 & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, alpha);
}

bool valid_id(const std::wstring& id) {
    if (id.size() != 32) return false;
    return std::all_of(id.begin(), id.end(), [](wchar_t ch) { return std::iswxdigit(ch) != 0; });
}

std::filesystem::path result_path(const std::wstring& id) {
    wchar_t local[32768]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, static_cast<DWORD>(std::size(local)));
    return std::filesystem::path(local) / L"超级中键" / L"shortcut-drop-handoff" / (id + L".result");
}

bool write_result(const std::wstring& id, const std::wstring& value) {
    const auto target = result_path(id);
    std::error_code ignored;
    std::filesystem::create_directories(target.parent_path(), ignored);
    const auto temporary = target.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, value.data(), static_cast<DWORD>(value.size() * sizeof(wchar_t)),
        &written, nullptr) != FALSE;
    CloseHandle(file);
    if (!ok || written != value.size() * sizeof(wchar_t)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return MoveFileExW(temporary.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool ensure_render_resources(State& state) {
    if (!state.d2d_factory
        && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, state.d2d_factory.GetAddressOf())))
        return false;
    if (!state.dwrite_factory
        && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(state.dwrite_factory.GetAddressOf())))) return false;
    if (!state.render_target) {
        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        if (FAILED(state.d2d_factory->CreateDCRenderTarget(&properties,
                state.render_target.GetAddressOf()))) return false;
    }
    const auto make_format = [&](float size, DWRITE_FONT_WEIGHT weight, auto& destination) {
        if (destination) return true;
        return SUCCEEDED(state.dwrite_factory->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
            weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN",
            destination.GetAddressOf()));
    };
    if (!make_format(16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, state.title_format)
        || !make_format(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, state.body_format)
        || !make_format(11.5f, DWRITE_FONT_WEIGHT_NORMAL, state.small_format)) return false;
    for (auto* format : {state.title_format.Get(), state.body_format.Get(), state.small_format.Get()}) {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    state.title_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    return true;
}

void draw_text(ID2D1RenderTarget* target, IDWriteTextFormat* format, ID2D1Brush* brush,
    const wchar_t* text, D2D1_RECT_F bounds) {
    target->DrawTextW(text, static_cast<UINT32>(wcslen(text)), format, bounds, brush,
        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void paint_helper(State& state) {
    PAINTSTRUCT paint{};
    BeginPaint(state.window, &paint);
    RECT client{};
    GetClientRect(state.window, &client);
    const bool resources_ready = ensure_render_resources(state);
    const HRESULT bind_result = resources_ready
        ? state.render_target->BindDC(paint.hdc, &client) : E_FAIL;
    if (!state.paint_logged) {
        state.paint_logged = true;
        SMK_DIAGNOSTIC_EVENT("shortcut_drop.paint", std::format(
            L"resources_ready={} bind={:#x} dpi={} width={} height={}", resources_ready,
            static_cast<unsigned>(bind_result), state.dpi, client.right, client.bottom));
    }
    if (resources_ready && SUCCEEDED(bind_result)) {
        state.render_target->BeginDraw();
        state.render_target->SetTransform(D2D1::Matrix3x2F::Scale(
            D2D1::SizeF(state.dpi / 96.0f, state.dpi / 96.0f)));

        ComPtr<ID2D1SolidColorBrush> background, top, card, border, accent, glow, text, secondary, red;
        state.render_target->CreateSolidColorBrush(color(0x06101F), background.GetAddressOf());
        state.render_target->CreateSolidColorBrush(color(0x0A1729), top.GetAddressOf());
        state.render_target->CreateSolidColorBrush(color(0x0E1D31), card.GetAddressOf());
        state.render_target->CreateSolidColorBrush(color(0x263B58), border.GetAddressOf());
        state.render_target->CreateSolidColorBrush(color(0x0B64F6), accent.GetAddressOf());
        state.render_target->CreateSolidColorBrush(color(0x43B7FF), glow.GetAddressOf());
        state.render_target->CreateSolidColorBrush(color(0xF3F7FF), text.GetAddressOf());
        state.render_target->CreateSolidColorBrush(color(0xAAB7CC), secondary.GetAddressOf());
        state.render_target->CreateSolidColorBrush(color(0xB84A5A), red.GetAddressOf());

        state.render_target->Clear(color(0x06101F));
        state.render_target->FillRectangle(D2D1::RectF(0, 0, 440, 44), top.Get());
        state.render_target->DrawLine(D2D1::Point2F(0, 44), D2D1::Point2F(440, 44), border.Get(), 1.0f);
        if (state.close_hovered)
            state.render_target->FillRectangle(D2D1::RectF(396, 0, 440, 44), red.Get());
        draw_text(state.render_target.Get(), state.title_format.Get(), text.Get(),
            L"超级中键 · 快捷方式拖放", D2D1::RectF(48, 0, 370, 44));
        state.render_target->DrawLine(D2D1::Point2F(410, 15), D2D1::Point2F(426, 31), text.Get(), 1.35f);
        state.render_target->DrawLine(D2D1::Point2F(426, 15), D2D1::Point2F(410, 31), text.Get(), 1.35f);

        ID2D1SolidColorBrush* state_border = border.Get();
        const wchar_t* primary = L"拖入一个 .lnk 快捷方式";
        const wchar_t* secondary_text = L"普通权限窗口会安全地把结果交回设置页";
        if (state.visual_state == ShortcutDropVisualState::accept) {
            state_border = glow.Get(); primary = L"松开以导入快捷方式";
            secondary_text = L"已识别一个有效的 Windows 快捷方式";
        } else if (state.visual_state == ShortcutDropVisualState::reject) {
            state_border = red.Get(); primary = L"无法导入此拖放内容";
            secondary_text = L"请只拖入一个现有的 .lnk 文件";
        } else if (state.visual_state == ShortcutDropVisualState::success) {
            state_border = accent.Get(); primary = L"导入成功";
            secondary_text = L"正在返回管理员设置窗口";
        }
        const D2D1_ROUNDED_RECT card_bounds{D2D1::RectF(16, 60, 424, 174), 8, 8};
        state.render_target->FillRoundedRectangle(card_bounds, card.Get());
        state.render_target->DrawRoundedRectangle(card_bounds, state_border,
            state.visual_state == ShortcutDropVisualState::idle ? 1.0f : 1.7f);

        const float arrow_y = 92.0f;
        state.render_target->DrawLine(D2D1::Point2F(52, arrow_y - 12),
            D2D1::Point2F(52, arrow_y + 8), state_border, 2.0f);
        state.render_target->DrawLine(D2D1::Point2F(44, arrow_y),
            D2D1::Point2F(52, arrow_y + 8), state_border, 2.0f);
        state.render_target->DrawLine(D2D1::Point2F(60, arrow_y),
            D2D1::Point2F(52, arrow_y + 8), state_border, 2.0f);
        state.render_target->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(39, 104, 65, 122), 4, 4), state_border, 1.5f);
        draw_text(state.render_target.Get(), state.body_format.Get(), text.Get(), primary,
            D2D1::RectF(78, 76, 404, 116));
        draw_text(state.render_target.Get(), state.small_format.Get(), secondary.Get(), secondary_text,
            D2D1::RectF(78, 115, 404, 153));

        const HRESULT end = state.render_target->EndDraw();
        if (end == D2DERR_RECREATE_TARGET) state.render_target.Reset();
        if (HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101),
                IMAGE_ICON, px(20, state.dpi), px(20, state.dpi), LR_DEFAULTCOLOR))) {
            DrawIconEx(paint.hdc, px(18, state.dpi), px(12, state.dpi), icon,
                px(20, state.dpi), px(20, state.dpi), 0, nullptr, DI_NORMAL);
            DestroyIcon(icon);
        }
    }
    EndPaint(state.window, &paint);
}

bool close_hit(const State& state, LPARAM position) {
    return GET_X_LPARAM(position) >= px(396, state.dpi)
        && GET_Y_LPARAM(position) < px(44, state.dpi);
}

LRESULT CALLBACK helper_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        state->window = window;
        state->dpi = GetDpiForWindow(window);
        if (state->dpi == 0) state->dpi = GetDpiForSystem();
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_CREATE: {
        state->dpi = std::max(96u, GetDpiForWindow(window));
        HRESULT drop_error = S_OK;
        if (!state->drop_target.register_window(window, {
                {},
                [state](ShortcutDropVisualState visual) {
                    state->visual_state = visual;
                    InvalidateRect(state->window, nullptr, FALSE);
                    SMK_DIAGNOSTIC_EVENT("shortcut_drop.state", std::format(
                        L"state={}", static_cast<int>(visual)));
                },
                [state](const std::wstring& path) {
                    if (write_result(state->id, path)) {
                        state->completed = true;
                        state->visual_state = ShortcutDropVisualState::success;
                        InvalidateRect(state->window, nullptr, FALSE);
                        SetTimer(state->window, kSuccessTimer, 360, nullptr);
                        SMK_DIAGNOSTIC_EVENT("shortcut_drop.result", L"result=success extension=.lnk");
                    } else {
                        state->visual_state = ShortcutDropVisualState::reject;
                        InvalidateRect(state->window, nullptr, FALSE);
                        SMK_DIAGNOSTIC_EVENT("shortcut_drop.result", std::format(
                            L"result=write_failed error={}", GetLastError()));
                    }
                },
            }, &drop_error)) return -1;
        SetTimer(window, kTimeoutTimer, kHandoffTimeoutMs, nullptr);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint_helper(*state); return 0;
    case WM_MOUSEMOVE: {
        if (!state->tracking_mouse) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            state->tracking_mouse = true;
        }
        const bool hovered = close_hit(*state, lparam);
        if (hovered != state->close_hovered) {
            state->close_hovered = hovered;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        state->tracking_mouse = false;
        state->close_hovered = false;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        if (close_hit(*state, lparam)) DestroyWindow(window);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) DestroyWindow(window);
        return 0;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(window, &point);
        if (point.y < px(44, state->dpi) && point.x < px(396, state->dpi)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_DPICHANGED: {
        state->dpi = HIWORD(wparam);
        const auto suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        state->render_target.Reset();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_TIMER:
        if (wparam == kSuccessTimer || wparam == kTimeoutTimer) DestroyWindow(window);
        return 0;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
        state->drop_target.reset();
        if (!state->completed) (void)write_result(state->id, L"");
        PostQuitMessage(0);
        return 0;
    default: return DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

int run_shortcut_drop_helper(HINSTANCE instance, const std::wstring& id) {
    if (!valid_id(id)) return 2;
    State state{id};
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = helper_proc;
    window_class.lpszClassName = kClassName;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    window_class.hIconSm = window_class.hIcon;
    RegisterClassExW(&window_class);

    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    UINT dpi_x = 96, dpi_y = 96;
    using GetDpiForMonitorPointer = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (const HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        if (const auto get_dpi = reinterpret_cast<GetDpiForMonitorPointer>(
                GetProcAddress(shcore, "GetDpiForMonitor")))
            (void)get_dpi(monitor, 0, &dpi_x, &dpi_y);
        FreeLibrary(shcore);
    }
    MONITORINFO monitor_info{sizeof(monitor_info)};
    GetMonitorInfoW(monitor, &monitor_info);
    const int width = px(440, dpi_x), height = px(190, dpi_y);
    const int x = monitor_info.rcWork.left + (monitor_info.rcWork.right - monitor_info.rcWork.left - width) / 2;
    const int y = monitor_info.rcWork.top + (monitor_info.rcWork.bottom - monitor_info.rcWork.top - height) / 2;
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_APPWINDOW, kClassName,
        L"超级中键 - 快捷方式拖放", WS_POPUP, x, y, width, height,
        nullptr, nullptr, instance, &state);
    if (!window) return 3;
    BOOL dark = TRUE;
    (void)DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    int rounded = 2;
    (void)DwmSetWindowAttribute(window, 33, &rounded, sizeof(rounded));
    ShowWindow(window, SW_SHOWNORMAL);
    SetForegroundWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

bool launch_shortcut_drop_helper(const std::wstring& id, std::wstring& error) {
    delete_shortcut_drop_result(id);
    return start_unelevated({L"--shortcut-drop-helper", id}, error);
}

std::optional<std::wstring> read_shortcut_drop_result(const std::wstring& id) {
    const auto path = result_path(id);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    std::wstring result(static_cast<std::size_t>(size.QuadPart / sizeof(wchar_t)), L'\0');
    DWORD read = 0;
    ReadFile(file, result.data(), static_cast<DWORD>(result.size() * sizeof(wchar_t)), &read, nullptr);
    CloseHandle(file);
    result.resize(read / sizeof(wchar_t));
    return result;
}

void delete_shortcut_drop_result(const std::wstring& id) noexcept {
    DeleteFileW(result_path(id).c_str());
}

} // namespace smk::windows
