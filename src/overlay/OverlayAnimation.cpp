#include "overlay/OverlayAnimation.h"

namespace orbitdeck {
namespace {

[[nodiscard]] float Clamp01(const float value) noexcept {
    if (value < 0.0F) {
        return 0.0F;
    }
    return value > 1.0F ? 1.0F : value;
}

[[nodiscard]] float Lerp(const float from, const float to, const float t) noexcept {
    return from + (to - from) * t;
}

}  // namespace

float OverlayAnimation::EaseOutCubic(const float t) noexcept {
    const float clamped = Clamp01(t);
    const float inverse = 1.0F - clamped;
    return 1.0F - inverse * inverse * inverse;
}

float OverlayAnimation::SmoothStep(const float t) noexcept {
    const float clamped = Clamp01(t);
    return clamped * clamped * (3.0F - 2.0F * clamped);
}

void OverlayAnimation::Configure(const bool reducedMotion,
                                 const std::int64_t qpcFrequency) noexcept {
    reducedMotion_ = reducedMotion;
    timings_ = reducedMotion ? kReducedTimings : kNormalTimings;
    frequency_ = qpcFrequency > 0 ? qpcFrequency : 1;
}

void OverlayAnimation::Open(const std::int64_t nowQpc) noexcept {
    phase_ = Phase::Opening;
    committed_ = false;
    startQpc_ = nowQpc;
    closeFrom_ = {};
}

void OverlayAnimation::BeginCommit(const std::int64_t nowQpc) noexcept {
    committed_ = true;
    BeginClose(Phase::ClosingCommitted, nowQpc);
}

void OverlayAnimation::BeginCancel(const std::int64_t nowQpc) noexcept {
    committed_ = false;
    BeginClose(Phase::ClosingCancelled, nowQpc);
}

void OverlayAnimation::Reset() noexcept {
    phase_ = Phase::Idle;
    committed_ = false;
    startQpc_ = 0;
    closeFrom_ = {};
}

bool OverlayAnimation::IsClosing() const noexcept {
    return phase_ == Phase::ClosingCancelled || phase_ == Phase::ClosingCommitted;
}

WheelVisualParams OverlayAnimation::Tick(const std::int64_t nowQpc) noexcept {
    switch (phase_) {
        case Phase::Idle:
            return {};
        case Phase::Open:
            return kOpenParams;
        case Phase::Finished:
            return {0.0F, 0.0F, ClosingEndScale(), 0.0F};
        case Phase::Opening: {
            const float progress = ElapsedSeconds(nowQpc) / timings_.opening;
            if (progress >= 1.0F) {
                phase_ = Phase::Open;
                return kOpenParams;
            }
            const float eased = EaseOutCubic(progress);
            WheelVisualParams params;
            params.scrimOpacity = eased;
            params.wheelOpacity = eased;
            if (reducedMotion_) {
                params.wheelScale = 1.0F;
                params.wheelRotationDegrees = 0.0F;
            } else {
                params.wheelScale = Lerp(kOpeningStartScale, 1.0F, eased);
                params.wheelRotationDegrees = Lerp(kOpeningStartRotationDegrees, 0.0F, eased);
            }
            return params;
        }
        case Phase::ClosingCancelled:
        case Phase::ClosingCommitted: {
            const float duration =
                phase_ == Phase::ClosingCommitted ? timings_.commit : timings_.cancel;
            const float endScale = ClosingEndScale();
            const float progress = ElapsedSeconds(nowQpc) / duration;
            if (progress >= 1.0F) {
                phase_ = Phase::Finished;
                return {0.0F, 0.0F, endScale, 0.0F};
            }
            const float eased = EaseOutCubic(progress);
            WheelVisualParams params;
            params.scrimOpacity = Lerp(closeFrom_.scrimOpacity, 0.0F, eased);
            params.wheelOpacity = Lerp(closeFrom_.wheelOpacity, 0.0F, eased);
            if (reducedMotion_) {
                params.wheelScale = 1.0F;
                params.wheelRotationDegrees = 0.0F;
            } else {
                params.wheelScale = Lerp(closeFrom_.wheelScale, endScale, eased);
                params.wheelRotationDegrees = Lerp(closeFrom_.wheelRotationDegrees, 0.0F, eased);
            }
            return params;
        }
    }
    return {};
}

float OverlayAnimation::ElapsedSeconds(const std::int64_t nowQpc) const noexcept {
    const std::int64_t delta = nowQpc - startQpc_;
    if (delta <= 0) {
        return 0.0F;
    }
    return static_cast<float>(static_cast<double>(delta) / static_cast<double>(frequency_));
}

float OverlayAnimation::ClosingEndScale() const noexcept {
    if (reducedMotion_) {
        return 1.0F;
    }
    return committed_ ? kCommitEndScale : kCancelEndScale;
}

void OverlayAnimation::BeginClose(const Phase phase, const std::int64_t nowQpc) noexcept {
    closeFrom_ = Tick(nowQpc);
    phase_ = phase;
    startQpc_ = nowQpc;
}

}  // namespace orbitdeck
