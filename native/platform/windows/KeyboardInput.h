#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>

namespace smk::windows {

inline constexpr UINT kEmergencyResetMessage = WM_APP + 240; // 设置录制的独立急停收尾通知。

using InputGeneration = std::uint64_t; // 输入代次；最低位为暂停锁存，旧代次永不复用。
using KeyboardEventSender = std::function<bool(WORD key, bool key_up)>; // 按键发送器，true 表示松开。
using KeyboardDownQuery = std::function<bool(WORD key)>; // 查询当前按下状态，测试可注入替身。

[[nodiscard]] bool is_emergency_reset_key(UINT key) noexcept;

// 共享键盘输入账本：钩子回调只更新原子状态，绝不在回调里加锁或调用 SendInput（模拟输入）。
class KeyboardInput final {
public:
    class DispatchPermit final {
    public:
        DispatchPermit() = default;
        ~DispatchPermit();
        DispatchPermit(const DispatchPermit&) = delete;
        DispatchPermit& operator=(const DispatchPermit&) = delete;
        DispatchPermit(DispatchPermit&& other) noexcept;
        DispatchPermit& operator=(DispatchPermit&& other) noexcept;
        explicit operator bool() const noexcept { return owner_ != nullptr; }
    private:
        friend class KeyboardInput;
        explicit DispatchPermit(KeyboardInput* owner) : owner_(owner) {}
        KeyboardInput* owner_ = nullptr; // 急停前已取得提交许可的在途操作。
    };

    KeyboardInput(KeyboardEventSender sender, KeyboardDownQuery down_query);
    [[nodiscard]] InputGeneration generation() const noexcept;
    [[nodiscard]] bool paused() const noexcept;
    [[nodiscard]] bool accepts(InputGeneration generation) const noexcept;
    [[nodiscard]] DispatchPermit try_dispatch(InputGeneration generation) noexcept;
    void pause() noexcept;
    void emergency_reset() noexcept;
    void resume() noexcept;
    void observe_key(WPARAM message, UINT key, DWORD flags, bool suppressed = false) noexcept;
    void seed_physical_modifiers() noexcept; // 安装钩子前记录已经按住的修饰键。
    void request_safety_hook_refresh() noexcept { refresh_hook_.store(true, std::memory_order_release); }
    [[nodiscard]] bool take_safety_hook_refresh() noexcept { return refresh_hook_.exchange(false, std::memory_order_acq_rel); }
    void poll_reset_keys() noexcept;

    [[nodiscard]] bool send_chord(std::span<const WORD> keys, InputGeneration generation) noexcept;
    // 失败的松开事件保留在账本中，由独立输入线程周期重试；锁忙时立即返回。
    void retry_releases() noexcept;
    [[nodiscard]] std::size_t pending_release_count() const noexcept;

private:
    [[nodiscard]] bool physically_held(WORD key) const noexcept;
    [[nodiscard]] bool query_down(WORD key) const noexcept;
    [[nodiscard]] bool send_key(WORD key, bool key_up) noexcept;
    [[nodiscard]] bool release_key(WORD key) noexcept;
    [[nodiscard]] bool release_pending() noexcept;

    KeyboardEventSender sender_;
    KeyboardDownQuery down_query_;
    std::mutex send_mutex_; // 仅发送/补偿线程使用，钩子回调不获取此锁。
    std::atomic_uint64_t generation_{0};
    std::atomic_bool refresh_hook_{false}; // 录制钩子后装时，将独立安全钩子重新放到链首。
    std::atomic_bool force_release_modifiers_{false};
    std::array<std::atomic_bool, 256> pending_{}; // 本程序注入后尚未可靠松开的键。
    std::array<std::atomic_bool, 256> physical_down_{}; // 物理按键状态，不计合成输入。
    std::array<std::atomic_bool, 256> physical_suppressed_{}; // 被录制器截断的物理按下不能接管注入键的释放。
    std::array<std::atomic_bool, 256> injected_down_{}; // 防止备用轮询把合成 Win 当成急停。
};

[[nodiscard]] KeyboardInput& keyboard_input();

} // namespace smk::windows
