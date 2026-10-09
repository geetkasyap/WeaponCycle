#include "overlay/WheelLayout.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace orbitdeck {
namespace {

constexpr float kPi{std::numbers::pi_v<float>};
constexpr float kTwoPi{2.0F * kPi};
constexpr float kRootGapRadians{0.55F * kPi / 180.0F};
constexpr float kChildLayerGapDip{10.0F};
constexpr float kSurfaceMarginDip{18.0F};

[[nodiscard]] float ChildThickness(const float rootInnerRadiusDip,
                                   const float rootOuterRadiusDip) noexcept {
    return std::clamp((rootOuterRadiusDip - rootInnerRadiusDip) * 0.68F, 54.0F, 74.0F);
}

[[nodiscard]] float ChildInnerRadius(const std::size_t layer, const float rootInnerRadiusDip,
                                     const float rootOuterRadiusDip) noexcept {
    const float thickness = ChildThickness(rootInnerRadiusDip, rootOuterRadiusDip);
    return rootOuterRadiusDip + kChildLayerGapDip +
           static_cast<float>(layer - 1) * (thickness + kChildLayerGapDip);
}

[[nodiscard]] float AngularDistance(const float left, const float right) noexcept {
    return std::abs(std::remainder(left - right, kTwoPi));
}

}  // namespace

WheelTileGeometry TileForWheelSegment(const WheelLayoutSegment& segment) noexcept {
    const float radius = (segment.innerRadiusDip + segment.outerRadiusDip) * 0.5F;
    const float chord = 2.0F * radius * std::sin(std::min(segment.halfSpanRadians, kPi * 0.5F));
    const float side =
        std::max(1.0F, std::min({76.0F, (segment.outerRadiusDip - segment.innerRadiusDip) * 0.84F,
                                 chord * 0.68F}));
    return {std::cos(segment.centreAngleRadians) * radius,
            std::sin(segment.centreAngleRadians) * radius, side * 1.13F, side};
}

