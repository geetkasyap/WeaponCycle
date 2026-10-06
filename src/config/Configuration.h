#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

#include "launch/Launcher.h"

namespace orbitdeck {

struct ActivationConfig {
    UINT modifiers{MOD_SHIFT | MOD_NOREPEAT};
    UINT virtualKey{VK_SPACE};
    bool experimentalLeftShiftOnly{false};
    UINT leftShiftHoldThresholdMs{240};
};

enum class PlacementMode {
    Cursor,
    MonitorCentre,
};

struct PlacementConfig {
    PlacementMode mode{PlacementMode::Cursor};
};

inline constexpr float kMinimumOuterExitRadiusDip{120.0F};

struct AppearanceConfig {
    std::wstring theme{L"orbitDark"};
    bool reducedMotion{false};
    float wheelDiameterDip{400.0F};
    float outerRadiusDip{156.0F};
    float innerRadiusDip{66.0F};
    float selectionThresholdDip{82.0F};
    float neutralThresholdDip{50.0F};
    float wheelOpacity{0.90F};
    float scrimOpacity{0.0F};
};

struct InteractionConfig {
    bool cancelOnRightClick{true};
    bool cancelOnOuterExit{true};
    bool numberQuickKeys{true};
    float outerExitRadiusDip{300.0F};
    UINT submenuDwellMs{350};
};

enum class BuiltInTool {
    None,
    Timer,
    Caffeine,
    Calculator,
    Scratchpad,
    SystemGlance,
    Mirror,
    MicCheck,
    ColorPicker,
    JsonEditor,
    Base64Text,
    GuidGenerator,
    JwtDecoder,
    UrlCodec,
    TimestampConverter,
    TextTransform,
    Base64Image,
    NumberBase,
    ColorConverter,
    HashCalculator,
    RegexTester,
    TextDiff,
    FileConverter,
};

struct LaunchCandidate {
    std::wstring configuredTarget;
    std::wstring target;
    std::wstring type{L"shell"};
    LaunchKind kind{LaunchKind::Shell};
};

enum class ExistingWindowAction {
    Focus,
    RestoreAndFocus,
    MaximizeAndFocus,
    OpenNewInstance,
    DoNothing,
    ToggleMinimize,
};

// One wheel entry. iconSource is a file path or executable; an empty or
// unreadable source falls back to a generated icon. Launch candidates are tried
// in configured order after the selected existing-window policy is applied.
// target/launchKind mirror the first candidate for editor and icon compatibility.
// A disabled item (enabled == false) or an empty target does nothing on commit.
struct WheelItem {
    std::wstring id;
    std::wstring label;
    wchar_t quickKey{};
    std::wstring iconSource;
    std::wstring configuredIconSource;
    bool iconAutomatic{true};
    std::wstring target;
    std::wstring configuredTarget;
    std::wstring launchType{L"shell"};
    std::wstring existingWindowAction{L"focus"};
    LaunchKind launchKind{LaunchKind::Shell};
    std::vector<LaunchCandidate> launchCandidates;
    ExistingWindowAction windowAction{ExistingWindowAction::Focus};
    BuiltInTool builtInTool{BuiltInTool::None};
    bool maximizeOnActivate{false};
    bool enabled{true};
    std::vector<WheelItem> children;

    [[nodiscard]] bool IsSubmenu() const noexcept {
        return !children.empty();
    }

    [[nodiscard]] bool IsBuiltIn() const noexcept {
        return builtInTool != BuiltInTool::None;
    }
};

// A safety ceiling, not a fixed wheel layout: every configured item up to this
// limit is rendered in its JSON order.
inline constexpr std::size_t kMaximumWheelItems{12};
inline constexpr std::size_t kMaximumWheelDepth{4};
inline constexpr std::size_t kMaximumTotalWheelItems{128};
inline constexpr std::size_t kMaximumLaunchCandidates{16};

struct Configuration {
    ActivationConfig activation{};
    PlacementConfig placement{};
    AppearanceConfig appearance{};
    InteractionConfig interaction{};
    std::vector<WheelItem> items;
};

}  // namespace orbitdeck
