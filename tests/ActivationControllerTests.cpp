#include "input/ActivationController.h"

#include <cstdio>

using orbitdeck::IsTypingCancellationKey;

namespace {
bool RawKeyboardRegistered(const HWND window) {
    RAWINPUTDEVICE devices[16]{};
    UINT count = 16;
    const UINT read = ::GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE));
    if (read == static_cast<UINT>(-1))
        return false;
    for (UINT i = 0; i < read; ++i) {
        if (devices[i].usUsagePage == 1 && devices[i].usUsage == 6 &&
            devices[i].hwndTarget == window && devices[i].dwFlags == RIDEV_INPUTSINK)
            return true;
    }
    return false;
}
}  // namespace

int main() {
    if (IsTypingCancellationKey(VK_LSHIFT))
        return 1;
    if (IsTypingCancellationKey(VK_RSHIFT))
        return 2;
    if (IsTypingCancellationKey(VK_CONTROL))
        return 3;
    if (IsTypingCancellationKey(VK_MENU))
        return 4;
    if (IsTypingCancellationKey(VK_LWIN))
        return 5;
    if (!IsTypingCancellationKey('A'))
        return 6;
    if (!IsTypingCancellationKey(VK_SPACE))
        return 7;
    if (!IsTypingCancellationKey(VK_ESCAPE))
        return 8;

    orbitdeck::ActivationConfig config;
    if (!orbitdeck::IsActivationReleaseKey(config, VK_SPACE))
        return 9;
    if (!orbitdeck::IsActivationReleaseKey(config, VK_LSHIFT))
        return 10;
    if (!orbitdeck::IsActivationReleaseKey(config, VK_RSHIFT))
        return 11;
    if (orbitdeck::IsActivationReleaseKey(config, VK_CONTROL))
        return 12;
    if (orbitdeck::IsActivationReleaseKey(config, 'A'))
        return 13;
    config.modifiers = MOD_CONTROL | MOD_ALT | MOD_WIN | MOD_NOREPEAT;
    if (!orbitdeck::IsActivationReleaseKey(config, VK_RCONTROL) ||
        !orbitdeck::IsActivationReleaseKey(config, VK_LMENU) ||
        !orbitdeck::IsActivationReleaseKey(config, VK_RWIN))
        return 14;
    if (orbitdeck::IsActivationReleaseKey(config, VK_SHIFT))
        return 15;
    config.experimentalLeftShiftOnly = true;
    if (!orbitdeck::IsActivationReleaseKey(config, VK_LSHIFT) ||
        orbitdeck::IsActivationReleaseKey(config, VK_RSHIFT) ||
        orbitdeck::IsActivationReleaseKey(config, VK_SPACE))
        return 16;
    orbitdeck::ActivationController activation;
    if (activation.WatchGesture(true))
        return 17;
    if (activation.AcceptsGestureMessage(0))
        return 18;
    activation.ResetHeldState();
    const HWND window =
        ::CreateWindowExW(0, L"STATIC", L"Activation test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                          ::GetModuleHandleW(nullptr), nullptr);
    if (window == nullptr)
        return 19;
    config = {};
    config.modifiers = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
    bool registered = false;
    for (UINT key = VK_F13; key <= VK_F24; ++key) {
        config.virtualKey = key;
        if (activation.Register(window, config)) {
            registered = true;
            break;
        }
        if (activation.LastError() != ERROR_HOTKEY_ALREADY_REGISTERED)
            return 20;
    }
    if (!registered)
        return 20;
    if (!activation.WatchGesture(true) || !activation.AcceptsGestureMessage(1))
        return 21;
    if (!RawKeyboardRegistered(window))
        return 28;
    RAWKEYBOARD input{};
    input.VKey = static_cast<USHORT>(config.virtualKey);
    input.Flags = RI_KEY_BREAK;
    activation.HandleKeyboard(input);
    MSG notification{};
    if (!::PeekMessageW(&notification, window,
                        orbitdeck::ActivationController::kGestureReleasedMessage,
                        orbitdeck::ActivationController::kGestureReleasedMessage, PM_REMOVE) ||
        notification.wParam != 1)
        return 29;
    activation.HandleKeyboard(input);
    if (::PeekMessageW(&notification, window,
                       orbitdeck::ActivationController::kGestureReleasedMessage,
                       orbitdeck::ActivationController::kGestureReleasedMessage, PM_REMOVE))
        return 30;
    if (!activation.WatchGesture(false) || activation.AcceptsGestureMessage(1))
        return 22;
    if (RawKeyboardRegistered(window))
        return 31;
    if (!activation.WatchGesture(true) || activation.AcceptsGestureMessage(1) ||
        !activation.AcceptsGestureMessage(2))
        return 23;
    input.VKey = VK_ESCAPE;
    input.Flags = RI_KEY_MAKE;
    activation.HandleKeyboard(input);
    if (!::PeekMessageW(&notification, window,
                        orbitdeck::ActivationController::kGestureCancelledMessage,
                        orbitdeck::ActivationController::kGestureCancelledMessage, PM_REMOVE) ||
        notification.wParam != 2)
        return 32;
    activation.ResetHeldState();
    if (activation.AcceptsGestureMessage(2) || !activation.IsRegistered())
        return 24;
    orbitdeck::ActivationController conflicting;
    if (conflicting.Register(window, config) || !activation.IsRegistered())
        return 25;
    if (!activation.Unregister() || activation.IsRegistered())
        return 26;
    if (!activation.Unregister())
        return 27;
    if (RawKeyboardRegistered(window))
        return 33;

    input = {};
    input.VKey = VK_SHIFT;
    input.MakeCode = 0x2A;
    if (orbitdeck::ActivationVirtualKey(input) != VK_LSHIFT)
        return 34;
    input.MakeCode = 0x36;
    if (orbitdeck::ActivationVirtualKey(input) != VK_RSHIFT)
        return 35;
    input.VKey = VK_CONTROL;
    input.Flags = RI_KEY_E0;
    if (orbitdeck::ActivationVirtualKey(input) != VK_RCONTROL)
        return 36;
    input.VKey = VK_MENU;
    if (orbitdeck::ActivationVirtualKey(input) != VK_RMENU)
        return 37;

    config.experimentalLeftShiftOnly = true;
    if (!activation.Register(window, config) || !RawKeyboardRegistered(window))
        return 38;
    if (!activation.WatchGesture(true))
        return 39;
    input = {};
    input.VKey = VK_SHIFT;
    input.MakeCode = 0x2A;
    input.Flags = RI_KEY_BREAK;
    activation.HandleKeyboard(input);
    if (!::PeekMessageW(&notification, window,
                        orbitdeck::ActivationController::kGestureReleasedMessage,
                        orbitdeck::ActivationController::kGestureReleasedMessage, PM_REMOVE) ||
        !activation.AcceptsGestureMessage(notification.wParam))
        return 40;
    if (!activation.WatchGesture(false) || !RawKeyboardRegistered(window))
        return 41;
    if (!activation.Unregister() || RawKeyboardRegistered(window))
        return 42;
    config.experimentalLeftShiftOnly = false;
    for (int gesture = 0; gesture < 50; ++gesture) {
        if (!activation.Register(window, config) || RawKeyboardRegistered(window))
            return 43;
        if (!activation.WatchGesture(true) || !RawKeyboardRegistered(window))
            return 44;
        activation.ResetHeldState();
        if (RawKeyboardRegistered(window) || activation.AcceptsGestureMessage(0))
            return 45;
        if (!activation.Unregister() || RawKeyboardRegistered(window))
            return 46;
    }
    (void)::DestroyWindow(window);

    std::puts("ActivationController tests passed");
    return 0;
}
