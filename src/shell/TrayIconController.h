#pragma once

#include <windows.h>

#include <string_view>

namespace orbitdeck {

class TrayIconController {
public:
    enum class Command : UINT {
        Preferences = 1001,
        OpenWheel = 1002,
        ReloadConfiguration = 1003,
        OpenConfigurationFolder = 1004,
        PauseShortcut = 1005,
        StartWithWindows = 1006,
        ReducedMotion = 1007,
        About = 1008,
        Exit = 1009,
        ConvertFile = 1010,
        ExplorerConvertMenu = 1011,
    };

    struct MenuState {
        bool canOpenWheel{false};
        bool shortcutPaused{false};
        bool startWithWindows{false};
        bool reducedMotion{false};
        bool canReloadConfiguration{false};
        bool canOpenConfigurationFolder{false};
        bool canChangeStartup{false};
        bool canChangeReducedMotion{false};
        bool conversionMenuEnabled{};
    };

    TrayIconController() = default;
    ~TrayIconController() noexcept;

    TrayIconController(const TrayIconController&) = delete;
    TrayIconController& operator=(const TrayIconController&) = delete;

    [[nodiscard]] bool Add(HWND owner, UINT callbackMessage) noexcept;
    [[nodiscard]] bool Restore() noexcept;
    void Remove() noexcept;
    [[nodiscard]] bool TryGetMenuAnchor(POINT& point) const noexcept;
    void ShowContextMenu(POINT position, MenuState state) const noexcept;
    [[nodiscard]] bool ShowBalloon(std::wstring_view title,
                                   std::wstring_view message) const noexcept;

private:
    [[nodiscard]] bool Install() noexcept;

    HWND owner_{};
    UINT callbackMessage_{};
    bool added_{false};
};

}  // namespace orbitdeck