WheelLayout BuildWheelLayout(const std::vector<WheelItem>& rootItems,
                             const std::vector<std::size_t>& submenuPath,
                             const float rootInnerRadiusDip, const float rootOuterRadiusDip) {
    WheelLayout layout;
    if (rootItems.empty() || !std::isfinite(rootInnerRadiusDip) ||
        !std::isfinite(rootOuterRadiusDip) || rootInnerRadiusDip < 0.0F ||
        rootOuterRadiusDip <= rootInnerRadiusDip) {
        return layout;
    }

    const std::vector<WheelItem>* items = &rootItems;
    std::vector<std::size_t> prefix;
    float parentAngle = -kPi * 0.5F;

    for (std::size_t layer = 0; items != nullptr && !items->empty(); ++layer) {
        const std::size_t count = items->size();
        const float innerRadius =
            layer == 0 ? rootInnerRadiusDip
                       : ChildInnerRadius(layer, rootInnerRadiusDip, rootOuterRadiusDip);
        const float outerRadius =
            layer == 0 ? rootOuterRadiusDip
                       : innerRadius + ChildThickness(rootInnerRadiusDip, rootOuterRadiusDip);

        float childStep = 0.0F;
        if (layer > 0 && count > 1) {
            childStep = std::min(0.48F, 2.80F / static_cast<float>(count - 1));
        }

        for (std::size_t index = 0; index < count; ++index) {
            WheelLayoutSegment segment;
            segment.path = prefix;
            segment.path.push_back(index);
            segment.layer = layer;
            segment.index = index;
            segment.innerRadiusDip = innerRadius;
            segment.outerRadiusDip = outerRadius;
            segment.enabled = (*items)[index].enabled;
            if (layer == 0) {
                const float step = kTwoPi / static_cast<float>(count);
                segment.centreAngleRadians = -kPi * 0.5F + static_cast<float>(index) * step;
                segment.halfSpanRadians = std::max(0.02F, (step - kRootGapRadians) * 0.5F);
            } else {
                segment.centreAngleRadians =
                    parentAngle +
                    (static_cast<float>(index) - static_cast<float>(count - 1) * 0.5F) * childStep;
                segment.halfSpanRadians = count == 1 ? 0.32F : std::max(0.08F, childStep * 0.475F);
            }
            layout.segments.push_back(std::move(segment));
        }

        layout.activeLayer = layer;
        layout.requiredRadiusDip =
            std::max(layout.requiredRadiusDip, outerRadius + kSurfaceMarginDip);

        if (layer >= submenuPath.size()) {
            const bool canExpand =
                std::any_of(items->begin(), items->end(),
                            [](const WheelItem& item) { return item.enabled && item.IsSubmenu(); });
            layout.surfaceRadiusDip =
                canExpand
                    ? std::max(layout.requiredRadiusDip,
                               ChildInnerRadius(layer + 1, rootInnerRadiusDip, rootOuterRadiusDip) +
                                   ChildThickness(rootInnerRadiusDip, rootOuterRadiusDip) +
                                   kSurfaceMarginDip)
                    : layout.requiredRadiusDip;
            break;
        }

        const std::size_t parentIndex = submenuPath[layer];
        if (parentIndex >= items->size() || !(*items)[parentIndex].enabled ||
            !(*items)[parentIndex].IsSubmenu()) {
            layout.surfaceRadiusDip = layout.requiredRadiusDip;
            break;
        }
        const WheelLayoutSegment* parent = FindWheelSegment(layout, [&] {
            std::vector<std::size_t> path = prefix;
            path.push_back(parentIndex);
            return path;
        }());
        if (parent == nullptr) {
            layout.surfaceRadiusDip = layout.requiredRadiusDip;
            break;
        }
        parentAngle = parent->centreAngleRadians;
        for (WheelLayoutSegment& segment : layout.segments) {
            if (segment.path == parent->path) {
                segment.expandedParent = true;
                break;
            }
        }
        prefix.push_back(parentIndex);
        items = &(*items)[parentIndex].children;
    }

    if (layout.surfaceRadiusDip <= 0.0F) {
        layout.surfaceRadiusDip = layout.requiredRadiusDip;
    }
    return layout;
}

std::optional<std::size_t> HitTestWheelLayout(const WheelLayout& layout, const float deltaXDip,
                                              const float deltaYDip,
                                              const float angularMarginRadians,
                                              const float radialMarginDip) noexcept {
    return HitTestWheelLayout(std::span<const WheelLayoutSegment>{layout.segments}, deltaXDip,
                              deltaYDip, angularMarginRadians, radialMarginDip);
}

std::optional<std::size_t> HitTestWheelLayout(const std::span<const WheelLayoutSegment> segments,
                                              const float deltaXDip, const float deltaYDip,
                                              const float angularMarginRadians,
                                              const float radialMarginDip) noexcept {
    if (!std::isfinite(deltaXDip) || !std::isfinite(deltaYDip)) {
        return std::nullopt;
    }
    const float radius = std::hypot(deltaXDip, deltaYDip);
    const float angle = std::atan2(deltaYDip, deltaXDip);
    for (std::size_t offset = 0; offset < segments.size(); ++offset) {
        const std::size_t index = segments.size() - 1 - offset;
        const WheelLayoutSegment& segment = segments[index];
        if (radius + radialMarginDip < segment.innerRadiusDip ||
            radius - radialMarginDip > segment.outerRadiusDip ||
            AngularDistance(angle, segment.centreAngleRadians) >
                segment.halfSpanRadians + angularMarginRadians) {
            continue;
        }
        return index;
    }
    return std::nullopt;
}

const WheelLayoutSegment* FindWheelSegment(const WheelLayout& layout,
                                           const std::vector<std::size_t>& path) noexcept {
    const auto found =
        std::find_if(layout.segments.begin(), layout.segments.end(),
                     [&](const WheelLayoutSegment& segment) { return segment.path == path; });
    return found == layout.segments.end() ? nullptr : &*found;
}

}  // namespace orbitdeck
