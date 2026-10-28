#include "overlay/SelectionEngine.h"
#include "overlay/WheelPlacement.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

namespace {

struct Vector {
    float x;
    float y;
};

[[nodiscard]] Vector AtDegrees(const float clockwiseDegrees, const float radius = 140.0F) {
    const float radians = clockwiseDegrees * std::numbers::pi_v<float> / 180.0F;
    return {std::sin(radians) * radius, -std::cos(radians) * radius};
}

[[nodiscard]] bool IsSelected(const orbitdeck::SelectionEngine::Selection selection,
                              const std::size_t expected) {
    return selection.has_value() && *selection == expected;
}

}  // namespace

int main() {
    using orbitdeck::SegmentOrder;
    using orbitdeck::SelectionEngine;

    SelectionEngine clockwise;
    if (clockwise.Selected().has_value()) {
        return 1;
    }

    for (std::size_t index = 0; index < 8; ++index) {
        clockwise.Reset();
        const Vector centre = AtDegrees(static_cast<float>(index) * 45.0F);
        if (!IsSelected(clockwise.Update(centre.x, centre.y), index)) {
            return 10 + static_cast<int>(index);
        }
    }

    for (std::size_t index = 0; index < 8; ++index) {
        const float boundary = static_cast<float>(index) * 45.0F + 22.5F;
        clockwise.Reset();
        Vector point = AtDegrees(boundary - 0.25F);
        if (!IsSelected(clockwise.Update(point.x, point.y), index)) {
            return 20 + static_cast<int>(index);
        }

        clockwise.Reset();
        point = AtDegrees(boundary + 0.25F);
        if (!IsSelected(clockwise.Update(point.x, point.y), (index + 1) % 8)) {
            return 30 + static_cast<int>(index);
        }
    }

    clockwise.Reset();
    Vector point = AtDegrees(-45.0F);
    if (!IsSelected(clockwise.Update(point.x, point.y), 7)) {
        return 40;
    }
    clockwise.Reset();
    point = AtDegrees(405.0F);
    if (!IsSelected(clockwise.Update(point.x, point.y), 1)) {
        return 41;
    }

    clockwise.Reset();
    point = AtDegrees(90.0F);
    (void)clockwise.Update(point.x, point.y);
    point = AtDegrees(0.0F, 90.0F);
    if (!IsSelected(clockwise.Update(point.x, point.y), 2)) {
        return 42;
    }
    point = AtDegrees(0.0F, 79.0F);
    if (clockwise.Update(point.x, point.y).has_value()) {
        return 43;
    }
    point = AtDegrees(180.0F, 80.0F);
    if (clockwise.Update(point.x, point.y).has_value()) {
        return 44;
    }
    point = AtDegrees(180.0F, 104.0F);
    if (clockwise.Update(point.x, point.y).has_value()) {
        return 45;
    }
    point = AtDegrees(180.0F, 104.1F);
    if (!IsSelected(clockwise.Update(point.x, point.y), 4)) {
        return 46;
    }

    for (std::size_t index = 0; index < 8; ++index) {
        const float centre = static_cast<float>(index) * 45.0F;
        clockwise.Reset();
        point = AtDegrees(centre);
        (void)clockwise.Update(point.x, point.y);
        point = AtDegrees(centre + 28.0F);
        if (!IsSelected(clockwise.Update(point.x, point.y), index)) {
            return 50 + static_cast<int>(index);
        }
        point = AtDegrees(centre + 29.0F);
        if (!IsSelected(clockwise.Update(point.x, point.y), (index + 1) % 8)) {
            return 60 + static_cast<int>(index);
        }

        clockwise.Reset();
        const std::size_t next = (index + 1) % 8;
        point = AtDegrees(static_cast<float>(next) * 45.0F);
        (void)clockwise.Update(point.x, point.y);
        point = AtDegrees(centre + 17.0F);
        if (!IsSelected(clockwise.Update(point.x, point.y), next)) {
            return 70 + static_cast<int>(index);
        }
        point = AtDegrees(centre + 16.0F);
        if (!IsSelected(clockwise.Update(point.x, point.y), index)) {
            return 80 + static_cast<int>(index);
        }
    }

    SelectionEngine counterClockwise(8, SegmentOrder::CounterClockwise);
    for (std::size_t index = 0; index < 8; ++index) {
        counterClockwise.Reset();
        point = AtDegrees(-static_cast<float>(index) * 45.0F);
        if (!IsSelected(counterClockwise.Update(point.x, point.y), index)) {
            return 90 + static_cast<int>(index);
        }
    }

    SelectionEngine fourItems(4);
    for (std::size_t index = 0; index < 4; ++index) {
        fourItems.Reset();
        point = AtDegrees(static_cast<float>(index) * 90.0F);
        if (!IsSelected(fourItems.Update(point.x, point.y), index)) {
            return 100 + static_cast<int>(index);
        }
    }

    SelectionEngine twelveItems(12);
    for (std::size_t index = 0; index < 12; ++index) {
        twelveItems.Reset();
        point = AtDegrees(static_cast<float>(index) * 30.0F);
        if (!IsSelected(twelveItems.Update(point.x, point.y), index)) {
            return 110 + static_cast<int>(index);
        }
    }

    SelectionEngine noItems(0);
    if (noItems.Update(0.0F, -140.0F).has_value()) {
        return 122;
    }
    if (clockwise.Update(std::numeric_limits<float>::infinity(), 0.0F).has_value()) {
        return 123;
    }

    SelectionEngine enabledItems;
    enabledItems.Configure(8, SegmentOrder::Clockwise, {1, 0, 1, 1, 1, 1, 1, 1});
    point = AtDegrees(45.0F);
    if (enabledItems.Update(point.x, point.y).has_value()) {
        return 129;
    }
    point = AtDegrees(90.0F);
    if (!IsSelected(enabledItems.Update(point.x, point.y), 2)) {
        return 130;
    }
    enabledItems.Configure(3, SegmentOrder::Clockwise, {1, 1, 1});
    point = AtDegrees(120.0F);
    if (!IsSelected(enabledItems.Update(point.x, point.y), 1)) {
        return 131;
    }

    SelectionEngine configuredThresholds;
    configuredThresholds.Configure(8, SegmentOrder::Clockwise, {1, 1, 1, 1, 1, 1, 1, 1}, 100.0F,
                                   130.0F);
    point = AtDegrees(0.0F, 120.0F);
    if (configuredThresholds.Update(point.x, point.y).has_value()) {
        return 133;
    }
    point = AtDegrees(0.0F, 131.0F);
    if (!IsSelected(configuredThresholds.Update(point.x, point.y), 0)) {
        return 134;
    }
    point = AtDegrees(0.0F, 99.0F);
    if (configuredThresholds.Update(point.x, point.y).has_value()) {
        return 135;
    }

    SelectionEngine dpiIndependentA;
    SelectionEngine dpiIndependentB;
    point = AtDegrees(135.0F);
    const auto atOneX = dpiIndependentA.Update(point.x, point.y);
    const float twoXScale = 2.0F;
    const auto atTwoX =
        dpiIndependentB.Update(point.x * twoXScale / twoXScale, point.y * twoXScale / twoXScale);
    if (atOneX != atTwoX || !IsSelected(atOneX, 3)) {
        return 124;
    }

    constexpr orbitdeck::PixelPoint edgeCursor{0, 0};
    constexpr auto edgePlacement =
        orbitdeck::CalculateSquarePlacement(edgeCursor, {0, 0, 1920, 1080}, 620);
    constexpr float edgeDeltaX =
        static_cast<float>(edgeCursor.x) -
        (static_cast<float>(edgePlacement.left) + static_cast<float>(edgePlacement.size) * 0.5F);
    constexpr float edgeDeltaY =
        static_cast<float>(edgeCursor.y) -
        (static_cast<float>(edgePlacement.top) + static_cast<float>(edgePlacement.size) * 0.5F);
    SelectionEngine clampedGesture;
    clampedGesture.BeginGesture(edgeDeltaX, edgeDeltaY);
    if (clampedGesture.GestureArmed() || clampedGesture.Selected().has_value() ||
        clampedGesture.Update(edgeDeltaX, edgeDeltaY).has_value()) {
        return 125;
    }
    point = AtDegrees(90.0F, 104.0F);
    if (clampedGesture.Update(point.x, point.y).has_value()) {
        return 126;
    }
    if (!clampedGesture.GestureArmed()) {
        return 132;
    }
    point = AtDegrees(90.0F, 140.0F);
    if (!IsSelected(clampedGesture.Update(point.x, point.y), 2)) {
        return 127;
    }

    SelectionEngine centredGesture;
    centredGesture.BeginGesture(0.0F, 0.0F);
    point = AtDegrees(0.0F);
    if (!IsSelected(centredGesture.Update(point.x, point.y), 0)) {
        return 128;
    }

    std::vector<orbitdeck::WheelItem> nestedItems(4);
    for (std::size_t index = 0; index < nestedItems.size(); ++index) {
        nestedItems[index].id = std::to_wstring(index);
        nestedItems[index].enabled = true;
    }
    nestedItems[1].children.resize(3);
    for (orbitdeck::WheelItem& child : nestedItems[1].children) {
        child.enabled = true;
    }
    const auto nestedLayout = orbitdeck::BuildWheelLayout(nestedItems, {1}, 88.0F, 210.0F);
    SelectionEngine nestedSelection;
    nestedSelection.Configure(nestedLayout, 88.0F, 112.0F);
    point = AtDegrees(90.0F, 150.0F);
    if (!IsSelected(nestedSelection.Update(point.x, point.y), 1)) {
        return 136;
    }
    const auto* childSegment = orbitdeck::FindWheelSegment(nestedLayout, {1, 1});
    if (childSegment == nullptr)
        return 137;
    const float childRadius = (childSegment->innerRadiusDip + childSegment->outerRadiusDip) * 0.5F;
    point = {std::cos(childSegment->centreAngleRadians) * childRadius,
             std::sin(childSegment->centreAngleRadians) * childRadius};
    const auto childSelection = nestedSelection.Update(point.x, point.y);
    if (!childSelection.has_value() ||
        nestedLayout.segments[*childSelection].path != std::vector<std::size_t>({1, 1})) {
        return 138;
    }

    return 0;
}
