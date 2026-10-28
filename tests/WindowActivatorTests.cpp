// Proves ExecutableMatchName, the Phase 7 rule that decides whether a committed
// item can match an existing window. Executable targets yield a lowercase
// basename; URLs, URIs, and AUMIDs yield empty so the caller launches instead.
// ActivateExisting itself drives real windows and is covered by the manual
// smoke, not here. Plain return-code harness.

#include "window/WindowActivator.h"

#include <cstdio>
#include <string>

using orbitdeck::ExecutableMatchName;

int main() {
    // A bare executable name passes through, lowercased.
    if (ExecutableMatchName(L"notepad.exe") != L"notepad.exe") {
        return 1;
    }
    // A full path reduces to its basename.
    if (ExecutableMatchName(L"C:\\Windows\\System32\\notepad.exe") != L"notepad.exe") {
        return 2;
    }
    // Environment-style prefixes still split on the final backslash.
    if (ExecutableMatchName(L"%SystemRoot%\\notepad.exe") != L"notepad.exe") {
        return 3;
    }
    // Trimming and case-folding both apply.
    if (ExecutableMatchName(L"  Notepad.EXE  ") != L"notepad.exe") {
        return 4;
    }
    // Forward-slash paths split too.
    if (ExecutableMatchName(L"C:/tools/Foo.exe") != L"foo.exe") {
        return 5;
    }
    // URLs are not executables: no window to match, so empty.
    if (!ExecutableMatchName(L"https://example.com").empty()) {
        return 6;
    }
    if (!ExecutableMatchName(L"https://example.com/download/tool.exe").empty() ||
        !ExecutableMatchName(L"sample:tool.exe").empty()) {
        return 10;
    }
    // A name without the .exe extension is not a match target.
    if (!ExecutableMatchName(L"explorer").empty()) {
        return 7;
    }
    // An AUMID is not an executable.
    if (!ExecutableMatchName(L"Contoso.App_8wekyb3d8bbwe!App").empty()) {
        return 8;
    }
    // Empty stays empty.
    if (!ExecutableMatchName(L"").empty()) {
        return 9;
    }

    std::puts("WindowActivator tests passed");
    return 0;
}
