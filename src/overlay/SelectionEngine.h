#pragma once

#include "overlay/WheelLayout.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace orbitdeck {

enum class SegmentOrder {
    Clockwise,
    CounterClockwise,
};

class SelectionEngine {
public:
    using Selection = std::optional<std::size_t>;

    static constexpr std::size_t kDefaultItemCount{8};
    static constexpr float kNeutralRadiusDip{80.0F};
    static constexpr float kActiveRadiusDip{104.0F};
    static constexpr float kAngularHysteresisDegrees{6.0F};

    explicit SelectionEngine(std::size_t itemCount = kDefaultItemCount,
                             SegmentOrder order = SegmentOrder::Clockwise) noexcept;

    void Configure(std::size_t itemCount, SegmentOrder order,
                   std::vector<std::uint8_t> enabledItems,
                   float neutralRadiusDip = kNeutralRadiusDip,
                   float activeRadiusDip = kActiveRadiusDip) noexcept;
    void Configure(WheelLayout layout, float neutralRadiusDip = kNeutralRadiusDip,
                   float activeRadiusDip = kActiveRadiusDip) noexcept;
    void BeginGesture(float deltaXDip, float deltaYDip) noexcept;
    [[nodiscard]] Selection Update(float deltaXDip, float deltaYDip) noexcept;
    void Reset() noexcept;
    [[nodiscard]] Selection Selected() const noexcept;
    [[nodiscard]] bool GestureArmed() const noexcept;

private:
    [[nodiscard]] bool IsEnabled(std::size_t index) const noexcept;

    std::size_t itemCount_;
    SegmentOrder order_;
    std::vector<std::uint8_t> enabledItems_;
    WheelLayout layout_;
    Selection selected_{};
    bool awaitingNeutralEntry_{false};
    float neutralRadiusDip_{kNeutralRadiusDip};
    float activeRadiusDip_{kActiveRadiusDip};
};

}  // namespace orbitdeck
