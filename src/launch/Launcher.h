#pragma once

#include <string>

namespace orbitdeck {

// How a wheel item's target is handed to the shell. Every ordinary target
// (executable, App Paths name, shell target, URL, URI protocol, folder, file)
// launches identically through ShellExecuteEx; only a packaged application needs
// the shell:AppsFolder moniker, so two kinds cover every Phase 6 launch type.
enum class LaunchKind {
    Shell,
    Aumid,
};

struct LaunchResult {
    bool succeeded{false};
    std::wstring error;  // human-readable; empty on success
};

// Builds the string handed to ShellExecuteEx: trims, expands environment
// variables, and maps an AUMID to shell:AppsFolder\<AUMID>. Returns empty for an
// empty or whitespace-only target so the caller can treat it as "nothing to
// launch". Pure and unit-testable.
[[nodiscard]] std::wstring ResolveShellTarget(const std::wstring& target, LaunchKind kind);

// Launches target via ShellExecuteEx (verb "open"). The overlay must already be
// hidden. Never throws; returns a clear error message on failure.
[[nodiscard]] LaunchResult Launch(const std::wstring& target, LaunchKind kind) noexcept;

}  // namespace orbitdeck
