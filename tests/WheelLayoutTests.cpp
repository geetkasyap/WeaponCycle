#include "overlay/WheelLayout.h"

#include <cmath>
#include <numbers>
#include <vector>

namespace {

orbitdeck::WheelItem Item(const wchar_t* id) {
    orbitdeck::WheelItem item;
    item.id = id;
    item.label = id;
    item.target = L"test";
    return item;
}

}  // namespace

int main() {
    using orbitdeck::BuildWheelLayout;
    using orbitdeck::FindWheelSegment;
    using orbitdeck::HitTestWheelLayout;

    std::vector<orbitdeck::WheelItem> root{Item(L"top"), Item(L"right"), Item(L"bottom"),
                                           Item(L"left")};
    root[1].children = {Item(L"one"), Item(L"two"), Item(L"three")};

    const auto rootOnly = BuildWheelLayout(root, {}, 88.0F, 210.0F);
    if (rootOnly.segments.size() != 4 || rootOnly.activeLayer != 0 ||
        rootOnly.surfaceRadiusDip <= rootOnly.requiredRadiusDip) {
        return 1;
    }
    const auto top = HitTestWheelLayout(rootOnly, 0.0F, -150.0F);
    if (!top.has_value() || rootOnly.segments[*top].path != std::vector<std::size_t>{0}) {
        return 2;
    }

    const auto expanded = BuildWheelLayout(root, {1}, 88.0F, 210.0F);
    if (expanded.segments.size() != 7 || expanded.activeLayer != 1) {
        return 3;
    }
    const auto* middle = FindWheelSegment(expanded, {1, 1});
    if (middle == nullptr || middle->layer != 1 || std::abs(middle->centreAngleRadians) > 0.001F ||
        middle->innerRadiusDip <= 210.0F) {
        return 4;
    }
    const float childRadius = (middle->innerRadiusDip + middle->outerRadiusDip) * 0.5F;
    const auto child = HitTestWheelLayout(expanded, childRadius, 0.0F);
    if (!child.has_value() || expanded.segments[*child].path != std::vector<std::size_t>({1, 1})) {
        return 5;
    }

    root[1].children[1].enabled = false;
    const auto disabled = BuildWheelLayout(root, {1}, 88.0F, 210.0F);
    const auto* disabledMiddle = FindWheelSegment(disabled, {1, 1});
    if (disabledMiddle == nullptr || disabledMiddle->enabled) {
        return 6;
    }

    const auto invalid = BuildWheelLayout(root, {99}, 88.0F, 210.0F);
    if (invalid.activeLayer != 0 || invalid.segments.size() != 4) {
        return 7;
    }
    for (std::size_t count = 1; count <= orbitdeck::kMaximumWheelItems; ++count) {
        std::vector<orbitdeck::WheelItem> items(count, Item(L"tile"));
        const auto layout = BuildWheelLayout(items, {}, 66.0F, 156.0F);
        for (std::size_t a = 0; a < count; ++a) {
            const auto tile = orbitdeck::TileForWheelSegment(layout.segments[a]);
            if (!(tile.width > 0.0F && tile.height > 0.0F))
                return 8;
            const auto hit = HitTestWheelLayout(layout, tile.centreX, tile.centreY);
            if (!hit.has_value() || *hit != a)
                return 9;
            for (std::size_t b = a + 1; b < count; ++b) {
                const auto other = orbitdeck::TileForWheelSegment(layout.segments[b]);
                const bool overlap =
                    std::abs(tile.centreX - other.centreX) < (tile.width + other.width) * 0.5F &&
                    std::abs(tile.centreY - other.centreY) < (tile.height + other.height) * 0.5F;
                if (overlap)
                    return 10;
            }
        }
    }
    return 0;
}
