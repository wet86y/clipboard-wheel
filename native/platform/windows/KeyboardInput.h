#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>

namespace smk::windows {

using InputGeneration = std::uint64_t; // 输入代次；最低位为暂停锁存，旧代次永不复用。
using KeyboardEventSender = std::function<bool(WORD key, bool key_up)>; // 按键发送器，true 表示松开。
using KeyboardDownQuery = std::function<bool(WORD key)>; // 查询当前按下状态，测试可注入替身。

[[nodiscard]] bool is_emergency_reset_key(UINT key) noexcept;

// 原始输入在独立输入线程处理，模拟输入的收尾在共享释放账本中登记。
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
    void emergency_reset(InputGeneration expected) noexcept;
    void resume() noexcept;
    void observe_raw_key(const RAWINPUT& input, DWORD message_time) noexcept;
    void observe_suppressed_key(UINT key, bool down) noexcept;
    void reset_physical_keys() noexcept;

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

    struct ResetGesture { // 仅独立输入线程访问。
        InputGeneration generation = 0;
        HANDLE device = nullptr;
        UINT key = 0;
        DWORD first_down_time = 0;
        bool first_tap_completed = false;
    };

    KeyboardEventSender sender_;
    KeyboardDownQuery down_query_;
    std::mutex send_mutex_; // 仅发送和补偿使用，观察按键时不获取此锁。
    std::atomic_uint64_t generation_{0};
    std::array<std::atomic_bool, 256> pending_{}; // 本程序注入后尚未可靠松开的键。
    std::array<std::atomic_bool, 256> physical_down_{}; // 物理按键状态，不计合成输入。
    std::array<std::atomic_bool, 256> physical_suppressed_{}; // 被录制器截断的物理按下不能接管注入键的释放。
    ResetGesture reset_gesture_{};
};

[[nodiscard]] KeyboardInput& keyboard_input();

} // namespace smk::windows
