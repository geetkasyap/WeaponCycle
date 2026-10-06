#pragma once

#include "config/Configuration.h"

#include <iterator>
#include <string_view>

namespace orbitdeck {

enum ActionChoice : int {
    kActionApplication,
    kActionPackagedApp,
    kActionShellCommand,
    kActionClipboard,
    kActionUrl,
    kActionFile,
    kActionShortcut,
    kActionSystem,
    kActionClock,
    kActionFocus,
    kActionWindowsCalculator,
    kActionMagnifier,
    kActionNotepad,
    kActionScreenshots,
    kActionCaffeine,
    kActionScratchpad,
    kActionSystemGlance,
    kActionMirror,
    kActionMicCheck,
    kActionColorPicker,
    kActionJsonEditor,
    kActionBase64Text,
    kActionGuidGenerator,
    kActionJwtDecoder,
    kActionUrlCodec,
    kActionTimestampConverter,
    kActionTextTransform,
    kActionBase64Image,
    kActionNumberBase,
    kActionColorConverter,
    kActionHashCalculator,
    kActionRegexTester,
    kActionTextDiff,
    kActionFileConverter,
    kActionChoiceCount
};

struct EditorActionChoice {
    const wchar_t* label;
    const wchar_t* launchType{L"shell"};
    BuiltInTool tool{BuiltInTool::None};
    const wchar_t* systemTarget{L""};
};

inline constexpr EditorActionChoice kEditorActionChoices[]{
    {L"Application / command"},
    {L"Packaged app (AUMID)", L"aumid"},
    {L"Shell command", L"shellCommand"},
    {L"Clipboard snippet", L"clipboard"},
    {L"URL", L"url"},
    {L"File or folder", L"file"},
    {L"Keyboard shortcut", L"shortcut"},
    {L"System action", L"system"},
    {L"Windows Clock / Timer", L"system", BuiltInTool::None, L"timer"},
    {L"Windows Focus", L"system", BuiltInTool::None, L"focus"},
    {L"Windows Calculator", L"system", BuiltInTool::None, L"calculator"},
    {L"Windows Magnifier", L"system", BuiltInTool::None, L"magnifier"},
    {L"Windows Notepad", L"system", BuiltInTool::None, L"notepad"},
    {L"Screenshots folder", L"system", BuiltInTool::None, L"screenshots"},
    {L"Keep Awake", L"shell", BuiltInTool::Caffeine},
    {L"Scratchpad", L"shell", BuiltInTool::Scratchpad},
    {L"System Glance", L"shell", BuiltInTool::SystemGlance},
    {L"Mirror", L"shell", BuiltInTool::Mirror},
    {L"Mic Check", L"shell", BuiltInTool::MicCheck},
    {L"Color Picker", L"shell", BuiltInTool::ColorPicker},
    {L"JSON Editor", L"shell", BuiltInTool::JsonEditor},
    {L"Base64 Encoder / Decoder", L"shell", BuiltInTool::Base64Text},
    {L"GUID Generator", L"shell", BuiltInTool::GuidGenerator},
    {L"JWT Decoder", L"shell", BuiltInTool::JwtDecoder},
    {L"URL Encoder / Decoder", L"shell", BuiltInTool::UrlCodec},
    {L"Timestamp Converter", L"shell", BuiltInTool::TimestampConverter},
    {L"Text Transformation", L"shell", BuiltInTool::TextTransform},
    {L"Base64 Image Converter", L"shell", BuiltInTool::Base64Image},
    {L"Number Base Converter", L"shell", BuiltInTool::NumberBase},
    {L"Color Converter", L"shell", BuiltInTool::ColorConverter},
    {L"Hash Calculator", L"shell", BuiltInTool::HashCalculator},
    {L"Regex Tester", L"shell", BuiltInTool::RegexTester},
    {L"Text Diff", L"shell", BuiltInTool::TextDiff},
    {L"File Converter", L"shell", BuiltInTool::FileConverter}};
static_assert(std::size(kEditorActionChoices) == kActionChoiceCount);

inline bool SameActionToken(std::wstring_view left, std::wstring_view right) noexcept {
    return left.size() == right.size() &&
           ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                  static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

inline bool ActionUsesTarget(int action) noexcept {
    return action >= kActionApplication && action <= kActionSystem;
}

inline bool ActionSupportsWindowPolicy(int action) noexcept {
    return action == kActionApplication || action == kActionPackagedApp;
}

inline int EditorActionIndex(const WheelItem& item) noexcept {
    if (item.builtInTool == BuiltInTool::Timer) {
        return kActionClock;
    }
    if (item.builtInTool == BuiltInTool::Calculator) {
        return kActionWindowsCalculator;
    }
    if (item.IsBuiltIn()) {
        for (int index = kActionCaffeine; index < kActionChoiceCount; ++index) {
            if (kEditorActionChoices[index].tool == item.builtInTool) {
                return index;
            }
        }
        return kActionApplication;
    }
    if (item.launchKind == LaunchKind::Aumid) {
        return kActionPackagedApp;
    }
    if (SameActionToken(item.launchType, L"system")) {
        const std::wstring_view target =
            item.configuredTarget.empty() ? item.target : item.configuredTarget;
        if (SameActionToken(target, L"clock")) {
            return kActionClock;
        }
        for (int index = kActionClock; index <= kActionScreenshots; ++index) {
            if (SameActionToken(target, kEditorActionChoices[index].systemTarget)) {
                return index;
            }
        }
        return kActionSystem;
    }
    if (SameActionToken(item.launchType, L"path")) {
        return kActionFile;
    }
    for (int index = kActionShellCommand; index <= kActionShortcut; ++index) {
        if (SameActionToken(item.launchType, kEditorActionChoices[index].launchType)) {
            return index;
        }
    }
    return kActionApplication;
}

struct EditorWindowChoice {
    const wchar_t* label;
    ExistingWindowAction action;
    const wchar_t* configurationName;
};

inline constexpr EditorWindowChoice kEditorWindowChoices[]{
    {L"Focus", ExistingWindowAction::Focus, L"focus"},
    {L"Restore and focus", ExistingWindowAction::RestoreAndFocus, L"restoreAndFocus"},
    {L"Maximize and focus", ExistingWindowAction::MaximizeAndFocus, L"maximizeAndFocus"},
    {L"Always open new", ExistingWindowAction::OpenNewInstance, L"openNewInstance"},
    {L"Do nothing", ExistingWindowAction::DoNothing, L"doNothing"},
    {L"Toggle minimize", ExistingWindowAction::ToggleMinimize, L"toggleMinimize"}};

inline int EditorWindowIndex(ExistingWindowAction action) noexcept {
    for (std::size_t index = 0; index < std::size(kEditorWindowChoices); ++index) {
        if (kEditorWindowChoices[index].action == action) {
            return static_cast<int>(index);
        }
    }
    return 0;
}

}  // namespace orbitdeck
