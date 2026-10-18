#pragma once

#include "config/Configuration.h"
#include "input/ActivationController.h"
#include "overlay/OverlayAnimation.h"
#include "overlay/OverlayController.h"
#include "overlay/SelectionEngine.h"
#include "overlay/WheelLayout.h"
#include "platform/MainMessageWindow.h"
#include "platform/SingleInstanceCoordinator.h"
#include "shell/TrayIconController.h"
#include "settings/SettingsWindow.h"
#include "tools/MiniAppWindow.h"
#include "tools/DeveloperToolWindow.h"
#include "tools/FileConversionWindow.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace orbitdeck {

class OrbitDeckApp {
public:
    [[nodiscard]] int Run(HINSTANCE instance, bool background = false,
                          const std::wstring& conversionSource = {}) noexcept;

private:
    static constexpr UINT kTrayCallbackMessage{WM_APP + 1};
    static constexpr UINT kOpenWheelMessage{WM_APP + 2};
    static constexpr UINT kLaunchMessage{WM_APP + 3};
    static constexpr int kConvertHotkeyId{2};
    static constexpr UINT_PTR kVisibleTimerId{1};
    static constexpr UINT kVisibleTimerIntervalMs{16};

    static LRESULT RouteMessage(void* context, HWND window, UINT message, WPARAM wParam,
                                LPARAM lParam) noexcept;
    static bool RouteSaveSettings(void* context, const Configuration& configuration,
                                  bool replaceInvalidExisting, const std::string* replacementJson,
                                  std::wstring& error) noexcept;
    static void RoutePreviewSettings(void* context, const Configuration* configuration) noexcept;
    static void RouteTestSettings(void* context, const WheelItem* item) noexcept;

    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept;
    void HandleCommand(TrayIconController::Command command) noexcept;
    void HandleTrayCallback(LPARAM lParam) noexcept;
    void ShowPreferences() noexcept;
    void PostOpenWheelRequest(bool trackActivation = false) noexcept;
    void ShowOverlay(bool trackActivation) noexcept;
    void TrackOverlay(bool activationReleased = false, bool cancelRequested = false) noexcept;
    void EndOverlayGesture(bool cancelled) noexcept;
    void UpdateSelection(std::int64_t nowQpc, POINT cursor) noexcept;
    void UpdateSubmenuDwell(std::int64_t nowQpc, POINT cursor) noexcept;
    [[nodiscard]] bool TryActivateQuickKey(std::int64_t nowQpc, POINT cursor) noexcept;
    [[nodiscard]] bool OpenSubmenu(std::size_t index, POINT cursor, bool pointerInitiated) noexcept;
    void BeginCommit(std::size_t index, std::int64_t nowQpc) noexcept;
    void LaunchPendingItem() noexcept;
    void LaunchItem(const WheelItem& item) noexcept;
    void HideOverlay() noexcept;
    void PreviewSettings(const Configuration* configuration) noexcept;
    void RecoverOverlayGraphics() noexcept;
    void ToggleShortcut() noexcept;
    void ToggleStartWithWindows() noexcept;
    void LoadConfigurationAtStartup() noexcept;
    void ReloadConfiguration(bool reRegisterHotkey) noexcept;
    void MaybeReloadOnOpen() noexcept;
    void ApplyConfiguration() noexcept;
    [[nodiscard]] bool SaveSettings(const Configuration& configuration, bool replaceInvalidExisting,
                                    const std::string* replacementJson,
                                    std::wstring& error) noexcept;
    [[nodiscard]] bool ApplyCurrentMenu() noexcept;
    void SnapshotQuickKeys() noexcept;
    [[nodiscard]] std::wstring CurrentMenuTitle() const;
    void Shutdown() noexcept;
    void ShowHotkeyError() const noexcept;
    void ShowTrayMenu(POINT position) const noexcept;
    [[nodiscard]] static std::int64_t QueryNow() noexcept;

    Configuration configuration_{};
    Configuration previewConfiguration_{};
    MainMessageWindow messageWindow_{};
    SingleInstanceCoordinator singleInstance_{};
    TrayIconController trayIcon_{};
    SettingsWindow settings_{};
    MiniAppWindow miniApp_{};
    DeveloperToolWindow developerTool_{};
    FileConversionWindow fileConverter_{};
    ActivationController activation_{};
    OverlayController overlay_{};
    SelectionEngine selection_{};
    WheelLayout wheelLayout_{};
    OverlayAnimation animation_{};
    std::int64_t qpcFrequency_{1};
    std::int64_t selectionStartQpc_{0};
    std::int64_t lastTrackQpc_{0};
    long long configLastWrite_{0};
    std::uint64_t configContentHash_{0};
    bool configSnapshotValid_{false};
    SelectionEngine::Selection selectionFrom_{};
    SelectionEngine::Selection selectionShown_{};
    SelectionEngine::Selection committedItem_{};
    SelectionEngine::Selection submenuDwellItem_{};
    std::optional<WheelItem> committedAction_{};
    std::optional<WheelItem> pendingAction_{};
    std::vector<std::size_t> menuPath_{};
    std::array<bool, 256> quickKeyDown_{};
    std::vector<UINT> quickVirtualKeys_;
    std::int64_t submenuDwellStartQpc_{0};
    std::int64_t submenuRevealStartQpc_{0};
    bool selectionAnimating_{false};
    bool submenuAnimating_{false};
    bool insideNeutral_{true};
    bool outerExitArmed_{false};
    bool rightButtonDown_{false};
    bool enterDown_{false};
    UINT activationMessage_{};
    UINT taskbarCreatedMessage_{};
    bool overlayAvailable_{false};
    bool trackActivation_{false};
    bool shortcutPaused_{false};
    bool reopenRequested_{false};
    bool sessionNotifications_{false};
    bool previewActive_{false};
    HWND sourceWindow_{};
    std::wstring queuedConversionSource_;
    bool convertShortcutRegistered_{};
};

}  // namespace orbitdeck
