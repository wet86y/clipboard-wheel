#include "app/AppHost.h"
#include "Version.h"

#include "platform/windows/AutoStart.h"
#include "platform/windows/CrashHandler.h"
#include "platform/windows/ElevationService.h"
#include "platform/windows/StartupTrace.h"
#include "platform/windows/ShortcutDropHelper.h"
#include "updater/UpdatePolicy.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <wtsapi32.h>

namespace smk::app {
namespace {

bool has_argument(const std::vector<std::wstring>& arguments, const wchar_t* expected) {
    return std::any_of(arguments.begin(), arguments.end(), [&](const std::wstring& value) {
        return _wcsicmp(value.c_str(), expected) == 0;
    });
}

std::optional<std::wstring> argument_value(const std::vector<std::wstring>& arguments, const wchar_t* expected) {
    for (std::size_t index = 0; index + 1 < arguments.size(); ++index)
        if (_wcsicmp(arguments[index].c_str(), expected) == 0) return arguments[index + 1];
    return {};
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(0, count)), '\0');
    if (count) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

} // namespace

constexpr wchar_t kInputSafetyClass[] = L"SuperMiddleKeyNativeInputSafety";
constexpr UINT_PTR kInputHeartbeatTimer = 1;
constexpr UINT kInputHeartbeatMs = 250;

AppHost::~AppHost() { shutdown(); }

int AppHost::run(HINSTANCE instance, const std::vector<std::wstring>& arguments) {
    for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
        if (_wcsicmp(arguments[index].c_str(), L"--shortcut-drop-helper") == 0)
            return smk::windows::run_shortcut_drop_helper(instance, arguments[index + 1]);
    }
    if (has_argument(arguments, L"--verify-release")) return verify_release_bundle(instance);
#if !defined(SMK_DIAGNOSTICS)
    if (has_argument(arguments, L"--update-test-repository")) return 4;
