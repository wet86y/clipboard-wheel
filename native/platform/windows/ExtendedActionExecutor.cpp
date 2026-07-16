#include "platform/windows/ExtendedActionExecutor.h"

#include "platform/windows/DiagnosticLog.h"

#include <ole2.h>
#include <oaidl.h>
#include <ocidl.h>
#include <exdisp.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <format>
#include <set>
#include <unordered_map>
#include <utility>

namespace smk::windows {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
constexpr auto kLaunchCooldown = std::chrono::milliseconds(1200);
constexpr auto kUnmanagedLease = std::chrono::seconds(6);

std::wstring trim(std::wstring_view value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring_view::npos) return {};
    const auto last = value.find_last_not_of(L" \t\r\n");
    return std::wstring(value.substr(first, last - first + 1));
}

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::wstring file_name(const std::wstring& path) {
    return lower(std::filesystem::path(path).filename().wstring());
}

bool is_executable(const std::wstring& path) {
    const auto extension = lower(std::filesystem::path(path).extension().wstring());
    return extension == L".exe" || extension == L".com";
}

bool is_wps(const std::wstring& path) {
    const auto name = file_name(path);
    return name == L"wps.exe" || name == L"et.exe" || name == L"wpp.exe" || name == L"wpspdf.exe";
}

bool is_firefox(const std::wstring& path) {
    const auto name = file_name(path);
    return name == L"firefox.exe" || name == L"waterfox.exe";
}

bool is_browser(const std::wstring& path) {
    const auto name = file_name(path);
    return name == L"msedge.exe" || name == L"chrome.exe" || name == L"brave.exe"
        || name == L"opera.exe" || name == L"vivaldi.exe" || is_firefox(path);
}

std::wstring append_argument(std::wstring current, std::wstring_view argument) {
    if (!current.empty()) current.push_back(L' ');
    current.append(argument);
    return current;
}

std::wstring quote(std::wstring_view value) {
    std::wstring result = L"\"";
    unsigned slashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'\"') result.append(slashes * 2 + 1, L'\\');
        else result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(ch);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::wstring normalized_path(const std::wstring& value) {
    try {
        auto path = std::filesystem::weakly_canonical(value).wstring();
        while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
        return lower(std::move(path));
    } catch (...) {
        return lower(value);
    }
}

std::wstring window_executable(HWND window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid) return {};
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const bool ok = QueryFullProcessImageNameW(process, 0, path.data(), &length) != FALSE;
    CloseHandle(process);
    if (!ok) return {};
    path.resize(length);
    return normalized_path(path);
}

struct Profile {
    const wchar_t* window_class = nullptr;
    int wait_attempts = 20;
    const wchar_t* program_arguments = nullptr;
    const wchar_t* document_prefix = nullptr;
};

Profile profile_for(const std::wstring& executable) {
    const auto name = file_name(executable);
    if (name == L"excel.exe") return {L"XLMAIN", 80, L"/x", L"/x"};
    if (name == L"winword.exe") return {L"OpusApp", 60, L"/w", nullptr};
    if (name == L"powerpnt.exe") return {L"PPTFrameClass", 60, nullptr, nullptr};
    if (is_wps(executable)) return {nullptr, 50, nullptr, nullptr};
    return {};
}

bool matching_window(HWND window, const std::wstring& executable) {
    if (!IsWindowVisible(window) || window_executable(window) != normalized_path(executable)) return false;
    const auto profile = profile_for(executable);
    if (!profile.window_class) return true;
    wchar_t class_name[256]{};
    GetClassNameW(window, class_name, static_cast<int>(std::size(class_name)));
    return _wcsicmp(class_name, profile.window_class) == 0;
}

std::set<HWND> visible_windows(const std::wstring& executable) {
    struct Context { const std::wstring* executable; std::set<HWND>* windows; } context{&executable, nullptr};
    std::set<HWND> result;
    context.windows = &result;
    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        const auto* context = reinterpret_cast<Context*>(value);
        if (matching_window(window, *context->executable)) context->windows->insert(window);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return result;
}

