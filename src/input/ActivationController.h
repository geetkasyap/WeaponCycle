#pragma once

#include "config/Configuration.h"

namespace orbitdeck {

class ActivationController {
public:
    static constexpr int kHotkeyId{1};
    static constexpr UINT_PTR kLeftShiftHoldTimerId{2};
    static constexpr UINT kGestureReleasedMessage{WM_APP + 70};
    static constexpr UINT kGestureCancelledMessage{WM_APP + 71};

    ActivationController() = default;
    ~ActivationController() noexcept;

    ActivationController(const ActivationController&) = delete;
    ActivationController& operator=(const ActivationController&) = delete;

    [[nodiscard]] bool Register(HWND window, const ActivationConfig& config) noexcept;
    [[nodiscard]] bool Unregister() noexcept;
    [[nodiscard]] bool IsRegistered() const noexcept;
    [[nodiscard]] DWORD LastError() const noexcept;
    [[nodiscard]] bool CompleteLeftShiftHold() noexcept;
    [[nodiscard]] bool WatchGesture(bool enabled) noexcept;
    [[nodiscard]] bool AcceptsGestureMessage(WPARAM token) const noexcept;
    void ResetHeldState() noexcept;
    void HandleRawInput(HRAWINPUT input) noexcept;
    void HandleKeyboard(const RAWKEYBOARD& key) noexcept;

private:
    [[nodiscard]] bool RegisterRawKeyboard(bool enabled) noexcept;
    void ClearRegistration() noexcept;

    HWND window_{};
    DWORD lastError_{ERROR_SUCCESS};
    UINT leftShiftHoldThresholdMs_{240};
    bool registered_{false};
    bool usingRawActivation_{false};
    bool rawKeyboardRegistered_{false};
    bool leftShiftDown_{false};
    bool cancelledForPress_{false};
    bool holdOpened_{false};
    ActivationConfig config_{};
    bool watchingGesture_{false};
    bool releasePosted_{false};
    UINT_PTR gestureToken_{};
};

[[nodiscard]] bool IsTypingCancellationKey(DWORD virtualKey) noexcept;
[[nodiscard]] UINT ActivationVirtualKey(const RAWKEYBOARD& key) noexcept;
[[nodiscard]] bool IsActivationReleaseKey(const ActivationConfig& config,
                                          DWORD virtualKey) noexcept;

}  // namespace orbitdeck
