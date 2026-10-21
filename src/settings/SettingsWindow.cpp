#include "settings/SettingsWindow.h"

#include "config/ConfigLoader.h"
#include "settings/ActionChoices.h"
#include "resource.h"
#include "shell/TrayIconController.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wrl/client.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

namespace orbitdeck {
namespace {

constexpr int kPreferencesWidth{1100};
constexpr int kPreferencesHeight{720};

enum ControlId : int {
    kItemList = 2001,
    kAppFilter,
    kAppList,
    kCatalogStatus,
    kRefreshApps,
    kAddCheckedApps,
    kAddStarterPacks,
    kDuplicate,
    kTestAction,
    kLabelEdit,
    kTargetEdit,
    kIconEdit,
    kEnabledCheck,
    kQuickKeyEdit,
    kActionCombo,
    kWindowActionCombo,
    kPlacementCombo,
    kHotkeyEdit,
    kRightClickCheck,
    kOuterExitCheck,
    kQuickKeysCheck,
    kResetDefaults,
    kAddApp,
    kAddSubmenu,
    kRemove,
    kMoveUp,
    kMoveDown,
    kWheelSizeEdit,
    kOuterRadiusEdit,
    kInnerRadiusEdit,
    kNeutralRadiusEdit,
    kSelectionRadiusEdit,
    kWheelOpacityEdit,
    kScrimEdit,
    kAccentEdit,
    kChooseAccent,
    kReducedMotionCheck,
    kImport,
    kExport,
    kActionsPage,
    kAppearancePage,
    kAppsPage,
    kOpenConverter,
    kTryWheel,
};

std::wstring Lower(std::wstring value) {
    for (wchar_t& character : value) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    return value;
}

UINT FittedLayoutDpi(const HMONITOR monitor, UINT desiredDpi) noexcept {
    if (desiredDpi == 0)
        desiredDpi = USER_DEFAULT_SCREEN_DPI;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || !::GetMonitorInfoW(monitor, &info)) {
        return desiredDpi;
    }
    const int width = info.rcWork.right - info.rcWork.left;
    const int height = info.rcWork.bottom - info.rcWork.top;
    const UINT widthDpi = static_cast<UINT>(std::max(
        24, ::MulDiv(std::max(1, width - 24), USER_DEFAULT_SCREEN_DPI, kPreferencesWidth)));
    const UINT heightDpi = static_cast<UINT>(std::max(
        24, ::MulDiv(std::max(1, height - 24), USER_DEFAULT_SCREEN_DPI, kPreferencesHeight)));
    return std::min({desiredDpi, widthDpi, heightDpi});
}

struct AppDiscoveryResult {
    std::vector<InstalledApp> apps;
    std::wstring error;
    std::shared_ptr<AppDiscoveryState> state;
};

struct AppDiscoveryRegistry {
    std::mutex mutex;
    std::unordered_map<AppDiscoveryState*, std::unique_ptr<AppDiscoveryResult>> completed;
};

AppDiscoveryRegistry& DiscoveryRegistry() {
    // ponytail: process-lifetime storage avoids static-destruction races with a
    // Shell call that ignores cancellation during process shutdown.
    static AppDiscoveryRegistry* const registry = new AppDiscoveryRegistry;
    return *registry;
}

std::wstring WindowText(const HWND window) {
    const int length = ::GetWindowTextLengthW(window);
    if (length <= 0)
        return {};
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    (void)::GetWindowTextW(window, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

void SetFont(const HWND control, const HFONT font) noexcept {
    (void)::SendMessageW(
        control, WM_SETFONT,
        reinterpret_cast<WPARAM>(font != nullptr ? font : ::GetStockObject(DEFAULT_GUI_FONT)),
        TRUE);
}

void DrawOwnerButton(const DRAWITEMSTRUCT& item, const HFONT font,
                     const bool selected = false) noexcept {
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    const bool primary = item.CtlID == IDOK;
    const COLORREF fill = disabled   ? RGB(28, 30, 35)
                          : primary  ? RGB(216, 240, 230)
                          : selected ? RGB(48, 58, 65)
                          : pressed  ? RGB(61, 66, 76)
                                     : RGB(39, 43, 50);
    const COLORREF border = focused ? RGB(158, 190, 220) : RGB(76, 82, 94);
    const HBRUSH brush = ::CreateSolidBrush(fill);
    const HPEN pen = ::CreatePen(PS_SOLID, focused ? 2 : 1, border);
    const HGDIOBJ oldBrush = ::SelectObject(item.hDC, brush);
    const HGDIOBJ oldPen = ::SelectObject(item.hDC, pen);
    (void)::RoundRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right,
                      item.rcItem.bottom, 8, 8);
    (void)::SelectObject(item.hDC, oldBrush);
    (void)::SelectObject(item.hDC, oldPen);
    (void)::DeleteObject(brush);
    (void)::DeleteObject(pen);

    wchar_t text[256]{};
    (void)::GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    RECT label = item.rcItem;
    if (pressed) {
        (void)::OffsetRect(&label, 1, 1);
    }
    (void)::SetBkMode(item.hDC, TRANSPARENT);
    (void)::SetTextColor(item.hDC, disabled  ? RGB(112, 117, 126)
                                   : primary ? RGB(18, 31, 26)
                                             : RGB(239, 242, 246));
    const HGDIOBJ oldFont =
        ::SelectObject(item.hDC, font != nullptr ? font : ::GetStockObject(DEFAULT_GUI_FONT));
    (void)::DrawTextW(item.hDC, text, -1, &label,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    (void)::SelectObject(item.hDC, oldFont);
}

std::wstring NumberText(const float value) {
    char text[64]{};
    const auto formatted = std::to_chars(text, text + std::size(text), value);
    if (formatted.ec != std::errc{}) {
        ::OutputDebugStringW(L"Weapon Cycle: numeric field formatting failed.\n");
        return L"Invalid number";
    }
    return std::wstring(text, formatted.ptr);
}

bool ParseNumber(const HWND control, const wchar_t* const name, const float minimum,
                 const float maximum, float& value, std::wstring& error) {
    const std::wstring text = WindowText(control);
    if (text.empty()) {
        error = std::wstring(name) + L" is required.";
        return false;
    }
    errno = 0;
    wchar_t* end = nullptr;
    const float parsed = std::wcstof(text.c_str(), &end);
    while (end != nullptr && *end != L'\0' && std::iswspace(*end))
        ++end;
    if (errno == ERANGE || end == text.c_str() || (end != nullptr && *end != L'\0') ||
        !std::isfinite(parsed) || parsed < minimum || parsed > maximum) {
        error = std::wstring(name) + L" must be between " + NumberText(minimum) + L" and " +
                NumberText(maximum) + L".";
        return false;
    }
    value = parsed;
    return true;
}

bool IsValidTheme(const std::wstring& value) noexcept {
    if (value.empty())
        return false;
    if (value.front() != L'#')
        return true;
    const std::size_t digits = value.size() - 1;
    if (digits != 3 && digits != 4 && digits != 6 && digits != 8) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), [](const wchar_t character) {
        return (character >= L'0' && character <= L'9') ||
               (character >= L'a' && character <= L'f') || (character >= L'A' && character <= L'F');
    });
}

HRESULT ChooseConfigurationPath(const HWND owner, const bool save, std::wstring& path) noexcept {
    Microsoft::WRL::ComPtr<IFileDialog> dialog;
    HRESULT result = save ? ::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                               IID_PPV_ARGS(dialog.GetAddressOf()))
                          : ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                               IID_PPV_ARGS(dialog.GetAddressOf()));
    if (FAILED(result))
        return result;

    DWORD options = 0;
    result = dialog->GetOptions(&options);
    if (SUCCEEDED(result)) {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        options |= save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST;
        result = dialog->SetOptions(options);
    }
    constexpr COMDLG_FILTERSPEC filter[]{{L"Weapon Cycle configuration", L"*.json"},
                                         {L"All files", L"*.*"}};
    if (SUCCEEDED(result)) {
        result = dialog->SetFileTypes(static_cast<UINT>(std::size(filter)), filter);
    }
    if (SUCCEEDED(result)) {
        result = dialog->SetTitle(save ? L"Export Weapon Cycle configuration"
                                       : L"Import Weapon Cycle configuration");
    }
    if (save && SUCCEEDED(result)) {
        Microsoft::WRL::ComPtr<IFileSaveDialog> saveDialog;
        result = dialog.As(&saveDialog);
        if (SUCCEEDED(result)) {
            result = saveDialog->SetDefaultExtension(L"json");
        }
        if (SUCCEEDED(result)) {
            result = saveDialog->SetFileName(L"orbitdeck-config.json");
        }
    }
    if (SUCCEEDED(result))
        result = dialog->Show(owner);
    if (FAILED(result))
        return result;

    Microsoft::WRL::ComPtr<IShellItem> item;
    result = dialog->GetResult(item.GetAddressOf());
    PWSTR raw = nullptr;
    if (SUCCEEDED(result)) {
        result = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    }
    if (SUCCEEDED(result) && raw != nullptr)
        path.assign(raw);
    if (raw != nullptr)
        ::CoTaskMemFree(raw);
    return result;
}

}  // namespace

SettingsWindow::~SettingsWindow() noexcept {
    Destroy();
}

bool SettingsWindow::Create(const HINSTANCE instance, const HWND owner, const SaveCallback save,
                            const PreviewCallback preview, const TestCallback test,
                            void* const context) noexcept {
    if (instance == nullptr || owner == nullptr || save == nullptr) {
        return false;
    }

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    (void)::InitCommonControlsEx(&controls);
    dragListMessage_ = ::RegisterWindowMessageW(DRAGLISTMSGSTRING);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &SettingsWindow::WndProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(IDI_ORBITDECK));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    if (backgroundBrush_ == nullptr) {
        backgroundBrush_ = ::CreateSolidBrush(RGB(18, 23, 29));
    }
    if (fieldBrush_ == nullptr) {
        fieldBrush_ = ::CreateSolidBrush(RGB(30, 36, 44));
    }
    windowClass.hbrBackground = backgroundBrush_;
    windowClass.lpszClassName = ClassName();
    const ATOM atom = ::RegisterClassExW(&windowClass);
    if (atom == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    instance_ = instance;
    owner_ = owner;
    save_ = save;
    preview_ = preview;
    test_ = test;
    context_ = context;
    ownsClassRegistration_ = atom != 0;
    UINT actualDpi = ::GetDpiForWindow(owner);
    if (actualDpi == 0)
        actualDpi = USER_DEFAULT_SCREEN_DPI;
    const HMONITOR monitor = ::MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    dpi_ = FittedLayoutDpi(monitor, actualDpi);
    UpdateFont(dpi_);
    RECT work{};
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor != nullptr && ::GetMonitorInfoW(monitor, &monitorInfo)) {
        work = monitorInfo.rcWork;
    } else {
        (void)::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    }
    const int workWidth = std::max(1L, work.right - work.left);
    const int workHeight = std::max(1L, work.bottom - work.top);
    const int windowWidth =
        std::min(::MulDiv(kPreferencesWidth, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI),
                 std::max(1, static_cast<int>(workWidth) - 16));
    const int windowHeight =
        std::min(::MulDiv(kPreferencesHeight, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI),
                 std::max(1, static_cast<int>(workHeight) - 16));
    window_ =
        ::CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, ClassName(), L"Weapon Cycle",
                          WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
                          CW_USEDEFAULT, windowWidth, windowHeight, owner, nullptr, instance, this);
    if (window_ == nullptr) {
        Destroy();
        return false;
    }
    const BOOL dark = TRUE;
    (void)::DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    ::DragAcceptFiles(window_, TRUE);
    return true;
}

