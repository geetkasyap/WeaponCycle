#include "config/Configuration.h"
#include "overlay/WheelPlacement.h"

int main() {
    const orbitdeck::Configuration configuration;

    if ((configuration.activation.modifiers & MOD_SHIFT) == 0 ||
        (configuration.activation.modifiers & MOD_NOREPEAT) == 0 ||
        configuration.activation.virtualKey != VK_SPACE ||
        configuration.activation.experimentalLeftShiftOnly ||
        configuration.activation.leftShiftHoldThresholdMs != 240 ||
        configuration.placement.mode != orbitdeck::PlacementMode::Cursor ||
        !configuration.interaction.cancelOnRightClick ||
        !configuration.interaction.cancelOnOuterExit ||
        !configuration.interaction.numberQuickKeys ||
        configuration.interaction.outerExitRadiusDip != 300.0F ||
        configuration.interaction.submenuDwellMs != 350 || configuration.appearance.reducedMotion ||
        configuration.appearance.wheelOpacity != 0.90F ||
        configuration.appearance.wheelDiameterDip != 400.0F ||
        configuration.appearance.outerRadiusDip != 156.0F ||
        configuration.appearance.innerRadiusDip != 66.0F ||
        configuration.appearance.scrimOpacity != 0.0F) {
        return 1;
    }

    constexpr orbitdeck::PixelRect primary{0, 0, 1920, 1080};
    constexpr auto centred = orbitdeck::CalculateSquarePlacement({960, 540}, primary, 620);
    constexpr auto topLeft = orbitdeck::CalculateSquarePlacement({5, 5}, primary, 620);
    constexpr auto bottomRight = orbitdeck::CalculateSquarePlacement({1915, 1075}, primary, 620);
    constexpr auto negativeMonitor =
        orbitdeck::CalculateSquarePlacement({-1900, 20}, {-1920, 0, 0, 1080}, 620);
    constexpr auto reduced =
        orbitdeck::CalculateSquarePlacement({400, 300}, {0, 0, 800, 600}, 1200);

    static_assert(centred.left == 650 && centred.top == 230 && centred.size == 620);
    static_assert(topLeft.left == 0 && topLeft.top == 0 && topLeft.size == 620);
    static_assert(bottomRight.left == 1300 && bottomRight.top == 460 && bottomRight.size == 620);
    static_assert(negativeMonitor.left == -1920 && negativeMonitor.top == 0 &&
                  negativeMonitor.size == 620);
    static_assert(reduced.left == 100 && reduced.top == 0 && reduced.size == 600);
    return 0;
}
