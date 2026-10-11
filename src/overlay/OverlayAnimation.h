#pragma once

#include <cstdint>

namespace orbitdeck {

// Visual parameters applied to the composition tree for one animation frame.
// Opacities are in [0, 1]; scale is a multiplier around the wheel centre;
// rotation is in degrees. These are intentionally free of any Win32 or
// Direct2D type so the lifecycle can be unit tested without a display.
struct WheelVisualParams {
    float scrimOpacity{0.0F};
    float wheelOpacity{0.0F};
    float wheelScale{1.0F};
    float wheelRotationDegrees{0.0F};
};

// Explicit, time-based overlay animation lifecycle. Callers pass raw
// QueryPerformanceCounter values; the class converts them to seconds using the
// configured frequency so animation never depends on frame cadence.
class OverlayAnimation {
public:
    enum class Phase {
        Idle,
        Opening,
        Open,
        ClosingCancelled,
        ClosingCommitted,
        Finished,
    };

    // Phase durations in seconds.
    struct Timings {
        float opening;
        float selection;
        float commit;
        float cancel;
    };

    static constexpr Timings kNormalTimings{0.100F, 0.060F, 0.045F, 0.050F};
    static constexpr Timings kReducedTimings{0.045F, 0.040F, 0.045F, 0.040F};

    static constexpr float kOpeningStartScale{0.93F};
    static constexpr float kOpeningStartRotationDegrees{-1.25F};
    static constexpr float kCancelEndScale{0.97F};
    static constexpr float kCommitEndScale{1.02F};

    static constexpr WheelVisualParams kOpenParams{1.0F, 1.0F, 1.0F, 0.0F};

    void Configure(bool reducedMotion, std::int64_t qpcFrequency) noexcept;

    void Open(std::int64_t nowQpc) noexcept;
    void BeginCommit(std::int64_t nowQpc) noexcept;
    void BeginCancel(std::int64_t nowQpc) noexcept;
    void Reset() noexcept;

    // Advances the phase for the supplied time and returns the frame params.
    [[nodiscard]] WheelVisualParams Tick(std::int64_t nowQpc) noexcept;

    [[nodiscard]] Phase phase() const noexcept {
        return phase_;
    }
    [[nodiscard]] bool IsClosing() const noexcept;
    [[nodiscard]] bool IsFinished() const noexcept {
        return phase_ == Phase::Finished;
    }
    [[nodiscard]] bool committed() const noexcept {
        return committed_;
    }
    [[nodiscard]] bool reducedMotion() const noexcept {
        return reducedMotion_;
    }
    [[nodiscard]] float SelectionDurationSeconds() const noexcept {
        return timings_.selection;
    }

    [[nodiscard]] static float EaseOutCubic(float t) noexcept;
    [[nodiscard]] static float SmoothStep(float t) noexcept;

private:
    [[nodiscard]] float ElapsedSeconds(std::int64_t nowQpc) const noexcept;
    [[nodiscard]] float ClosingEndScale() const noexcept;
    void BeginClose(Phase phase, std::int64_t nowQpc) noexcept;

    Phase phase_{Phase::Idle};
    bool reducedMotion_{false};
    bool committed_{false};
    Timings timings_{kNormalTimings};
    std::int64_t frequency_{1};
    std::int64_t startQpc_{0};
    WheelVisualParams closeFrom_{};
};

}  // namespace orbitdeck
