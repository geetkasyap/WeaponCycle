#include "overlay/SelectionEngine.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace orbitdeck {
namespace {

constexpr float kPi{std::numbers::pi_v<float>};
constexpr float kTwoPi{2.0F * kPi};

[[nodiscard]] float NormalizeAngle(const float angle) noexcept {
    const float normalized = std::fmod(angle, kTwoPi);
    return normalized < 0.0F ? normalized + kTwoPi : normalized;
}

}  // namespace

SelectionEngine::SelectionEngine(const std::size_t itemCount, const SegmentOrder order) noexcept
    : itemCount_(itemCount), order_(order) {}

void SelectionEngine::Configure(const std::size_t itemCount, const SegmentOrder order,
                                std::vector<std::uint8_t> enabledItems,
                                const float neutralRadiusDip,
                                const float activeRadiusDip) noexcept {
    itemCount_ = itemCount;
    order_ = order;
    neutralRadiusDip_ = std::max(0.0F, neutralRadiusDip);
    activeRadiusDip_ = std::max(neutralRadiusDip_, activeRadiusDip);
    if (enabledItems.size() == itemCount) {
        enabledItems_ = std::move(enabledItems);
    } else {
        itemCount_ = 0;
        enabledItems_.clear();
    }
    layout_ = {};
    Reset();
}

void SelectionEngine::Configure(WheelLayout layout, const float neutralRadiusDip,
                                const float activeRadiusDip) noexcept {
    itemCount_ = layout.segments.size();
    order_ = SegmentOrder::Clockwise;
    neutralRadiusDip_ = std::max(0.0F, neutralRadiusDip);
    activeRadiusDip_ = std::max(neutralRadiusDip_, activeRadiusDip);
    enabledItems_.clear();
    enabledItems_.reserve(itemCount_);
    for (const WheelLayoutSegment& segment : layout.segments) {
        enabledItems_.push_back(segment.enabled ? 1 : 0);
    }
    layout_ = std::move(layout);
    Reset();
}

void SelectionEngine::BeginGesture(const float deltaXDip, const float deltaYDip) noexcept {
    selected_.reset();
    const float distanceSquared = deltaXDip * deltaXDip + deltaYDip * deltaYDip;
    awaitingNeutralEntry_ =
        !std::isfinite(distanceSquared) || distanceSquared > activeRadiusDip_ * activeRadiusDip_;
}

SelectionEngine::Selection SelectionEngine::Update(const float deltaXDip,
                                                   const float deltaYDip) noexcept {
    if (itemCount_ == 0 || !std::isfinite(deltaXDip) || !std::isfinite(deltaYDip)) {
        selected_.reset();
        return selected_;
    }

    const float distanceSquared = deltaXDip * deltaXDip + deltaYDip * deltaYDip;
    if (awaitingNeutralEntry_) {
        if (distanceSquared > activeRadiusDip_ * activeRadiusDip_) {
            return selected_;
        }
        awaitingNeutralEntry_ = false;
    }
    if (distanceSquared < neutralRadiusDip_ * neutralRadiusDip_) {
        selected_.reset();
        return selected_;
    }
    if (distanceSquared <= activeRadiusDip_ * activeRadiusDip_) {
        return selected_;
    }

    if (!layout_.segments.empty()) {
        constexpr float angularHysteresis = kAngularHysteresisDegrees * kPi / 180.0F;
        constexpr float radialHysteresisDip{8.0F};
        const Selection candidate = HitTestWheelLayout(layout_, deltaXDip, deltaYDip);
        if (selected_.has_value() && candidate != selected_ &&
            *selected_ < layout_.segments.size()) {
            if (HitTestWheelLayout(
                    std::span<const WheelLayoutSegment>{&layout_.segments[*selected_], 1},
                    deltaXDip, deltaYDip, angularHysteresis, radialHysteresisDip)
                    .has_value()) {
                return selected_;
            }
        }
        if (candidate.has_value() && IsEnabled(*candidate)) {
            selected_ = candidate;
        } else {
            selected_.reset();
        }
        return selected_;
    }

    const float clockwiseAngle = NormalizeAngle(std::atan2(deltaYDip, deltaXDip) + kPi * 0.5F);
    const float angle =
        order_ == SegmentOrder::Clockwise ? clockwiseAngle : NormalizeAngle(-clockwiseAngle);
    const float segmentAngle = kTwoPi / static_cast<float>(itemCount_);
    const float halfSegment = segmentAngle * 0.5F;
    const std::size_t candidate =
        static_cast<std::size_t>((angle + halfSegment) / segmentAngle) % itemCount_;

    if (!selected_.has_value()) {
        if (IsEnabled(candidate)) {
            selected_ = candidate;
        }
        return selected_;
    }
    if (*selected_ == candidate) {
        return selected_;
    }

    const float selectedCentre = static_cast<float>(*selected_) * segmentAngle;
    const float deviation = std::abs(std::remainder(angle - selectedCentre, kTwoPi));
    constexpr float hysteresis = kAngularHysteresisDegrees * kPi / 180.0F;
    const float effectiveHysteresis = std::min(hysteresis, halfSegment);
    if (deviation > halfSegment + effectiveHysteresis) {
        if (IsEnabled(candidate)) {
            selected_ = candidate;
        } else {
            selected_.reset();
        }
    }
    return selected_;
}

bool SelectionEngine::IsEnabled(const std::size_t index) const noexcept {
    return index < itemCount_ && (enabledItems_.empty() || enabledItems_[index] != 0);
}

void SelectionEngine::Reset() noexcept {
    selected_.reset();
    awaitingNeutralEntry_ = false;
}

SelectionEngine::Selection SelectionEngine::Selected() const noexcept {
    return selected_;
}

bool SelectionEngine::GestureArmed() const noexcept {
    return !awaitingNeutralEntry_;
}

}  // namespace orbitdeck