bool bring_to_front(HWND window, bool preserve_state = false, bool force = false) {
    if (!window || !IsWindow(window)) return false;
    if (!preserve_state || IsIconic(window)) ShowWindow(window, SW_RESTORE);
    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(window);
    if (GetForegroundWindow() == window || !force) return GetForegroundWindow() == window;
    const DWORD current = GetCurrentThreadId();
    const HWND foreground = GetForegroundWindow();
    const DWORD foreground_thread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD target_thread = GetWindowThreadProcessId(window, nullptr);
    const bool attached_foreground = foreground_thread && foreground_thread != current
        && AttachThreadInput(current, foreground_thread, TRUE);
    const bool attached_target = target_thread && target_thread != current
        && AttachThreadInput(current, target_thread, TRUE);
    BringWindowToTop(window); SetForegroundWindow(window); SetFocus(window);
    if (attached_target) AttachThreadInput(current, target_thread, FALSE);
    if (attached_foreground) AttachThreadInput(current, foreground_thread, FALSE);
    return GetForegroundWindow() == window;
}

void apply_second_trigger(HWND window, const std::wstring& behavior) {
    if (!window || !IsWindow(window)) return;
    if (_wcsicmp(behavior.c_str(), L"close") == 0) PostMessageW(window, WM_CLOSE, 0, 0);
    else if (IsIconic(window)) bring_to_front(window);
    else ShowWindow(window, SW_MINIMIZE);
}

std::optional<std::wstring> associated_executable(const std::wstring& document) {
    std::wstring result(32768, L'\0');
    const HINSTANCE status = FindExecutableW(document.c_str(),
        std::filesystem::path(document).parent_path().c_str(), result.data());
    if (reinterpret_cast<INT_PTR>(status) <= 32) return std::nullopt;
    result.resize(std::wcslen(result.c_str()));
    return result.empty() ? std::nullopt : std::optional<std::wstring>{result};
}

struct StartedProcess {
    HANDLE handle = nullptr;
    DWORD pid = 0;
    std::wstring kind;
    bool succeeded = false;
};

StartedProcess create_process(const std::wstring& executable, const std::wstring& arguments,
    const std::wstring& working_directory, std::wstring kind) {
    std::wstring command = quote(executable);
    if (!arguments.empty()) command = append_argument(std::move(command), arguments);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
        nullptr, working_directory.empty() ? nullptr : working_directory.c_str(), &startup, &process);
    if (!started) return {};
    CloseHandle(process.hThread);
    return {process.hProcess, process.dwProcessId, std::move(kind), true};
}

StartedProcess shell_open(const std::wstring& target, const std::wstring& arguments = {},
    const std::wstring& working_directory = {}, std::wstring kind = L"shell") {
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = target.c_str();
    info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    info.lpDirectory = working_directory.empty() ? nullptr : working_directory.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) return {};
    return {info.hProcess, info.hProcess ? GetProcessId(info.hProcess) : 0, std::move(kind), true};
}

std::optional<HWND> find_folder_window(const std::wstring& folder) {
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(windows.GetAddressOf()))))
        return std::nullopt;
    long count = 0;
    if (FAILED(windows->get_Count(&count))) return std::nullopt;
    const auto target = normalized_path(folder);
    for (long index = 0; index < count; ++index) {
        VARIANT item{}; VariantInit(&item); item.vt = VT_I4; item.lVal = index;
        ComPtr<IDispatch> dispatch;
        if (FAILED(windows->Item(item, dispatch.GetAddressOf())) || !dispatch) continue;
        ComPtr<IWebBrowserApp> browser;
        if (FAILED(dispatch.As(&browser))) continue;
        BSTR url = nullptr;
        if (FAILED(browser->get_LocationURL(&url)) || !url) continue;
        std::wstring path(32768, L'\0');
        DWORD length = static_cast<DWORD>(path.size());
        const HRESULT converted = PathCreateFromUrlW(url, path.data(), &length, 0);
        SysFreeString(url);
        if (FAILED(converted)) continue;
        path.resize(length);
        if (normalized_path(path) != target) continue;
        SHANDLE_PTR handle = 0;
        if (SUCCEEDED(browser->get_HWND(&handle)) && handle) return reinterpret_cast<HWND>(handle);
    }
    return std::nullopt;
}