void SettingsWindow::Show(const Configuration& configuration) noexcept {
    if (window_ == nullptr)
        return;
    if (IsVisible()) {
        if (::IsIconic(window_)) {
            (void)::ShowWindow(window_, SW_RESTORE);
        }
        (void)::SetForegroundWindow(window_);
        UpdatePreview();
        return;
    }
    working_ = configuration;
    selectedPath_.clear();
    nextId_ = 1;
    RebuildList();
    LoadGlobalFields();
    RECT work{};
    if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        (void)::SetWindowPos(window_, nullptr, work.left + 28, work.top + 28, 0, 0,
                             SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
    }
    FitToWorkArea();
    (void)::ShowWindow(window_, SW_SHOWNORMAL);
    if (currentPage_ == 2 && installedApps_.empty())
        RefreshInstalledApps();
    (void)::SetFocus(itemList_);
    (void)::SetForegroundWindow(window_);
    UpdatePreview();
}

bool SettingsWindow::IsVisible() const noexcept {
    return window_ != nullptr && ::IsWindowVisible(window_) != FALSE;
}

HWND SettingsWindow::Handle() const noexcept {
    return window_;
}

HWND SettingsWindow::PreviewHandle() const noexcept {
    return previewHost_ != nullptr ? previewHost_ : window_;
}

void SettingsWindow::Destroy() noexcept {
    if (window_ != nullptr)
        (void)::KillTimer(window_, kPreviewTimerId);
    if (preview_ != nullptr)
        preview_(context_, nullptr);
    if (appDiscoveryState_ != nullptr) {
        AppDiscoveryRegistry& registry = DiscoveryRegistry();
        std::lock_guard lock(registry.mutex);
        appDiscoveryState_->target = nullptr;
    }
    CancelInstalledAppDiscovery();
    appDiscoveryState_.reset();
    if (window_ != nullptr) {
        MSG message{};
        while (::PeekMessageW(&message, window_, kAppsDiscoveredMessage, kAppsDiscoveredMessage,
                              PM_REMOVE)) {
            AppDiscoveryRegistry& registry = DiscoveryRegistry();
            std::lock_guard lock(registry.mutex);
            registry.completed.erase(reinterpret_cast<AppDiscoveryState*>(message.lParam));
        }
    }
    if (window_ != nullptr) {
        const HWND window = window_;
        window_ = nullptr;
        (void)::DestroyWindow(window);
    }
    if (ownsClassRegistration_) {
        (void)::UnregisterClassW(ClassName(), instance_);
    }
    instance_ = nullptr;
    owner_ = nullptr;
    save_ = nullptr;
    preview_ = nullptr;
    test_ = nullptr;
    context_ = nullptr;
    ownsClassRegistration_ = false;
    if (font_ != nullptr) {
        (void)::DeleteObject(font_);
        font_ = nullptr;
    }
    if (backgroundBrush_ != nullptr) {
        (void)::DeleteObject(backgroundBrush_);
        backgroundBrush_ = nullptr;
    }
    if (fieldBrush_ != nullptr) {
        (void)::DeleteObject(fieldBrush_);
        fieldBrush_ = nullptr;
    }
    flatPaths_.clear();
    selectedPath_.clear();
    pageControls_.clear();
    if (titleFont_ != nullptr) {
        (void)::DeleteObject(titleFont_);
        titleFont_ = nullptr;
    }
}

