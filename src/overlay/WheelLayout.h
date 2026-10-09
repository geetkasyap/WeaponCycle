#pragma once

#include "config/Configuration.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace orbitdeck {

struct WheelLayoutSegment {
    std::vector<std::size_t> path;
    std::size_t layer{};
    std::size_t index{};
    float centreAngleRadians{};
    float halfSpanRadians{};
    float innerRadiusDip{};
    float outerRadiusDip{};
    bool enabled{true};
    bool expandedParent{false};
};

struct WheelLayout {
    std::vector<WheelLayoutSegment> segments;
    std::size_t activeLayer{};
    float requiredRadiusDip{};
    float surfaceRadiusDip{};
};

struct WheelTileGeometry {
    float centreX{};
    float centreY{};
    float width{};
    float height{};
};

[[nodiscard]] WheelTileGeometry TileForWheelSegment(const WheelLayoutSegment& segment) noexcept;

[[nodiscard]] WheelLayout BuildWheelLayout(const std::vector<WheelItem>& rootItems,
                                           const std::vector<std::size_t>& submenuPath,
                                           float rootInnerRadiusDip, float rootOuterRadiusDip);

[[nodiscard]] std::optional<std::size_t> HitTestWheelLayout(
    std::span<const WheelLayoutSegment> segments, float deltaXDip, float deltaYDip,
    float angularMarginRadians = 0.0F, float radialMarginDip = 0.0F) noexcept;

[[nodiscard]] std::optional<std::size_t> HitTestWheelLayout(const WheelLayout& layout,
                                                            float deltaXDip, float deltaYDip,
                                                            float angularMarginRadians = 0.0F,
                                                            float radialMarginDip = 0.0F) noexcept;

[[nodiscard]] const WheelLayoutSegment* FindWheelSegment(
    const WheelLayout& layout, const std::vector<std::size_t>& path) noexcept;

}  // namespace orbitdeck
