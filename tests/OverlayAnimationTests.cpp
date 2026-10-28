#include "overlay/OverlayAnimation.h"

#include <cmath>
#include <cstdint>

namespace {

using orbitdeck::OverlayAnimation;
using orbitdeck::WheelVisualParams;

constexpr std::int64_t kFrequency{1000};  // 1 count == 1 millisecond.

[[nodiscard]] bool Approx(const float actual, const float expected, const float epsilon = 2.0e-3F) {
    return std::fabs(actual - expected) <= epsilon;
}

}  // namespace

int main() {
    // Easing endpoints, midpoint and clamping.
    if (!Approx(OverlayAnimation::EaseOutCubic(0.0F), 0.0F)) {
        return 1;
    }
    if (!Approx(OverlayAnimation::EaseOutCubic(1.0F), 1.0F)) {
        return 2;
    }
    if (!Approx(OverlayAnimation::EaseOutCubic(0.5F), 0.875F)) {
        return 3;
    }
    if (!Approx(OverlayAnimation::EaseOutCubic(-1.0F), 0.0F) ||
        !Approx(OverlayAnimation::EaseOutCubic(2.0F), 1.0F)) {
        return 4;
    }
    float previousEase = -1.0F;
    for (int step = 0; step <= 10; ++step) {
        const float value = OverlayAnimation::EaseOutCubic(static_cast<float>(step) / 10.0F);
        if (value < previousEase) {
            return 5;
        }
        previousEase = value;
    }
    for (int step = 0; step <= 10; ++step) {
        const float progress = static_cast<float>(step) / 10.0F;
        if (!Approx(OverlayAnimation::SmoothStep(progress) +
                        OverlayAnimation::SmoothStep(1.0F - progress),
                    1.0F)) {
            return 6;
        }
    }

    // Idle animation is fully transparent with an unscaled wheel.
    {
        OverlayAnimation idle;
        idle.Configure(false, kFrequency);
        const WheelVisualParams params = idle.Tick(0);
        if (!Approx(params.scrimOpacity, 0.0F) || !Approx(params.wheelOpacity, 0.0F) ||
            !Approx(params.wheelScale, 1.0F) || !Approx(params.wheelRotationDegrees, 0.0F)) {
            return 10;
        }
        if (idle.phase() != OverlayAnimation::Phase::Idle || idle.IsClosing() ||
            idle.IsFinished()) {
            return 11;
        }
    }

    // Opening ramps opacity, scale, and the subtle initial rotation to rest.
    {
        OverlayAnimation opening;
        opening.Configure(false, kFrequency);
        opening.Open(0);

        const WheelVisualParams start = opening.Tick(0);
        if (!Approx(start.wheelOpacity, 0.0F) ||
            !Approx(start.wheelScale, OverlayAnimation::kOpeningStartScale) ||
            !Approx(start.wheelRotationDegrees, OverlayAnimation::kOpeningStartRotationDegrees)) {
            return 20;
        }
        if (opening.phase() != OverlayAnimation::Phase::Opening) {
            return 21;
        }

        float previousOpacity = -1.0F;
        for (std::int64_t t = 0; t <= 120; t += 20) {
            const WheelVisualParams params = opening.Tick(t);
            if (params.wheelOpacity < previousOpacity) {
                return 22;
            }
            previousOpacity = params.wheelOpacity;
        }

        const WheelVisualParams open = opening.Tick(200);
        if (!Approx(open.scrimOpacity, 1.0F) || !Approx(open.wheelOpacity, 1.0F) ||
            !Approx(open.wheelScale, 1.0F) || !Approx(open.wheelRotationDegrees, 0.0F)) {
            return 23;
        }
        if (opening.phase() != OverlayAnimation::Phase::Open) {
            return 24;
        }
    }

    // Reduced motion animates opacity only: never scale or rotate.
    {
        OverlayAnimation reduced;
        reduced.Configure(true, kFrequency);
        reduced.Open(0);
        if (!reduced.reducedMotion()) {
            return 30;
        }
        bool sawPartialOpacity = false;
        for (std::int64_t t = 0; t <= 45; t += 5) {
            const WheelVisualParams params = reduced.Tick(t);
            if (!Approx(params.wheelScale, 1.0F) || !Approx(params.wheelRotationDegrees, 0.0F)) {
                return 31;
            }
            if (params.wheelOpacity > 0.05F && params.wheelOpacity < 0.95F) {
                sawPartialOpacity = true;
            }
        }
        if (!sawPartialOpacity) {
            return 32;
        }
        if (!Approx(reduced.SelectionDurationSeconds(), 0.040F)) {
            return 33;
        }
    }

    // Commit closes to the commit end-scale and reports committed().
    {
        OverlayAnimation commit;
        commit.Configure(false, kFrequency);
        commit.Open(0);
        (void)commit.Tick(200);
        commit.BeginCommit(200);
        if (!commit.committed() || !commit.IsClosing() ||
            commit.phase() != OverlayAnimation::Phase::ClosingCommitted) {
            return 40;
        }
        const WheelVisualParams finished = commit.Tick(400);
        if (commit.phase() != OverlayAnimation::Phase::Finished || !commit.IsFinished()) {
            return 41;
        }
        if (!Approx(finished.scrimOpacity, 0.0F) || !Approx(finished.wheelOpacity, 0.0F) ||
            !Approx(finished.wheelScale, OverlayAnimation::kCommitEndScale)) {
            return 42;
        }
        // A finished animation stays finished and stable.
        const WheelVisualParams again = commit.Tick(10000);
        if (!Approx(again.wheelOpacity, 0.0F) ||
            !Approx(again.wheelScale, OverlayAnimation::kCommitEndScale)) {
            return 43;
        }
    }

    // Cancel closes to the cancel end-scale and is not committed.
    {
        OverlayAnimation cancel;
        cancel.Configure(false, kFrequency);
        cancel.Open(0);
        (void)cancel.Tick(200);
        cancel.BeginCancel(200);
        if (cancel.committed() || cancel.phase() != OverlayAnimation::Phase::ClosingCancelled) {
            return 50;
        }
        const WheelVisualParams finished = cancel.Tick(400);
        if (!cancel.IsFinished() || !Approx(finished.wheelOpacity, 0.0F) ||
            !Approx(finished.wheelScale, OverlayAnimation::kCancelEndScale)) {
            return 51;
        }
    }

    // Reduced motion keeps the closing scale at 1.0.
    {
        OverlayAnimation reducedCancel;
        reducedCancel.Configure(true, kFrequency);
        reducedCancel.Open(0);
        (void)reducedCancel.Tick(200);
        reducedCancel.BeginCancel(200);
        const WheelVisualParams finished = reducedCancel.Tick(400);
        if (!Approx(finished.wheelScale, 1.0F)) {
            return 60;
        }
    }

    // Interrupting an open mid-flight fades from the current frame, not from 1.
    {
        OverlayAnimation interrupt;
        interrupt.Configure(false, kFrequency);
        interrupt.Open(0);
        const WheelVisualParams mid = interrupt.Tick(60);
        interrupt.BeginCancel(60);
        const WheelVisualParams closingStart = interrupt.Tick(60);
        if (!Approx(closingStart.wheelOpacity, mid.wheelOpacity) ||
            !Approx(closingStart.scrimOpacity, mid.scrimOpacity)) {
            return 70;
        }
        if (closingStart.wheelOpacity >= 0.999F) {
            return 71;
        }
    }

    // Reset returns the lifecycle to idle.
    {
        OverlayAnimation reset;
        reset.Configure(false, kFrequency);
        reset.Open(0);
        (void)reset.Tick(200);
        reset.Reset();
        if (reset.phase() != OverlayAnimation::Phase::Idle) {
            return 80;
        }
        const WheelVisualParams params = reset.Tick(0);
        if (!Approx(params.wheelOpacity, 0.0F)) {
            return 81;
        }
    }

    return 0;
}
