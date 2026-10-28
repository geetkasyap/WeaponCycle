#include "actions/ActionExecutor.h"

#include <windows.h>

int main() {
    orbitdeck::ShortcutStroke stroke;
    if (!orbitdeck::ParseShortcut(L"Ctrl+Shift+K", stroke) || !stroke.control || !stroke.shift ||
        stroke.virtualKey != L'K') {
        return 1;
    }
    if (orbitdeck::ParseShortcut(L"Ctrl+Alt", stroke) || orbitdeck::ParseShortcut(L"A+B", stroke) ||
        orbitdeck::ParseShortcut(L"Ctrl++K", stroke)) {
        return 2;
    }
    if (!orbitdeck::ParseShortcut(L"Win+Shift+/", stroke) || !stroke.windows || !stroke.shift ||
        stroke.virtualKey != VK_OEM_2) {
        return 4;
    }
    if (orbitdeck::ResolveSystemActionTarget(L"Timer") != L"ms-clock:" ||
        orbitdeck::ResolveSystemActionTarget(L"calculator") != L"calculator:" ||
        orbitdeck::ResolveSystemActionTarget(L"Magnifier") != L"magnify.exe" ||
        !orbitdeck::IsDirectActionType(L"clipboard") || orbitdeck::IsDirectActionType(L"url")) {
        return 3;
    }
    return 0;
}
