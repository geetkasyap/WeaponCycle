#include "tools/DeveloperToolWindow.h"

#include "actions/ActionExecutor.h"
#include "tools/DeveloperTools.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace orbitdeck {
namespace {

constexpr int kWindowWidth{760};
constexpr int kWindowHeight{660};
constexpr UINT_PTR kColorTimerId{1};

enum ControlId : int {
    kToolPicker = 4001,
    kMode,
    kInput,
    kSecondaryInput,
    kOutput,
    kRun,
    kCopy,
    kClear,
    kPin,
};

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

void ShowControl(const HWND control, const bool visible) noexcept {
    (void)::ShowWindow(control, visible ? SW_SHOWNORMAL : SW_HIDE);
}

}  // namespace

DeveloperToolWindow::~DeveloperToolWindow() noexcept {
    Destroy();
}

bool DeveloperToolWindow::Create(const HINSTANCE instance, const HWND owner) noexcept {
    if (instance == nullptr || owner == nullptr)
        return false;
    backgroundBrush_ = ::CreateSolidBrush(RGB(18, 20, 24));
    fieldBrush_ = ::CreateSolidBrush(RGB(31, 34, 40));

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &DeveloperToolWindow::WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = backgroundBrush_;
    windowClass.lpszClassName = ClassName();
    const ATOM atom = ::RegisterClassExW(&windowClass);
    if (atom == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    instance_ = instance;
    owner_ = owner;
    ownsClassRegistration_ = atom != 0;
    const HMONITOR monitor = ::MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor != nullptr && ::GetMonitorInfoW(monitor, &monitorInfo)) {
        const int workWidth = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
        const int workHeight = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
        layoutScale_ = std::min(
            {1.0F,
             static_cast<float>(std::max(1, workWidth - 16)) / static_cast<float>(kWindowWidth),
             static_cast<float>(std::max(1, workHeight - 16)) / static_cast<float>(kWindowHeight)});
    }

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        metrics.lfMessageFont.lfHeight = static_cast<LONG>(
            std::lround(static_cast<float>(metrics.lfMessageFont.lfHeight) * layoutScale_));
        font_ = ::CreateFontIndirectW(&metrics.lfMessageFont);
    }

    window_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, ClassName(), L"Weapon Cycle Utility",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
        static_cast<int>(std::lround(kWindowWidth * layoutScale_)),
        static_cast<int>(std::lround(kWindowHeight * layoutScale_)), owner, nullptr, instance,
        this);
    if (window_ == nullptr) {
        Destroy();
        return false;
    }
    const BOOL dark = TRUE;
    (void)::DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    return true;
}

void DeveloperToolWindow::Show(const BuiltInTool tool, const POINT anchor) noexcept {
    if (window_ == nullptr || !IsDeveloperTool(tool))
        return;
    HMONITOR monitor = ::MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    FitToMonitor(monitor);
    ConfigureTool(tool);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info)) {
        RECT bounds{};
        (void)::GetWindowRect(window_, &bounds);
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        const int x =
            std::clamp(static_cast<int>(anchor.x) + 20, static_cast<int>(info.rcWork.left),
                       std::max(static_cast<int>(info.rcWork.left),
                                static_cast<int>(info.rcWork.right) - width));
        const int y = std::clamp(static_cast<int>(anchor.y) + 20, static_cast<int>(info.rcWork.top),
                                 std::max(static_cast<int>(info.rcWork.top),
                                          static_cast<int>(info.rcWork.bottom) - height));
        (void)::SetWindowPos(window_, pinned_ ? HWND_TOPMOST : HWND_TOP, x, y, 0, 0,
                             SWP_NOSIZE | SWP_NOACTIVATE);
    }
    (void)::ShowWindow(window_, SW_SHOWNORMAL);
    (void)::SetForegroundWindow(window_);
}

void DeveloperToolWindow::SetToolAvailable(const BuiltInTool tool, const bool available) noexcept {
    if (!available && tool_ == tool)
        Hide();
}

void DeveloperToolWindow::Destroy() noexcept {
    Hide();
    if (window_ != nullptr) {
        const HWND window = window_;
        window_ = nullptr;
        (void)::DestroyWindow(window);
    }
    if (ownsClassRegistration_) {
        (void)::UnregisterClassW(ClassName(), instance_);
    }
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
    instance_ = nullptr;
    owner_ = nullptr;
}

bool DeveloperToolWindow::IsVisible() const noexcept {
    return window_ != nullptr && ::IsWindowVisible(window_) != FALSE;
}