bool activate_wps_tab(HWND host, const std::wstring& document) {
    if (!host || document.empty()) return false;
    ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(automation.GetAddressOf())))) return false;
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(host, root.GetAddressOf())) || !root) return false;
    VARIANT value{}; value.vt = VT_I4; value.lVal = UIA_TabItemControlTypeId;
    ComPtr<IUIAutomationCondition> condition;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value,
            condition.GetAddressOf()))) return false;
    ComPtr<IUIAutomationElementArray> items;
    if (FAILED(root->FindAll(TreeScope_Descendants, condition.Get(), items.GetAddressOf())) || !items) return false;
    const auto name = lower(std::filesystem::path(document).filename().wstring());
    const auto stem = lower(std::filesystem::path(document).stem().wstring());
    int length = 0; items->get_Length(&length);
    for (int index = 0; index < length; ++index) {
        ComPtr<IUIAutomationElement> item;
        if (FAILED(items->GetElement(index, item.GetAddressOf())) || !item) continue;
        BSTR raw = nullptr;
        if (FAILED(item->get_CurrentName(&raw)) || !raw) continue;
        auto candidate = lower(trim(raw)); SysFreeString(raw);
        while (!candidate.empty() && (candidate.back() == L'*' || std::iswspace(candidate.back()))) candidate.pop_back();
        if (candidate != name && candidate != stem) continue;
        ComPtr<IUIAutomationSelectionItemPattern> selection;
        if (SUCCEEDED(item->GetCurrentPatternAs(UIA_SelectionItemPatternId,
                IID_PPV_ARGS(selection.GetAddressOf()))) && selection && SUCCEEDED(selection->Select())) {
            bring_to_front(host, true, true);
            return true;
        }
    }
    return false;
}

} // namespace

bool dispatch_extended_hotkey(
    const std::vector<WORD>& keys,
    const ExtendedKeyEventSender& sender) {
    if (keys.empty() || !sender) return false;
    std::vector<WORD> pressed;
    bool success = true;
    for (const WORD key : keys) {
        if (!sender(key, false)) {
            success = false;
            break;
        }
        pressed.push_back(key);
    }
    for (auto it = pressed.rbegin(); it != pressed.rend(); ++it)
        if (!sender(*it, true)) success = false;
    return success;
}

bool send_extended_hotkey(const std::vector<WORD>& keys) {
    return dispatch_extended_hotkey(keys, [](WORD key, bool key_up) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = key;
        input.ki.dwFlags = key_up ? KEYEVENTF_KEYUP : 0;
        return SendInput(1, &input, sizeof(input)) == 1;
    });
}

std::optional<std::wstring> normalize_browser_launch_url(std::wstring_view value) {
    auto result = trim(value);
    if (result.empty() || std::any_of(result.begin(), result.end(), [](wchar_t ch) { return std::iswspace(ch); }))
        return std::nullopt;
    const auto lowered = lower(result);
    if (lowered.find(L"://") != std::wstring::npos
        && !lowered.starts_with(L"http://") && !lowered.starts_with(L"https://")) return std::nullopt;
    if (!lowered.starts_with(L"http://") && !lowered.starts_with(L"https://")) result = L"https://" + result;
    const auto scheme = result.find(L"://");
    if (scheme == std::wstring::npos || result.find_first_of(L"/?#", scheme + 3) == scheme + 3
        || result.size() <= scheme + 3) return std::nullopt;
    return result;
}

std::optional<ShortcutLaunchInfo> resolve_shortcut(const std::wstring& shortcut_path) {
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(link.GetAddressOf())))) return std::nullopt;
    ComPtr<IPersistFile> persist;
    if (FAILED(link.As(&persist)) || FAILED(persist->Load(shortcut_path.c_str(), STGM_READ))) return std::nullopt;
    wchar_t target[32768]{}, arguments[32768]{}, working[32768]{};
    WIN32_FIND_DATAW data{};
    if (FAILED(link->GetPath(target, static_cast<int>(std::size(target)), &data, SLGP_RAWPATH)) || !target[0])
        return std::nullopt;
    (void)link->GetArguments(arguments, static_cast<int>(std::size(arguments)));
    (void)link->GetWorkingDirectory(working, static_cast<int>(std::size(working)));
    return ShortcutLaunchInfo{target, arguments, working};
}

