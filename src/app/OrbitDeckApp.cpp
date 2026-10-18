#include "app/OrbitDeckApp.h"

#include "actions/ActionExecutor.h"
#include "shell/FileSelection.h"
#include "config/ConfigLoader.h"
#include "launch/Launcher.h"
#include "tools/DeveloperTools.h"
#include "window/WindowActivator.h"

#include <shellapi.h>
#include <shlobj.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orbitdeck {
namespace {

bool IsKeyDown(const int virtualKey) noexcept {
    return (::GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

UINT QuickVirtualKey(const wchar_t key) noexcept {
    if (key == L'\0')
        return 0;
    const SHORT mapped = ::VkKeyScanW(key);
    return mapped == -1 ? 0 : LOBYTE(mapped);
}

bool IsActivationHeld(const ActivationConfig& activation) noexcept {
    if (activation.experimentalLeftShiftOnly) {
        return IsKeyDown(VK_LSHIFT);
    }
    if ((activation.modifiers & MOD_CONTROL) != 0 && !IsKeyDown(VK_CONTROL)) {
        return false;
    }
    if ((activation.modifiers & MOD_ALT) != 0 && !IsKeyDown(VK_MENU)) {
        return false;
    }
    if ((activation.modifiers & MOD_SHIFT) != 0 && !IsKeyDown(VK_SHIFT)) {
        return false;
    }
    if ((activation.modifiers & MOD_WIN) != 0 && !IsKeyDown(VK_LWIN) && !IsKeyDown(VK_RWIN)) {
        return false;
    }
    return IsKeyDown(static_cast<int>(activation.virtualKey));
}

bool SameActivation(const ActivationConfig& left, const ActivationConfig& right) noexcept {
    return left.modifiers == right.modifiers && left.virtualKey == right.virtualKey &&
           left.experimentalLeftShiftOnly == right.experimentalLeftShiftOnly &&
           left.leftShiftHoldThresholdMs == right.leftShiftHoldThresholdMs;
}

bool ContainsBuiltIn(const std::vector<WheelItem>& items, const BuiltInTool tool) noexcept {
    for (const WheelItem& item : items) {
        if (!item.enabled)
            continue;
        if (item.builtInTool == tool)
            return true;
        if (ContainsBuiltIn(item.children, tool))
            return true;
    }
    return false;
}

const WheelItem* ItemAtPath(const std::vector<WheelItem>& rootItems,
                            const std::vector<std::size_t>& path) noexcept {
    const std::vector<WheelItem>* items = &rootItems;
    const WheelItem* item = nullptr;
    for (const std::size_t index : path) {
        if (index >= items->size())
            return nullptr;
        item = &(*items)[index];
        items = &item->children;
    }
    return item;
}

bool PathHasPrefix(const std::vector<std::size_t>& path,
                   const std::vector<std::size_t>& prefix) noexcept {
    return prefix.size() <= path.size() && std::equal(prefix.begin(), prefix.end(), path.begin());
}

constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"OrbitDeck";

std::wstring ExecutablePath() {
    std::wstring path(512, L'\0');
    while (path.size() <= 32768) {
        const DWORD length =
            ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0)
            return {};
        if (length < path.size() - 1) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
    return {};
}

std::wstring StartupCommand() {
    const std::wstring path = ExecutablePath();
    return path.empty() ? std::wstring{} : L"\"" + path + L"\" --background";
}

bool StartWithWindowsEnabled() noexcept {
    HKEY key{};
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    LONG result = ::RegQueryValueExW(key, kRunValueName, nullptr, &type, nullptr, &bytes);
    std::wstring value;
    if (result == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) &&
        bytes >= sizeof(wchar_t) && bytes <= 64 * 1024 && bytes % sizeof(wchar_t) == 0) {
        value.resize(bytes / sizeof(wchar_t));
        result = ::RegQueryValueExW(key, kRunValueName, nullptr, &type,
                                    reinterpret_cast<BYTE*>(value.data()), &bytes);
        if (!value.empty() && value.back() == L'\0')
            value.pop_back();
    }
    (void)::RegCloseKey(key);
    const std::wstring expected = StartupCommand();
    return result == ERROR_SUCCESS && !expected.empty() &&
           ::CompareStringOrdinal(value.c_str(), static_cast<int>(value.size()), expected.c_str(),
                                  static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL;
}

bool SetStartWithWindows(const bool enabled, std::wstring& error) noexcept {
    HKEY key{};
    const LONG open =
        ::RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, REG_OPTION_NON_VOLATILE,
                          KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (open != ERROR_SUCCESS) {
        error = L"Windows denied access to the current user's startup list.";
        return false;
    }
    LONG result = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring command = StartupCommand();
        if (command.empty()) {
            result = ERROR_FILE_NOT_FOUND;
        } else {
            result = ::RegSetValueExW(key, kRunValueName, 0, REG_SZ,
                                      reinterpret_cast<const BYTE*>(command.c_str()),
                                      static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        }
    } else {
        result = ::RegDeleteValueW(key, kRunValueName);
        if (result == ERROR_FILE_NOT_FOUND)
            result = ERROR_SUCCESS;
    }
    (void)::RegCloseKey(key);
    if (result != ERROR_SUCCESS) {
        error = enabled
                    ? L"Weapon Cycle could not be added to the current user's startup list."
                    : L"Weapon Cycle could not be removed from the current user's startup list.";
        return false;
    }
    return true;
}

bool SystemAnimationsEnabled() noexcept {
    BOOL enabled = TRUE;
    return !::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) || enabled != FALSE;
}

}  // namespace

int OrbitDeckApp::Run(const HINSTANCE instance, const bool background,
                      const std::wstring& conversionSource) noexcept {
    LARGE_INTEGER frequency{};
    if (::QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
        qpcFrequency_ = frequency.QuadPart;
    }

    activationMessage_ = SingleInstanceCoordinator::ActivationMessage();
    if (activationMessage_ == 0 ||
        !messageWindow_.Create(instance, &OrbitDeckApp::RouteMessage, this)) {
        (void)::MessageBoxW(nullptr, L"Weapon Cycle could not create its Windows message endpoint.",
                            L"Weapon Cycle", MB_OK | MB_ICONERROR | MB_TASKMODAL);
        return 1;
    }

    switch (singleInstance_.Start(!background && conversionSource.empty())) {
        case SingleInstanceCoordinator::StartResult::ExistingSignaled:
            if (!conversionSource.empty() &&
                !SingleInstanceCoordinator::ForwardFile(conversionSource)) {
                (void)::MessageBoxW(
                    nullptr,
                    L"The running Weapon Cycle did not accept the file. Close an older build "
                    L"or finish startup, then try again.",
                    L"Weapon Cycle", MB_OK | MB_ICONERROR);
                return 1;
            }
            return 0;
        case SingleInstanceCoordinator::StartResult::Error:
            (void)::MessageBoxW(nullptr,
                                L"Weapon Cycle could not establish single-instance coordination.",
                                L"Weapon Cycle", MB_OK | MB_ICONERROR | MB_TASKMODAL);
            return 1;
        case SingleInstanceCoordinator::StartResult::Primary:
            break;
    }

    taskbarCreatedMessage_ = ::RegisterWindowMessageW(L"TaskbarCreated");
    if (taskbarCreatedMessage_ == 0 ||
        !trayIcon_.Add(messageWindow_.Handle(), kTrayCallbackMessage)) {
        (void)::MessageBoxW(nullptr, L"Weapon Cycle could not create its notification-area icon.",
                            L"Weapon Cycle", MB_OK | MB_ICONERROR | MB_TASKMODAL);
        return 1;
    }

    LoadConfigurationAtStartup();
    sessionNotifications_ =
        ::WTSRegisterSessionNotification(messageWindow_.Handle(), NOTIFY_FOR_THIS_SESSION) != FALSE;
    if (!sessionNotifications_) {
        (void)trayIcon_.ShowBalloon(
            L"Session notifications unavailable",
            L"Windows could not enable lock/unlock notifications for the wheel.");
    }
    if (!settings_.Create(instance, messageWindow_.Handle(), &OrbitDeckApp::RouteSaveSettings,
                          &OrbitDeckApp::RoutePreviewSettings, &OrbitDeckApp::RouteTestSettings,
                          this)) {
        (void)trayIcon_.ShowBalloon(L"Preferences unavailable",
                                    L"Weapon Cycle could not create its settings window.");
    }
    if (!miniApp_.Create(instance, messageWindow_.Handle())) {
        (void)trayIcon_.ShowBalloon(L"Built-in tools unavailable",
                                    L"Weapon Cycle could not create its mini-app window.");
    }
    if (!developerTool_.Create(instance, messageWindow_.Handle())) {
        (void)trayIcon_.ShowBalloon(L"Developer tools unavailable",
                                    L"Weapon Cycle could not create its developer utility window.");
    }
    if (!fileConverter_.Create(instance, messageWindow_.Handle())) {
        (void)trayIcon_.ShowBalloon(L"File conversion unavailable",
                                    L"Weapon Cycle could not create its file conversion window.");
    }

    overlayAvailable_ = overlay_.Create(instance);
    if (!overlayAvailable_) {
        (void)trayIcon_.ShowBalloon(L"Graphics unavailable",
                                    L"Weapon Cycle could not initialize the radial overlay.");
    }

    ApplyConfiguration();

    if (!activation_.Register(messageWindow_.Handle(), configuration_.activation)) {
        ShowHotkeyError();
    }
    convertShortcutRegistered_ =
        ::RegisterHotKey(messageWindow_.Handle(), kConvertHotkeyId,
                         MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C') != FALSE;
    if (!convertShortcutRegistered_) {
        (void)trayIcon_.ShowBalloon(
            L"File shortcut unavailable",
            L"Ctrl+Alt+C is used by another app. Convert files from Preferences, the tray, "
            L"or by dropping a file onto WeaponCycle.exe.");
    }
    if (!conversionSource.empty()) {
        POINT cursor{};
        (void)::GetCursorPos(&cursor);
        fileConverter_.Show(cursor, conversionSource);
    } else if (!background)
        ShowPreferences();

    MSG message{};
    int exitCode = 0;
    for (;;) {
        const int result = static_cast<int>(::GetMessageW(&message, nullptr, 0, 0));
        if (result == 0) {
            exitCode = static_cast<int>(message.wParam);
            break;
        }
        if (result == -1) {
            exitCode = 1;
            break;
        }

        if (settings_.IsVisible() && ::IsDialogMessageW(settings_.Handle(), &message)) {
            continue;
        }
        if (miniApp_.IsVisible() && ::IsDialogMessageW(miniApp_.Handle(), &message)) {
            continue;
        }
        if (fileConverter_.IsVisible() && ::IsDialogMessageW(fileConverter_.Handle(), &message)) {
            continue;
        }
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    Shutdown();
    return exitCode;
}

LRESULT OrbitDeckApp::RouteMessage(void* const context, const HWND window, const UINT message,
                                   const WPARAM wParam, const LPARAM lParam) noexcept {
    auto* const app = static_cast<OrbitDeckApp*>(context);
    return app != nullptr ? app->HandleMessage(window, message, wParam, lParam)
                          : ::DefWindowProcW(window, message, wParam, lParam);
}

bool OrbitDeckApp::RouteSaveSettings(void* const context, const Configuration& configuration,
                                     const bool replaceInvalidExisting,
                                     const std::string* const replacementJson,
                                     std::wstring& error) noexcept {
    auto* const app = static_cast<OrbitDeckApp*>(context);
    return app != nullptr &&
           app->SaveSettings(configuration, replaceInvalidExisting, replacementJson, error);
}

void OrbitDeckApp::RoutePreviewSettings(void* const context,
                                        const Configuration* const configuration) noexcept {
    auto* const app = static_cast<OrbitDeckApp*>(context);
    if (app != nullptr)
        app->PreviewSettings(configuration);
}

void OrbitDeckApp::RouteTestSettings(void* const context, const WheelItem* const item) noexcept {
    auto* const app = static_cast<OrbitDeckApp*>(context);
    if (app != nullptr && item != nullptr) {
        app->LaunchItem(*item);
    }
}

LRESULT OrbitDeckApp::HandleMessage(const HWND window, const UINT message, const WPARAM wParam,
                                    const LPARAM lParam) noexcept {
    if (message == activationMessage_) {
        ShowPreferences();
        return 0;
    }
    if (message == taskbarCreatedMessage_) {
        (void)trayIcon_.Restore();
        return 0;
    }

    switch (message) {
        case WM_INPUT:
            activation_.HandleRawInput(reinterpret_cast<HRAWINPUT>(lParam));
            return ::DefWindowProcW(window, message, wParam, lParam);
        case WM_COPYDATA: {
            const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if (data == nullptr || data->dwData != SingleInstanceCoordinator::kConvertFileTag ||
                data->lpData == nullptr || data->cbData < 2 * sizeof(wchar_t) ||
                data->cbData > 32768 * sizeof(wchar_t) || data->cbData % sizeof(wchar_t) != 0)
                return FALSE;
            const auto* path = static_cast<const wchar_t*>(data->lpData);
            const std::size_t count = data->cbData / sizeof(wchar_t);
            if (path[count - 1] != L'\0' || wcsnlen_s(path, count) != count - 1)
                return FALSE;
            if (!queuedConversionSource_.empty())
                return FALSE;
            queuedConversionSource_.assign(path, count - 1);
            if (::PostMessageW(window, WM_COMMAND,
                               static_cast<WPARAM>(TrayIconController::Command::ConvertFile), 0))
                return TRUE;
            queuedConversionSource_.clear();
            return FALSE;
        }
        case WM_HOTKEY:
            if (static_cast<int>(wParam) == kConvertHotkeyId) {
                sourceWindow_ = ::GetForegroundWindow();
                HideOverlay();
                POINT cursor{};
                (void)::GetCursorPos(&cursor);
                fileConverter_.Show(cursor, {}, sourceWindow_);
                return 0;
            }
            if (static_cast<int>(wParam) == ActivationController::kHotkeyId) {
                ShowOverlay(true);
            }
            return 0;

        case ActivationController::kGestureReleasedMessage:
        case ActivationController::kGestureCancelledMessage:
            if (activation_.AcceptsGestureMessage(wParam)) {
                EndOverlayGesture(message == ActivationController::kGestureCancelledMessage);
            }
            return 0;

        case kOpenWheelMessage:
            ShowOverlay(wParam != 0);
            return 0;

        case kLaunchMessage:
            LaunchPendingItem();
            return 0;

        case WM_TIMER:
            if (wParam == ActivationController::kLeftShiftHoldTimerId) {
                if (activation_.CompleteLeftShiftHold()) {
                    PostOpenWheelRequest(true);
                }
                return 0;
            }
            if (wParam == kVisibleTimerId) {
                TrackOverlay();
                return 0;
            }
            break;

        case WM_DISPLAYCHANGE:
            HideOverlay();
            return 0;

        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_LOCK || wParam == WTS_SESSION_LOGOFF ||
                wParam == WTS_CONSOLE_DISCONNECT || wParam == WTS_REMOTE_DISCONNECT) {
                HideOverlay();
                activation_.ResetHeldState();
            }
            return 0;

        case WM_POWERBROADCAST:
            if (wParam == PBT_APMSUSPEND || wParam == PBT_APMRESUMEAUTOMATIC ||
                wParam == PBT_APMRESUMESUSPEND) {
                HideOverlay();
                activation_.ResetHeldState();
            }
            return TRUE;

        case WM_CANCELMODE:
            HideOverlay();
            activation_.ResetHeldState();
            return 0;

        case WM_SETTINGCHANGE:
            if (wParam == SPI_SETWORKAREA) {
                HideOverlay();
                return 0;
            }
            break;

        case kTrayCallbackMessage:
            HandleTrayCallback(lParam);
            return 0;

        case WM_COMMAND:
            if (HIWORD(wParam) == 0) {
                HandleCommand(static_cast<TrayIconController::Command>(LOWORD(wParam)));
                return 0;
            }
            break;

        case WM_QUERYENDSESSION:
            return miniApp_.PrepareForShutdown() ? TRUE : FALSE;

        case WM_ENDSESSION:
            if (wParam != FALSE) {
                ::PostQuitMessage(0);
            }
            return 0;

        case WM_CLOSE:
            ::PostQuitMessage(0);
            return 0;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    return ::DefWindowProcW(window, message, wParam, lParam);
}

void OrbitDeckApp::HandleCommand(const TrayIconController::Command command) noexcept {
    switch (command) {
        case TrayIconController::Command::Preferences:
            ShowPreferences();
            break;
        case TrayIconController::Command::OpenWheel:
            PostOpenWheelRequest();
            break;
        case TrayIconController::Command::ConvertFile: {
            POINT cursor{};
            (void)::GetCursorPos(&cursor);
            std::wstring source = std::move(queuedConversionSource_);
            queuedConversionSource_.clear();
            fileConverter_.Show(cursor, source, source.empty() ? sourceWindow_ : nullptr);
            break;
        }
        case TrayIconController::Command::ExplorerConvertMenu: {
            const bool enable = !ConversionContextMenuEnabled();
            if (::MessageBoxW(
                    settings_.IsVisible() ? settings_.Handle() : messageWindow_.Handle(),
                    enable ? L"Add 'Convert with Weapon Cycle' to your file right-click menu?\n\n"
                             L"On Windows 11 it appears under Show more options. Default file "
                             L"opening is unchanged."
                           : L"Remove Weapon Cycle's file right-click menu entry?",
                    L"Explorer conversion menu",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                std::wstring error;
                if (!SetConversionContextMenu(enable, error)) {
                    (void)trayIcon_.ShowBalloon(L"Explorer menu not changed", error);
                }
            }
            break;
        }
        case TrayIconController::Command::PauseShortcut:
            ToggleShortcut();
            break;
        case TrayIconController::Command::About:
            (void)::MessageBoxW(messageWindow_.Handle(),
                                L"Weapon Cycle\nNative radial action launcher\n\n"
                                L"Apps, nested actions, Windows tools, and developer utilities",
                                L"About Weapon Cycle", MB_OK | MB_ICONINFORMATION);
            break;
        case TrayIconController::Command::Exit:
            if (miniApp_.PrepareForShutdown()) {
                ::PostQuitMessage(0);
            } else {
                (void)trayIcon_.ShowBalloon(L"Scratchpad not saved",
                                            L"Copy the Scratchpad text or free disk space before "
                                            L"exiting.");
            }
            break;
        case TrayIconController::Command::ReloadConfiguration:
            ReloadConfiguration(true);
            break;
        case TrayIconController::Command::OpenConfigurationFolder: {
            const std::wstring folder = ConfigFolderPath();
            if (!folder.empty()) {
                ::SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
                (void)::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr,
                                      SW_SHOWNORMAL);
            }
            break;
        }
        case TrayIconController::Command::StartWithWindows:
            ToggleStartWithWindows();
            break;
        case TrayIconController::Command::ReducedMotion:
            break;
    }
}

void OrbitDeckApp::HandleTrayCallback(const LPARAM lParam) noexcept {
    if (::GetForegroundWindow() != settings_.Handle() &&
        ::GetForegroundWindow() != fileConverter_.Handle()) {
        sourceWindow_ = ::GetForegroundWindow();
    }
    const UINT event = LOWORD(static_cast<DWORD_PTR>(lParam));
    switch (event) {
        case WM_LBUTTONDBLCLK:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            ShowPreferences();
            break;
        case WM_RBUTTONUP: {
            POINT cursor{};
            if (::GetCursorPos(&cursor)) {
                ShowTrayMenu(cursor);
            }
            break;
        }
        case WM_CONTEXTMENU: {
            POINT anchor{};
            if (trayIcon_.TryGetMenuAnchor(anchor)) {
                ShowTrayMenu(anchor);
            }
            break;
        }
        default:
            break;
    }
}

void OrbitDeckApp::ShowPreferences() noexcept {
    HideOverlay();
    if (!settings_.IsVisible()) {
        MaybeReloadOnOpen();
    }
    settings_.Show(configuration_);
}

void OrbitDeckApp::PostOpenWheelRequest(const bool trackActivation) noexcept {
    if (!::PostMessageW(messageWindow_.Handle(), kOpenWheelMessage, trackActivation ? 1 : 0, 0)) {
        (void)trayIcon_.ShowBalloon(L"Wheel request failed",
                                    L"Windows could not queue the wheel. Try the shortcut again.");
    }
}

void OrbitDeckApp::ShowOverlay(const bool trackActivation) noexcept {
    if (!overlay_.IsVisible() || previewActive_)
        sourceWindow_ = ::GetForegroundWindow();
    if (!overlayAvailable_) {
        RecoverOverlayGraphics();
        if (!overlayAvailable_)
            return;
    }
    if (trackActivation && !IsActivationHeld(configuration_.activation)) {
        if (settings_.IsVisible())
            settings_.UpdatePreview();
        return;
    }
    if (previewActive_)
        HideOverlay();
    if (overlay_.IsVisible()) {
        if (trackActivation) {
            if (animation_.IsClosing()) {
                reopenRequested_ = true;
            } else {
                trackActivation_ = true;
            }
        } else {
            HideOverlay();
            if (settings_.IsVisible())
                settings_.UpdatePreview();
        }
        return;
    }

    if (!settings_.IsVisible())
        MaybeReloadOnOpen();
    if (trackActivation && !IsActivationHeld(configuration_.activation)) {
        return;
    }
    overlay_.PrepareItems(configuration_.items);
    menuPath_.clear();
    if (!ApplyCurrentMenu()) {
        overlayAvailable_ = false;
        (void)trayIcon_.ShowBalloon(L"Wheel unavailable",
                                    L"Weapon Cycle could not prepare the configured wheel.");
        return;
    }

    POINT cursor{};
    if (!::GetCursorPos(&cursor)) {
        return;
    }
    if (!overlay_.ShowAt(cursor, configuration_.placement.mode, trackActivation)) {
        RecoverOverlayGraphics();
        if (!overlayAvailable_ || !ApplyCurrentMenu() ||
            !overlay_.ShowAt(cursor, configuration_.placement.mode, trackActivation)) {
            overlayAvailable_ = false;
            (void)trayIcon_.ShowBalloon(L"Graphics unavailable",
                                        L"Weapon Cycle could not display the radial overlay.");
            return;
        }
    }

    if (!overlay_.IsVisible()) {
        overlayAvailable_ = false;
        (void)trayIcon_.ShowBalloon(L"Graphics unavailable",
                                    L"Weapon Cycle could not display the radial overlay.");
        return;
    }

    const D2D1_POINT_2F centre = overlay_.WheelCentre();
    const float pixelsPerDip = overlay_.PixelsPerDip();
    selection_.BeginGesture((static_cast<float>(cursor.x) - centre.x) / pixelsPerDip,
                            (static_cast<float>(cursor.y) - centre.y) / pixelsPerDip);
    outerExitArmed_ = selection_.GestureArmed();
    const float initialDeltaX = (static_cast<float>(cursor.x) - centre.x) / pixelsPerDip;
    const float initialDeltaY = (static_cast<float>(cursor.y) - centre.y) / pixelsPerDip;
    insideNeutral_ = initialDeltaX * initialDeltaX + initialDeltaY * initialDeltaY <
                     configuration_.appearance.neutralThresholdDip *
                         configuration_.appearance.neutralThresholdDip;

    const std::int64_t now = QueryNow();
    lastTrackQpc_ = now;
    animation_.Configure(configuration_.appearance.reducedMotion || !SystemAnimationsEnabled(),
                         qpcFrequency_);
    animation_.Open(now);
    selectionShown_ = selection_.Selected();
    selectionFrom_ = selectionShown_;
    selectionAnimating_ = false;
    selectionStartQpc_ = now;
    rightButtonDown_ = IsKeyDown(VK_RBUTTON);
    enterDown_ = IsKeyDown(VK_RETURN);
    SnapshotQuickKeys();

    trackActivation_ = trackActivation;
    if (trackActivation && !activation_.WatchGesture(true)) {
        (void)trayIcon_.ShowBalloon(
            L"Using fallback input tracking",
            L"Windows denied raw keyboard notifications. The visible wheel will "
            L"use its normal input timer instead.");
    }
    if (::SetTimer(messageWindow_.Handle(), kVisibleTimerId, kVisibleTimerIntervalMs, nullptr) ==
        0) {
        overlayAvailable_ = false;
        HideOverlay();
        (void)trayIcon_.ShowBalloon(L"Wheel unavailable",
                                    L"Weapon Cycle could not start visible input tracking.");
    }
}

void OrbitDeckApp::TrackOverlay(const bool activationReleased,
                                const bool cancelRequested) noexcept {
    if (previewActive_)
        return;
    if (!overlayAvailable_ || !overlay_.IsVisible()) {
        HideOverlay();
        return;
    }

    const std::int64_t now = QueryNow();
    if (lastTrackQpc_ != 0 &&
        static_cast<double>(now - lastTrackQpc_) / static_cast<double>(qpcFrequency_) > 1.0) {
        HideOverlay();
        return;
    }
    lastTrackQpc_ = now;

    // A display or DPI change invalidates the surface: close immediately with
    // no animation so nothing is drawn against stale geometry.
    if (overlay_.CancellationRequested()) {
        HideOverlay();
        if (settings_.IsVisible())
            settings_.UpdatePreview();
        return;
    }

    const OverlayAnimation::Phase phaseBefore = animation_.phase();

    if (!animation_.IsClosing()) {
        POINT cursor{};
        const bool haveCursor = ::GetCursorPos(&cursor) != FALSE;
        if (!haveCursor) {
            HideOverlay();
            return;
        }
        const bool rightButtonNow = IsKeyDown(VK_RBUTTON);
        const bool rightButtonPressed = rightButtonNow && !rightButtonDown_;
        rightButtonDown_ = rightButtonNow;
        const bool enterNow = IsKeyDown(VK_RETURN);
        const bool enterPressed = enterNow && !enterDown_;
        enterDown_ = enterNow;

        if (haveCursor) {
            UpdateSelection(now, cursor);
        }

        bool outerExit = false;
        if (haveCursor && outerExitArmed_ && configuration_.interaction.cancelOnOuterExit) {
            const float pixelsPerDip = overlay_.PixelsPerDip();
            if (pixelsPerDip > 0.0F) {
                const D2D1_POINT_2F centre = overlay_.WheelCentre();
                const float deltaX = (static_cast<float>(cursor.x) - centre.x) / pixelsPerDip;
                const float deltaY = (static_cast<float>(cursor.y) - centre.y) / pixelsPerDip;
                const float radius = std::max(configuration_.interaction.outerExitRadiusDip,
                                              wheelLayout_.requiredRadiusDip + 18.0F);
                outerExit = deltaX * deltaX + deltaY * deltaY > radius * radius;
            }
        }

        if (cancelRequested || IsKeyDown(VK_ESCAPE) ||
            (configuration_.interaction.cancelOnRightClick && rightButtonPressed) || outerExit) {
            animation_.BeginCancel(now);
        } else if (!trackActivation_ && enterPressed) {
            const auto selected = selection_.Selected();
            if (selected.has_value()) {
                BeginCommit(*selected, now);
                if (!animation_.IsClosing()) {
                    animation_.BeginCancel(now);
                }
            } else {
                animation_.BeginCancel(now);
            }
        } else {
            const bool quickKeyActivated = TryActivateQuickKey(now, cursor);
            if (!animation_.IsClosing() && trackActivation_ &&
                (activationReleased || !IsActivationHeld(configuration_.activation))) {
                const auto selected = selection_.Selected();
                if (selected.has_value()) {
                    BeginCommit(*selected, now);
                } else {
                    animation_.BeginCancel(now);
                }
            } else if (!quickKeyActivated && haveCursor) {
                UpdateSubmenuDwell(now, cursor);
            }
        }
    }

    const WheelVisualParams params = animation_.Tick(now);

    bool needCommit = false;
    bool submenuRedraw = false;

    if (submenuAnimating_) {
        constexpr float durationSeconds{0.11F};
        const float elapsed = static_cast<float>(static_cast<double>(now - submenuRevealStartQpc_) /
                                                 static_cast<double>(qpcFrequency_));
        float progress = durationSeconds > 0.0F ? elapsed / durationSeconds : 1.0F;
        if (progress >= 1.0F) {
            progress = 1.0F;
            submenuAnimating_ = false;
        }
        overlay_.SetSubmenuReveal(OverlayAnimation::EaseOutCubic(progress));
        submenuRedraw = true;
    }

    // Advance an in-flight selection cross-fade and redraw the wheel surface.
    if (selectionAnimating_) {
        const float duration = animation_.SelectionDurationSeconds();
        const float elapsed = static_cast<float>(static_cast<double>(now - selectionStartQpc_) /
                                                 static_cast<double>(qpcFrequency_));
        float strength = duration > 0.0F ? elapsed / duration : 1.0F;
        if (strength >= 1.0F) {
            strength = 1.0F;
            selectionAnimating_ = false;
            selectionFrom_ = selectionShown_;
        }
        strength = std::clamp(strength, 0.0F, 1.0F);
        strength = OverlayAnimation::SmoothStep(strength);
        if (!overlay_.DrawSelection(selectionFrom_, selectionShown_, strength)) {
            RecoverOverlayGraphics();
            return;
        }
        needCommit = true;
    }
    if (submenuRedraw && !selectionAnimating_) {
        if (!overlay_.DrawSelection(selectionShown_, selectionShown_, 1.0F)) {
            RecoverOverlayGraphics();
            return;
        }
        needCommit = true;
    }

    // Apply opacity/transform while opening or closing. A steady-open frame with
    // no selection change performs no work, keeping the wheel visually idle.
    if (phaseBefore != OverlayAnimation::Phase::Open ||
        animation_.phase() != OverlayAnimation::Phase::Open) {
        overlay_.ApplyVisualParams(params);
        needCommit = true;
    }

    if (needCommit) {
        if (!overlay_.Commit()) {
            RecoverOverlayGraphics();
            return;
        }
    }

    if (animation_.IsFinished()) {
        // Commit remembered the selection; launch only after the overlay is
        // fully hidden so it never obstructs or steals foreground from the
        // target. Posting (rather than launching inline) lets the hide finish
        // first, matching the specified ClosingCommitted -> Launching order.
        std::optional<WheelItem> action = std::move(committedAction_);
        const bool reopen = reopenRequested_;
        HideOverlay();
        if (action.has_value()) {
            pendingAction_ = std::move(action);
            if (!::PostMessageW(messageWindow_.Handle(), kLaunchMessage, 0, 0)) {
                LaunchPendingItem();
            }
        }
        if (reopen)
            PostOpenWheelRequest(true);
        else if (settings_.IsVisible())
            settings_.UpdatePreview();
    }
}

void OrbitDeckApp::EndOverlayGesture(const bool cancelled) noexcept {
    if (!trackActivation_ || !overlay_.IsVisible() || animation_.IsClosing())
        return;
    TrackOverlay(!cancelled, cancelled);
    (void)activation_.WatchGesture(false);
}

void OrbitDeckApp::UpdateSelection(const std::int64_t nowQpc, const POINT cursor) noexcept {
    const D2D1_POINT_2F centre = overlay_.WheelCentre();
    const float pixelsPerDip = overlay_.PixelsPerDip();
    if (pixelsPerDip <= 0.0F) {
        overlayAvailable_ = false;
        HideOverlay();
        return;
    }

    const float deltaX = (static_cast<float>(cursor.x) - centre.x) / pixelsPerDip;
    const float deltaY = (static_cast<float>(cursor.y) - centre.y) / pixelsPerDip;
    const float distanceSquared = deltaX * deltaX + deltaY * deltaY;
    const bool insideNeutral = distanceSquared < configuration_.appearance.neutralThresholdDip *
                                                     configuration_.appearance.neutralThresholdDip;
    insideNeutral_ = insideNeutral;

    const SelectionEngine::Selection before = selection_.Selected();
    SelectionEngine::Selection after = selection_.Update(deltaX, deltaY);
    outerExitArmed_ = outerExitArmed_ || selection_.GestureArmed();
    if (after == before) {
        return;
    }

    if (after.has_value() && *after < wheelLayout_.segments.size()) {
        const WheelLayoutSegment& selectedSegment = wheelLayout_.segments[*after];
        if (selectedSegment.layer < menuPath_.size() &&
            !PathHasPrefix(menuPath_, selectedSegment.path)) {
            menuPath_.resize(selectedSegment.layer);
            submenuAnimating_ = false;
            overlay_.SetSubmenuReveal(1.0F);
            if (!ApplyCurrentMenu()) {
                RecoverOverlayGraphics();
                return;
            }
            SnapshotQuickKeys();
            after = selection_.Update(deltaX, deltaY);
            selectionFrom_.reset();
            selectionShown_ = after;
            selectionStartQpc_ = nowQpc;
            selectionAnimating_ = after.has_value();
            submenuDwellItem_.reset();
            return;
        }
    }

    // Reversing an in-flight blend continues from the exact visible mix.
    if (selectionAnimating_ && after == selectionFrom_) {
        const float duration = animation_.SelectionDurationSeconds();
        const float elapsed = static_cast<float>(static_cast<double>(nowQpc - selectionStartQpc_) /
                                                 static_cast<double>(qpcFrequency_));
        const float progress = std::clamp(duration > 0.0F ? elapsed / duration : 1.0F, 0.0F, 1.0F);
        const float reverseProgress = 1.0F - progress;
        selectionFrom_ = selectionShown_;
        selectionShown_ = after;
        selectionStartQpc_ =
            nowQpc - static_cast<std::int64_t>(static_cast<double>(reverseProgress) *
                                               static_cast<double>(duration) *
                                               static_cast<double>(qpcFrequency_));
        selectionAnimating_ = reverseProgress < 1.0F;
        if (!selectionAnimating_)
            selectionFrom_ = selectionShown_;
        submenuDwellItem_.reset();
        return;
    }

    // A third direction cannot be represented by a two-wedge blend; continue
    // from whichever of the two visible wedges is currently dominant.
    SelectionEngine::Selection outgoing = selectionShown_;
    if (selectionAnimating_) {
        const float duration = animation_.SelectionDurationSeconds();
        const float elapsed = static_cast<float>(static_cast<double>(nowQpc - selectionStartQpc_) /
                                                 static_cast<double>(qpcFrequency_));
        if (duration > 0.0F && elapsed / duration < 0.5F) {
            outgoing = selectionFrom_;
        }
    }
    selectionFrom_ = outgoing;
    selectionShown_ = after;
    selectionStartQpc_ = nowQpc;
    selectionAnimating_ = true;
    submenuDwellItem_.reset();
}

void OrbitDeckApp::UpdateSubmenuDwell(const std::int64_t nowQpc, const POINT cursor) noexcept {
    const auto selected = selection_.Selected();
    if (!selected.has_value() || *selected >= wheelLayout_.segments.size()) {
        submenuDwellItem_.reset();
        return;
    }
    const WheelLayoutSegment& segment = wheelLayout_.segments[*selected];
    const WheelItem* const item = ItemAtPath(configuration_.items, segment.path);
    if (item == nullptr || !item->enabled || !item->IsSubmenu() || menuPath_ == segment.path) {
        submenuDwellItem_.reset();
        return;
    }
    if (submenuDwellItem_ != selected) {
        submenuDwellItem_ = selected;
        submenuDwellStartQpc_ = nowQpc;
        return;
    }

    const double elapsedMs = static_cast<double>(nowQpc - submenuDwellStartQpc_) * 1000.0 /
                             static_cast<double>(qpcFrequency_);
    if (elapsedMs >= configuration_.interaction.submenuDwellMs) {
        (void)OpenSubmenu(*selected, cursor, true);
    }
}

bool OrbitDeckApp::TryActivateQuickKey(const std::int64_t nowQpc, const POINT cursor) noexcept {
    for (std::size_t flatIndex = 0; flatIndex < quickVirtualKeys_.size(); ++flatIndex) {
        const UINT virtualKey = quickVirtualKeys_[flatIndex];
        if (virtualKey == 0 || virtualKey >= quickKeyDown_.size())
            continue;
        const bool down = IsKeyDown(static_cast<int>(virtualKey));
        const bool pressed = down && !quickKeyDown_[virtualKey];
        quickKeyDown_[virtualKey] = down;
        if (!pressed)
            continue;
        const WheelItem* const item =
            ItemAtPath(configuration_.items, wheelLayout_.segments[flatIndex].path);
        if (item == nullptr || !item->enabled)
            continue;
        if (item->IsSubmenu()) {
            return OpenSubmenu(flatIndex, cursor, false);
        } else if (item->IsBuiltIn() || !item->target.empty()) {
            BeginCommit(flatIndex, nowQpc);
            return true;
        }
    }
    return false;
}

bool OrbitDeckApp::OpenSubmenu(const std::size_t index, const POINT cursor,
                               const bool pointerInitiated) noexcept {
    if (index >= wheelLayout_.segments.size()) {
        return false;
    }
    const WheelLayoutSegment& segment = wheelLayout_.segments[index];
    const std::size_t parentLayer = segment.layer;
    const WheelItem* const item = ItemAtPath(configuration_.items, segment.path);
    if (item == nullptr || !item->enabled || !item->IsSubmenu()) {
        return false;
    }

    menuPath_ = segment.path;
    submenuAnimating_ = !configuration_.appearance.reducedMotion && SystemAnimationsEnabled();
    submenuRevealStartQpc_ = QueryNow();
    overlay_.SetSubmenuReveal(submenuAnimating_ ? 0.0F : 1.0F);
    if (!ApplyCurrentMenu()) {
        menuPath_.resize(parentLayer);
        RecoverOverlayGraphics();
        return false;
    }
    SnapshotQuickKeys();

    const D2D1_POINT_2F centre = overlay_.WheelCentre();
    const float pixelsPerDip = overlay_.PixelsPerDip();
    const float deltaX = (static_cast<float>(cursor.x) - centre.x) / pixelsPerDip;
    const float deltaY = (static_cast<float>(cursor.y) - centre.y) / pixelsPerDip;
    if (pointerInitiated) {
        selection_.BeginGesture(0.0F, 0.0F);
    } else {
        selection_.BeginGesture(deltaX, deltaY);
    }
    selectionFrom_.reset();
    selectionShown_.reset();
    selectionAnimating_ = false;
    submenuDwellItem_.reset();
    insideNeutral_ =
        deltaX * deltaX + deltaY * deltaY < configuration_.appearance.neutralThresholdDip *
                                                configuration_.appearance.neutralThresholdDip;
    return true;
}

void OrbitDeckApp::BeginCommit(const std::size_t index, const std::int64_t nowQpc) noexcept {
    if (index >= wheelLayout_.segments.size()) {
        animation_.BeginCancel(nowQpc);
        return;
    }
    const WheelItem* const item =
        ItemAtPath(configuration_.items, wheelLayout_.segments[index].path);
    if (item == nullptr || !item->enabled || item->IsSubmenu() ||
        (!item->IsBuiltIn() && item->target.empty())) {
        animation_.BeginCancel(nowQpc);
        return;
    }
    committedItem_ = index;
    committedAction_ = *item;
    animation_.BeginCommit(nowQpc);
}

void OrbitDeckApp::HideOverlay() noexcept {
    previewActive_ = false;
    (void)activation_.WatchGesture(false);
    (void)::KillTimer(messageWindow_.Handle(), kVisibleTimerId);
    reopenRequested_ = false;
    lastTrackQpc_ = 0;
    trackActivation_ = false;
    selectionAnimating_ = false;
    committedItem_.reset();
    committedAction_.reset();
    submenuDwellItem_.reset();
    submenuDwellStartQpc_ = 0;
    submenuRevealStartQpc_ = 0;
    submenuAnimating_ = false;
    overlay_.SetSubmenuReveal(1.0F);
    insideNeutral_ = true;
    outerExitArmed_ = false;
    menuPath_.clear();
    quickKeyDown_.fill(false);
    quickVirtualKeys_.clear();
    rightButtonDown_ = false;
    enterDown_ = false;
    selectionFrom_.reset();
    selectionShown_.reset();
    selection_.Reset();
    animation_.Reset();
    overlay_.Hide();
}

void OrbitDeckApp::PreviewSettings(const Configuration* const configuration) noexcept {
    if (overlay_.IsVisible() && !previewActive_)
        return;
    HideOverlay();
    if (configuration == nullptr) {
        if (overlayAvailable_)
            (void)ApplyCurrentMenu();
        return;
    }
    if (!overlayAvailable_ || configuration->items.empty() || ::IsIconic(settings_.Handle())) {
        return;
    }

    // Deferred redraws must not borrow items from the mutable editor draft.
    previewConfiguration_ = *configuration;
    const Configuration& preview = previewConfiguration_;
    overlay_.ConfigureAppearance(preview.appearance, preview.interaction.numberQuickKeys,
                                 L"Choose");
    overlay_.PrepareItems(preview.items);
    WheelLayout previewLayout = BuildWheelLayout(
        preview.items, {}, preview.appearance.innerRadiusDip, preview.appearance.outerRadiusDip);
    previewLayout.surfaceRadiusDip = previewLayout.requiredRadiusDip;
    if (!overlay_.SetLayout(preview.items, std::move(previewLayout)) ||
        !overlay_.ShowPreview(settings_.PreviewHandle())) {
        RecoverOverlayGraphics();
    } else {
        previewActive_ = true;
    }
}

void OrbitDeckApp::RecoverOverlayGraphics() noexcept {
    std::optional<WheelItem> committed;
    if (animation_.committed() && committedAction_.has_value()) {
        committed = std::move(committedAction_);
    }
    HideOverlay();
    overlayAvailable_ = overlay_.RecoverGraphics();
    if (!overlayAvailable_) {
        overlay_.Destroy();
        overlayAvailable_ = overlay_.Create(::GetModuleHandleW(nullptr));
    }
    if (!overlayAvailable_) {
        (void)trayIcon_.ShowBalloon(L"Graphics unavailable",
                                    L"Windows could not restore the wheel. The next shortcut or "
                                    L"tray Open Wheel request will retry.");
    }
    if (committed.has_value()) {
        pendingAction_ = std::move(committed);
        if (!::PostMessageW(messageWindow_.Handle(), kLaunchMessage, 0, 0)) {
            LaunchPendingItem();
        }
    }
}

void OrbitDeckApp::LaunchPendingItem() noexcept {
    if (!pendingAction_.has_value()) {
        return;
    }
    WheelItem item = std::move(*pendingAction_);
    pendingAction_.reset();
    LaunchItem(item);
}

void OrbitDeckApp::LaunchItem(const WheelItem& item) noexcept {
    if (!item.enabled || (!item.IsBuiltIn() && item.target.empty())) {
        return;
    }
    if (item.IsBuiltIn()) {
        if (item.builtInTool == BuiltInTool::FileConverter) {
            POINT cursor{};
            (void)::GetCursorPos(&cursor);
            fileConverter_.Show(cursor, {}, sourceWindow_);
            return;
        }
        if (IsDeveloperTool(item.builtInTool)) {
            POINT cursor{};
            if (!::GetCursorPos(&cursor))
                cursor = {};
            developerTool_.Show(item.builtInTool, cursor);
            return;
        }
        if (item.builtInTool == BuiltInTool::Timer || item.builtInTool == BuiltInTool::Calculator) {
            const LaunchResult result = ExecuteDirectAction(
                L"system", item.builtInTool == BuiltInTool::Timer ? L"timer" : L"calculator");
            if (!result.succeeded) {
                (void)trayIcon_.ShowBalloon(L"Action failed", result.error.c_str());
            }
            return;
        }
        POINT cursor{};
        if (!::GetCursorPos(&cursor))
            cursor = {};
        miniApp_.Show(item.builtInTool, cursor);
        return;
    }
    std::vector<LaunchCandidate> candidates = item.launchCandidates;
    if (candidates.empty()) {
        candidates.push_back(
            {item.configuredTarget, item.target, item.launchType, item.launchKind});
    }

    if (item.windowAction != ExistingWindowAction::OpenNewInstance) {
        for (const LaunchCandidate& candidate : candidates) {
            if (IsDirectActionType(candidate.type))
                continue;
            const std::wstring matchName = ExecutableMatchName(candidate.target);
            if (matchName.empty())
                continue;
            if (item.windowAction == ExistingWindowAction::DoNothing) {
                if (HasExistingWindow(matchName))
                    return;
            } else if (item.windowAction == ExistingWindowAction::ToggleMinimize) {
                if (ToggleExistingWindow(matchName))
                    return;
            } else if (ActivateExisting(matchName, item.windowAction ==
                                                       ExistingWindowAction::MaximizeAndFocus)) {
                return;
            }
        }
    }

    LaunchResult result{false, L"No valid launch candidate was configured."};
    for (const LaunchCandidate& candidate : candidates) {
        result =
            IsDirectActionType(candidate.type)
                ? ExecuteDirectAction(candidate.type, candidate.target, messageWindow_.Handle())
                : Launch(candidate.target, candidate.kind);
        if (result.succeeded)
            return;
    }
    (void)trayIcon_.ShowBalloon(L"Launch failed", result.error.c_str());
}

std::int64_t OrbitDeckApp::QueryNow() noexcept {
    LARGE_INTEGER counter{};
    if (::QueryPerformanceCounter(&counter)) {
        return counter.QuadPart;
    }
    return 0;
}

void OrbitDeckApp::ToggleShortcut() noexcept {
    if (activation_.IsRegistered()) {
        HideOverlay();
        (void)activation_.Unregister();
        shortcutPaused_ = true;
        return;
    }

    if (!shortcutPaused_) {
        shortcutPaused_ = true;
        return;
    }
    shortcutPaused_ = false;
    if (!activation_.Register(messageWindow_.Handle(), configuration_.activation)) {
        shortcutPaused_ = true;
        ShowHotkeyError();
    }
}

void OrbitDeckApp::ToggleStartWithWindows() noexcept {
    std::wstring error;
    const bool enable = !StartWithWindowsEnabled();
    if (!SetStartWithWindows(enable, error)) {
        (void)trayIcon_.ShowBalloon(L"Startup setting not changed", error);
    }
}

void OrbitDeckApp::LoadConfigurationAtStartup() noexcept {
    ConfigLoadResult loaded = LoadConfiguration();
    if (loaded.ok) {
        configuration_ = std::move(loaded.config);
        configLastWrite_ = loaded.lastWrite;
        configContentHash_ = loaded.contentHash;
        configSnapshotValid_ = true;
        if (!loaded.error.empty()) {
            (void)trayIcon_.ShowBalloon(L"Configuration cleanup warning", loaded.error);
        }
        return;
    }
    // The disk file was unavailable: fall back to the embedded example
    // configuration so the wheel still works.
    ConfigParseResult fallback = ParseConfiguration(DefaultConfigJson());
    if (fallback.ok) {
        configuration_ = std::move(fallback.config);
    }
    configLastWrite_ = ConfigLastWrite();
    configContentHash_ = loaded.contentHash;
    configSnapshotValid_ = false;
    if (!loaded.error.empty()) {
        (void)trayIcon_.ShowBalloon(L"Configuration", loaded.error);
    }
}

void OrbitDeckApp::ReloadConfiguration(const bool reRegisterHotkey) noexcept {
    if (settings_.IsVisible()) {
        (void)trayIcon_.ShowBalloon(L"Configuration not reloaded",
                                    L"Close Preferences before reloading config.json so unsaved "
                                    L"edits are not discarded.");
        return;
    }
    ConfigLoadResult loaded = LoadConfiguration();
    if (!loaded.ok) {
        configLastWrite_ = loaded.lastWrite;
        configContentHash_ = loaded.contentHash;
        configSnapshotValid_ = false;
        // Last-known-good: a corrupt or invalid file leaves the running
        // configuration untouched.
        const std::wstring_view message =
            loaded.error.empty() ? std::wstring_view(
                                       L"The configuration file is invalid; keeping the "
                                       L"previous settings.")
                                 : std::wstring_view(loaded.error);
        (void)trayIcon_.ShowBalloon(L"Configuration not reloaded", message);
        return;
    }
    if (overlay_.IsVisible()) {
        HideOverlay();
    }
    configuration_ = std::move(loaded.config);
    configLastWrite_ = loaded.lastWrite;
    configContentHash_ = loaded.contentHash;
    configSnapshotValid_ = true;
    ApplyConfiguration();
    if (!loaded.error.empty()) {
        (void)trayIcon_.ShowBalloon(L"Configuration cleanup warning", loaded.error);
    }
    if (reRegisterHotkey && !shortcutPaused_) {
        if (activation_.IsRegistered()) {
            (void)activation_.Unregister();
        }
        if (!activation_.Register(messageWindow_.Handle(), configuration_.activation)) {
            ShowHotkeyError();
        }
    }
}

void OrbitDeckApp::MaybeReloadOnOpen() noexcept {
    const long long now = ConfigLastWrite();
    if (!configSnapshotValid_ || now != configLastWrite_) {
        ReloadConfiguration(true);
    }
}

void OrbitDeckApp::ApplyConfiguration() noexcept {
    menuPath_.clear();
    for (const BuiltInTool tool :
         {BuiltInTool::Timer, BuiltInTool::Caffeine, BuiltInTool::Calculator,
          BuiltInTool::Scratchpad, BuiltInTool::SystemGlance, BuiltInTool::Mirror,
          BuiltInTool::MicCheck}) {
        miniApp_.SetToolAvailable(tool, ContainsBuiltIn(configuration_.items, tool));
    }
    for (int value = static_cast<int>(BuiltInTool::ColorPicker);
         value <= static_cast<int>(BuiltInTool::TextDiff); ++value) {
        const BuiltInTool tool = static_cast<BuiltInTool>(value);
        developerTool_.SetToolAvailable(tool, ContainsBuiltIn(configuration_.items, tool));
    }
    if (overlayAvailable_) {
        overlay_.InvalidateItems();
        overlay_.PrepareItems(configuration_.items);
    }
    if (!ApplyCurrentMenu()) {
        overlayAvailable_ = false;
    }
}

bool OrbitDeckApp::SaveSettings(const Configuration& configuration,
                                const bool replaceInvalidExisting,
                                const std::string* const replacementJson,
                                std::wstring& error) noexcept {
    if (ConfigLastWrite() != configLastWrite_) {
        error =
            L"config.json changed outside Preferences. Close and reopen "
            L"Preferences to load the newer file before saving.";
        return false;
    }

    ConfigParseResult parsed = ParseConfiguration(SerializeConfiguration(configuration));
    if (!parsed.ok) {
        error = parsed.error.empty() ? L"The settings contain an invalid value." : parsed.error;
        return false;
    }

    const ActivationConfig previousActivation = configuration_.activation;
    const bool shortcutRegistered = activation_.IsRegistered();
    const bool activationChanged = !SameActivation(previousActivation, parsed.config.activation);
    bool registeredDuringSave = false;
    if (shortcutRegistered && activationChanged) {
        (void)activation_.Unregister();
        if (!activation_.Register(messageWindow_.Handle(), parsed.config.activation)) {
            const bool restored = activation_.Register(messageWindow_.Handle(), previousActivation);
            error = restored ? L"The new shortcut is unavailable. The previous shortcut "
                               L"remains active."
                             : L"The new shortcut is unavailable and the previous "
                               L"shortcut could not be restored. Use the tray menu after "
                               L"resolving the shortcut conflict.";
            return false;
        }
    } else if (!shortcutRegistered && !shortcutPaused_ && activationChanged) {
        registeredDuringSave =
            activation_.Register(messageWindow_.Handle(), parsed.config.activation);
        if (!registeredDuringSave) {
            error =
                L"The new shortcut is unavailable. Resolve the conflict and "
                L"try again, or pause the shortcut from the tray.";
            return false;
        }
    }

    ConfigSaveResult saved = SaveConfiguration(parsed.config, configLastWrite_, configContentHash_,
                                               replaceInvalidExisting, replacementJson);
    if (!saved.ok) {
        if (shortcutRegistered && activationChanged) {
            (void)activation_.Unregister();
            if (!activation_.Register(messageWindow_.Handle(), previousActivation)) {
                error = saved.error + L" The previous shortcut also could not be restored.";
                return false;
            }
        } else if (registeredDuringSave) {
            (void)activation_.Unregister();
        }
        error = saved.error;
        return false;
    }

    HideOverlay();
    configuration_ = std::move(parsed.config);
    configLastWrite_ = saved.lastWrite;
    configContentHash_ = saved.contentHash;
    configSnapshotValid_ = true;
    ApplyConfiguration();
    if (!saved.warning.empty()) {
        (void)trayIcon_.ShowBalloon(L"Configuration saved with warning", saved.warning);
    }
    return true;
}

bool OrbitDeckApp::ApplyCurrentMenu() noexcept {
    wheelLayout_ =
        BuildWheelLayout(configuration_.items, menuPath_, configuration_.appearance.innerRadiusDip,
                         configuration_.appearance.outerRadiusDip);
    if (wheelLayout_.segments.empty()) {
        return false;
    }
    selection_.Configure(wheelLayout_, configuration_.appearance.neutralThresholdDip,
                         configuration_.appearance.selectionThresholdDip);

    if (overlayAvailable_) {
        overlay_.ConfigureAppearance(configuration_.appearance,
                                     configuration_.interaction.numberQuickKeys,
                                     CurrentMenuTitle());
        return overlay_.SetLayout(configuration_.items, wheelLayout_);
    }
    return true;
}

void OrbitDeckApp::SnapshotQuickKeys() noexcept {
    quickKeyDown_.fill(false);
    quickVirtualKeys_.assign(wheelLayout_.segments.size(), 0);
    for (std::size_t index = 0; index < wheelLayout_.segments.size(); ++index) {
        const WheelLayoutSegment& segment = wheelLayout_.segments[index];
        if (segment.layer != wheelLayout_.activeLayer)
            continue;
        const WheelItem* const item = ItemAtPath(configuration_.items, segment.path);
        if (item == nullptr || !item->enabled)
            continue;
        const wchar_t key = item->quickKey != L'\0'
                                ? item->quickKey
                                : (configuration_.interaction.numberQuickKeys && segment.index < 9
                                       ? static_cast<wchar_t>(L'1' + segment.index)
                                       : L'\0');
        const UINT virtualKey = QuickVirtualKey(key);
        if (virtualKey != 0 && virtualKey < quickKeyDown_.size()) {
            quickVirtualKeys_[index] = virtualKey;
            quickKeyDown_[virtualKey] = IsKeyDown(static_cast<int>(virtualKey));
        }
    }
}

std::wstring OrbitDeckApp::CurrentMenuTitle() const {
    const std::vector<WheelItem>* items = &configuration_.items;
    const WheelItem* parent = nullptr;
    for (const std::size_t index : menuPath_) {
        if (index >= items->size()) {
            return L"Choose";
        }
        parent = &(*items)[index];
        items = &parent->children;
    }
    return parent != nullptr && !parent->label.empty() ? parent->label : L"Choose";
}

void OrbitDeckApp::Shutdown() noexcept {
    HideOverlay();
    if (sessionNotifications_) {
        (void)::WTSUnRegisterSessionNotification(messageWindow_.Handle());
        sessionNotifications_ = false;
    }
    settings_.Destroy();
    developerTool_.Destroy();
    fileConverter_.Destroy();
    miniApp_.Destroy();
    overlay_.Destroy();
    (void)activation_.Unregister();
    if (convertShortcutRegistered_) {
        (void)::UnregisterHotKey(messageWindow_.Handle(), kConvertHotkeyId);
        convertShortcutRegistered_ = false;
    }
    trayIcon_.Remove();
    singleInstance_.Stop();
    messageWindow_.Destroy();
}

void OrbitDeckApp::ShowHotkeyError() const noexcept {
    (void)trayIcon_.ShowBalloon(
        L"Shortcut unavailable",
        L"The configured OrbitDeck activation shortcut could not be registered. "
        L"Weapon Cycle remains available from the tray icon.");
}

void OrbitDeckApp::ShowTrayMenu(const POINT position) const noexcept {
    TrayIconController::MenuState state{};
    state.canOpenWheel = true;
    state.conversionMenuEnabled = ConversionContextMenuEnabled();
    state.shortcutPaused = shortcutPaused_;
    state.reducedMotion = configuration_.appearance.reducedMotion;
    state.canReloadConfiguration = true;
    state.canOpenConfigurationFolder = true;
    state.canChangeStartup = !StartupCommand().empty();
    state.startWithWindows = StartWithWindowsEnabled();
    trayIcon_.ShowContextMenu(position, state);
}

}  // namespace orbitdeck