LRESULT CALLBACK SettingsWindow::WndProc(const HWND window, const UINT message, const WPARAM wParam,
                                         const LPARAM lParam) noexcept {
    auto* self = reinterpret_cast<SettingsWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<SettingsWindow*>(create->lpCreateParams);
        if (self == nullptr)
            return FALSE;
        self->window_ = window;
        (void)::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr) {
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
    const LRESULT result = self->HandleMessage(message, wParam, lParam);
    if (message == WM_NCDESTROY) {
        self->window_ = nullptr;
        (void)::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return result;
}

LRESULT SettingsWindow::HandleMessage(const UINT message, const WPARAM wParam,
                                      const LPARAM lParam) noexcept {
    if (message == kAppsDiscoveredMessage) {
        if (lParam == 0) {
            if (IsVisible() && currentPage_ == 2 && appDiscoveryState_ != nullptr &&
                appDiscoveryState_->cancel && !appDiscoveryState_->running) {
                RefreshInstalledApps();
            }
            return 0;
        }
        const auto token = reinterpret_cast<AppDiscoveryState*>(lParam);
        std::unique_ptr<AppDiscoveryResult> result;
        {
            AppDiscoveryRegistry& registry = DiscoveryRegistry();
            std::lock_guard lock(registry.mutex);
            const auto found = registry.completed.find(token);
            if (found != registry.completed.end()) {
                result = std::move(found->second);
                registry.completed.erase(found);
            }
        }
        if (result != nullptr && result->state == appDiscoveryState_) {
            result->state->running = false;
            installedApps_ = std::move(result->apps);
            FilterInstalledApps();
            const std::wstring status = result->error.empty()
                                            ? std::to_wstring(installedApps_.size()) +
                                                  L" apps found. Check one or more, then add."
                                            : result->error;
            (void)::SetWindowTextW(catalogStatus_, status.c_str());
            (void)::EnableWindow(appList_, TRUE);
        }
        return 0;
    }
    if (message == dragListMessage_ && lParam != 0) {
        auto* const info = reinterpret_cast<DRAGLISTINFO*>(lParam);
        const int target = LBItemFromPt(itemList_, info->ptCursor, TRUE);
        if (info->uNotification == DL_BEGINDRAG) {
            const int source = static_cast<int>(::SendMessageW(itemList_, LB_GETCURSEL, 0, 0));
            if (source < 0 || static_cast<std::size_t>(source) >= flatPaths_.size()) {
                return FALSE;
            }
            dragSourcePath_ = flatPaths_[static_cast<std::size_t>(source)];
            return TRUE;
        }
        if (info->uNotification == DL_DRAGGING) {
            if (target < 0 || static_cast<std::size_t>(target) >= flatPaths_.size() ||
                dragSourcePath_.empty()) {
                return DL_STOPCURSOR;
            }
            const auto& targetPath = flatPaths_[static_cast<std::size_t>(target)];
            if (targetPath.size() != dragSourcePath_.size() ||
                !std::equal(targetPath.begin(), targetPath.end() - 1, dragSourcePath_.begin())) {
                return DL_STOPCURSOR;
            }
            RECT itemBounds{};
            POINT cursor = info->ptCursor;
            (void)::ScreenToClient(itemList_, &cursor);
            const bool after =
                ::SendMessageW(itemList_, LB_GETITEMRECT, target,
                               reinterpret_cast<LPARAM>(&itemBounds)) != LB_ERR &&
                cursor.y >= itemBounds.top + (itemBounds.bottom - itemBounds.top) / 2;
            DrawInsert(window_, itemList_, target + (after ? 1 : 0));
            return DL_MOVECURSOR;
        }
        if (info->uNotification == DL_DROPPED) {
            DrawInsert(window_, itemList_, -1);
            bool after = target < 0;
            if (target >= 0) {
                RECT itemBounds{};
                POINT cursor = info->ptCursor;
                (void)::ScreenToClient(itemList_, &cursor);
                if (::SendMessageW(itemList_, LB_GETITEMRECT, target,
                                   reinterpret_cast<LPARAM>(&itemBounds)) != LB_ERR) {
                    after = cursor.y >= itemBounds.top + (itemBounds.bottom - itemBounds.top) / 2;
                }
            }
            MoveItemToFlatIndex(target, after);
            dragSourcePath_.clear();
            return TRUE;
        }
        if (info->uNotification == DL_CANCELDRAG) {
            DrawInsert(window_, itemList_, -1);
            dragSourcePath_.clear();
            return TRUE;
        }
    }

    switch (message) {
        case WM_TIMER:
            if (wParam == kPreviewTimerId) {
                (void)::KillTimer(window_, kPreviewTimerId);
                RenderPreview();
                return 0;
            }
            break;
        case WM_CREATE:
            return CreateControls() ? 0 : -1;
        case WM_ERASEBKGND: {
            RECT client{};
            (void)::GetClientRect(window_, &client);
            (void)::FillRect(reinterpret_cast<HDC>(wParam), &client, backgroundBrush_);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            const HDC context = reinterpret_cast<HDC>(wParam);
            (void)::SetTextColor(context, RGB(232, 235, 239));
            (void)::SetBkMode(context, TRANSPARENT);
            return reinterpret_cast<LRESULT>(backgroundBrush_);
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            const HDC context = reinterpret_cast<HDC>(wParam);
            (void)::SetTextColor(context, RGB(242, 244, 247));
            (void)::SetBkColor(context, RGB(30, 36, 44));
            return reinterpret_cast<LRESULT>(fieldBrush_);
        }
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == kActionsPage || id == kAppearancePage || id == kAppsPage) {
                ShowPage(id - kActionsPage);
            } else if (id == kTryWheel) {
                if (!::PostMessageW(owner_, WM_COMMAND,
                                    static_cast<WPARAM>(TrayIconController::Command::OpenWheel),
                                    0)) {
                    (void)::MessageBoxW(window_, L"Windows could not queue the wheel preview.",
                                        L"Weapon Cycle", MB_OK | MB_ICONERROR);
                }
            } else if (id == kOpenConverter && test_ != nullptr) {
                WheelItem item;
                item.label = L"Convert a file";
                item.builtInTool = BuiltInTool::FileConverter;
                test_(context_, &item);
            } else if (id == kItemList && HIWORD(wParam) == LBN_SELCHANGE) {
                SaveSelectedFields();
                const int selected =
                    static_cast<int>(::SendMessageW(itemList_, LB_GETCURSEL, 0, 0));
                selectedPath_ =
                    selected >= 0 && static_cast<std::size_t>(selected) < flatPaths_.size()
                        ? flatPaths_[static_cast<std::size_t>(selected)]
                        : std::vector<std::size_t>{};
                LoadSelectedFields();
                UpdatePreview();
            } else if (id == kActionCombo && HIWORD(wParam) == CBN_SELCHANGE) {
                actionEdited_ = true;
                const WheelItem* const item = ItemAt(selectedPath_);
                const int action =
                    static_cast<int>(::SendMessageW(actionCombo_, CB_GETCURSEL, 0, 0));
                const bool launchAction =
                    item != nullptr && !item->IsSubmenu() && ActionUsesTarget(action);
                (void)::EnableWindow(targetEdit_, launchAction);
                (void)::EnableWindow(windowActionCombo_,
                                     item != nullptr && ActionSupportsWindowPolicy(action));
                SaveSelectedFields();
                UpdatePreview();
            } else if (id == kWindowActionCombo && HIWORD(wParam) == CBN_SELCHANGE) {
                windowActionEdited_ = true;
                SaveSelectedFields();
                UpdatePreview();
            } else if ((id == kLabelEdit && HIWORD(wParam) == EN_CHANGE) ||
                       (id == kQuickKeyEdit && HIWORD(wParam) == EN_CHANGE) ||
                       ((id == kTargetEdit || id == kIconEdit) && HIWORD(wParam) == EN_KILLFOCUS) ||
                       (id == kEnabledCheck && HIWORD(wParam) == BN_CLICKED)) {
                UpdatePreview();
            } else if (id == kAppFilter && HIWORD(wParam) == EN_CHANGE) {
                FilterInstalledApps();
            } else if (id == kRefreshApps && HIWORD(wParam) == BN_CLICKED) {
                RefreshInstalledApps();
            } else if (id == kAddCheckedApps && HIWORD(wParam) == BN_CLICKED) {
                AddCheckedApps();
            } else if (id == kAddStarterPacks && HIWORD(wParam) == BN_CLICKED) {
                AddStarterPacks();
            } else if (id == kDuplicate && HIWORD(wParam) == BN_CLICKED) {
                DuplicateSelectedItem();
            } else if (id == kTestAction && HIWORD(wParam) == BN_CLICKED) {
                TestSelectedItem();
            } else if (id == kChooseAccent && HIWORD(wParam) == BN_CLICKED) {
                ChooseAccentColor();
            } else if ((id == kPlacementCombo && HIWORD(wParam) == CBN_SELCHANGE) ||
                       ((id == kRightClickCheck || id == kOuterExitCheck || id == kQuickKeysCheck ||
                         id == kReducedMotionCheck) &&
                        HIWORD(wParam) == BN_CLICKED) ||
                       ((id == kWheelSizeEdit || id == kOuterRadiusEdit || id == kInnerRadiusEdit ||
                         id == kNeutralRadiusEdit || id == kSelectionRadiusEdit ||
                         id == kWheelOpacityEdit || id == kScrimEdit || id == kAccentEdit) &&
                        HIWORD(wParam) == EN_CHANGE)) {
                UpdatePreview();
            } else if (id == kAddApp) {
                AddItem(false);
            } else if (id == kAddSubmenu) {
                AddItem(true);
            } else if (id == kRemove) {
                RemoveItem();
            } else if (id == kMoveUp) {
                MoveItem(-1);
            } else if (id == kMoveDown) {
                MoveItem(1);
            } else if (id == kResetDefaults) {
                ResetToDefaults();
            } else if (id == kImport) {
                ImportConfiguration();
            } else if (id == kExport) {
                ExportConfiguration();
            } else if (id == IDOK) {
                Save();
            } else if (id == IDCANCEL) {
                (void)::KillTimer(window_, kPreviewTimerId);
                CancelInstalledAppDiscovery();
                if (preview_ != nullptr)
                    preview_(context_, nullptr);
                (void)::ShowWindow(window_, SW_HIDE);
            }
            return 0;
        }
        case WM_NOTIFY: {
            const auto* const header = reinterpret_cast<const NMHDR*>(lParam);
            if (header != nullptr && header->hwndFrom == appList_ && header->code == NM_DBLCLK) {
                const int selected = ListView_GetNextItem(appList_, -1, LVNI_SELECTED);
                if (selected >= 0) {
                    ListView_SetCheckState(appList_, selected, TRUE);
                    AddCheckedApps();
                }
            }
            return 0;
        }
        case WM_DRAWITEM: {
            const auto* const item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (item != nullptr && item->CtlType == ODT_BUTTON) {
                DrawOwnerButton(*item, font_,
                                item->CtlID == static_cast<UINT>(kActionsPage + currentPage_));
                return TRUE;
            }
            if (item != nullptr && item->CtlID == kItemList &&
                item->itemID != static_cast<UINT>(-1) && item->itemID < flatPaths_.size()) {
                const bool selected = (item->itemState & ODS_SELECTED) != 0;
                const HBRUSH background =
                    ::CreateSolidBrush(selected ? RGB(48, 58, 65) : RGB(30, 36, 44));
                (void)::FillRect(item->hDC, &item->rcItem, background);
                (void)::DeleteObject(background);

                const std::vector<std::size_t>& path = flatPaths_[item->itemID];
                const WheelItem* const wheelItem = ItemAt(path);
                if (wheelItem == nullptr)
                    return TRUE;
                const int depth = static_cast<int>(path.size() > 0 ? path.size() - 1 : 0);
                const int left = item->rcItem.left + 10 + depth * 22;
                RECT ordinal{left, item->rcItem.top + 6, left + 20, item->rcItem.top + 26};
                const HBRUSH ordinalBrush =
                    ::CreateSolidBrush(selected ? RGB(245, 246, 242) : RGB(67, 73, 84));
                const HGDIOBJ oldBrush = ::SelectObject(item->hDC, ordinalBrush);
                const HGDIOBJ oldPen = ::SelectObject(item->hDC, ::GetStockObject(NULL_PEN));
                (void)::Ellipse(item->hDC, ordinal.left, ordinal.top, ordinal.right,
                                ordinal.bottom);
                (void)::SelectObject(item->hDC, oldBrush);
                (void)::SelectObject(item->hDC, oldPen);
                (void)::DeleteObject(ordinalBrush);

                const HGDIOBJ oldFont = ::SelectObject(
                    item->hDC, font_ != nullptr ? font_ : ::GetStockObject(DEFAULT_GUI_FONT));
                (void)::SetBkMode(item->hDC, TRANSPARENT);
                (void)::SetTextColor(item->hDC, selected ? RGB(20, 22, 25) : RGB(238, 241, 244));
                const std::wstring ordinalText = std::to_wstring(path.back() + 1);
                (void)::DrawTextW(item->hDC, ordinalText.c_str(), -1, &ordinal,
                                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                RECT label{ordinal.right + 10, item->rcItem.top, item->rcItem.right - 78,
                           item->rcItem.bottom};
                (void)::SetTextColor(item->hDC,
                                     wheelItem->enabled ? RGB(238, 241, 244) : RGB(126, 131, 140));
                const std::wstring displayLabel =
                    wheelItem->label.empty() ? L"(unnamed)" : wheelItem->label;
                (void)::DrawTextW(item->hDC, displayLabel.c_str(), -1, &label,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                const std::wstring type = wheelItem->IsSubmenu()                     ? L"MENU"
                                          : wheelItem->IsBuiltIn()                   ? L"TOOL"
                                          : wheelItem->launchType == L"shortcut"     ? L"KEY"
                                          : wheelItem->launchType == L"clipboard"    ? L"CLIP"
                                          : wheelItem->launchType == L"system"       ? L"SYS"
                                          : wheelItem->launchType == L"url"          ? L"URL"
                                          : wheelItem->launchType == L"file"         ? L"FILE"
                                          : wheelItem->launchType == L"shellCommand" ? L"CMD"
                                                                                     : L"APP";
                RECT tag{item->rcItem.right - 68, item->rcItem.top + 7, item->rcItem.right - 10,
                         item->rcItem.bottom - 7};
                const HBRUSH tagBrush = ::CreateSolidBrush(RGB(42, 46, 54));
                (void)::FillRect(item->hDC, &tag, tagBrush);
                (void)::DeleteObject(tagBrush);
                (void)::SetTextColor(item->hDC, RGB(184, 190, 199));
                (void)::DrawTextW(item->hDC, type.c_str(), -1, &tag,
                                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                (void)::SelectObject(item->hDC, oldFont);
                if ((item->itemState & ODS_FOCUS) != 0) {
                    (void)::DrawFocusRect(item->hDC, &item->rcItem);
                }
                return TRUE;
            }
            break;
        }
        case WM_DROPFILES:
            AddDroppedFiles(reinterpret_cast<HDROP>(wParam));
            return 0;
        case WM_CLOSE:
            (void)::KillTimer(window_, kPreviewTimerId);
            CancelInstalledAppDiscovery();
            if (preview_ != nullptr)
                preview_(context_, nullptr);
            (void)::ShowWindow(window_, SW_HIDE);
            return 0;
        case WM_MOVE:
            UpdatePreview();
            return 0;
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                if (preview_ != nullptr)
                    preview_(context_, nullptr);
            } else {
                UpdatePreview();
            }
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            FitToWorkArea();
            UpdatePreview();
            return 0;
        case WM_DPICHANGED: {
            const UINT actualDpi = HIWORD(wParam);
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            HMONITOR monitor = nullptr;
            if (suggested != nullptr) {
                monitor = ::MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST);
            }
            const UINT newDpi = FittedLayoutDpi(monitor, actualDpi);
            if (suggested != nullptr) {
                MONITORINFO info{};
                info.cbSize = sizeof(info);
                const bool haveMonitor = monitor != nullptr && ::GetMonitorInfoW(monitor, &info);
                const int desiredWidth =
                    ::MulDiv(kPreferencesWidth, static_cast<int>(newDpi), USER_DEFAULT_SCREEN_DPI);
                const int desiredHeight =
                    ::MulDiv(kPreferencesHeight, static_cast<int>(newDpi), USER_DEFAULT_SCREEN_DPI);
                const int maximumWidth =
                    haveMonitor ? info.rcWork.right - info.rcWork.left - 16 : desiredWidth;
                const int maximumHeight =
                    haveMonitor ? info.rcWork.bottom - info.rcWork.top - 16 : desiredHeight;
                (void)::SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                                     std::min(desiredWidth, maximumWidth),
                                     std::min(desiredHeight, maximumHeight),
                                     SWP_NOACTIVATE | SWP_NOZORDER);
            }
            ScaleChildren(dpi_, newDpi);
            dpi_ = newDpi;
            UpdateFont(dpi_);
            UpdatePreview();
            return 0;
        }
        default:
            return ::DefWindowProcW(window_, message, wParam, lParam);
    }
    return ::DefWindowProcW(window_, message, wParam, lParam);
}

