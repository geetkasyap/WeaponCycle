#include "shell/TrayIconController.h"

#include "resource.h"

#include <shellapi.h>

#include <algorithm>

namespace orbitdeck {
namespace {

constexpr UINT kIconId{1};
constexpr GUID kIconGuid{
    0x6f08b5c7,
    0x60de,
    0x4f43,
    {0x97, 0x1f, 0xc2, 0x8a, 0x29, 0xc4, 0x82, 0x75},
};

template <std::size_t Size>
void CopyText(wchar_t (&destination)[Size], const std::wstring_view source) noexcept {
    const auto copied = source.copy(destination, std::min(source.size(), Size - 1));
    destination[copied] = L'\0';
}

NOTIFYICONDATAW MakeIconData(const HWND owner, const UINT callbackMessage,
                             const UINT flags) noexcept {
    NOTIFYICONDATAW data{};
    data.cbSize = static_cast<DWORD>(sizeof(data));
    data.hWnd = owner;
    data.uID = kIconId;
    data.uFlags = flags;
    data.uCallbackMessage = callbackMessage;
    data.guidItem = kIconGuid;
    return data;
}

UINT MenuFlags(const bool enabled, const bool checked) noexcept {
    return MF_STRING | (enabled ? MF_ENABLED : MF_GRAYED) | (checked ? MF_CHECKED : MF_UNCHECKED);
}

UINT_PTR CommandId(const TrayIconController::Command command) noexcept {
    return static_cast<UINT_PTR>(command);
}

}  // namespace

TrayIconController::~TrayIconController() noexcept {
    Remove();
}

bool TrayIconController::Add(const HWND owner, const UINT callbackMessage) noexcept {
    if (owner == nullptr || callbackMessage == 0) {
        return false;
    }

    Remove();
    owner_ = owner;
    callbackMessage_ = callbackMessage;
    added_ = Install();

    if (!added_) {
        owner_ = nullptr;
        callbackMessage_ = 0;
    }

    return added_;
}

bool TrayIconController::Restore() noexcept {
    if (owner_ == nullptr || callbackMessage_ == 0) {
        return false;
    }

    added_ = Install();
    return added_;
}

void TrayIconController::Remove() noexcept {
    if (added_) {
        auto data = MakeIconData(owner_, callbackMessage_, NIF_GUID);
        (void)::Shell_NotifyIconW(NIM_DELETE, &data);
    }

    owner_ = nullptr;
    callbackMessage_ = 0;
    added_ = false;
}

bool TrayIconController::TryGetMenuAnchor(POINT& point) const noexcept {
    if (!added_) {
        return false;
    }

    NOTIFYICONIDENTIFIER identifier{};
    identifier.cbSize = static_cast<DWORD>(sizeof(identifier));
    identifier.hWnd = owner_;
    identifier.uID = kIconId;
    identifier.guidItem = kIconGuid;

    RECT bounds{};
    if (SUCCEEDED(::Shell_NotifyIconGetRect(&identifier, &bounds))) {
        point.x = bounds.left + (bounds.right - bounds.left) / 2;
        point.y = bounds.bottom;
        return true;
    }
    return ::GetCursorPos(&point) != FALSE;
}

void TrayIconController::ShowContextMenu(const POINT position,
                                         const MenuState state) const noexcept {
    if (owner_ == nullptr) {
        return;
    }

    const HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    const bool complete =
        ::AppendMenuW(menu, MenuFlags(true, false), CommandId(Command::Preferences),
                      L"Preferences...") &&
        ::AppendMenuW(menu, MenuFlags(state.canOpenWheel, false), CommandId(Command::OpenWheel),
                      L"Open Wheel") &&
        ::AppendMenuW(menu, MenuFlags(state.canReloadConfiguration, false),
                      CommandId(Command::ConvertFile), L"Convert a File...  Ctrl+Alt+C") &&
        ::AppendMenuW(menu, MenuFlags(true, state.conversionMenuEnabled),
                      CommandId(Command::ExplorerConvertMenu), L"Explorer conversion menu...") &&
        ::AppendMenuW(menu, MenuFlags(state.canReloadConfiguration, false),
                      CommandId(Command::ReloadConfiguration), L"Reload Configuration") &&
        ::AppendMenuW(menu, MenuFlags(state.canOpenConfigurationFolder, false),
                      CommandId(Command::OpenConfigurationFolder), L"Open Configuration Folder") &&
        ::AppendMenuW(menu, MenuFlags(true, state.shortcutPaused),
                      CommandId(Command::PauseShortcut),
                      state.shortcutPaused ? L"Resume Shortcut" : L"Pause Shortcut") &&
        ::AppendMenuW(menu, MenuFlags(state.canChangeStartup, state.startWithWindows),
                      CommandId(Command::StartWithWindows), L"Start with Windows") &&
        ::AppendMenuW(menu, MenuFlags(state.canChangeReducedMotion, state.reducedMotion),
                      CommandId(Command::ReducedMotion), L"Reduced Motion") &&
        ::AppendMenuW(menu, MenuFlags(true, false), CommandId(Command::About),
                      L"About Weapon Cycle") &&
        ::AppendMenuW(menu, MenuFlags(true, false), CommandId(Command::Exit), L"Exit");

    if (complete) {
        // A notification-area menu needs a foreground owner so clicking
        // elsewhere dismisses it even though OrbitDeck's owner is hidden.
        (void)::SetForegroundWindow(owner_);
        (void)::TrackPopupMenuEx(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, position.x,
                                 position.y, owner_, nullptr);
        (void)::PostMessageW(owner_, WM_NULL, 0, 0);
    }

    (void)::DestroyMenu(menu);
}

bool TrayIconController::ShowBalloon(const std::wstring_view title,
                                     const std::wstring_view message) const noexcept {
    if (!added_) {
        return false;
    }

    auto data = MakeIconData(owner_, callbackMessage_, NIF_GUID | NIF_INFO);
    CopyText(data.szInfoTitle, title);
    CopyText(data.szInfo, message);
    data.dwInfoFlags = NIIF_INFO;
    return ::Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE;
}

bool TrayIconController::Install() noexcept {
    auto data = MakeIconData(owner_, callbackMessage_,
                             NIF_GUID | NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP);
    data.hIcon = static_cast<HICON>(::LoadImageW(::GetModuleHandleW(nullptr),
                                                 MAKEINTRESOURCEW(IDI_ORBITDECK), IMAGE_ICON, 0, 0,
                                                 LR_DEFAULTSIZE | LR_SHARED));
    if (data.hIcon == nullptr) {
        data.hIcon = static_cast<HICON>(
            ::LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    }
    CopyText(data.szTip, L"Weapon Cycle");

    if (data.hIcon == nullptr || !::Shell_NotifyIconW(NIM_ADD, &data)) {
        return false;
    }

    auto versionData = MakeIconData(owner_, callbackMessage_, NIF_GUID);
    versionData.uVersion = NOTIFYICON_VERSION_4;
    if (::Shell_NotifyIconW(NIM_SETVERSION, &versionData)) {
        return true;
    }

    (void)::Shell_NotifyIconW(NIM_DELETE, &versionData);
    return false;
}

}  // namespace orbitdeck