#endif
    if (!initialize(instance, arguments)) return 1;
    (void)write_update_health_marker(arguments);
    MSG message{};
    BOOL get_message = FALSE;
    while ((get_message = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (get_message == -1)
        smk::windows::startup_trace(L"main GetMessage failed error=" + std::to_wstring(GetLastError()));
    shutdown();
    return get_message == -1 ? 2 : static_cast<int>(message.wParam);
}

bool AppHost::initialize(HINSTANCE instance, const std::vector<std::wstring>& arguments) {
    smk::windows::crash_set_phase(L"app_initialize");
    smk::windows::startup_trace(L"initialize begin");
    instance_ = instance;
    const bool takeover = has_argument(arguments, L"--admin-restart");
    if (!single_instance_.acquire(L"Local\\SuperMiddleKey.SingleInstance", takeover)) {
        smk::windows::startup_trace(L"single instance rejected");
        return false;
    }
    smk::windows::startup_trace(L"single instance acquired");

    settings_ = settings_store_.load();
    smk::windows::startup_trace(L"settings loaded");
    enabled_ = settings_.mouse.middle_button_capture_enabled;
    if (!wheel_.create(instance)) {
        smk::windows::startup_trace(L"wheel create failed HRESULT=" + std::to_wstring(static_cast<unsigned long>(wheel_.creation_error())));
        return false;
    }
    smk::windows::startup_trace(L"wheel created");
    wheel_.set_failure_callback([this] {
        if (mouse_hook_) mouse_hook_->cancel_session(mouse_hook_->generation());
        cancel_wheel_no_action();
    });
    std::string update_repository = "wet86y/clipboard-wheel";
#if defined(SMK_DIAGNOSTICS)
    if (const auto test_repository = argument_value(arguments, L"--update-test-repository"))
        update_repository = utf8(*test_repository);
#endif
    updater_ = std::make_unique<smk::updater::NativeUpdateCoordinator>(instance, executable_path(),
        update_repository, desktop_update_kit::Version{kVersionMajor, kVersionMinor, kVersionPatch},
        settings_.update.use_acceleration_nodes,
        smk::updater::production_update_ui_enabled,
        [] { PostQuitMessage(0); });
    if (!settings_window_.create(instance,
            [this](const smk::core::AppSettings& settings) { return save_settings(settings); },
            updater_.get(), std::wstring(kVersionText) + L"（原生迁移内部构建）")) {
        smk::windows::startup_trace(L"settings window create failed");
        return false;
    }
    smk::windows::startup_trace(L"settings window created");
    if (!create_input_safety_window()) {
        smk::windows::startup_trace(L"input safety window create failed");
        return false;
    }

    clipboard_ = std::make_unique<smk::windows::ClipboardService>(history_, [] {}, settings_.clipboard.capture_images);
    if (!clipboard_->start(instance)) {
        smk::windows::startup_trace(L"clipboard listener start failed");
        return false;
    }
    smk::windows::startup_trace(L"clipboard listener started");
    clipboard_->capture_current();
    smk::windows::startup_trace(L"initial clipboard captured");
    windows_history_ = std::make_unique<smk::windows::WindowsClipboardHistory>(history_, [] {});
    if (settings_.clipboard.load_windows_clipboard_history_on_startup) {
        (void)windows_history_->start(instance, static_cast<std::size_t>(settings_.clipboard.max_history_items), settings_.clipboard.capture_images);
    }
    smk::windows::startup_trace(L"WinRT history scheduled");
    if (!extended_actions_.start()) {
        smk::windows::startup_trace(L"extended action executor start failed");
        return false;
    }

    mouse_hook_ = std::make_unique<smk::windows::MouseHook>(input_window_);
    mouse_hook_->set_enabled(enabled_);
    if (!mouse_hook_->start()) {
        smk::windows::startup_trace(L"mouse hook thread start failed; capture unavailable");
        mouse_hook_.reset();
    } else {
        mouse_hook_->heartbeat();
        smk::windows::startup_trace(L"mouse hook thread started");
    }

    smk::windows::TrayIcon::Callbacks tray_callbacks;
    tray_callbacks.toggle = [this] { toggle_enabled(); };
    tray_callbacks.settings = [this] { show_settings(); };
    tray_callbacks.clear_history = [this] { history_.clear(); };
    tray_callbacks.exit = [] { PostQuitMessage(0); };
    if (!tray_.create(instance, std::move(tray_callbacks))) {
        smk::windows::startup_trace(L"tray create failed");
        return false;
    }
    tray_.set_enabled(enabled_);
    smk::windows::startup_trace(L"initialize complete");
    smk::windows::crash_set_phase(L"app_running");
    return true;
}

void AppHost::shutdown() {
    if (shutting_down_) return;
    shutting_down_ = true;
    smk::windows::crash_set_phase(L"app_shutdown");
    smk::windows::startup_trace(L"shutdown begin");
    cancel_wheel_no_action();
    if (mouse_hook_) {
        mouse_hook_->set_enabled(false);
        if (!mouse_hook_->stop(1500)) {
            smk::windows::startup_trace(L"emergency: mouse hook thread did not stop within 1500ms");
            smk::windows::crash_set_phase(L"hook_shutdown_timeout");
            smk::windows::crash_write_emergency(0xE0000004);
            TerminateProcess(GetCurrentProcess(), 0xE001);
        }
        mouse_hook_.reset();
    }
    destroy_input_safety_window();
    extended_actions_.shutdown();
    if (updater_) { updater_->shutdown(); updater_.reset(); }
    windows_history_.reset();
    clipboard_.reset();
    tray_.destroy();
    smk::windows::startup_trace(L"shutdown complete");
}

bool AppHost::create_input_safety_window() {
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.hInstance = instance_;
    window_class.lpfnWndProc = input_window_proc;
    window_class.lpszClassName = kInputSafetyClass;
    RegisterClassExW(&window_class);
    input_window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kInputSafetyClass, L"", WS_POPUP, 0, 0, 0, 0,
        nullptr, nullptr, instance_, this);
    if (!input_window_) return false;
    (void)WTSRegisterSessionNotification(input_window_, NOTIFY_FOR_THIS_SESSION);
    return SetTimer(input_window_, kInputHeartbeatTimer, kInputHeartbeatMs, nullptr) != 0;
}

void AppHost::destroy_input_safety_window() noexcept {
    if (!input_window_) return;
    KillTimer(input_window_, kInputHeartbeatTimer);
    WTSUnRegisterSessionNotification(input_window_);
    DestroyWindow(input_window_);
    input_window_ = nullptr;
}

