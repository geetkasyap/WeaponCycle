#pragma once

#include "launch/Launcher.h"

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace orbitdeck {

struct ShortcutStroke {
    unsigned short virtualKey{};
    bool control{};
    bool alt{};
    bool shift{};
    bool windows{};
};

[[nodiscard]] bool IsDirectActionType(std::wstring_view type) noexcept;
[[nodiscard]] bool ParseShortcut(std::wstring_view text, ShortcutStroke& stroke) noexcept;
[[nodiscard]] std::wstring ResolveSystemActionTarget(std::wstring_view action);
[[nodiscard]] LaunchResult ExecuteDirectAction(std::wstring_view type, const std::wstring& value,
                                               HWND clipboardOwner = nullptr) noexcept;

}  // namespace orbitdeck