bool SettingsWindow::CreateControls() noexcept {
    bool complete = true;
    int page = -1;
    const auto control = [&](const wchar_t* className, const wchar_t* text, const DWORD style,
                             const int id, const int x, const int y, const int width,
                             const int height) {
        const auto scaled = [&](const int value) {
            return ::MulDiv(value, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
        };
        DWORD controlStyle = style;
        if (std::wstring_view(className) == L"STATIC") {
            controlStyle |= SS_NOPREFIX;
        }
        if (std::wstring_view(className) == L"BUTTON") {
            const DWORD type = controlStyle & BS_TYPEMASK;
            if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) {
                controlStyle = (controlStyle & ~BS_TYPEMASK) | BS_OWNERDRAW;
            }
        }
        const HWND created = ::CreateWindowExW(
            0, className, text, WS_CHILD | WS_VISIBLE | controlStyle, scaled(x), scaled(y),
            scaled(width), scaled(height), window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (created != nullptr) {
            if (page >= 0)
                pageControls_.emplace_back(created, page);
            SetFont(created, font_);
            (void)::SetWindowTheme(created, L"DarkMode_Explorer", nullptr);
        } else {
            complete = false;
        }
        return created;
    };

    brandTitle_ = control(L"STATIC", L"Weapon Cycle", SS_LEFT, 0, 24, 18, 420, 32);
    (void)control(L"STATIC", L"Your desktop, one gesture away.", SS_LEFT, 0, 24, 54, 650, 22);
    (void)control(L"BUTTON", L"Your actions", BS_PUSHBUTTON | WS_TABSTOP, kActionsPage, 24, 90, 164,
                  34);
    (void)control(L"BUTTON", L"Look && behavior", BS_PUSHBUTTON | WS_TABSTOP, kAppearancePage, 198,
                  90, 184, 34);
    (void)control(L"BUTTON", L"Installed apps", BS_PUSHBUTTON | WS_TABSTOP, kAppsPage, 392, 90, 164,
                  34);

    page = 0;
    (void)control(L"STATIC", L"Your actions", SS_LEFT, 0, 24, 144, 250, 24);
    itemList_ = control(L"LISTBOX", L"",
                        WS_BORDER | WS_VSCROLL | WS_TABSTOP | WS_GROUP | LBS_NOTIFY |
                            LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                        kItemList, 24, 178, 324, 326);
    if (itemList_ != nullptr) {
        (void)MakeDragList(itemList_);
        (void)::SendMessageW(itemList_, LB_SETITEMHEIGHT, 0,
                             ::MulDiv(32, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI));
    }
    (void)control(L"BUTTON", L"+ Segment", BS_PUSHBUTTON | WS_TABSTOP, kAddApp, 24, 516, 100, 32);
    (void)control(L"BUTTON", L"+ Submenu", BS_PUSHBUTTON | WS_TABSTOP, kAddSubmenu, 134, 516, 104,
                  32);
    (void)control(L"BUTTON", L"Remove", BS_PUSHBUTTON | WS_TABSTOP, kRemove, 248, 516, 100, 32);
    (void)control(L"BUTTON", L"Move up", BS_PUSHBUTTON | WS_TABSTOP, kMoveUp, 24, 558, 100, 32);
    (void)control(L"BUTTON", L"Move down", BS_PUSHBUTTON | WS_TABSTOP, kMoveDown, 134, 558, 104,
                  32);

    page = 2;
    (void)control(L"STATIC", L"Add the apps you already use", SS_LEFT, 0, 24, 144, 500, 22);
    (void)control(L"STATIC", L"Search installed apps", SS_LEFT, 0, 24, 178, 400, 20);
    appFilter_ = control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, kAppFilter, 24, 204,
                         520, 30);
    (void)control(L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, kRefreshApps, 556, 204, 160,
                  30);
    appList_ = control(WC_LISTVIEWW, L"", WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS,
                       kAppList, 24, 250, 692, 278);
    if (appList_ != nullptr) {
        ListView_SetExtendedListViewStyle(
            appList_, LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT);
        LVCOLUMNW nameColumn{};
        nameColumn.mask = LVCF_TEXT | LVCF_WIDTH;
        nameColumn.pszText = const_cast<wchar_t*>(L"App");
        nameColumn.cx = ::MulDiv(510, static_cast<int>(dpi_), 96);
        (void)ListView_InsertColumn(appList_, 0, &nameColumn);
        LVCOLUMNW typeColumn{};
        typeColumn.mask = LVCF_TEXT | LVCF_WIDTH;
        typeColumn.pszText = const_cast<wchar_t*>(L"Type");
        typeColumn.cx = ::MulDiv(150, static_cast<int>(dpi_), 96);
        (void)ListView_InsertColumn(appList_, 1, &typeColumn);
    }
    catalogStatus_ = control(
        L"STATIC", L"App discovery runs only on this page. Nothing is indexed in the background.",
        SS_LEFT, kCatalogStatus, 24, 540, 692, 26);
    (void)control(L"BUTTON", L"Add checked", BS_DEFPUSHBUTTON | WS_TABSTOP, kAddCheckedApps, 24,
                  576, 164, 32);

    page = 0;
    (void)control(L"STATIC", L"Selected segment", SS_LEFT, 0, 376, 144, 250, 24);
    (void)control(L"BUTTON", L"Duplicate", BS_PUSHBUTTON | WS_TABSTOP, kDuplicate, 248, 558, 100,
                  32);
    (void)control(L"BUTTON", L"Test action", BS_PUSHBUTTON | WS_TABSTOP, kTestAction, 376, 558, 160,
                  32);
    (void)control(L"STATIC", L"Name", 0, 0, 376, 178, 340, 20);
    labelEdit_ = control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, kLabelEdit, 376,
                         202, 340, 30);
    (void)control(L"STATIC", L"File, URL, command, or shortcut", 0, 0, 376, 310, 340, 20);
    targetEdit_ = control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, kTargetEdit, 376,
                          334, 340, 30);
    (void)control(L"STATIC", L"Custom icon (optional)", 0, 0, 376, 378, 340, 20);
    iconEdit_ = control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, kIconEdit, 376, 402,
                        340, 30);
    enabledCheck_ = control(L"BUTTON", L"Enabled", BS_AUTOCHECKBOX | WS_TABSTOP, kEnabledCheck, 376,
                            448, 140, 24);
    (void)control(L"STATIC", L"Quick key", 0, 0, 552, 450, 90, 20);
    quickKeyEdit_ = control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_CENTER | ES_UPPERCASE,
                            kQuickKeyEdit, 656, 444, 60, 30);
    if (quickKeyEdit_ != nullptr) {
        (void)::SendMessageW(quickKeyEdit_, EM_SETLIMITTEXT, 1, 0);
    }
    (void)control(L"STATIC", L"Action type", 0, 0, 376, 244, 340, 20);
    actionCombo_ = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                           kActionCombo, 376, 268, 340, 440);
    for (const EditorActionChoice& action : kEditorActionChoices) {
        (void)::SendMessageW(actionCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(action.label));
    }

    (void)control(L"STATIC", L"When the app is already open", 0, 0, 376, 486, 340, 20);
    windowActionCombo_ = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                 kWindowActionCombo, 376, 510, 340, 180);
    for (const EditorWindowChoice& action : kEditorWindowChoices) {
        (void)::SendMessageW(windowActionCombo_, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(action.label));
    }

    page = 1;
    (void)control(L"STATIC", L"Make it feel like yours", SS_LEFT, 0, 24, 144, 600, 24);
    (void)control(L"STATIC", L"Global shortcut", 0, 0, 24, 182, 280, 20);
    hotkeyEdit_ = control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, kHotkeyEdit, 24,
                          206, 280, 30);
    (void)control(L"STATIC", L"Placement", 0, 0, 344, 182, 360, 20);
    placementCombo_ = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                              kPlacementCombo, 344, 206, 372, 120);
    (void)::SendMessageW(placementCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"At cursor"));
    (void)::SendMessageW(placementCombo_, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(L"Monitor centre"));

    (void)control(L"STATIC", L"Use a chord such as Shift+Space. Your other app keeps focus.",
                  SS_LEFT, 0, 24, 248, 692, 22);
    rightClickCheck_ = control(L"BUTTON", L"Right-click cancels", BS_AUTOCHECKBOX | WS_TABSTOP,
                               kRightClickCheck, 24, 286, 300, 24);
    outerExitCheck_ = control(L"BUTTON", L"Moving past the outer ring cancels",
                              BS_AUTOCHECKBOX | WS_TABSTOP, kOuterExitCheck, 344, 286, 372, 24);
    quickKeysCheck_ = control(L"BUTTON", L"Number keys 1-9 activate visible items",
                              BS_AUTOCHECKBOX | WS_TABSTOP, kQuickKeysCheck, 24, 322, 480, 24);

    (void)control(L"STATIC", L"Size, contrast & motion", 0, 0, 24, 370, 600, 22);
    (void)control(L"STATIC", L"Wheel diameter", 0, 0, 24, 408, 148, 20);
    wheelSizeEdit_ = control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, kWheelSizeEdit,
                             24, 432, 148, 30);
    (void)control(L"STATIC", L"Outer radius", 0, 0, 204, 408, 148, 20);
    outerRadiusEdit_ = control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                               kOuterRadiusEdit, 204, 432, 148, 30);
    (void)control(L"STATIC", L"Inner radius", 0, 0, 384, 408, 148, 20);
    innerRadiusEdit_ = control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                               kInnerRadiusEdit, 384, 432, 148, 30);

    (void)control(L"STATIC", L"Neutral radius", 0, 0, 24, 482, 148, 20);
    neutralRadiusEdit_ = control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                                 kNeutralRadiusEdit, 24, 506, 148, 30);
    (void)control(L"STATIC", L"Selection radius", 0, 0, 564, 408, 152, 20);
    selectionRadiusEdit_ = control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                                   kSelectionRadiusEdit, 564, 432, 152, 30);
    (void)control(L"STATIC", L"Opacity (0-1)", 0, 0, 204, 482, 148, 20);
    wheelOpacityEdit_ = control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                                kWheelOpacityEdit, 204, 506, 148, 30);

    (void)control(L"STATIC", L"Backdrop (0-1)", 0, 0, 384, 482, 148, 20);
    scrimEdit_ = control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, kScrimEdit, 384,
                         506, 148, 30);

    (void)control(L"STATIC", L"Accent / theme", 0, 0, 564, 482, 152, 20);
    accentEdit_ = control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, kAccentEdit, 564,
                          506, 92, 30);
    (void)control(L"BUTTON", L"Pick...", BS_PUSHBUTTON | WS_TABSTOP, kChooseAccent, 664, 506, 52,
                  30);
    reducedMotionCheck_ = control(L"BUTTON", L"Reduce motion", BS_AUTOCHECKBOX | WS_TABSTOP,
                                  kReducedMotionCheck, 24, 564, 280, 24);
    (void)control(L"STATIC", L"Dimensions are in DIPs and follow your display scaling.", SS_LEFT, 0,
                  344, 564, 372, 36);
    page = -1;
    (void)control(L"STATIC", L"Live preview", SS_LEFT, 0, 770, 144, 280, 22);
    previewHost_ = control(L"STATIC", L"", SS_LEFT, 0, 770, 178, 280, 280);
    (void)control(L"STATIC",
                  L"Hold your shortcut. Point to an action.\nRelease to open. Escape to cancel.",
                  SS_LEFT, 0, 770, 480, 280, 54);
    (void)control(L"BUTTON", L"Convert a file...", BS_PUSHBUTTON | WS_TABSTOP, kOpenConverter, 916,
                  550, 134, 34);
    (void)control(L"BUTTON", L"Try saved wheel", BS_PUSHBUTTON | WS_TABSTOP, kTryWheel, 770, 550,
                  134, 34);
    (void)control(L"STATIC", L"Select a file in Explorer: Ctrl+Alt+C", SS_LEFT, 0, 770, 596, 280,
                  22);
    if (appList_ != nullptr) {
        ListView_SetBkColor(appList_, RGB(30, 36, 44));
        ListView_SetTextBkColor(appList_, RGB(30, 36, 44));
        ListView_SetTextColor(appList_, RGB(236, 239, 243));
        ListView_SetOutlineColor(appList_, RGB(74, 79, 90));
    }

    (void)control(L"BUTTON", L"Save changes", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, 906, 632, 144,
                  34);
    (void)control(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 804, 632, 90, 34);
    (void)control(L"BUTTON", L"Reset defaults...", BS_PUSHBUTTON | WS_TABSTOP, kResetDefaults, 564,
                  632, 148, 34);
    (void)control(L"BUTTON", L"Import...", BS_PUSHBUTTON | WS_TABSTOP, kImport, 24, 632, 100, 34);
    (void)control(L"BUTTON", L"Export...", BS_PUSHBUTTON | WS_TABSTOP, kExport, 136, 632, 100, 34);
    (void)control(L"BUTTON", L"Add starter packs", BS_PUSHBUTTON | WS_TABSTOP, kAddStarterPacks,
                  248, 632, 156, 34);
    ShowPage(0);
    UpdateFont(dpi_);
    return complete && itemList_ != nullptr && previewHost_ != nullptr && appFilter_ != nullptr &&
           appList_ != nullptr && catalogStatus_ != nullptr && labelEdit_ != nullptr &&
           targetEdit_ != nullptr && iconEdit_ != nullptr && enabledCheck_ != nullptr &&
           quickKeyEdit_ != nullptr && actionCombo_ != nullptr && windowActionCombo_ != nullptr &&
           placementCombo_ != nullptr && hotkeyEdit_ != nullptr && rightClickCheck_ != nullptr &&
           outerExitCheck_ != nullptr && quickKeysCheck_ != nullptr && wheelSizeEdit_ != nullptr &&
           outerRadiusEdit_ != nullptr && innerRadiusEdit_ != nullptr &&
           neutralRadiusEdit_ != nullptr && selectionRadiusEdit_ != nullptr &&
           wheelOpacityEdit_ != nullptr && scrimEdit_ != nullptr && accentEdit_ != nullptr &&
           reducedMotionCheck_ != nullptr;
}

