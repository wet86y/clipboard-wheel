#include "platform/windows/TrayIcon.h"

#include "platform/windows/DiagnosticLog.h"

#include <objidl.h>
#include <gdiplus.h>

#include <format>

namespace smk::windows {
namespace {
constexpr wchar_t kClassName[] = L"SuperMiddleKeyNativeTrayHost";
constexpr UINT kToggle = 1001;
constexpr UINT kSettings = 1002;
constexpr UINT kClearHistory = 1003;
constexpr UINT kExit = 1004;
constexpr UINT_PTR kMaintenanceTimer = 1;
constexpr UINT kMaintenanceMs = 2000;

HICON build_wheel_icon(Gdiplus::Color fill, Gdiplus::Color center) {
    Gdiplus::Bitmap bitmap(32, 32, PixelFormat32bppARGB);
    Gdiplus::Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
    Gdiplus::SolidBrush outer(fill);
    Gdiplus::SolidBrush dot(center);
    graphics.FillEllipse(&outer, 2, 2, 28, 28);
    graphics.FillEllipse(&dot, 12, 12, 8, 8);
    HICON icon = nullptr;
    return bitmap.GetHICON(&icon) == Gdiplus::Ok ? icon : nullptr;
}
}

TrayIcon::~TrayIcon() { destroy(); }

bool TrayIcon::create(HINSTANCE instance, Callbacks callbacks) {
    instance_ = instance;
    callbacks_ = std::move(callbacks);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = instance;
    wc.lpfnWndProc = window_proc;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    // 必须使用隐藏顶层窗口；HWND_MESSAGE（仅消息窗口）收不到任务栏重建广播。
    taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (!taskbar_created_message_) return false;
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!window_) return false;
    Gdiplus::GdiplusStartupInput startup_input;
    if (Gdiplus::GdiplusStartup(&gdiplus_token_, &startup_input, nullptr) == Gdiplus::Ok) {
        enabled_icon_ = build_wheel_icon(
            Gdiplus::Color(0xFF, 0x3F, 0x6A, 0xFF), Gdiplus::Color(0xFF, 0xFF, 0xFF, 0xFF));
        disabled_icon_ = build_wheel_icon(
            Gdiplus::Color(0xFF, 0x80, 0x80, 0x80), Gdiplus::Color(0xFF, 0xD3, 0xD3, 0xD3));
    }
    if (!enabled_icon_) enabled_icon_ = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    if (!disabled_icon_ && enabled_icon_) disabled_icon_ = CopyIcon(enabled_icon_);
    icon_.cbSize = sizeof(icon_);
    icon_.hWnd = window_;
    icon_.uID = 1;
    icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon_.uCallbackMessage = kCallbackMessage;
    icon_.hIcon = enabled_icon_;
    wcscpy_s(icon_.szTip, L"超级中键");
    if (!enabled_icon_) return false;
    // 自启时 Explorer（资源管理器）可能尚未就绪，保留窗口并后台重试注册。
    if (!SetTimer(window_, kMaintenanceTimer, kMaintenanceMs, nullptr)) return false;
    refresh_icon();
    return true;
}

void TrayIcon::destroy() {
    if (window_) {
        KillTimer(window_, kMaintenanceTimer);
        (void)send_icon_message(NIM_DELETE);
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (enabled_icon_) DestroyIcon(enabled_icon_);
    if (disabled_icon_) DestroyIcon(disabled_icon_);
    enabled_icon_ = disabled_icon_ = nullptr;
    if (gdiplus_token_) Gdiplus::GdiplusShutdown(gdiplus_token_);
    gdiplus_token_ = 0;
    registered_ = false;
}

void TrayIcon::set_enabled(bool enabled) {
    enabled_ = enabled;
    refresh_icon();
}

void TrayIcon::set_capture_available(bool available, bool notify) {
    capture_available_ = available;
    refresh_icon(notify);
}

bool TrayIcon::send_icon_message(DWORD message) {
    return notify_icon_ ? notify_icon_(message, &icon_) : Shell_NotifyIconW(message, &icon_) != FALSE;
}

void TrayIcon::refresh_icon(bool notify) {
    if (!window_) return;
    icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon_.hIcon = enabled_ && capture_available_ ? enabled_icon_ : disabled_icon_;
    wcscpy_s(icon_.szTip, !enabled_ ? L"超级中键（已暂停）"
        : capture_available_ ? L"超级中键" : L"超级中键（中键捕获暂不可用）");
    if (notify) {
        icon_.uFlags |= NIF_INFO;
        wcscpy_s(icon_.szInfoTitle, L"超级中键");
        wcscpy_s(icon_.szInfo, L"中键捕获暂不可用，程序将在后台自动重试。其他功能不受影响。");
        icon_.dwInfoFlags = NIIF_WARNING;
    }
    if (registered_ && send_icon_message(NIM_MODIFY)) {
        icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        return;
    }
    registered_ = send_icon_message(NIM_ADD);
    // 某些任务栏重建通知也可能保留现有条目；修改成功同样视为已恢复。
    if (!registered_) registered_ = send_icon_message(NIM_MODIFY);
    if (registered_) {
        icon_.uVersion = NOTIFYICON_VERSION_4;
        [[maybe_unused]] const bool version_set = send_icon_message(NIM_SETVERSION);
        SMK_DIAGNOSTIC_EVENT("tray.icon.registered", std::format(L"version_set={}", version_set));
    } else {
        SMK_DIAGNOSTIC_EVENT("tray.icon.retry", L"registered=false retry_ms=2000");
    }
    icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}

LRESULT CALLBACK TrayIcon::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<TrayIcon*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle_message(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT TrayIcon::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (taskbar_created_message_ && message == taskbar_created_message_) {
        registered_ = false;
        refresh_icon();
        return 0;
    }
    if (message == WM_TIMER && wparam == kMaintenanceTimer) {
        refresh_icon();
        return 0;
    }
    if (message == kCallbackMessage) {
        if (LOWORD(lparam) == WM_LBUTTONUP && callbacks_.toggle) callbacks_.toggle();
        if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_CONTEXTMENU) show_menu();
        return 0;
    }
    if (message == WM_COMMAND) {
        switch (LOWORD(wparam)) {
        case kToggle: if (callbacks_.toggle) callbacks_.toggle(); break;
        case kSettings: if (callbacks_.settings) callbacks_.settings(); break;
        case kClearHistory: if (callbacks_.clear_history) callbacks_.clear_history(); break;
        case kExit: if (callbacks_.exit) callbacks_.exit(); break;
        default: break;
        }
        return 0;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

void TrayIcon::show_menu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (enabled_ ? MF_CHECKED : MF_UNCHECKED), kToggle, L"启用中键轮盘");
    AppendMenuW(menu, MF_STRING, kSettings, L"设置");
    AppendMenuW(menu, MF_STRING, kClearHistory, L"清空历史");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExit, L"退出");
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenu(menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        point.x, point.y, 0, window_, nullptr);
    DestroyMenu(menu);
    SMK_DIAGNOSTIC_EVENT("tray.menu.closed", std::format(L"command={}", command));
    // TrackPopupMenu owns a nested message loop. Opening the settings window
    // from the menu's synchronous WM_COMMAND can validate child paint regions
    // before that loop unwinds, leaving owner-drawn controls blank. Queue the
    // command only after the menu has closed so the regular app loop owns the
    // complete first frame.
    if (command != 0)
        PostMessageW(window_, WM_COMMAND, MAKEWPARAM(command, 0), 0);
}

} // namespace smk::windows
