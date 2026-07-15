#pragma once

#include <cstdint>

namespace smk::core {

enum class Easing { linear, cubic_in, cubic_out, quadratic_in, quadratic_out };

struct AnimationTrack {
    double from = 0.0;
    double to = 0.0;
    double started_ms = 0.0;
    double duration_ms = 0.0;
    Easing easing = Easing::linear;
    bool active = false;

    void set(double value) noexcept;
    void begin(double current, double target, double now_ms, double duration, Easing curve) noexcept;
    [[nodiscard]] double sample(double now_ms) noexcept;
};

[[nodiscard]] double apply_easing(double progress, Easing easing) noexcept;

} // namespace smk::core