LRESULT CALLBACK DeveloperToolWindow::WndProc(const HWND window, const UINT message,
                                              const WPARAM wParam, const LPARAM lParam) noexcept {
    auto* self = reinterpret_cast<DeveloperToolWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<DeveloperToolWindow*>(create->lpCreateParams);
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

LRESULT DeveloperToolWindow::HandleMessage(const UINT message, const WPARAM wParam,
                                           const LPARAM lParam) noexcept {
    switch (message) {
        case WM_CREATE:
            return CreateControls() ? 0 : -1;
        case WM_COMMAND:
            if (LOWORD(wParam) == kToolPicker && HIWORD(wParam) == CBN_SELCHANGE &&
                !updatingToolPicker_) {
                const LRESULT selected = ::SendMessageW(toolPicker_, CB_GETCURSEL, 0, 0);
                if (selected >= 0 && selected <= static_cast<int>(BuiltInTool::TextDiff) -
                                                     static_cast<int>(BuiltInTool::ColorPicker)) {
                    ConfigureTool(static_cast<BuiltInTool>(
                        static_cast<int>(BuiltInTool::ColorPicker) + static_cast<int>(selected)));
                }
                return 0;
            }
            switch (LOWORD(wParam)) {
                case kRun:
                    RunTool();
                    break;
                case kCopy:
                    CopyOutput();
                    break;
                case kClear:
                    (void)::SetWindowTextW(input_, L"");
                    (void)::SetWindowTextW(secondaryInput_, L"");
                    (void)::SetWindowTextW(output_, L"");
                    break;
                case kPin:
                    pinned_ = ::SendMessageW(pin_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    (void)::SetWindowPos(window_, pinned_ ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0,
                                         0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    break;
                case IDCANCEL:
                    Hide();
                    break;
            }
            return 0;
        case WM_TIMER:
            if (wParam == kColorTimerId && tool_ == BuiltInTool::ColorPicker && IsVisible()) {
                UpdateColorPicker();
            }
            return 0;
        case WM_CLOSE:
            Hide();
            return 0;
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            const HMONITOR monitor = suggested != nullptr
                                         ? ::MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST)
                                         : ::MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
            FitToMonitor(monitor);
            return 0;
        }
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            FitToMonitor(::MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST));
            return 0;
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
            (void)::SetBkColor(context, RGB(31, 34, 40));
            return reinterpret_cast<LRESULT>(fieldBrush_);
        }
        default:
            return ::DefWindowProcW(window_, message, wParam, lParam);
    }
}