void SettingsWindow::UpdateFont(const UINT dpi) noexcept {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    HFONT replacement = nullptr;
    if (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
        metrics.lfMessageFont.lfHeight = -::MulDiv(14, static_cast<int>(dpi), 96);
        replacement = ::CreateFontIndirectW(&metrics.lfMessageFont);
    }
    if (font_ != nullptr) {
        (void)::DeleteObject(font_);
    }
    font_ = replacement;
    for (HWND child = ::GetWindow(window_, GW_CHILD); child != nullptr;
         child = ::GetWindow(child, GW_HWNDNEXT)) {
        SetFont(child, font_);
    }
    if (titleFont_ != nullptr)
        (void)::DeleteObject(titleFont_);
    titleFont_ =
        ::CreateFontW(-::MulDiv(24, static_cast<int>(dpi), 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                      FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                      CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Display");
    SetFont(brandTitle_, titleFont_);
}

void SettingsWindow::ScaleChildren(const UINT oldDpi, const UINT newDpi) noexcept {
    if (window_ == nullptr || oldDpi == 0 || oldDpi == newDpi)
        return;
    for (HWND child = ::GetWindow(window_, GW_CHILD); child != nullptr;
         child = ::GetWindow(child, GW_HWNDNEXT)) {
        RECT bounds{};
        if (!::GetWindowRect(child, &bounds))
            continue;
        POINT points[]{{bounds.left, bounds.top}, {bounds.right, bounds.bottom}};
        (void)::MapWindowPoints(nullptr, window_, points, 2);
        (void)::SetWindowPos(
            child, nullptr,
            ::MulDiv(points[0].x, static_cast<int>(newDpi), static_cast<int>(oldDpi)),
            ::MulDiv(points[0].y, static_cast<int>(newDpi), static_cast<int>(oldDpi)),
            ::MulDiv(points[1].x - points[0].x, static_cast<int>(newDpi), static_cast<int>(oldDpi)),
            ::MulDiv(points[1].y - points[0].y, static_cast<int>(newDpi), static_cast<int>(oldDpi)),
            SWP_NOACTIVATE | SWP_NOZORDER);
    }
}

void SettingsWindow::FitToWorkArea() noexcept {
    if (window_ == nullptr)
        return;
    const HMONITOR monitor = ::MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || !::GetMonitorInfoW(monitor, &info))
        return;
    UINT actualDpi = ::GetDpiForWindow(window_);
    if (actualDpi == 0)
        actualDpi = USER_DEFAULT_SCREEN_DPI;
    const UINT fittedDpi = FittedLayoutDpi(monitor, actualDpi);
    if (fittedDpi != dpi_) {
        ScaleChildren(dpi_, fittedDpi);
        dpi_ = fittedDpi;
        UpdateFont(dpi_);
    }
    const int width =
        std::min(::MulDiv(kPreferencesWidth, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI),
                 static_cast<int>(info.rcWork.right - info.rcWork.left) - 16);
    const int height =
        std::min(::MulDiv(kPreferencesHeight, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI),
                 static_cast<int>(info.rcWork.bottom - info.rcWork.top) - 16);
    RECT current{};
    (void)::GetWindowRect(window_, &current);
    const int left = std::clamp(
        static_cast<int>(current.left), static_cast<int>(info.rcWork.left),
        std::max(static_cast<int>(info.rcWork.left), static_cast<int>(info.rcWork.right) - width));
    const int top = std::clamp(
        static_cast<int>(current.top), static_cast<int>(info.rcWork.top),
        std::max(static_cast<int>(info.rcWork.top), static_cast<int>(info.rcWork.bottom) - height));
    (void)::SetWindowPos(window_, nullptr, left, top, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
}

void SettingsWindow::RebuildList(const std::vector<std::size_t>& selectPath) noexcept {
    (void)::SendMessageW(itemList_, LB_RESETCONTENT, 0, 0);
    flatPaths_.clear();
    std::vector<std::size_t> path;
    AppendItems(working_.items, path, 0);
    const std::vector<std::size_t>& wanted = selectPath.empty() ? selectedPath_ : selectPath;
    int index = FlatIndexOf(wanted);
    if (index < 0 && !flatPaths_.empty())
        index = 0;
    (void)::SendMessageW(itemList_, LB_SETCURSEL, index, 0);
    selectedPath_ =
        index >= 0 ? flatPaths_[static_cast<std::size_t>(index)] : std::vector<std::size_t>{};
    LoadSelectedFields();
}

void SettingsWindow::AppendItems(const std::vector<WheelItem>& items,
                                 std::vector<std::size_t>& path, const std::size_t depth) noexcept {
    for (std::size_t index = 0; index < items.size(); ++index) {
        path.push_back(index);
        std::wstring text(depth * 3, L' ');
        text += items[index].IsSubmenu() ? L"> " : L"  ";
        text += items[index].label.empty() ? L"(unnamed)" : items[index].label;
        (void)::SendMessageW(itemList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        flatPaths_.push_back(path);
        AppendItems(items[index].children, path, depth + 1);
        path.pop_back();
    }
}

void SettingsWindow::SaveSelectedFields() noexcept {
    WheelItem* const item = ItemAt(selectedPath_);
    if (item == nullptr)
        return;
    item->label = WindowText(labelEdit_);
    item->enabled = ::SendMessageW(enabledCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const std::wstring quickKey = WindowText(quickKeyEdit_);
    item->quickKey = quickKey.size() == 1 && std::iswalnum(quickKey.front())
                         ? static_cast<wchar_t>(std::towupper(quickKey.front()))
                         : L'\0';
    if (!item->IsSubmenu()) {
        const int action = static_cast<int>(::SendMessageW(actionCombo_, CB_GETCURSEL, 0, 0));
        const int choiceIndex =
            action >= 0 && action < kActionChoiceCount ? action : kActionApplication;
        const EditorActionChoice& choice = kEditorActionChoices[choiceIndex];
        if (actionEdited_) {
            item->builtInTool = choice.tool;
        }
        if (item->builtInTool == BuiltInTool::None) {
            std::wstring target = WindowText(targetEdit_);
            if (actionEdited_) {
                if (choice.systemTarget[0] != L'\0') {
                    target = choice.systemTarget;
                }
                item->launchType = choice.launchType;
                item->launchKind =
                    action == kActionPackagedApp ? LaunchKind::Aumid : LaunchKind::Shell;
            }
            item->configuredTarget = target;
            const std::wstring normalizedType = Lower(item->launchType);
            item->target = normalizedType == L"clipboard" || normalizedType == L"shellcommand" ||
                                   normalizedType == L"shortcut" || normalizedType == L"system" ||
                                   normalizedType == L"url" || normalizedType == L"uri"
                               ? target
                               : ExpandConfigurationValue(target);
            if (item->launchCandidates.empty()) {
                item->launchCandidates.push_back({});
            }
            LaunchCandidate& first = item->launchCandidates.front();
            first.configuredTarget = item->configuredTarget;
            first.target = item->target;
            first.type = item->launchType;
            first.kind = item->launchKind;
            if (windowActionEdited_ && ActionSupportsWindowPolicy(action)) {
                const LRESULT windowAction = ::SendMessageW(windowActionCombo_, CB_GETCURSEL, 0, 0);
                const std::size_t windowIndex =
                    windowAction >= 0 &&
                            static_cast<std::size_t>(windowAction) < std::size(kEditorWindowChoices)
                        ? static_cast<std::size_t>(windowAction)
                        : 0;
                const EditorWindowChoice& windowChoice = kEditorWindowChoices[windowIndex];
                item->windowAction = windowChoice.action;
                item->existingWindowAction = windowChoice.configurationName;
                item->maximizeOnActivate =
                    item->windowAction == ExistingWindowAction::MaximizeAndFocus;
            }
        } else if (actionEdited_) {
            item->configuredTarget.clear();
            item->target.clear();
            item->launchCandidates.clear();
            item->launchType = L"shell";
            item->launchKind = LaunchKind::Shell;
        }
    }
    const std::wstring icon = WindowText(iconEdit_);
    item->iconAutomatic = icon.empty();
    item->configuredIconSource = icon;
    item->iconSource = item->iconAutomatic ? item->target : ExpandConfigurationValue(icon);
}

void SettingsWindow::LoadSelectedFields() noexcept {
    const bool wasUpdating = updatingControls_;
    updatingControls_ = true;
    WheelItem* const item = ItemAt(selectedPath_);
    const bool available = item != nullptr;
    (void)::EnableWindow(labelEdit_, available);
    (void)::EnableWindow(iconEdit_, available);
    (void)::EnableWindow(enabledCheck_, available);
    (void)::EnableWindow(quickKeyEdit_, available);
    (void)::EnableWindow(
        targetEdit_,
        available && !item->IsSubmenu() &&
            ActionUsesTarget(static_cast<int>(::SendMessageW(actionCombo_, CB_GETCURSEL, 0, 0))));
    (void)::EnableWindow(actionCombo_, available && !item->IsSubmenu());
    (void)::EnableWindow(windowActionCombo_, available && !item->IsSubmenu() && !item->IsBuiltIn());
    (void)::SetWindowTextW(labelEdit_, available ? item->label.c_str() : L"");
    const wchar_t quickKey[] = {available ? item->quickKey : L'\0', L'\0'};
    (void)::SetWindowTextW(quickKeyEdit_, quickKey);
    (void)::SetWindowTextW(targetEdit_,
                           available && !item->IsSubmenu()
                               ? (item->configuredTarget.empty() ? item->target.c_str()
                                                                 : item->configuredTarget.c_str())
                               : L"");
    (void)::SetWindowTextW(
        iconEdit_, available && !item->iconAutomatic
                       ? (item->configuredIconSource.empty() ? item->iconSource.c_str()
                                                             : item->configuredIconSource.c_str())
                       : L"");
    (void)::SendMessageW(enabledCheck_, BM_SETCHECK,
                         available && item->enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    const int action = available ? EditorActionIndex(*item) : kActionApplication;
    (void)::SendMessageW(actionCombo_, CB_SETCURSEL, action, 0);
    actionEdited_ = false;
    (void)::EnableWindow(targetEdit_, available && !item->IsSubmenu() && ActionUsesTarget(action));
    (void)::EnableWindow(windowActionCombo_,
                         available && !item->IsSubmenu() && ActionSupportsWindowPolicy(action));
    const int windowAction = available ? EditorWindowIndex(item->windowAction) : 0;
    (void)::SendMessageW(windowActionCombo_, CB_SETCURSEL, windowAction, 0);
    windowActionEdited_ = false;
    updatingControls_ = wasUpdating;
}

bool SettingsWindow::SaveAppearanceFields(const bool showValidationError) noexcept {
    AppearanceConfig appearance = working_.appearance;
    std::wstring error;
    if (!ParseNumber(wheelSizeEdit_, L"Wheel diameter", 240.0F, 2000.0F,
                     appearance.wheelDiameterDip, error) ||
        !ParseNumber(outerRadiusEdit_, L"Outer radius", 1.0F, 1000.0F, appearance.outerRadiusDip,
                     error) ||
        !ParseNumber(innerRadiusEdit_, L"Inner radius", 1.0F, 1000.0F, appearance.innerRadiusDip,
                     error) ||
        !ParseNumber(neutralRadiusEdit_, L"Neutral radius", 1.0F, 1000.0F,
                     appearance.neutralThresholdDip, error) ||
        !ParseNumber(selectionRadiusEdit_, L"Selection radius", 1.0F, 1000.0F,
                     appearance.selectionThresholdDip, error) ||
        !ParseNumber(wheelOpacityEdit_, L"Wheel opacity", 0.2F, 1.0F, appearance.wheelOpacity,
                     error) ||
        !ParseNumber(scrimEdit_, L"Backdrop opacity", 0.0F, 1.0F, appearance.scrimOpacity, error)) {
        if (showValidationError) {
            (void)::MessageBoxW(window_, error.c_str(), L"Weapon Cycle", MB_OK | MB_ICONWARNING);
        }
        return false;
    }
    appearance.theme = WindowText(accentEdit_);
    if (!IsValidTheme(appearance.theme)) {
        error =
            L"Accent / theme must be a name or #RGB, #RGBA, #RRGGBB, or "
            L"#RRGGBBAA.";
    } else if (appearance.innerRadiusDip >= appearance.outerRadiusDip) {
        error = L"Inner radius must be smaller than outer radius.";
    } else if (appearance.outerRadiusDip - appearance.innerRadiusDip < 24.0F) {
        error = L"Inner and outer radii must be at least 24 DIPs apart.";
    } else if (appearance.outerRadiusDip > appearance.wheelDiameterDip * 0.5F - 8.0F) {
        error = L"Outer radius must fit inside the wheel diameter.";
    } else if (appearance.neutralThresholdDip > appearance.selectionThresholdDip) {
        error = L"Neutral radius must not exceed selection radius.";
    } else if (appearance.selectionThresholdDip > appearance.outerRadiusDip) {
        error = L"Selection radius must not exceed outer radius.";
    }
    if (!error.empty()) {
        if (showValidationError) {
            (void)::MessageBoxW(window_, error.c_str(), L"Weapon Cycle", MB_OK | MB_ICONWARNING);
        }
        return false;
    }
    appearance.reducedMotion =
        ::SendMessageW(reducedMotionCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    working_.appearance = std::move(appearance);
    return true;
}

bool SettingsWindow::SaveHotkeyField(const bool showValidationError) noexcept {
    ActivationConfig activation = working_.activation;
    if (!ParseHotkeyText(WindowText(hotkeyEdit_), activation)) {
        if (showValidationError) {
            (void)::MessageBoxW(window_,
                                L"Hotkey must contain one key and optional Ctrl, Alt, Shift, "
                                L"or Win modifiers.",
                                L"Weapon Cycle", MB_OK | MB_ICONWARNING);
        }
        return false;
    }
    activation.experimentalLeftShiftOnly = working_.activation.experimentalLeftShiftOnly;
    activation.leftShiftHoldThresholdMs = working_.activation.leftShiftHoldThresholdMs;
    working_.activation = activation;
    return true;
}

void SettingsWindow::LoadGlobalFields() noexcept {
    const bool wasUpdating = updatingControls_;
    updatingControls_ = true;
    (void)::SetWindowTextW(hotkeyEdit_, FormatHotkey(working_.activation).c_str());
    (void)::SendMessageW(placementCombo_, CB_SETCURSEL,
                         working_.placement.mode == PlacementMode::MonitorCentre ? 1 : 0, 0);
    (void)::SendMessageW(rightClickCheck_, BM_SETCHECK,
                         working_.interaction.cancelOnRightClick ? BST_CHECKED : BST_UNCHECKED, 0);
    (void)::SendMessageW(outerExitCheck_, BM_SETCHECK,
                         working_.interaction.cancelOnOuterExit ? BST_CHECKED : BST_UNCHECKED, 0);
    (void)::SendMessageW(quickKeysCheck_, BM_SETCHECK,
                         working_.interaction.numberQuickKeys ? BST_CHECKED : BST_UNCHECKED, 0);
    (void)::SetWindowTextW(wheelSizeEdit_,
                           NumberText(working_.appearance.wheelDiameterDip).c_str());
    (void)::SetWindowTextW(outerRadiusEdit_,
                           NumberText(working_.appearance.outerRadiusDip).c_str());
    (void)::SetWindowTextW(innerRadiusEdit_,
                           NumberText(working_.appearance.innerRadiusDip).c_str());
    (void)::SetWindowTextW(neutralRadiusEdit_,
                           NumberText(working_.appearance.neutralThresholdDip).c_str());
    (void)::SetWindowTextW(selectionRadiusEdit_,
                           NumberText(working_.appearance.selectionThresholdDip).c_str());
    (void)::SetWindowTextW(wheelOpacityEdit_, NumberText(working_.appearance.wheelOpacity).c_str());
    (void)::SetWindowTextW(scrimEdit_, NumberText(working_.appearance.scrimOpacity).c_str());
    (void)::SetWindowTextW(accentEdit_, working_.appearance.theme.c_str());
    (void)::SendMessageW(reducedMotionCheck_, BM_SETCHECK,
                         working_.appearance.reducedMotion ? BST_CHECKED : BST_UNCHECKED, 0);
    updatingControls_ = wasUpdating;
}

void SettingsWindow::UpdatePreview() noexcept {
    if (updatingControls_ || !IsVisible() || preview_ == nullptr)
        return;
    if (::SetTimer(window_, kPreviewTimerId, 75, nullptr) == 0) {
        ::OutputDebugStringW(L"Weapon Cycle: preview debounce timer unavailable.\n");
        RenderPreview();
    }
}

void SettingsWindow::ShowPage(const int page) noexcept {
    currentPage_ = page;
    for (const auto& [control, controlPage] : pageControls_) {
        (void)::ShowWindow(control, controlPage == page ? SW_SHOW : SW_HIDE);
    }
    for (int id = kActionsPage; id <= kAppsPage; ++id) {
        (void)::InvalidateRect(::GetDlgItem(window_, id), nullptr, TRUE);
    }
    if (page == 2 && IsVisible() && installedApps_.empty()) {
        RefreshInstalledApps();
    } else if (page != 2) {
        CancelInstalledAppDiscovery();
    }
    UpdatePreview();
}

void SettingsWindow::RenderPreview() noexcept {
    if (updatingControls_ || !IsVisible() || preview_ == nullptr)
        return;
    SaveSelectedFields();
    working_.placement.mode = ::SendMessageW(placementCombo_, CB_GETCURSEL, 0, 0) == 1
                                  ? PlacementMode::MonitorCentre
                                  : PlacementMode::Cursor;
    working_.interaction.cancelOnRightClick =
        ::SendMessageW(rightClickCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    working_.interaction.cancelOnOuterExit =
        ::SendMessageW(outerExitCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    working_.interaction.numberQuickKeys =
        ::SendMessageW(quickKeysCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (SaveAppearanceFields(false)) {
        preview_(context_, &working_);
    }
}

void SettingsWindow::ImportConfiguration() noexcept {
    std::wstring path;
    const HRESULT chosen = ChooseConfigurationPath(window_, false, path);
    if (chosen == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return;
    if (FAILED(chosen)) {
        (void)::MessageBoxW(window_, L"The Windows file picker could not be opened.",
                            L"Weapon Cycle", MB_OK | MB_ICONERROR);
        return;
    }
    std::string sourceJson;
    ConfigParseResult imported = LoadConfigurationFile(path, &sourceJson);
    if (!imported.ok) {
        (void)::MessageBoxW(window_,
                            imported.error.empty() ? L"The selected configuration is invalid."
                                                   : imported.error.c_str(),
                            L"Weapon Cycle", MB_OK | MB_ICONERROR);
        return;
    }
    if (::MessageBoxW(window_, L"Replace the current settings with this validated configuration?",
                      L"Import Weapon Cycle configuration",
                      MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    std::wstring error;
    if (!save_(context_, imported.config, true, &sourceJson, error)) {
        (void)::MessageBoxW(
            window_, error.empty() ? L"The configuration could not be imported." : error.c_str(),
            L"Weapon Cycle", MB_OK | MB_ICONERROR);
        return;
    }
    working_ = std::move(imported.config);
    selectedPath_.clear();
    LoadGlobalFields();
    RebuildList();
    UpdatePreview();
}

void SettingsWindow::ExportConfiguration() noexcept {
    SaveSelectedFields();
    if (!SaveAppearanceFields(true) || !SaveHotkeyField(true)) {
        return;
    }
    working_.placement.mode = ::SendMessageW(placementCombo_, CB_GETCURSEL, 0, 0) == 1
                                  ? PlacementMode::MonitorCentre
                                  : PlacementMode::Cursor;
    working_.interaction.cancelOnRightClick =
        ::SendMessageW(rightClickCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    working_.interaction.cancelOnOuterExit =
        ::SendMessageW(outerExitCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    working_.interaction.numberQuickKeys =
        ::SendMessageW(quickKeysCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;

    std::wstring path;
    const HRESULT chosen = ChooseConfigurationPath(window_, true, path);
    if (chosen == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return;
    if (FAILED(chosen)) {
        (void)::MessageBoxW(window_, L"The Windows file picker could not be opened.",
                            L"Weapon Cycle", MB_OK | MB_ICONERROR);
        return;
    }
    const ConfigExportResult exported = ExportConfigurationFile(working_, path);
    (void)::MessageBoxW(window_,
                        exported.ok
                            ? L"Configuration exported."
                            : (exported.error.empty() ? L"The configuration could not be exported."
                                                      : exported.error.c_str()),
                        L"Weapon Cycle", MB_OK | (exported.ok ? MB_ICONINFORMATION : MB_ICONERROR));
}

void SettingsWindow::AddItem(const bool submenu) noexcept {
    SaveSelectedFields();
    std::vector<WheelItem>* destination = &working_.items;
    std::vector<std::size_t> addedPath;
    std::size_t itemDepth = 1;
    if (WheelItem* const selected = ItemAt(selectedPath_);
        selected != nullptr && selected->IsSubmenu()) {
        destination = &selected->children;
        addedPath = selectedPath_;
        itemDepth = selectedPath_.size() + 1;
    }
    if (destination->size() >= kMaximumWheelItems) {
        (void)::MessageBoxW(window_, L"This wheel level already has the maximum number of items.",
                            L"Weapon Cycle", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const auto countItems = [&](const auto& self,
                                const std::vector<WheelItem>& items) -> std::size_t {
        std::size_t count = items.size();
        for (const WheelItem& item : items) {
            count += self(self, item.children);
        }
        return count;
    };
    const std::size_t addedCount = submenu ? 2 : 1;
    if (countItems(countItems, working_.items) + addedCount > kMaximumTotalWheelItems ||
        itemDepth + (submenu ? 1 : 0) > kMaximumWheelDepth) {
        (void)::MessageBoxW(window_,
                            L"Adding this item would exceed the supported menu depth or "
                            L"total item limit.",
                            L"Weapon Cycle", MB_OK | MB_ICONINFORMATION);
        return;
    }

    WheelItem item;
    item.id = NextId();
    item.label = submenu ? L"New Menu" : L"New App";
    if (submenu) {
        WheelItem child;
        child.id = NextId();
        child.label = L"New App";
        item.children.push_back(std::move(child));
    }
    const std::size_t newIndex = destination->size();
    destination->push_back(std::move(item));
    addedPath.push_back(newIndex);
    if (submenu)
        addedPath.push_back(0);
    RebuildList(addedPath);
    UpdatePreview();
}

void SettingsWindow::DuplicateSelectedItem() noexcept {
    if (selectedPath_.empty())
        return;
    SaveSelectedFields();
    std::vector<WheelItem>* const siblings = SiblingsAt(selectedPath_);
    if (siblings == nullptr || siblings->size() >= kMaximumWheelItems) {
        (void)::MessageBoxW(window_, L"This wheel level cannot accept another segment.",
                            L"Weapon Cycle", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::size_t source = selectedPath_.back();
    if (source >= siblings->size())
        return;
    WheelItem copy = (*siblings)[source];
    const auto assignIds = [&](const auto& self, WheelItem& item) -> void {
        item.id = NextId();
        for (WheelItem& child : item.children)
            self(self, child);
    };
    assignIds(assignIds, copy);
    const std::size_t inserted = source + 1;
    siblings->insert(siblings->begin() + inserted, std::move(copy));
    ConfigParseResult validation = ParseConfiguration(SerializeConfiguration(working_));
    if (!validation.ok) {
        siblings->erase(siblings->begin() + inserted);
        (void)::MessageBoxW(window_,
                            validation.error.empty() ? L"This segment cannot be duplicated."
                                                     : validation.error.c_str(),
                            L"Weapon Cycle", MB_OK | MB_ICONWARNING);
        return;
    }
    working_ = std::move(validation.config);
    selectedPath_.back() = inserted;
    RebuildList(selectedPath_);
    UpdatePreview();
}

void SettingsWindow::TestSelectedItem() noexcept {
    SaveSelectedFields();
    const WheelItem* const item = ItemAt(selectedPath_);
    if (item == nullptr || !item->enabled || item->IsSubmenu() ||
        (!item->IsBuiltIn() && item->target.empty()) || test_ == nullptr) {
        (void)::MessageBoxW(window_, L"Select an enabled leaf action to test.", L"Weapon Cycle",
                            MB_OK | MB_ICONINFORMATION);
        return;
    }
    test_(context_, item);
}

void SettingsWindow::RefreshInstalledApps() noexcept {
    if (appList_ == nullptr || catalogStatus_ == nullptr)
        return;
    if (appDiscoveryState_ != nullptr && appDiscoveryState_->running) {
        if (appDiscoveryState_->cancel) {
            (void)::SetWindowTextW(catalogStatus_,
                                   L"Finishing the previous discovery before retrying...");
        }
        return;
    }
    CancelInstalledAppDiscovery();
    (void)::EnableWindow(appList_, FALSE);
    (void)::SetWindowTextW(catalogStatus_, L"Discovering installed apps...");
    auto state = std::make_shared<AppDiscoveryState>();
    state->target = window_;
    appDiscoveryState_ = state;
    try {
        std::thread([state] {
            const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            auto result = std::make_unique<AppDiscoveryResult>();
            result->state = state;
            result->apps = DiscoverInstalledApps(result->error, &state->cancel);
            if (SUCCEEDED(com))
                ::CoUninitialize();
            state->running = false;
            AppDiscoveryRegistry& registry = DiscoveryRegistry();
            std::lock_guard lock(registry.mutex);
            if (state->target == nullptr)
                return;
            if (state->cancel) {
                if (!::PostMessageW(state->target, kAppsDiscoveredMessage, 0, 0)) {
                    ::OutputDebugStringW(
                        L"Weapon Cycle: app discovery retry notification failed.\n");
                }
                return;
            }
            registry.completed[state.get()] = std::move(result);
            if (!::PostMessageW(state->target, kAppsDiscoveredMessage, 0,
                                reinterpret_cast<LPARAM>(state.get()))) {
                registry.completed.erase(state.get());
                ::OutputDebugStringW(
                    L"Weapon Cycle: app discovery completion notification failed.\n");
            }
        }).detach();
    } catch (const std::system_error&) {
        state->running = false;
        (void)::EnableWindow(appList_, TRUE);
        (void)::SetWindowTextW(catalogStatus_, L"Windows could not start installed-app discovery.");
    }
}

void SettingsWindow::CancelInstalledAppDiscovery() noexcept {
    if (appDiscoveryState_ == nullptr)
        return;
    {
        AppDiscoveryRegistry& registry = DiscoveryRegistry();
        std::lock_guard lock(registry.mutex);
        appDiscoveryState_->cancel = true;
        registry.completed.erase(appDiscoveryState_.get());
    }
    if (appList_ != nullptr) {
        (void)::EnableWindow(appList_, TRUE);
    }
    if (catalogStatus_ != nullptr) {
        const std::wstring status =
            installedApps_.empty()
                ? L"Discovery paused. Open this page or Refresh to try again."
                : std::to_wstring(installedApps_.size()) + L" apps cached. Refresh to rescan.";
        (void)::SetWindowTextW(catalogStatus_, status.c_str());
    }
}

void SettingsWindow::FilterInstalledApps() noexcept {
    if (appList_ == nullptr)
        return;
    const std::wstring filter = Lower(WindowText(appFilter_));
    ListView_DeleteAllItems(appList_);
    visibleAppIndexes_.clear();
    for (std::size_t index = 0; index < installedApps_.size(); ++index) {
        const InstalledApp& app = installedApps_[index];
        const std::wstring searchable = Lower(app.name + L" " + app.target);
        if (!filter.empty() && searchable.find(filter) == std::wstring::npos) {
            continue;
        }
        visibleAppIndexes_.push_back(index);
        LVITEMW row{};
        row.mask = LVIF_TEXT | LVIF_PARAM;
        row.iItem = static_cast<int>(visibleAppIndexes_.size() - 1);
        row.pszText = const_cast<wchar_t*>(app.name.c_str());
        row.lParam = static_cast<LPARAM>(index);
        const int inserted = ListView_InsertItem(appList_, &row);
        if (inserted >= 0) {
            const wchar_t* const type =
                app.launchKind == LaunchKind::Aumid ? L"Packaged" : L"Desktop";
            ListView_SetItemText(appList_, inserted, 1, const_cast<wchar_t*>(type));
        }
    }
}

void SettingsWindow::AddCheckedApps() noexcept {
    if (appList_ == nullptr || installedApps_.empty())
        return;
    SaveSelectedFields();
    std::vector<WheelItem>* destination = &working_.items;
    std::vector<std::size_t> parentPath;
    std::size_t depth = 1;
    if (WheelItem* const selected = ItemAt(selectedPath_);
        selected != nullptr && selected->IsSubmenu()) {
        destination = &selected->children;
        parentPath = selectedPath_;
        depth = selectedPath_.size() + 1;
    }
    if (depth > kMaximumWheelDepth)
        return;

    const auto countItems = [&](const auto& self,
                                const std::vector<WheelItem>& items) -> std::size_t {
        std::size_t count = items.size();
        for (const WheelItem& item : items) {
            count += self(self, item.children);
        }
        return count;
    };
    std::size_t totalItems = countItems(countItems, working_.items);
    std::vector<std::size_t> lastAddedPath;
    const int rowCount = ListView_GetItemCount(appList_);
    for (int row = 0; row < rowCount; ++row) {
        if (!ListView_GetCheckState(appList_, row))
            continue;
        if (destination->size() >= kMaximumWheelItems || totalItems >= kMaximumTotalWheelItems) {
            break;
        }
        LVITEMW rowData{};
        rowData.mask = LVIF_PARAM;
        rowData.iItem = row;
        if (!ListView_GetItem(appList_, &rowData) || rowData.lParam < 0 ||
            static_cast<std::size_t>(rowData.lParam) >= installedApps_.size()) {
            continue;
        }
        const InstalledApp& app = installedApps_[static_cast<std::size_t>(rowData.lParam)];
        WheelItem item;
        item.id = NextId();
        item.label = app.name;
        item.configuredTarget = app.target;
        item.target = app.target;
        item.configuredIconSource = app.iconSource;
        item.iconSource = app.iconSource;
        item.iconAutomatic = app.iconSource == app.target;
        item.launchKind = app.launchKind;
        item.launchType = app.launchKind == LaunchKind::Aumid ? L"aumid" : L"shell";
        item.launchCandidates.push_back({app.target, app.target, item.launchType, app.launchKind});
        const std::size_t index = destination->size();
        destination->push_back(std::move(item));
        lastAddedPath = parentPath;
        lastAddedPath.push_back(index);
        ++totalItems;
    }
    if (lastAddedPath.empty()) {
        (void)::MessageBoxW(window_, L"Check at least one app before adding.", L"Weapon Cycle",
                            MB_OK | MB_ICONINFORMATION);
        return;
    }
    RebuildList(lastAddedPath);
    FilterInstalledApps();
    UpdatePreview();
}

void SettingsWindow::AddStarterPacks() noexcept {
    SaveSelectedFields();
    ConfigParseResult defaults = ParseConfiguration(DefaultConfigJson());
    if (!defaults.ok)
        return;
    std::vector<WheelItem> missing;
    for (WheelItem& item : defaults.config.items) {
        if (item.id != L"windows-tools" && item.id != L"utilities" && item.id != L"developer" &&
            item.id != L"powertoys") {
            continue;
        }
        const bool exists =
            std::any_of(working_.items.begin(), working_.items.end(),
                        [&](const WheelItem& existing) { return existing.id == item.id; });
        if (!exists)
            missing.push_back(std::move(item));
    }
    if (missing.empty()) {
        (void)::MessageBoxW(window_,
                            L"The Windows, Utilities, Developer, and PowerToys packs are "
                            L"already present, or the root wheel is full.",
                            L"Weapon Cycle", MB_OK | MB_ICONINFORMATION);
        return;
    }
    Configuration candidate = working_;
    const std::size_t available = candidate.items.size() < kMaximumWheelItems
                                      ? kMaximumWheelItems - candidate.items.size()
                                      : 0;
    if (available == 0) {
        (void)::MessageBoxW(window_,
                            L"The root wheel is full. Remove one segment before adding the "
                            L"starter packs.",
                            L"Weapon Cycle", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (missing.size() <= available) {
        for (WheelItem& item : missing) {
            candidate.items.push_back(std::move(item));
        }
    } else {
        WheelItem pack;
        pack.id = NextId();
        pack.label = L"Starter Packs";
        pack.children = std::move(missing);
        candidate.items.push_back(std::move(pack));
    }
    ConfigParseResult validation = ParseConfiguration(SerializeConfiguration(candidate));
    if (!validation.ok) {
        (void)::MessageBoxW(window_,
                            validation.error.empty()
                                ? L"The starter packs do not fit this configuration."
                                : validation.error.c_str(),
                            L"Weapon Cycle", MB_OK | MB_ICONWARNING);
        return;
    }
    working_ = std::move(validation.config);
    selectedPath_ = {working_.items.size() - 1};
    RebuildList(selectedPath_);
    UpdatePreview();
}

void SettingsWindow::ChooseAccentColor() noexcept {
    static COLORREF customColors[16]{};
    COLORREF color = RGB(246, 246, 242);
    const std::wstring current = WindowText(accentEdit_);
    if (current.size() == 7 && current.front() == L'#') {
        wchar_t* end = nullptr;
        const unsigned long value = std::wcstoul(current.c_str() + 1, &end, 16);
        if (end != nullptr && *end == L'\0') {
            color = RGB((value >> 16) & 0xFFUL, (value >> 8) & 0xFFUL, value & 0xFFUL);
        }
    }
    CHOOSECOLORW picker{};
    picker.lStructSize = sizeof(picker);
    picker.hwndOwner = window_;
    picker.rgbResult = color;
    picker.lpCustColors = customColors;
    picker.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!::ChooseColorW(&picker))
        return;
    wchar_t text[16]{};
    (void)swprintf_s(text, L"#%02X%02X%02X", GetRValue(picker.rgbResult),
                     GetGValue(picker.rgbResult), GetBValue(picker.rgbResult));
    (void)::SetWindowTextW(accentEdit_, text);
    UpdatePreview();
}

void SettingsWindow::AddDroppedFiles(const HDROP drop) noexcept {
    if (drop == nullptr)
        return;
    SaveSelectedFields();
    std::vector<WheelItem>* destination = &working_.items;
    std::vector<std::size_t> parentPath;
    if (WheelItem* const selected = ItemAt(selectedPath_);
        selected != nullptr && selected->IsSubmenu()) {
        destination = &selected->children;
        parentPath = selectedPath_;
    }
    const auto countItems = [&](const auto& self,
                                const std::vector<WheelItem>& items) -> std::size_t {
        std::size_t total = items.size();
        for (const WheelItem& item : items) {
            total += self(self, item.children);
        }
        return total;
    };
    std::size_t totalItems = countItems(countItems, working_.items);
    const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFFU, nullptr, 0);
    std::vector<std::size_t> lastPath;
    for (UINT index = 0; index < count && destination->size() < kMaximumWheelItems &&
                         totalItems < kMaximumTotalWheelItems;
         ++index) {
        const UINT length = ::DragQueryFileW(drop, index, nullptr, 0);
        if (length == 0 || length >= 32767)
            continue;
        std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
        if (::DragQueryFileW(drop, index, path.data(), static_cast<UINT>(path.size())) == 0) {
            continue;
        }
        path.resize(length);
        const std::size_t slash = path.find_last_of(L"\\/");
        std::wstring label = slash == std::wstring::npos ? path : path.substr(slash + 1);
        if (label.empty())
            label = path;
        WheelItem item;
        item.id = NextId();
        item.label = std::move(label);
        item.configuredTarget = path;
        item.target = path;
        item.configuredIconSource = path;
        item.iconSource = path;
        item.iconAutomatic = true;
        item.launchType = L"file";
        item.launchKind = LaunchKind::Shell;
        item.launchCandidates.push_back({path, path, L"file", LaunchKind::Shell});
        const std::size_t added = destination->size();
        destination->push_back(std::move(item));
        ++totalItems;
        lastPath = parentPath;
        lastPath.push_back(added);
    }
    ::DragFinish(drop);
    if (!lastPath.empty()) {
        RebuildList(lastPath);
        UpdatePreview();
    }
}

void SettingsWindow::MoveItemToFlatIndex(const int flatIndex, const bool after) noexcept {
    if (dragSourcePath_.empty()) {
        return;
    }
    SaveSelectedFields();
    std::vector<WheelItem>* const siblings = SiblingsAt(dragSourcePath_);
    if (siblings == nullptr)
        return;
    const std::size_t source = dragSourcePath_.back();
    std::size_t target = siblings->size();
    if (flatIndex >= 0) {
        if (static_cast<std::size_t>(flatIndex) >= flatPaths_.size()) {
            return;
        }
        const std::vector<std::size_t>& targetPath =
            flatPaths_[static_cast<std::size_t>(flatIndex)];
        if (targetPath.size() != dragSourcePath_.size() ||
            !std::equal(targetPath.begin(), targetPath.end() - 1, dragSourcePath_.begin())) {
            return;
        }
        target = targetPath.back() + (after ? 1U : 0U);
    }
    if (source >= siblings->size() || target > siblings->size()) {
        return;
    }
    if (source < target)
        --target;
    if (source == target)
        return;
    WheelItem moved = std::move((*siblings)[source]);
    siblings->erase(siblings->begin() + source);
    siblings->insert(siblings->begin() + target, std::move(moved));
    selectedPath_ = dragSourcePath_;
    selectedPath_.back() = target;
    RebuildList(selectedPath_);
    UpdatePreview();
}

void SettingsWindow::RemoveItem() noexcept {
    if (selectedPath_.empty())
        return;
    SaveSelectedFields();
    std::vector<WheelItem>* const siblings = SiblingsAt(selectedPath_);
    if (siblings == nullptr)
        return;
    const std::size_t index = selectedPath_.back();
    if (selectedPath_.size() == 1 && siblings->size() == 1) {
        (void)::MessageBoxW(window_, L"The root wheel must contain at least one item.",
                            L"Weapon Cycle", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (selectedPath_.size() > 1 && siblings->size() == 1) {
        (void)::MessageBoxW(window_,
                            L"A submenu must contain at least one item. Remove the submenu "
                            L"instead.",
                            L"Weapon Cycle", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (index < siblings->size())
        siblings->erase(siblings->begin() + index);
    selectedPath_.clear();
    RebuildList();
    UpdatePreview();
}

void SettingsWindow::MoveItem(const int direction) noexcept {
    if (selectedPath_.empty())
        return;
    SaveSelectedFields();
    std::vector<WheelItem>* const siblings = SiblingsAt(selectedPath_);
    if (siblings == nullptr)
        return;
    const std::size_t index = selectedPath_.back();
    const long long target = static_cast<long long>(index) + direction;
    if (index >= siblings->size() || target < 0 ||
        target >= static_cast<long long>(siblings->size())) {
        return;
    }
    std::swap((*siblings)[index], (*siblings)[static_cast<std::size_t>(target)]);
    selectedPath_.back() = static_cast<std::size_t>(target);
    RebuildList(selectedPath_);
    UpdatePreview();
}

void SettingsWindow::Save() noexcept {
    SaveSelectedFields();
    if (!SaveAppearanceFields(true) || !SaveHotkeyField(true)) {
        return;
    }
    working_.placement.mode = ::SendMessageW(placementCombo_, CB_GETCURSEL, 0, 0) == 1
                                  ? PlacementMode::MonitorCentre
                                  : PlacementMode::Cursor;
    working_.interaction.cancelOnRightClick =
        ::SendMessageW(rightClickCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    working_.interaction.cancelOnOuterExit =
        ::SendMessageW(outerExitCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    working_.interaction.numberQuickKeys =
        ::SendMessageW(quickKeysCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;

    std::wstring error;
    if (!save_(context_, working_, false, nullptr, error)) {
        (void)::MessageBoxW(
            window_, error.empty() ? L"The configuration could not be saved." : error.c_str(),
            L"Weapon Cycle", MB_OK | MB_ICONERROR);
        return;
    }
    (void)::MessageBoxW(window_, L"Configuration saved.", L"Weapon Cycle",
                        MB_OK | MB_ICONINFORMATION);
    RebuildList(selectedPath_);
    UpdatePreview();
}

void SettingsWindow::ResetToDefaults() noexcept {
    if (::MessageBoxW(window_,
                      L"Replace the current configuration with OrbitDeck defaults? "
                      L"This is intended for recovering an invalid config.json.",
                      L"Reset OrbitDeck", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    ConfigParseResult defaults = ParseConfiguration(DefaultConfigJson());
    if (!defaults.ok) {
        (void)::MessageBoxW(window_, L"The embedded defaults are unavailable.", L"Weapon Cycle",
                            MB_OK | MB_ICONERROR);
        return;
    }
    std::wstring error;
    if (!save_(context_, defaults.config, true, nullptr, error)) {
        (void)::MessageBoxW(
            window_, error.empty() ? L"The configuration could not be reset." : error.c_str(),
            L"Weapon Cycle", MB_OK | MB_ICONERROR);
        return;
    }
    working_ = std::move(defaults.config);
    selectedPath_.clear();
    LoadGlobalFields();
    RebuildList();
    UpdatePreview();
}

WheelItem* SettingsWindow::ItemAt(const std::vector<std::size_t>& path) noexcept {
    std::vector<WheelItem>* items = &working_.items;
    WheelItem* item = nullptr;
    for (const std::size_t index : path) {
        if (index >= items->size())
            return nullptr;
        item = &(*items)[index];
        items = &item->children;
    }
    return item;
}

std::vector<WheelItem>* SettingsWindow::SiblingsAt(const std::vector<std::size_t>& path) noexcept {
    if (path.empty())
        return nullptr;
    std::vector<WheelItem>* items = &working_.items;
    for (std::size_t depth = 0; depth + 1 < path.size(); ++depth) {
        const std::size_t index = path[depth];
        if (index >= items->size())
            return nullptr;
        items = &(*items)[index].children;
    }
    return items;
}

std::wstring SettingsWindow::NextId() noexcept {
    for (;;) {
        const std::wstring candidate = L"custom-" + std::to_wstring(nextId_++);
        bool exists = false;
        for (const auto& path : flatPaths_) {
            const WheelItem* item = const_cast<SettingsWindow*>(this)->ItemAt(path);
            if (item != nullptr && item->id == candidate) {
                exists = true;
                break;
            }
        }
        if (!exists)
            return candidate;
    }
}

int SettingsWindow::FlatIndexOf(const std::vector<std::size_t>& path) const noexcept {
    const auto found = std::find(flatPaths_.begin(), flatPaths_.end(), path);
    return found == flatPaths_.end() ? -1 : static_cast<int>(found - flatPaths_.begin());
}

}  // namespace orbitdeck
