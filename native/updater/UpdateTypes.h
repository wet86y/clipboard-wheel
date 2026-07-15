#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace smk::updater {

enum class UpdateState {
    disabled,
    idle,
    checking,
    available,
    downloading,
    paused,
    completed,
    failed,
    cancelled,
    launching
};

struct UpdateViewState {
    UpdateState state{UpdateState::disabled};
    std::wstring version;
    std::wstring release_notes;
    std::wstring status;
    std::wstring node;
    std::uint64_t received{};
    std::uint64_t total{};
    double bytes_per_second{};
    int connections{1};
    bool parallel_fallback{};
    bool acceleration{true};
    bool background{};
};

class UpdateController {
public:
    using Observer = std::function<void(const UpdateViewState&)>;
    virtual ~UpdateController() = default;
    virtual void set_observer(Observer observer) = 0;
    virtual UpdateViewState state() const = 0;
    virtual void check() = 0;
    virtual void download_or_resume() = 0;
    virtual void pause() = 0;
    virtual void continue_in_background() = 0;
    virtual void cancel() = 0;
    virtual void set_acceleration(bool enabled) = 0;
    virtual void next_node() = 0;
    virtual bool install() = 0;
    virtual void settings_closed() = 0;
};

} // namespace smk::updater
