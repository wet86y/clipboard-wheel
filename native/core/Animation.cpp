#include "core/Animation.h"

#include <algorithm>
#include <cmath>

namespace smk::core {

void AnimationTrack::set(double value) noexcept {
    from = value;
    to = value;
    started_ms = 0.0;
    duration_ms = 0.0;
    easing = Easing::linear;
    active = false;
}

double apply_easing(double progress, Easing easing) noexcept {
    progress = std::clamp(progress, 0.0, 1.0);
    switch (easing) {
    case Easing::cubic_in:
        return progress * progress * progress;
    case Easing::cubic_out: {
        const double inverse = 1.0 - progress;
        return 1.0 - inverse * inverse * inverse;
    }
    case Easing::quadratic_in:
        return progress * progress;
    case Easing::quadratic_out:
        return 1.0 - (1.0 - progress) * (1.0 - progress);
    default:
        return progress;
    }
}

void AnimationTrack::begin(double current, double target, double now_ms, double duration, Easing curve) noexcept {
    from = current;
    to = target;
    started_ms = now_ms;
    duration_ms = std::max(0.0, duration);
    easing = curve;
    active = duration_ms > 0.0 && std::abs(to - from) > 0.0001;
    if (!active) from = to;
}

double AnimationTrack::sample(double now_ms) noexcept {
    if (!active) return to;
    const double progress = duration_ms <= 0.0 ? 1.0 : (now_ms - started_ms) / duration_ms;
    if (progress >= 1.0) {
        active = false;
        return to;
    }
    if (progress <= 0.0) return from;
    return from + (to - from) * apply_easing(progress, easing);
}

} // namespace smk::core
