#include "settings/ActionChoices.h"

#include <cstdio>

int main() {
    using namespace orbitdeck;
    for (int index = 0; index < kActionChoiceCount; ++index) {
        const EditorActionChoice& choice = kEditorActionChoices[index];
        WheelItem item;
        item.launchType = choice.launchType;
        item.builtInTool = choice.tool;
        item.target = choice.systemTarget;
        item.launchKind = index == kActionPackagedApp ? LaunchKind::Aumid : LaunchKind::Shell;
        if (EditorActionIndex(item) != index || choice.label[0] == L'\0') {
            return 1;
        }
        if (ActionUsesTarget(index) != (index <= kActionSystem)) {
            return 2;
        }
    }
    WheelItem legacy;
    legacy.builtInTool = BuiltInTool::Timer;
    if (EditorActionIndex(legacy) != kActionClock) {
        return 3;
    }
    legacy.builtInTool = BuiltInTool::Calculator;
    if (EditorActionIndex(legacy) != kActionWindowsCalculator) {
        return 4;
    }
    legacy.builtInTool = BuiltInTool::None;
    legacy.launchType = L"SYSTEM";
    legacy.target = L"CLOCK";
    if (EditorActionIndex(legacy) != kActionClock) {
        return 5;
    }
    legacy.target = L"unsupported-system-token";
    if (EditorActionIndex(legacy) != kActionSystem) {
        return 6;
    }
    legacy.launchType = L"PATH";
    if (EditorActionIndex(legacy) != kActionFile) {
        return 7;
    }
    for (std::size_t index = 0; index < std::size(kEditorWindowChoices); ++index) {
        if (EditorWindowIndex(kEditorWindowChoices[index].action) != static_cast<int>(index)) {
            return 8;
        }
    }
    if (!ActionSupportsWindowPolicy(kActionApplication) ||
        !ActionSupportsWindowPolicy(kActionPackagedApp) ||
        ActionSupportsWindowPolicy(kActionShellCommand)) {
        return 9;
    }
    std::puts("Editor action/window catalogs and legacy mappings passed");
    return 0;
}