std::wstring build_browser_launch_arguments(
    const ShortcutLaunchInfo& shortcut, std::wstring_view configured_url, bool& url_applied) {
    auto arguments = shortcut.arguments;
    url_applied = false;
    if (!is_browser(shortcut.target_path)) return arguments;
    const std::wstring switch_value = is_firefox(shortcut.target_path) ? L"-new-window" : L"--new-window";
    if (lower(arguments).find(lower(switch_value)) == std::wstring::npos)
        arguments = append_argument(std::move(arguments), switch_value);
    if (const auto url = normalize_browser_launch_url(configured_url)) {
        arguments = append_argument(std::move(arguments), quote(*url));
        url_applied = true;
    }
    return arguments;
}

struct ExtendedActionExecutor::Implementation {
    explicit Implementation(const std::atomic_bool& stop_requested) : stop_requested(stop_requested) {}

    const std::atomic_bool& stop_requested;
    struct Session {
        HANDLE process = nullptr;
        DWORD pid = 0;
        HWND window = nullptr;
        HWND wps_host = nullptr;
        std::wstring executable;
        std::wstring document;
        std::set<HWND> windows_before;
        Clock::time_point started = Clock::now();
        Clock::time_point unmanaged_until{};
        bool shared_wps_document = false;

        Session() = default;
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        Session(Session&& other) noexcept { *this = std::move(other); }
        Session& operator=(Session&& other) noexcept {
            if (this == &other) return *this;
            if (process) CloseHandle(process);
            process = std::exchange(other.process, nullptr);
            pid = other.pid; window = other.window; wps_host = other.wps_host;
            executable = std::move(other.executable); document = std::move(other.document);
            windows_before = std::move(other.windows_before); started = other.started;
            unmanaged_until = other.unmanaged_until; shared_wps_document = other.shared_wps_document;
            return *this;
        }
        ~Session() { if (process) CloseHandle(process); }
        [[nodiscard]] bool lease_active() const noexcept {
            return unmanaged_until != Clock::time_point{} && Clock::now() < unmanaged_until;
        }
    };

    std::unordered_map<std::wstring, std::vector<Session>> sessions;
    std::unordered_map<std::wstring, Clock::time_point> launch_attempts;

    static std::wstring tracking_key(const std::wstring& shortcut,
        const std::optional<ShortcutLaunchInfo>& info, const std::wstring& browser_url) {
        auto key = normalized_path(shortcut);
        if (info && is_browser(info->target_path)) {
            if (const auto url = normalize_browser_launch_url(browser_url)) key += L"\x1f" + lower(*url);
        }
        return key;
    }

    bool reserve_launch(const std::wstring& key) {
        const auto now = Clock::now();
        if (const auto found = launch_attempts.find(key); found != launch_attempts.end()
            && now - found->second < kLaunchCooldown) return false;
        launch_attempts[key] = now;
        return true;
    }

    void clear_launch_attempt(const std::wstring& key) { launch_attempts.erase(key); }

    HWND wait_for_new_window(const std::wstring& executable, const std::set<HWND>& before) {
        const int attempts = profile_for(executable).wait_attempts;
        for (int attempt = 0; attempt < attempts; ++attempt) {
            if (stop_requested.load(std::memory_order_relaxed)) return nullptr;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto current = visible_windows(executable);
            std::vector<HWND> added;
            std::set_difference(current.begin(), current.end(), before.begin(), before.end(),
                std::back_inserter(added));
            if (added.empty()) continue;
            const HWND foreground = GetForegroundWindow();
            return std::find(added.begin(), added.end(), foreground) != added.end() ? foreground : added.front();
        }
        return nullptr;
    }

    HWND session_window(Session& session) {
        if (session.window && IsWindow(session.window)) return session.window;
        if (session.shared_wps_document || session.executable.empty()) return nullptr;
        const auto current = visible_windows(session.executable);
        for (const HWND candidate : current) {
            if (!session.windows_before.contains(candidate)) {
                session.window = candidate;
                return candidate;
            }
        }
        return nullptr;
    }

    HWND wps_host(Session& session) {
        if (session.wps_host && IsWindow(session.wps_host)) return session.wps_host;
        if (session.document.empty() || !is_wps(session.executable)) return nullptr;
        const HWND foreground = GetForegroundWindow();
        if (foreground && matching_window(foreground, session.executable)) return session.wps_host = foreground;
        const auto current = visible_windows(session.executable);
        if (current.size() == 1) return session.wps_host = *current.begin();
        return nullptr;
    }

