// Proves the ResolveShellTarget logic that backs Phase 6 launching: empty
// targets are rejected, AUMIDs map to the shell:AppsFolder moniker, and
// environment variables expand. The ShellExecuteEx call itself is a thin OS
// syscall covered by the manual smoke, not here. Plain return-code harness.

#include "launch/Launcher.h"

#include <cstdio>
#include <string>

using orbitdeck::LaunchKind;
using orbitdeck::ResolveShellTarget;

int main() {
    // Empty or whitespace-only targets are invalid: nothing to launch.
    if (!ResolveShellTarget(L"", LaunchKind::Shell).empty()) {
        return 1;
    }
    if (!ResolveShellTarget(L"   \t ", LaunchKind::Shell).empty()) {
        return 2;
    }
    if (!ResolveShellTarget(L"", LaunchKind::Aumid).empty()) {
        return 3;
    }

    // A shell target with no variables passes through trimmed and unchanged.
    if (ResolveShellTarget(L"  notepad.exe  ", LaunchKind::Shell) != L"notepad.exe") {
        return 4;
    }

    // URLs and URI protocols are handed to ShellExecuteEx verbatim.
    if (ResolveShellTarget(L"https://example.com", LaunchKind::Shell) != L"https://example.com") {
        return 5;
    }

    // A packaged-app AUMID maps to the shell AppsFolder moniker.
    if (ResolveShellTarget(L"Contoso.App_8wekyb3d8bbwe!App", LaunchKind::Aumid) !=
        L"shell:AppsFolder\\Contoso.App_8wekyb3d8bbwe!App") {
        return 6;
    }

    // Environment variables expand (deterministic: %SystemRoot% always exists).
    const std::wstring expanded =
        ResolveShellTarget(L"%SystemRoot%\\notepad.exe", LaunchKind::Shell);
    if (expanded.empty() || expanded.find(L'%') != std::wstring::npos) {
        return 7;
    }

    std::puts("Launcher tests passed");
    return 0;
}