LRESULT CALLBACK AppHost::input_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<AppHost*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<AppHost*>(create->lpCreateParams);
        self->input_window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handle_input_message(message, wparam, lparam)
        : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT AppHost::handle_input_message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == smk::windows::MouseHook::kEventMessage && mouse_hook_) {
        const auto event = static_cast<smk::windows::MouseHookEvent>(wparam);
        const auto generation = static_cast<std::uint32_t>(lparam);
        if (event == smk::windows::MouseHookEvent::availability_changed) {
            const bool available = mouse_hook_->available();
            tray_.set_capture_available(available, !available && !hook_unavailable_notified_);
            hook_unavailable_notified_ = hook_unavailable_notified_ || !available;
            if (available) hook_unavailable_notified_ = false;
            return 0;
        }
        if (generation != mouse_hook_->generation()) {
            if (event == smk::windows::MouseHookEvent::mouse_move)
                mouse_hook_->complete_move(generation);
            return 0;
        }
        const POINT point = mouse_hook_->event_point(event);
        switch (event) {
        case smk::windows::MouseHookEvent::middle_down: {
            if (!mouse_hook_->session_active()) break;
            const bool shown = show_wheel(point);
            mouse_hook_->acknowledge_show(generation, shown);
            if (!shown) cancel_wheel_no_action();
            break;
        }
        case smk::windows::MouseHookEvent::mouse_move:
            if (mouse_hook_->session_active()) update_wheel(point);
            mouse_hook_->complete_move(generation);
            break;
        case smk::windows::MouseHookEvent::middle_up:
            finish_wheel(point);
            break;
        case smk::windows::MouseHookEvent::right_down:
            if (mouse_hook_->session_active()) toggle_selected_lock();
            break;
        case smk::windows::MouseHookEvent::cancel:
            cancel_wheel_no_action();
            break;
        default: break;
        }
        return 0;
    }
    if (message == WM_TIMER && wparam == kInputHeartbeatTimer) {
        if (mouse_hook_) mouse_hook_->heartbeat();
        return 0;
    }
    if (message == WM_WTSSESSION_CHANGE) {
        switch (wparam) {
        case WTS_SESSION_LOCK:
        case WTS_SESSION_LOGOFF:
        case WTS_CONSOLE_DISCONNECT:
        case WTS_REMOTE_DISCONNECT:
            cancel_wheel_no_action();
            if (mouse_hook_) mouse_hook_->suspend();
            break;
        case WTS_SESSION_UNLOCK:
        case WTS_SESSION_LOGON:
        case WTS_CONSOLE_CONNECT:
        case WTS_REMOTE_CONNECT:
            if (mouse_hook_) mouse_hook_->resume();
            break;
        default: break;
        }
        return 0;
    }
    if (message == WM_POWERBROADCAST) {
        if (wparam == PBT_APMSUSPEND) {
            cancel_wheel_no_action();
            if (mouse_hook_) mouse_hook_->suspend();
        } else if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
            if (mouse_hook_) mouse_hook_->resume();
        }
        return TRUE;
    }
    if (message == WM_DISPLAYCHANGE) {
        cancel_wheel_no_action();
        if (mouse_hook_) { mouse_hook_->suspend(); mouse_hook_->resume(); }
        return 0;
    }
    if (message == WM_QUERYENDSESSION) {
        cancel_wheel_no_action();
        if (mouse_hook_) mouse_hook_->suspend();
        return TRUE;
    }
    if (message == WM_ENDSESSION && wparam) {
        cancel_wheel_no_action();
        if (mouse_hook_) mouse_hook_->suspend();
        return 0;
    }
    return DefWindowProcW(input_window_, message, wparam, lparam);
}

bool AppHost::show_wheel(POINT point) {
    if (!enabled_) return false;
    lock_changed_ = false;
    const auto capacity = static_cast<std::size_t>(settings_.wheel.sector_count - (settings_.wheel.quick_copy ? 1 : 0));
    const auto entries = history_.snapshot_for_wheel(capacity);
    return wheel_.show(point, smk::core::build_wheel_slots(entries, settings_.wheel.sector_count,
        settings_.wheel.quick_copy), settings_);
}

void AppHost::update_wheel(POINT point) { wheel_.update_pointer(point); }

void AppHost::finish_wheel(POINT point) {
    if (!wheel_.visible()) return;
    wheel_.update_pointer(point);
    const auto selection = wheel_.selection();
    wheel_.hide();
    if (lock_changed_) return;
    if (const auto* action = std::get_if<smk::core::ExtendedWheelActionSlot>(&selection)) {
        extended_actions_.enqueue(*action);
        return;
    }
    const auto* entry = std::get_if<smk::core::ClipboardEntry>(&selection);
    if (!entry) return;
    if (entry->is_quick_copy) send_ctrl_c();
    else (void)clipboard_->paste_entry(*entry, smk::core::PasteMode::smart);
}

void AppHost::cancel_wheel_no_action() {
    lock_changed_ = false;
    wheel_.hide_immediately();
}

void AppHost::toggle_selected_lock() {
    const auto* selected = wheel_.selected_entry();
    if (!selected || selected->is_quick_copy) return;
    const bool locked = !selected->is_locked;
    lock_changed_ = history_.toggle_lock(selected->id);
    if (lock_changed_) (void)wheel_.refresh_selected_lock(locked);
}

void AppHost::toggle_enabled() {
    enabled_ = !enabled_;
    settings_.mouse.middle_button_capture_enabled = enabled_;
    if (mouse_hook_) {
        mouse_hook_->set_enabled(enabled_);
        if (enabled_) mouse_hook_->retry_now();
    }
    tray_.set_enabled(enabled_);
    std::wstring ignored;
    (void)settings_store_.save(settings_, ignored);
}

