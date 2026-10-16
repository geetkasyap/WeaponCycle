#pragma once

#include <string>

namespace orbitdeck {

// Derives the executable basename used to match an existing window to a wheel
// item: lowercase "name.exe" for an executable target, or empty when the target
// is not an executable (URL, URI protocol, or AUMID). A target with no window to
// match always launches instead. Pure and unit-testable.
[[nodiscard]] std::wstring ExecutableMatchName(const std::wstring& target);

// Returns true when a matching visible top-level window exists without
// activating or otherwise changing it.
[[nodiscard]] bool HasExistingWindow(const std::wstring& matchName) noexcept;
[[nodiscard]] bool ToggleExistingWindow(const std::wstring& matchName) noexcept;

// Finds the best existing top-level window whose owning process image basename
// equals matchName (case-insensitive) and activates it: restore if minimized,
// maximize when requested, then bring to the foreground; if Windows denies the
// foreground change, flash the taskbar instead. Returns true when a window was
// matched (so the caller must not also launch), false when none matched or
// matchName is empty. Never throws.
[[nodiscard]] bool ActivateExisting(const std::wstring& matchName, bool maximize) noexcept;

}  // namespace orbitdeck
