#include "input/ActivationController.h"

namespace orbitdeck {
UINT ActivationVirtualKey(const RAWKEYBOARD& key) noexcept {
    if (key.VKey == VK_SHIFT) {
        return ::MapVirtualKeyW(key.MakeCode, MAPVK_VSC_TO_VK_EX);
    }
    if (key.VKey == VK_CONTROL)
        return (key.Flags & RI_KEY_E0) ? VK_RCONTROL : VK_LCONTROL;
    if (key.VKey == VK_MENU)
        return (key.Flags & RI_KEY_E0) ? VK_RMENU : VK_LMENU;
    return key.VKey;
}

bool IsActivationReleaseKey(const ActivationConfig& config, const DWORD key) noexcept {
    if (config.experimentalLeftShiftOnly)
        return key == VK_LSHIFT;
    if (key == config.virtualKey)
        return true;
    if ((config.modifiers & MOD_SHIFT) != 0 &&
        (key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT))
        return true;
    if ((config.modifiers & MOD_CONTROL) != 0 &&
        (key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL))
        return true;
    if ((config.modifiers & MOD_ALT) != 0 && (key == VK_MENU || key == VK_LMENU || key == VK_RMENU))
        return true;
    return (config.modifiers & MOD_WIN) != 0 && (key == VK_LWIN || key == VK_RWIN);
}

bool IsTypingCancellationKey(const DWORD virtualKey) noexcept {
    switch (virtualKey) {
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
            return false;
        default:
            return true;
    }
}

ActivationController::~ActivationController() noexcept {
    (void)Unregister();
}

bool ActivationController::Register(const HWND window, const ActivationConfig& config) noexcept {
    if (!Unregister()) {
        return false;
    }

    window_ = window;
    config_ = config;
    leftShiftHoldThresholdMs_ = config.leftShiftHoldThresholdMs;

    if (config.experimentalLeftShiftOnly) {
        if (!RegisterRawKeyboard(true)) {
            ClearRegistration();
            return false;
        }

        registered_ = true;
        usingRawActivation_ = true;
        lastError_ = ERROR_SUCCESS;
        return true;
    }

    if (!::RegisterHotKey(window, kHotkeyId, config.modifiers, config.virtualKey)) {
        lastError_ = ::GetLastError();
        ClearRegistration();
        return false;
    }

    registered_ = true;
    lastError_ = ERROR_SUCCESS;
    return true;
}

bool ActivationController::Unregister() noexcept {
    if (!registered_) {
        ClearRegistration();
        lastError_ = ERROR_SUCCESS;
        return true;
    }

    const bool stoppedWatching = WatchGesture(false);
    const DWORD watchError = lastError_;
    (void)::KillTimer(window_, kLeftShiftHoldTimerId);

    bool ok = stoppedWatching;
    DWORD error = stoppedWatching ? ERROR_SUCCESS : watchError;
    if (!RegisterRawKeyboard(false)) {
        ok = false;
        error = lastError_;
    }
    if (!usingRawActivation_ && !::UnregisterHotKey(window_, kHotkeyId)) {
        ok = false;
        error = ::GetLastError();
    }

    ClearRegistration();
    lastError_ = error;
    return ok;
}

bool ActivationController::WatchGesture(const bool enabled) noexcept {
    if (enabled == watchingGesture_)
        return true;
    if (enabled && !registered_) {
        lastError_ = ERROR_INVALID_STATE;
        return false;
    }
    if (!usingRawActivation_ && !RegisterRawKeyboard(enabled))
        return false;
    watchingGesture_ = enabled;
    releasePosted_ = false;
    if (enabled)
        ++gestureToken_;
    lastError_ = ERROR_SUCCESS;
    return true;
}

bool ActivationController::RegisterRawKeyboard(const bool enabled) noexcept {
    if (enabled == rawKeyboardRegistered_)
        return true;
    const RAWINPUTDEVICE device{0x01, 0x06, enabled ? DWORD{RIDEV_INPUTSINK} : DWORD{RIDEV_REMOVE},
                                enabled ? window_ : nullptr};
    if (!::RegisterRawInputDevices(&device, 1, sizeof(device))) {
        lastError_ = ::GetLastError();
        return false;
    }
    rawKeyboardRegistered_ = enabled;
    lastError_ = ERROR_SUCCESS;
    return true;
}

bool ActivationController::AcceptsGestureMessage(const WPARAM token) const noexcept {
    return watchingGesture_ && token == gestureToken_;
}

void ActivationController::ResetHeldState() noexcept {
    (void)WatchGesture(false);
    if (window_ != nullptr)
        (void)::KillTimer(window_, kLeftShiftHoldTimerId);
    leftShiftDown_ = false;
    cancelledForPress_ = false;
    holdOpened_ = false;
}

bool ActivationController::CompleteLeftShiftHold() noexcept {
    if (window_ != nullptr)
        (void)::KillTimer(window_, kLeftShiftHoldTimerId);
    if (!registered_ || !usingRawActivation_ || !leftShiftDown_ || cancelledForPress_ ||
        holdOpened_ || (::GetAsyncKeyState(VK_LSHIFT) & 0x8000) == 0) {
        return false;
    }

    holdOpened_ = true;
    return true;
}

bool ActivationController::IsRegistered() const noexcept {
    return registered_;
}

DWORD ActivationController::LastError() const noexcept {
    return lastError_;
}

void ActivationController::HandleRawInput(const HRAWINPUT input) noexcept {
    if (!rawKeyboardRegistered_)
        return;
    RAWINPUT data{};
    UINT bytes = sizeof(data);
    const UINT read = ::GetRawInputData(input, RID_INPUT, &data, &bytes, sizeof(RAWINPUTHEADER));
    if (read == static_cast<UINT>(-1) || read < sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD)) {
        lastError_ = ::GetLastError();
        ::OutputDebugStringW(L"Weapon Cycle: a raw input notification could not be read.\n");
        return;
    }
    if (data.header.dwType == RIM_TYPEKEYBOARD)
        HandleKeyboard(data.data.keyboard);
}