    void prune(std::vector<Session>& values) {
        std::erase_if(values, [&](Session& session) {
            return !session_window(session) && !wps_host(session) && !session.lease_active();
        });
    }

    bool handle_existing_session(const std::wstring& key, const std::wstring& behavior) {
        const auto found = sessions.find(key);
        if (found == sessions.end()) return false;
        auto& values = found->second;
        prune(values);
        if (values.empty()) { sessions.erase(found); return false; }
        std::sort(values.begin(), values.end(), [](const Session& left, const Session& right) {
            return left.started < right.started;
        });
        for (auto it = values.rbegin(); it != values.rend(); ++it) {
            if (const HWND window = session_window(*it)) {
                clear_launch_attempt(key);
                if (!IsWindowVisible(window) || IsIconic(window)) bring_to_front(window);
                else apply_second_trigger(window, behavior);
                SMK_DIAGNOSTIC_EVENT("extended.second_trigger", std::format(
                    L"pid={} behavior={} source=tracked", it->pid, behavior == L"close" ? L"close" : L"minimize"));
                return true;
            }
            if (const HWND host = wps_host(*it)) {
                clear_launch_attempt(key);
                bring_to_front(host, true, true);
                (void)activate_wps_tab(host, it->document);
                SMK_DIAGNOSTIC_EVENT("extended.second_trigger", std::format(
                    L"pid={} behavior=activate source=wps_host", it->pid));
                return true;
            }
            if (it->lease_active()) return true;
        }
        return false;
    }

    static std::wstring resolve_window_executable(const std::optional<ShortcutLaunchInfo>& info) {
        if (!info || info->target_path.empty()) return {};
        std::error_code error;
        if (!std::filesystem::exists(info->target_path, error)) return {};
        if (is_executable(info->target_path)) return info->target_path;
        return associated_executable(info->target_path).value_or(L"");
    }

    static StartedProcess start_target(const std::wstring& shortcut,
        const std::optional<ShortcutLaunchInfo>& info, const std::wstring& executable,
        const std::wstring& browser_url) {
        std::error_code error;
        if (!info || !std::filesystem::exists(info->target_path, error))
            return shell_open(shortcut, {}, {}, L"shortcut");
        if (!is_executable(info->target_path)) {
            const auto profile = profile_for(executable);
            if (profile.document_prefix && !executable.empty()) {
                auto arguments = append_argument(profile.document_prefix, quote(info->target_path));
                return create_process(executable, arguments,
                    std::filesystem::path(executable).parent_path().wstring(), L"isolated_document");
            }
            return shell_open(info->target_path, {}, info->working_directory, L"document_association");
        }

        bool browser_url_applied = false;
        auto arguments = build_browser_launch_arguments(*info, browser_url, browser_url_applied);
        const auto profile = profile_for(info->target_path);
        if (profile.program_arguments && lower(arguments).find(lower(profile.program_arguments)) == std::wstring::npos)
            arguments = append_argument(profile.program_arguments, arguments);
        auto working = info->working_directory.empty()
            ? std::filesystem::path(info->target_path).parent_path().wstring() : info->working_directory;
        return create_process(info->target_path, arguments, working,
            browser_url_applied ? L"browser_url" : L"executable");
    }

    void track(const std::wstring& key, StartedProcess started, const std::wstring& executable,
        std::set<HWND> before, const std::wstring& document) {
        HWND window = executable.empty() ? nullptr : wait_for_new_window(executable, before);
        if (window) {
            if (!bring_to_front(window)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
                (void)bring_to_front(window);
            }
        }
        HWND host = nullptr;
        const bool shared_wps = !window && !document.empty() && is_wps(executable);
        if (shared_wps) {
            const HWND foreground = GetForegroundWindow();
            if (foreground && matching_window(foreground, executable)) host = foreground;
        }
        Session session;
        session.process = started.handle; session.pid = started.pid; session.window = window;
        session.wps_host = host; session.executable = executable; session.document = document;
        session.windows_before = std::move(before); session.started = Clock::now();
        session.shared_wps_document = shared_wps;
        if (!window) session.unmanaged_until = Clock::now() + kUnmanagedLease;
        sessions[key].push_back(std::move(session));
        SMK_DIAGNOSTIC_EVENT("extended.launch", std::format(
            L"kind={} pid={} window_bound={} shared_wps={}", started.kind, started.pid,
            window != nullptr, shared_wps));
    }