bool DeveloperToolWindow::CreateControls() noexcept {
    const auto control = [&](const wchar_t* className, const wchar_t* text, const DWORD style,
                             const int id, const int x, const int y, const int width,
                             const int height) {
        const HWND created = ::CreateWindowExW(
            0, className, text, WS_CHILD | WS_VISIBLE | style,
            static_cast<int>(std::lround(x * layoutScale_)),
            static_cast<int>(std::lround(y * layoutScale_)),
            static_cast<int>(std::lround(width * layoutScale_)),
            static_cast<int>(std::lround(height * layoutScale_)), window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (created != nullptr) {
            SetFont(created, font_);
            (void)::SetWindowTheme(created, L"DarkMode_Explorer", nullptr);
        }
        return created;
    };
    (void)control(L"STATIC", L"Tool", SS_LEFT, 0, 22, 18, 60, 20);
    toolPicker_ =
        control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, kToolPicker, 88, 14, 634, 320);
    if (toolPicker_ != nullptr) {
        for (int value = static_cast<int>(BuiltInTool::ColorPicker);
             value <= static_cast<int>(BuiltInTool::TextDiff); ++value) {
            const std::wstring name = DeveloperToolName(static_cast<BuiltInTool>(value));
            (void)::SendMessageW(toolPicker_, CB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(name.c_str()));
        }
    }
    description_ = control(L"STATIC", L"", SS_LEFT, 0, 22, 52, 700, 34);
    modeLabel_ = control(L"STATIC", L"Mode", SS_LEFT, 0, 22, 92, 80, 20);
    mode_ = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, kMode, 22, 112, 700, 220);
    inputLabel_ = control(L"STATIC", L"Input", SS_LEFT, 0, 22, 145, 120, 20);
    input_ =
        control(L"EDIT", L"",
                WS_BORDER | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
                kInput, 22, 165, 700, 110);
    secondaryLabel_ = control(L"STATIC", L"Second input", SS_LEFT, 0, 22, 285, 120, 20);
    secondaryInput_ =
        control(L"EDIT", L"",
                WS_BORDER | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
                kSecondaryInput, 22, 305, 700, 82);
    outputLabel_ = control(L"STATIC", L"Output", SS_LEFT, 0, 22, 397, 120, 20);
    output_ = control(L"EDIT", L"",
                      WS_BORDER | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
                          WS_VSCROLL | ES_READONLY,
                      kOutput, 22, 417, 700, 135);
    if (input_ != nullptr) {
        (void)::SendMessageW(input_, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
    }
    if (secondaryInput_ != nullptr) {
        (void)::SendMessageW(secondaryInput_, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
    }
    if (output_ != nullptr) {
        (void)::SendMessageW(output_, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
    }
    run_ = control(L"BUTTON", L"Run", BS_DEFPUSHBUTTON | WS_TABSTOP, kRun, 22, 568, 120, 34);
    copy_ =
        control(L"BUTTON", L"Copy output", BS_PUSHBUTTON | WS_TABSTOP, kCopy, 152, 568, 120, 34);
    clear_ = control(L"BUTTON", L"Clear", BS_PUSHBUTTON | WS_TABSTOP, kClear, 282, 568, 100, 34);
    pin_ = control(L"BUTTON", L"Pin above other windows", BS_AUTOCHECKBOX | WS_TABSTOP, kPin, 406,
                   574, 190, 24);
    close_ = control(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 622, 568, 100, 34);
    return toolPicker_ != nullptr && description_ != nullptr && modeLabel_ != nullptr &&
           mode_ != nullptr && inputLabel_ != nullptr && input_ != nullptr &&
           secondaryLabel_ != nullptr && secondaryInput_ != nullptr && outputLabel_ != nullptr &&
           output_ != nullptr && run_ != nullptr && copy_ != nullptr && clear_ != nullptr &&
           pin_ != nullptr && close_ != nullptr;
}

void DeveloperToolWindow::ConfigureTool(const BuiltInTool tool) noexcept {
    (void)::KillTimer(window_, kColorTimerId);
    tool_ = tool;
    updatingToolPicker_ = true;
    (void)::SendMessageW(toolPicker_, CB_SETCURSEL,
                         static_cast<int>(tool) - static_cast<int>(BuiltInTool::ColorPicker), 0);
    updatingToolPicker_ = false;
    (void)::SetWindowTextW(window_, (L"Weapon Cycle — " + DeveloperToolName(tool)).c_str());
    (void)::SetWindowTextW(
        description_, tool == BuiltInTool::ColorPicker
                          ? L"Move the pointer anywhere on screen, then copy the sampled color."
                          : L"Runs locally when requested. Input is never sent anywhere.");
    (void)::SendMessageW(mode_, CB_RESETCONTENT, 0, 0);
    for (const std::wstring& mode : DeveloperToolModes(tool)) {
        (void)::SendMessageW(mode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(mode.c_str()));
    }
    (void)::SendMessageW(mode_, CB_SETCURSEL, 0, 0);
    (void)::SetWindowTextW(input_, L"");
    (void)::SetWindowTextW(secondaryInput_, L"");
    (void)::SetWindowTextW(output_, L"");

    const bool second = tool == BuiltInTool::RegexTester || tool == BuiltInTool::TextDiff ||
                        tool == BuiltInTool::Base64Image;
    const bool noInput = tool == BuiltInTool::GuidGenerator || tool == BuiltInTool::ColorPicker;
    ShowControl(input_, !noInput);
    ShowControl(inputLabel_, !noInput);
    ShowControl(secondaryInput_, second);
    ShowControl(secondaryLabel_, second);
    const bool multipleModes = !noInput && DeveloperToolModes(tool).size() > 1;
    ShowControl(mode_, multipleModes);
    ShowControl(modeLabel_, multipleModes);
    (void)::SetWindowTextW(run_, tool == BuiltInTool::GuidGenerator ? L"Generate"
                                 : tool == BuiltInTool::ColorPicker ? L"Copy color"
                                                                    : L"Run");
    if (tool == BuiltInTool::RegexTester) {
        (void)::SetWindowTextW(description_, L"Pattern (top) and test text (bottom).");
    } else if (tool == BuiltInTool::TextDiff) {
        (void)::SetWindowTextW(description_, L"Original text (top) and changed text (bottom).");
    } else if (tool == BuiltInTool::Base64Image) {
        (void)::SetWindowTextW(description_,
                               L"Encode: input file path. Decode: data URL plus output path.");
    }
    if (tool == BuiltInTool::ColorPicker) {
        UpdateColorPicker();
        (void)::SetTimer(window_, kColorTimerId, 100, nullptr);
    } else if (tool == BuiltInTool::GuidGenerator) {
        RunTool();
    } else {
        (void)::SetFocus(input_);
    }
}

void DeveloperToolWindow::RunTool() noexcept {
    if (tool_ == BuiltInTool::ColorPicker) {
        CopyOutput();
        return;
    }
    const LRESULT selection = ::SendMessageW(mode_, CB_GETCURSEL, 0, 0);
    wchar_t mode[128]{};
    if (selection >= 0) {
        (void)::SendMessageW(mode_, CB_GETLBTEXT, selection, reinterpret_cast<LPARAM>(mode));
    }
    std::wstring output;
    std::wstring error;
    if (RunDeveloperTool(tool_, WindowText(input_), WindowText(secondaryInput_), mode, output,
                         error)) {
        (void)::SetWindowTextW(output_, output.c_str());
    } else {
        (void)::SetWindowTextW(output_, error.c_str());
        (void)::MessageBeep(MB_ICONWARNING);
    }
}

void DeveloperToolWindow::CopyOutput() noexcept {
    const LaunchResult result = ExecuteDirectAction(L"clipboard", WindowText(output_), window_);
    if (!result.succeeded) {
        (void)::SetWindowTextW(description_, result.error.c_str());
    }
}

void DeveloperToolWindow::UpdateColorPicker() noexcept {
    POINT cursor{};
    if (!::GetCursorPos(&cursor))
        return;
    const HDC screen = ::GetDC(nullptr);
    if (screen == nullptr)
        return;
    const COLORREF color = ::GetPixel(screen, cursor.x, cursor.y);
    (void)::ReleaseDC(nullptr, screen);
    if (color == CLR_INVALID)
        return;
    wchar_t text[96]{};
    (void)swprintf_s(text, L"#%02X%02X%02X\r\nRGB %u, %u, %u", GetRValue(color), GetGValue(color),
                     GetBValue(color), GetRValue(color), GetGValue(color), GetBValue(color));
    (void)::SetWindowTextW(output_, text);
}

void DeveloperToolWindow::FitToMonitor(const HMONITOR monitor) noexcept {
    if (window_ == nullptr || monitor == nullptr)
        return;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(monitor, &info))
        return;
    const int workWidth = info.rcWork.right - info.rcWork.left;
    const int workHeight = info.rcWork.bottom - info.rcWork.top;
    const float nextScale = std::min(
        {1.0F, static_cast<float>(std::max(1, workWidth - 16)) / static_cast<float>(kWindowWidth),
         static_cast<float>(std::max(1, workHeight - 16)) / static_cast<float>(kWindowHeight)});
    if (std::abs(nextScale - layoutScale_) > 0.001F) {
        const float ratio = nextScale / layoutScale_;
        for (HWND child = ::GetWindow(window_, GW_CHILD); child != nullptr;
             child = ::GetWindow(child, GW_HWNDNEXT)) {
            RECT bounds{};
            if (!::GetWindowRect(child, &bounds))
                continue;
            POINT points[]{{bounds.left, bounds.top}, {bounds.right, bounds.bottom}};
            (void)::MapWindowPoints(nullptr, window_, points, 2);
            (void)::SetWindowPos(child, nullptr, static_cast<int>(std::lround(points[0].x * ratio)),
                                 static_cast<int>(std::lround(points[0].y * ratio)),
                                 static_cast<int>(std::lround((points[1].x - points[0].x) * ratio)),
                                 static_cast<int>(std::lround((points[1].y - points[0].y) * ratio)),
                                 SWP_NOACTIVATE | SWP_NOZORDER);
        }
        layoutScale_ = nextScale;
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        HFONT replacement = nullptr;
        if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
            metrics.lfMessageFont.lfHeight = static_cast<LONG>(
                std::lround(static_cast<float>(metrics.lfMessageFont.lfHeight) * layoutScale_));
            replacement = ::CreateFontIndirectW(&metrics.lfMessageFont);
        }
        if (font_ != nullptr)
            (void)::DeleteObject(font_);
        font_ = replacement;
        for (HWND child = ::GetWindow(window_, GW_CHILD); child != nullptr;
             child = ::GetWindow(child, GW_HWNDNEXT)) {
            SetFont(child, font_);
        }
    }
    (void)::SetWindowPos(window_, nullptr, 0, 0,
                         static_cast<int>(std::lround(kWindowWidth * layoutScale_)),
                         static_cast<int>(std::lround(kWindowHeight * layoutScale_)),
                         SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
}

void DeveloperToolWindow::Hide() noexcept {
    if (window_ == nullptr)
        return;
    (void)::KillTimer(window_, kColorTimerId);
    (void)::ShowWindow(window_, SW_HIDE);
    tool_ = BuiltInTool::None;
}

}  // namespace orbitdeck