void ActivationController::HandleKeyboard(const RAWKEYBOARD& key) noexcept {
    if (!registered_ || !rawKeyboardRegistered_ || key.VKey == 0 || key.VKey >= 255)
        return;
    const bool keyUp = (key.Flags & RI_KEY_BREAK) != 0;
    const bool keyDown = !keyUp;
    const UINT virtualKey = ActivationVirtualKey(key);
    if (watchingGesture_ && !releasePosted_ &&
        ((keyUp && IsActivationReleaseKey(config_, virtualKey)) ||
         (keyDown && virtualKey == VK_ESCAPE))) {
        releasePosted_ =
            ::PostMessageW(window_, keyDown ? kGestureCancelledMessage : kGestureReleasedMessage,
                           gestureToken_, 0) != FALSE;
        if (!releasePosted_)
            lastError_ = ::GetLastError();
    }
    if (!usingRawActivation_)
        return;

    if (virtualKey == VK_LSHIFT) {
        if (keyDown && !leftShiftDown_) {
            leftShiftDown_ = true;
            cancelledForPress_ = false;
            holdOpened_ = false;
            if (::SetTimer(window_, kLeftShiftHoldTimerId, leftShiftHoldThresholdMs_, nullptr) ==
                0) {
                cancelledForPress_ = true;
                lastError_ = ::GetLastError();
            }
        } else if (keyUp) {
            (void)::KillTimer(window_, kLeftShiftHoldTimerId);
            leftShiftDown_ = false;
            cancelledForPress_ = false;
            holdOpened_ = false;
        }
        return;
    }

    if (keyDown && leftShiftDown_ && !holdOpened_ && IsTypingCancellationKey(virtualKey)) {
        cancelledForPress_ = true;
        (void)::KillTimer(window_, kLeftShiftHoldTimerId);
    }
}

void ActivationController::ClearRegistration() noexcept {
    window_ = nullptr;
    leftShiftHoldThresholdMs_ = 240;
    registered_ = false;
    usingRawActivation_ = false;
    rawKeyboardRegistered_ = false;
    leftShiftDown_ = false;
    cancelledForPress_ = false;
    holdOpened_ = false;
    watchingGesture_ = false;
    releasePosted_ = false;
}

}  // namespace orbitdeck