    void execute_shortcut(const smk::core::ExtendedWheelActionSlot& action) {
        const auto info = resolve_shortcut(action.shortcut_path);
        const auto key = tracking_key(action.shortcut_path, info, action.browser_launch_url);
        if (info) {
            std::error_code error;
            if (std::filesystem::is_directory(info->target_path, error)) {
                if (const auto existing = find_folder_window(info->target_path)) {
                    apply_second_trigger(*existing, action.second_trigger_behavior);
                    clear_launch_attempt(key);
                    return;
                }
                sessions.erase(key);
                if (!reserve_launch(key)) return;
                auto started = shell_open(info->target_path, {}, {}, L"folder");
                if (started.handle) CloseHandle(started.handle);
                return;
            }
        }
        if (handle_existing_session(key, action.second_trigger_behavior)) return;
        if (!reserve_launch(key)) {
            SMK_DIAGNOSTIC_EVENT("extended.launch_throttled", std::format(L"slot={}", action.slot_index));
            return;
        }
        const auto executable = resolve_window_executable(info);
        const auto before = executable.empty() ? std::set<HWND>{} : visible_windows(executable);
        const auto document = info && !is_executable(info->target_path) ? info->target_path : L"";
        auto started = start_target(action.shortcut_path, info, executable, action.browser_launch_url);
        if (!started.succeeded) {
            clear_launch_attempt(key);
            SMK_DIAGNOSTIC_EVENT("extended.launch_failed", std::format(
                L"slot={} error={}", action.slot_index, GetLastError()));
            return;
        }
        track(key, std::move(started), executable, before, document);
    }

    void execute(const smk::core::ExtendedWheelActionSlot& action) {
        if (!action.configured()) return;
        if (_wcsicmp(action.mode.c_str(), L"hotkey") == 0) {
            const auto keys = parse_extended_hotkey(action.hotkey);
            [[maybe_unused]] const bool sent = send_extended_hotkey(keys);
            SMK_DIAGNOSTIC_EVENT("extended.hotkey", std::format(
                L"slot={} key_count={} success={}", action.slot_index, keys.size(), sent));
            return;
        }
        if (_wcsicmp(action.mode.c_str(), L"shortcut") == 0) execute_shortcut(action);
    }
};

ExtendedActionExecutor::ExtendedActionExecutor() = default;
ExtendedActionExecutor::~ExtendedActionExecutor() {
    if (worker_.joinable()) (void)shutdown(INFINITE);
    if (stopped_event_) CloseHandle(stopped_event_);
}

bool ExtendedActionExecutor::start() {
    std::scoped_lock lock(mutex_);
    if (worker_.joinable()) return true;
    stopping_ = false;
    stop_requested_.store(false, std::memory_order_relaxed);
    if (!stopped_event_) stopped_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopped_event_) return false;
    ResetEvent(stopped_event_);
    try {
        worker_ = std::thread([this] { worker_main(); });
        return true;
    } catch (...) {
        return false;
    }
}

void ExtendedActionExecutor::enqueue(smk::core::ExtendedWheelActionSlot action) {
    {
        std::scoped_lock lock(mutex_);
        if (stopping_ || !worker_.joinable()) return;
        queue_.push_back(std::move(action));
    }
    wake_.notify_one();
}

bool ExtendedActionExecutor::shutdown(DWORD timeout_ms) noexcept {
    {
        std::scoped_lock lock(mutex_);
        if (!worker_.joinable()) return true;
        stopping_ = true;
        stop_requested_.store(true, std::memory_order_relaxed);
        queue_.clear();
    }
    wake_.notify_one();
    if (WaitForSingleObject(stopped_event_, timeout_ms) != WAIT_OBJECT_0) return false;
    worker_.join();
    return true;
}

void ExtendedActionExecutor::worker_main() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    implementation_ = std::make_unique<Implementation>(stop_requested_);
    for (;;) {
        smk::core::ExtendedWheelActionSlot action;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) break;
            action = std::move(queue_.front());
            queue_.pop_front();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        implementation_->execute(action);
    }
    implementation_.reset();
    if (SUCCEEDED(initialized)) CoUninitialize();
    SetEvent(stopped_event_);
}

} // namespace smk::windows