void AppHost::show_settings() { settings_window_.show(settings_); }

bool AppHost::save_settings(const smk::core::AppSettings& settings) {
    const auto previous = settings_;
    settings_ = settings;
    std::wstring error;
    if (!settings_store_.save(settings_, error)) {
        MessageBoxW(nullptr, error.c_str(), L"超级中键", MB_OK | MB_ICONERROR);
        settings_ = previous;
        return false;
    }
    (void)smk::windows::apply_auto_start(settings_.auto_start_enabled, executable_path(), error);
    enabled_ = settings_.mouse.middle_button_capture_enabled;
    if (mouse_hook_) {
        mouse_hook_->set_enabled(enabled_);
        if (enabled_) mouse_hook_->retry_now();
    }
    clipboard_->set_capture_images(settings_.clipboard.capture_images);
    if (updater_) updater_->set_acceleration(settings_.update.use_acceleration_nodes);
    tray_.set_enabled(enabled_);
    if (settings_.run_as_administrator_enabled != smk::windows::is_administrator()) {
        if (!smk::windows::restart_with_privilege(settings_.run_as_administrator_enabled, error)) {
            settings_.run_as_administrator_enabled = previous.run_as_administrator_enabled;
            std::wstring ignored;
            (void)settings_store_.save(settings_, ignored);
            MessageBoxW(nullptr, error.c_str(), L"超级中键", MB_OK | MB_ICONWARNING);
            return false;
        }
        cancel_wheel_no_action();
        if (mouse_hook_) mouse_hook_->suspend();
        PostQuitMessage(0);
    }
    return true;
}

std::wstring AppHost::executable_path() {
    std::wstring path(32'768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(count);
    return path;
}

void AppHost::send_ctrl_c() {
    INPUT inputs[4]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'C';
    inputs[2] = inputs[1];
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3] = inputs[0];
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, inputs, sizeof(INPUT));
}

bool AppHost::write_update_health_marker(const std::vector<std::wstring>& arguments) {
    const auto marker = argument_value(arguments, L"--update-health");
    if (!marker) return false;
    try {
        const std::filesystem::path path(*marker);
        if (!path.is_absolute() || path.parent_path().empty()) return false;
        std::filesystem::create_directories(path.parent_path());
        const auto temporary = path.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId());
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        const auto text = utf8(kVersionText);
        DWORD written{};
        const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
        FlushFileBuffers(file);
        CloseHandle(file);
        if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str());
            return false;
        }
        return true;
    } catch (...) { return false; }
}

int AppHost::verify_release_bundle(HINSTANCE instance) {
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(201), RT_RCDATA);
    if (!resource) return 10;
    HGLOBAL loaded = LoadResource(instance, resource);
    const DWORD size = SizeofResource(instance, resource);
    const auto* bytes = static_cast<const std::byte*>(LockResource(loaded));
    if (!bytes || size < 128 || std::to_integer<unsigned char>(bytes[0]) != 'M' ||
        std::to_integer<unsigned char>(bytes[1]) != 'Z') return 11;
    std::uint32_t pe_offset{};
    std::memcpy(&pe_offset, bytes + 0x3c, sizeof(pe_offset));
    if (pe_offset > size - 24) return 12;
    std::uint32_t signature{};
    std::memcpy(&signature, bytes + pe_offset, sizeof(signature));
    std::uint16_t machine{}, sections{}, optional_size{};
    std::memcpy(&machine, bytes + pe_offset + 4, sizeof(machine));
    std::memcpy(&sections, bytes + pe_offset + 6, sizeof(sections));
    std::memcpy(&optional_size, bytes + pe_offset + 20, sizeof(optional_size));
    if (signature != 0x00004550 || machine != IMAGE_FILE_MACHINE_AMD64 || sections == 0 || sections > 96 || optional_size == 0) return 13;
    const std::uint64_t section_table = static_cast<std::uint64_t>(pe_offset) + 24 + optional_size;
    if (section_table + static_cast<std::uint64_t>(sections) * 40 > size) return 14;
    for (std::uint16_t index = 0; index < sections; ++index) {
        std::uint32_t raw_size{}, raw_offset{};
        const auto offset = section_table + static_cast<std::uint64_t>(index) * 40;
        std::memcpy(&raw_size, bytes + offset + 16, sizeof(raw_size));
        std::memcpy(&raw_offset, bytes + offset + 20, sizeof(raw_offset));
        if (raw_size && (!raw_offset || static_cast<std::uint64_t>(raw_offset) + raw_size > size)) return 15;
    }
    return 0;
}

} // namespace smk::app
