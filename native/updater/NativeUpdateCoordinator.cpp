#include "updater/NativeUpdateCoordinator.h"

#include "platform/windows/DiagnosticLog.h"

#include <format>
#include <limits>
#include <utility>

namespace smk::updater {
namespace {
constexpr int kUpdaterStubResource = 201;

std::vector<std::byte> load_updater_stub(HINSTANCE instance) {
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(kUpdaterStubResource), RT_RCDATA);
    if (!resource) return {};
    HGLOBAL loaded = LoadResource(instance, resource);
    const DWORD size = SizeofResource(instance, resource);
    const auto* bytes = static_cast<const std::byte*>(LockResource(loaded));
    return bytes && size ? std::vector<std::byte>(bytes, bytes + size) : std::vector<std::byte>{};
}

std::wstring widen_utf8(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(std::max(0, count)), L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::wstring format_bytes(double value) {
    constexpr const wchar_t* units[]{L"B", L"KiB", L"MiB", L"GiB"};
    std::size_t unit{};
    while (value >= 1024.0 && unit + 1 < std::size(units)) { value /= 1024.0; ++unit; }
    return std::format(L"{:.1f} {}", value, units[unit]);
}
}

std::optional<std::filesystem::path> canonical_executable_target(
    const std::filesystem::path& executable, const std::wstring& canonical_file_name) {
    if (!executable.is_absolute() || canonical_file_name.empty()) return {};
    const std::filesystem::path canonical(canonical_file_name);
    if (canonical.has_parent_path() || canonical.filename() != canonical
        || _wcsicmp(canonical.extension().c_str(), L".exe") != 0
        || _wcsicmp(executable.filename().c_str(), canonical_file_name.c_str()) == 0) return {};
    return executable.parent_path() / canonical;
}

ExecutableNameNormalizationResult normalize_executable_name(
    HINSTANCE instance, const std::filesystem::path& executable,
    const std::wstring& canonical_file_name, std::wstring& error) {
    try {
        const auto target = canonical_executable_target(executable, canonical_file_name);
        if (!target) return ExecutableNameNormalizationResult::unchanged;
        const auto stub = load_updater_stub(instance);
        if (stub.empty()) {
            error = L"内嵌更新替换组件不可用。";
            return ExecutableNameNormalizationResult::failed;
        }
        const auto result = desktop_update_kit::launch_rename(
            stub, executable, *target, static_cast<int>(GetCurrentProcessId()));
        if (!result.started) {
            error = widen_utf8(result.error);
            return ExecutableNameNormalizationResult::failed;
        }
        return ExecutableNameNormalizationResult::relaunch_started;
    } catch (const std::exception& exception) {
        error = widen_utf8(exception.what());
        return ExecutableNameNormalizationResult::failed;
    } catch (...) {
        error = L"无法恢复程序文件名。";
        return ExecutableNameNormalizationResult::failed;
    }
}

NativeUpdateCoordinator::NativeUpdateCoordinator(HINSTANCE instance, std::filesystem::path executable,
    std::string repository, desktop_update_kit::Version current_version, bool acceleration,
    bool ui_enabled, ExitCallback exit_callback)
    : instance_(instance), executable_(std::move(executable)), ui_enabled_(ui_enabled),
      exit_callback_(std::move(exit_callback)),
      client_(desktop_update_kit::ClientOptions{
          ui_enabled && repository != "wet86y/clipboard-wheel" ? L"clipboard-wheel-update-integration" : L"clipboard-wheel",
          std::move(repository), "super-middle-key.exe", "super-middle-key.exe.sha256", current_version}),
      session_(client_) {
    state_.state = ui_enabled_ ? UpdateState::idle : UpdateState::disabled;
    state_.acceleration = acceleration;
    state_.status = ui_enabled_ ? L"点击“检查更新”获取最新版本。" : L"原生更新能力验证中，正式 Release 暂未开放。";
    session_.set_changed_callback([this](const desktop_update_kit::SessionSnapshot& snapshot) {
        apply_snapshot(snapshot);
    });
}

NativeUpdateCoordinator::~NativeUpdateCoordinator() { (void)shutdown(INFINITE); }

void NativeUpdateCoordinator::set_observer(Observer observer) {
    UpdateViewState current;
    auto replacement = observer ? std::make_shared<ObserverSlot>() : nullptr;
    if (replacement) replacement->observer = std::move(observer);
    std::shared_ptr<ObserverSlot> previous;
    {
        std::scoped_lock lock(mutex_);
        previous = std::exchange(observer_slot_, replacement);
        current = state_;
    }
    deactivate_observer(previous);
    if (replacement) {
        std::scoped_lock lock(replacement->mutex);
        if (replacement->active && replacement->observer) replacement->observer(current);
    }
}

UpdateViewState NativeUpdateCoordinator::state() const { std::scoped_lock lock(mutex_); return state_; }

void NativeUpdateCoordinator::publish(UpdateViewState value) {
    std::shared_ptr<ObserverSlot> observer;
    UpdateViewState current;
    {
        std::scoped_lock lock(mutex_);
        if (shutting_down_) return;
        state_ = std::move(value);
        current = state_;
        observer = observer_slot_;
    }
    if (observer) {
        std::scoped_lock lock(observer->mutex);
        if (observer->active && observer->observer) {
            try {
                observer->observer(current);
            } catch (...) {
                // UI observers are isolated from update worker threads.
            }
        }
    }
}

void NativeUpdateCoordinator::deactivate_observer(
    const std::shared_ptr<ObserverSlot>& slot) noexcept {
    if (!slot) return;
    std::scoped_lock lock(slot->mutex);
    slot->active = false;
    slot->observer = {};
}

void NativeUpdateCoordinator::check() {
    if (!ui_enabled_) return;
    {
        std::scoped_lock lock(mutex_);
        if (shutting_down_ || state_.state == UpdateState::checking || state_.state == UpdateState::downloading ||
            state_.state == UpdateState::paused) return;
    }
    if (check_thread_.joinable()) check_thread_.join();
    auto checking = state();
    checking.state = UpdateState::checking;
    checking.status = L"正在检查更新…";
    SMK_DIAGNOSTIC_EVENT("update.check.started", L"channel=update");
    publish(checking);
    {
        std::scoped_lock lock(mutex_);
        check_finished_ = false;
    }
    check_thread_ = std::jthread([this](std::stop_token token) {
        try {
            auto release = client_.check_for_update(token);
            auto next = state();
            if (release) {
                {
                    std::scoped_lock lock(mutex_);
                    release_ = *release;
                }
                next.state = UpdateState::available;
                next.version = widen(desktop_update_kit::format_version(release->version));
                next.release_notes = widen(release->notes);
                next.status = L"发现新版本 " + next.version + L"。确认说明后可开始下载。";
                SMK_DIAGNOSTIC_EVENT("update.check.completed", L"result=available");
            } else {
                next.state = UpdateState::idle;
                next.status = L"当前已是最新版本。";
                SMK_DIAGNOSTIC_EVENT("update.check.completed", L"result=current");
            }
            publish(next);
        } catch (const std::exception& error) {
            SMK_DIAGNOSTIC_EVENT("update.check.failed", L"result=transport-or-contract-error");
            auto failed = state();
            failed.state = UpdateState::failed;
            failed.status = L"检查更新失败：" + widen(error.what());
            publish(failed);
        } catch (...) {
            SMK_DIAGNOSTIC_EVENT("update.check.failed", L"result=unknown-worker-exception");
            auto failed = state();
            failed.state = UpdateState::failed;
            failed.status = L"检查更新失败。";
            publish(failed);
        }
        {
            std::scoped_lock lock(mutex_);
            check_finished_ = true;
        }
        check_finished_signal_.notify_all();
    });
}

void NativeUpdateCoordinator::download_or_resume() {
    if (!ui_enabled_) return;
    const auto current = state();
    if (current.state == UpdateState::paused) { (void)session_.resume(); return; }
    if (current.state == UpdateState::completed) { (void)install(); return; }
    std::optional<desktop_update_kit::Release> release;
    { std::scoped_lock lock(mutex_); release = release_; }
    if (release) (void)session_.start(*release, current.acceleration);
}

void NativeUpdateCoordinator::pause() { if (ui_enabled_) (void)session_.pause(); }
void NativeUpdateCoordinator::continue_in_background() { if (ui_enabled_) (void)session_.continue_in_background(); }
void NativeUpdateCoordinator::cancel() { if (ui_enabled_) (void)session_.cancel(); }
void NativeUpdateCoordinator::set_acceleration(bool enabled) {
    auto next = state();
    next.acceleration = enabled;
    publish(next);
    (void)session_.set_acceleration(enabled);
}
void NativeUpdateCoordinator::next_node() { if (ui_enabled_) (void)session_.next_accelerated_node(); }

bool NativeUpdateCoordinator::install() {
    if (!ui_enabled_) return false;
    const auto snapshot = session_.snapshot();
    if (snapshot.state != desktop_update_kit::SessionState::completed || snapshot.downloaded_path.empty() ||
        !snapshot.release || snapshot.release->sha256.empty()) return false;
    auto launching = state();
    launching.state = UpdateState::launching;
    launching.status = L"更新助理已启动，程序即将退出…";
    publish(launching);
    const auto stub = updater_stub();
    const auto result = desktop_update_kit::launch_update(stub, snapshot.downloaded_path,
        std::filesystem::absolute(executable_), snapshot.release->sha256,
        static_cast<int>(GetCurrentProcessId()));
    if (!result.started) {
        SMK_DIAGNOSTIC_EVENT("update.install.failed", L"stage=launch-stub");
        auto failed = state();
        failed.state = UpdateState::failed;
        failed.status = L"无法启动更新助理：" + widen(result.error);
        publish(failed);
        return false;
    }
    {
        std::scoped_lock lock(mutex_);
        installation_started_ = true;
    }
    SMK_DIAGNOSTIC_EVENT("update.install.started", L"stage=stub-launched");
    if (exit_callback_) exit_callback_();
    return true;
}

void NativeUpdateCoordinator::settings_closed() {
    if (session_.snapshot().background) return;
    (void)session_.pause_when_ui_closes();
}

void NativeUpdateCoordinator::apply_snapshot(const desktop_update_kit::SessionSnapshot& snapshot) {
    auto next = state();
    next.background = snapshot.background;
    next.acceleration = snapshot.acceleration;
    if (snapshot.release) {
        next.version = widen(desktop_update_kit::format_version(snapshot.release->version));
        next.release_notes = widen(snapshot.release->notes);
    }
    if (snapshot.progress) {
        next.received = snapshot.progress->received;
        next.total = snapshot.progress->total;
        next.bytes_per_second = snapshot.progress->bytes_per_second;
        next.node = widen(snapshot.progress->node_id);
        next.connections = snapshot.progress->connections;
        next.parallel_fallback = snapshot.progress->parallel_fallback;
    }
    switch (snapshot.state) {
    case desktop_update_kit::SessionState::idle: next.state = UpdateState::idle; break;
    case desktop_update_kit::SessionState::downloading:
        next.state = UpdateState::downloading;
        next.status = std::format(L"正在下载：{} / {}，{}/s，节点 {}，{} 路{}",
            format_bytes(static_cast<double>(next.received)), format_bytes(static_cast<double>(next.total)),
            format_bytes(next.bytes_per_second), next.node, next.connections,
            next.parallel_fallback ? L"（分块失败，已回退单路）" : L"");
        break;
    case desktop_update_kit::SessionState::paused: next.state = UpdateState::paused; next.status = L"下载已暂停。"; break;
    case desktop_update_kit::SessionState::completed: next.state = UpdateState::completed; next.status = L"下载和校验完成，点击“立即安装”。"; break;
    case desktop_update_kit::SessionState::failed: next.state = UpdateState::failed; next.status = L"下载失败：" + widen(snapshot.error); break;
    case desktop_update_kit::SessionState::cancelled: next.state = UpdateState::cancelled; next.status = L"下载已取消。"; break;
    }
    if (snapshot.state != desktop_update_kit::SessionState::downloading ||
        !snapshot.progress || snapshot.progress->received == snapshot.progress->total) {
        SMK_DIAGNOSTIC_EVENT("update.session.state", std::format(L"state={} connections={} fallback={}",
            static_cast<int>(snapshot.state), next.connections, next.parallel_fallback ? 1 : 0));
    }
    publish(next);
}

std::vector<std::byte> NativeUpdateCoordinator::updater_stub() const {
    return load_updater_stub(instance_);
}

std::wstring NativeUpdateCoordinator::widen(const std::string& text) {
    return widen_utf8(text);
}

bool NativeUpdateCoordinator::shutdown(DWORD timeout_ms) {
    const ULONGLONG deadline = timeout_ms == INFINITE ? std::numeric_limits<ULONGLONG>::max()
        : GetTickCount64() + timeout_ms;
    const auto remaining = [&]() -> std::chrono::milliseconds {
        if (timeout_ms == INFINITE) return std::chrono::milliseconds::max();
        const auto now = GetTickCount64();
        return std::chrono::milliseconds(now >= deadline ? 0 : deadline - now);
    };
    std::shared_ptr<ObserverSlot> observer;
    {
        std::scoped_lock lock(mutex_);
        if (!shutting_down_) {
            shutting_down_ = true;
            observer = std::exchange(observer_slot_, {});
        }
    }
    deactivate_observer(observer);
    if (check_thread_.joinable()) check_thread_.request_stop();
    client_.cancel_active_requests();
    (void)session_.cancel();
    session_.set_changed_callback({});
    bool check_stopped{};
    {
        std::unique_lock lock(mutex_);
        if (timeout_ms == INFINITE) {
            check_finished_signal_.wait(lock, [this] { return check_finished_; });
            check_stopped = true;
        } else {
            check_stopped = check_finished_signal_.wait_for(lock, remaining(), [this] { return check_finished_; });
        }
    }
    if (check_stopped && check_thread_.joinable()) check_thread_.join();
    const bool session_stopped = session_.wait_for_stop(remaining());
    bool installation_started{};
    {
        std::scoped_lock lock(mutex_);
        installation_started = installation_started_;
    }
    const auto snapshot = session_.snapshot();
    if (check_stopped && session_stopped && !installation_started &&
        snapshot.state == desktop_update_kit::SessionState::completed)
        session_.discard_completed();
    return check_stopped && session_stopped;
}

} // namespace smk::updater
