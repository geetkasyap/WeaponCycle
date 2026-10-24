#include "tools/MiniAppWindow.h"

#include "config/ConfigLoader.h"

#include <shlobj.h>
#include <vfw.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <new>
#include <string>
#include <vector>

namespace orbitdeck {
namespace {

constexpr UINT_PTR kCountdownTimerId{3};
constexpr UINT_PTR kSystemTimerId{4};
constexpr UINT_PTR kCaffeineRetryTimerId{5};
constexpr UINT_PTR kMicCleanupTimerId{6};
constexpr int kPresetId{3001};
constexpr int kSecondaryId{3002};
constexpr int kPinId{3003};
constexpr int kCameraPreviewId{3004};
constexpr int kInputId{3005};
constexpr std::uint32_t kScratchpadMagic{0x4F444E54};
constexpr std::size_t kMaximumScratchpadCharacters{50000};
constexpr std::size_t kMaximumScratchpadBytes{kMaximumScratchpadCharacters * sizeof(wchar_t)};

struct ScratchpadHeader {
    std::uint32_t magic{kScratchpadMagic};
    std::uint32_t version{1};
    std::uint64_t generation{0};
};

enum class ScratchpadReadStatus {
    Missing,
    Loaded,
    Invalid,
    Unsupported,
};

class ExpressionParser {
public:
    explicit ExpressionParser(const std::wstring_view text) noexcept : text_(text) {}

    [[nodiscard]] bool Parse(double& result) noexcept {
        if (text_.size() > 4096)
            return false;
        result = Expression();
        SkipSpace();
        return ok_ && position_ == text_.size() && std::isfinite(result);
    }

private:
    double Expression() noexcept {
        double value = Term();
        while (ok_) {
            SkipSpace();
            if (Take(L'+'))
                value += Term();
            else if (Take(L'-'))
                value -= Term();
            else
                break;
        }
        return value;
    }

    double Term() noexcept {
        double value = Factor();
        while (ok_) {
            SkipSpace();
            if (Take(L'*')) {
                value *= Factor();
            } else if (Take(L'/')) {
                const double divisor = Factor();
                if (divisor == 0.0)
                    ok_ = false;
                else
                    value /= divisor;
            } else {
                break;
            }
        }
        return value;
    }

    double Factor() noexcept {
        if (depth_ >= 64) {
            ok_ = false;
            return 0.0;
        }
        ++depth_;
        const double value = FactorValue();
        --depth_;
        return value;
    }

    double FactorValue() noexcept {
        SkipSpace();
        if (Take(L'+'))
            return Factor();
        if (Take(L'-'))
            return -Factor();
        if (Take(L'(')) {
            const double value = Expression();
            SkipSpace();
            if (!Take(L')'))
                ok_ = false;
            return value;
        }

        const std::size_t start = position_;
        bool decimal = false;
        while (position_ < text_.size()) {
            const wchar_t value = text_[position_];
            if (value >= L'0' && value <= L'9') {
                ++position_;
            } else if (value == L'.' && !decimal) {
                decimal = true;
                ++position_;
            } else {
                break;
            }
        }
        if (start == position_) {
            ok_ = false;
            return 0.0;
        }
        const std::wstring number(text_.substr(start, position_ - start));
        wchar_t* end = nullptr;
        const double value = std::wcstod(number.c_str(), &end);
        if (end == nullptr || *end != L'\0')
            ok_ = false;
        return value;
    }

    void SkipSpace() noexcept {
        while (position_ < text_.size() && iswspace(text_[position_])) {
            ++position_;
        }
    }

    bool Take(const wchar_t expected) noexcept {
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }
    std::wstring_view text_;
    std::size_t position_{0};
    std::size_t depth_{0};
    bool ok_{true};
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

std::uint64_t ReadGeneration(const std::wstring& path) noexcept {
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    char bytes[32]{};
    DWORD read = 0;
    const bool ok = ::ReadFile(file, bytes, sizeof(bytes) - 1, &read, nullptr) != FALSE;
    (void)::CloseHandle(file);
    if (!ok || read == 0)
        return 0;
    std::uint64_t generation = 0;
    for (DWORD index = 0; index < read; ++index) {
        if (bytes[index] == '\r' || bytes[index] == '\n')
            break;
        if (bytes[index] < '0' || bytes[index] > '9')
            return 0;
        generation = generation * 10 + static_cast<std::uint64_t>(bytes[index] - '0');
    }
    return generation;
}

bool WriteGeneration(const std::wstring& path, const std::uint64_t generation) noexcept {
    const std::string bytes = std::to_string(generation) + "\n";
    const std::wstring temporary = path + L".new";
    (void)::DeleteFileW(temporary.c_str());
    const HANDLE file = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                                nullptr) != FALSE &&
                    static_cast<std::size_t>(written) == bytes.size() &&
                    ::FlushFileBuffers(file) != FALSE;
    (void)::CloseHandle(file);
    if (!ok) {
        (void)::DeleteFileW(temporary.c_str());
        return false;
    }
    if (!::MoveFileExW(temporary.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        (void)::DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

ScratchpadReadStatus ReadScratchpadFile(const std::wstring& path, std::wstring& text,
                                        std::uint64_t& generation) {
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                   ? ScratchpadReadStatus::Missing
                   : ScratchpadReadStatus::Invalid;
    }
    LARGE_INTEGER size{};
    bool ok =
        ::GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 &&
        size.QuadPart <= static_cast<LONGLONG>(kMaximumScratchpadBytes + sizeof(ScratchpadHeader));
    std::vector<BYTE> bytes;
    if (ok) {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        ok = bytes.empty() || (::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                                          &read, nullptr) != FALSE &&
                               static_cast<std::size_t>(read) == bytes.size());
    }
    (void)::CloseHandle(file);
    if (!ok)
        return ScratchpadReadStatus::Invalid;

    std::size_t offset = 0;
    generation = 0;
    if (bytes.size() >= sizeof(ScratchpadHeader)) {
        ScratchpadHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        if (header.magic == kScratchpadMagic && header.version != 1) {
            return ScratchpadReadStatus::Unsupported;
        }
        if (header.magic == kScratchpadMagic) {
            generation = header.generation;
            offset = sizeof(header);
        }
    }
    const std::size_t textBytes = bytes.size() - offset;
    if (textBytes % sizeof(wchar_t) != 0) {
        return ScratchpadReadStatus::Invalid;
    }
    text.assign(textBytes / sizeof(wchar_t), L'\0');
    if (textBytes != 0) {
        std::memcpy(text.data(), bytes.data() + offset, textBytes);
    }
    return ScratchpadReadStatus::Loaded;
}

bool WriteScratchpadFile(const std::wstring& path, const std::wstring& text,
                         const std::uint64_t generation) noexcept {
    const std::size_t textBytes = text.size() * sizeof(wchar_t);
    const std::size_t totalBytes = sizeof(ScratchpadHeader) + textBytes;
    if (totalBytes > MAXDWORD)
        return false;
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const ScratchpadHeader header{kScratchpadMagic, 1, generation};
    DWORD headerWritten = 0;
    DWORD textWritten = 0;
    const bool saved =
        ::WriteFile(file, &header, sizeof(header), &headerWritten, nullptr) != FALSE &&
        headerWritten == static_cast<DWORD>(sizeof(header)) &&
        (text.empty() || (::WriteFile(file, text.data(), static_cast<DWORD>(textBytes),
                                      &textWritten, nullptr) != FALSE &&
                          static_cast<std::size_t>(textWritten) == textBytes)) &&
        ::FlushFileBuffers(file) != FALSE;
    (void)::CloseHandle(file);
    return saved;
}

bool WriteScratchpadFileAtomic(const std::wstring& path, const std::wstring& text,
                               const std::uint64_t generation) noexcept {
    const std::wstring temporary = path + L".new";
    (void)::DeleteFileW(temporary.c_str());
    if (!WriteScratchpadFile(temporary, text, generation)) {
        (void)::DeleteFileW(temporary.c_str());
        return false;
    }
    if (!::MoveFileExW(temporary.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        (void)::DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

std::wstring ScratchpadEmergencyPath() {
    const std::wstring folder = ConfigFolderPath();
    return folder.empty() ? std::wstring{} : folder + L"\\scratchpad.emergency";
}

ScratchpadReadStatus ReadRegistryScratchpadRecovery(std::wstring& text, std::uint64_t& generation) {
    DWORD type = 0;
    DWORD bytes = 0;
    const LSTATUS sizeStatus =
        ::RegGetValueW(HKEY_CURRENT_USER, L"Software\\OrbitDeck", L"ScratchpadRecovery",
                       RRF_RT_REG_BINARY, &type, nullptr, &bytes);
    if (sizeStatus == ERROR_FILE_NOT_FOUND) {
        return ScratchpadReadStatus::Missing;
    }
    if (sizeStatus != ERROR_SUCCESS || bytes < sizeof(ScratchpadHeader) ||
        bytes > 1024 * 1024 + sizeof(ScratchpadHeader)) {
        return ScratchpadReadStatus::Invalid;
    }
    std::vector<BYTE> value(bytes);
    if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\OrbitDeck", L"ScratchpadRecovery",
                       RRF_RT_REG_BINARY, &type, value.data(), &bytes) != ERROR_SUCCESS) {
        return ScratchpadReadStatus::Invalid;
    }
    ScratchpadHeader header{};
    std::memcpy(&header, value.data(), sizeof(header));
    if (header.magic != kScratchpadMagic ||
        (value.size() - sizeof(header)) % sizeof(wchar_t) != 0) {
        return ScratchpadReadStatus::Invalid;
    }
    if (header.version != 1)
        return ScratchpadReadStatus::Unsupported;
    generation = header.generation;
    const std::size_t textBytes = value.size() - sizeof(header);
    text.assign(textBytes / sizeof(wchar_t), L'\0');
    if (textBytes != 0) {
        std::memcpy(text.data(), value.data() + sizeof(header), textBytes);
    }
    return ScratchpadReadStatus::Loaded;
}

void ShowControl(const HWND control, const bool visible) noexcept {
    (void)::ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
}

void SetFont(const HWND control, const HFONT font) noexcept {
    (void)::SendMessageW(
        control, WM_SETFONT,
        reinterpret_cast<WPARAM>(font != nullptr ? font : ::GetStockObject(DEFAULT_GUI_FONT)),
        TRUE);
}

void SetWantReturn(const HWND edit, const bool enabled) noexcept {
    LONG_PTR style = ::GetWindowLongPtrW(edit, GWL_STYLE);
    style = enabled ? style | ES_WANTRETURN : style & ~ES_WANTRETURN;
    (void)::SetWindowLongPtrW(edit, GWL_STYLE, style);
    (void)::SetWindowPos(
        edit, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
}

}  // namespace

bool EvaluateExpression(const std::wstring_view expression, double& result) noexcept {
    return ExpressionParser(expression).Parse(result);
}

MiniAppWindow::~MiniAppWindow() noexcept {
    Destroy();
}

bool MiniAppWindow::Create(const HINSTANCE instance, const HWND owner) noexcept {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &MiniAppWindow::WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = ClassName();
    const ATOM atom = ::RegisterClassExW(&windowClass);
    if (atom == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    instance_ = instance;
    owner_ = owner;
    ownsClassRegistration_ = atom != 0;
    dpi_ = ::GetDpiForWindow(owner);
    if (dpi_ == 0)
        dpi_ = USER_DEFAULT_SCREEN_DPI;
    UpdateFont(dpi_);
    window_ =
        ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, ClassName(), L"Weapon Cycle Tool",
                          WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                          ::MulDiv(430, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI),
                          ::MulDiv(410, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI), owner,
                          nullptr, instance, this);
    if (window_ == nullptr) {
        Destroy();
        return false;
    }
    return true;
}

void MiniAppWindow::Show(const BuiltInTool tool, const POINT anchor) noexcept {
    if (window_ == nullptr)
        return;
    ConfigureTool(tool);

    HMONITOR monitor = ::MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info)) {
        const auto place = [&]() {
            RECT bounds{};
            (void)::GetWindowRect(window_, &bounds);
            const int width = bounds.right - bounds.left;
            const int height = bounds.bottom - bounds.top;
            const int maximumX = std::max(info.rcWork.left, info.rcWork.right - width);
            const int maximumY = std::max(info.rcWork.top, info.rcWork.bottom - height);
            const int x = std::clamp(static_cast<int>(anchor.x) + 20,
                                     static_cast<int>(info.rcWork.left), maximumX);
            const int y = std::clamp(static_cast<int>(anchor.y) + 20,
                                     static_cast<int>(info.rcWork.top), maximumY);
            (void)::SetWindowPos(window_, pinned_ ? HWND_TOPMOST : HWND_TOP, x, y, 0, 0,
                                 SWP_NOSIZE | SWP_NOACTIVATE);
        };
        place();
        // SetWindowPos can synchronously apply WM_DPICHANGED. Clamp once more
        // using the resulting target-monitor dimensions.
        place();
    }
    (void)::ShowWindow(window_, SW_SHOWNORMAL);
    (void)::SetForegroundWindow(window_);
}

void MiniAppWindow::Destroy() noexcept {
    if (!PrepareForShutdown()) {
        ::OutputDebugStringW(
            L"OrbitDeck: Scratchpad could not be retained during forced "
            L"shutdown.\n");
    }
    Hide();
    StopMirror();
    for (int attempt = 0; waveIn_ != nullptr && attempt < 40; ++attempt) {
        (void)StopMicCheck();
        if (waveIn_ != nullptr)
            ::Sleep(10);
    }
    if (waveIn_ != nullptr) {
        ::OutputDebugStringW(
            L"OrbitDeck: microphone buffers remained driver-owned during "
            L"forced shutdown.\n");
        // ponytail: retain the small driver-owned buffer block until process
        // teardown rather than risk a callback writing into freed memory.
        (void)micBuffers_.release();
        waveIn_ = nullptr;
        micCapturing_ = false;
        return;
    }
    if (caffeineActive_) {
        (void)::SetThreadExecutionState(ES_CONTINUOUS);
        caffeineActive_ = false;
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
    ownsClassRegistration_ = false;
    scratchpadLoaded_ = false;
    tool_ = BuiltInTool::None;
    if (font_ != nullptr) {
        (void)::DeleteObject(font_);
        font_ = nullptr;
    }
}

bool MiniAppWindow::PrepareForShutdown() noexcept {
    if (tool_ != BuiltInTool::Scratchpad || !scratchpadLoaded_) {
        return true;
    }
    if (!SaveScratchpad() && !SaveScratchpadRecovery()) {
        return false;
    }
    return true;
}

void MiniAppWindow::SetToolAvailable(const BuiltInTool tool, const bool available) noexcept {
    if (tool == BuiltInTool::Caffeine) {
        SetCaffeineAvailable(available);
        return;
    }
    if (available || tool_ != tool)
        return;
    if (tool_ == BuiltInTool::Scratchpad && !PrepareForShutdown()) {
        return;
    }
    if (tool_ == BuiltInTool::Timer) {
        timerRunning_ = false;
        (void)::KillTimer(window_, kCountdownTimerId);
    }
    Hide();
    tool_ = BuiltInTool::None;
}

void MiniAppWindow::SetCaffeineAvailable(const bool available) noexcept {
    caffeineAvailable_ = available;
    if (!available && caffeineActive_) {
        if (::SetThreadExecutionState(ES_CONTINUOUS) != 0) {
            caffeineActive_ = false;
            (void)::KillTimer(window_, kCaffeineRetryTimerId);
        } else if (tool_ == BuiltInTool::Caffeine) {
            (void)::SetWindowTextW(display_, L"Windows did not release the power-state request");
            (void)::SetTimer(window_, kCaffeineRetryTimerId, 5000, nullptr);
        } else {
            (void)::SetTimer(window_, kCaffeineRetryTimerId, 5000, nullptr);
        }
    }
    if (!available && !caffeineActive_ && tool_ == BuiltInTool::Caffeine) {
        Hide();
    }
}

bool MiniAppWindow::IsVisible() const noexcept {
    return window_ != nullptr && ::IsWindowVisible(window_) != FALSE;
}

HWND MiniAppWindow::Handle() const noexcept {
    return window_;
}

LRESULT CALLBACK MiniAppWindow::WndProc(const HWND window, const UINT message, const WPARAM wParam,
                                        const LPARAM lParam) noexcept {
    auto* self = reinterpret_cast<MiniAppWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<MiniAppWindow*>(create->lpCreateParams);
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

LRESULT MiniAppWindow::HandleMessage(const UINT message, const WPARAM wParam,
                                     const LPARAM lParam) noexcept {
    switch (message) {
        case WM_CREATE:
            return CreateControls() ? 0 : -1;
        case WM_TIMER:
            if (wParam == kCountdownTimerId && timerRunning_) {
                UpdateTimer();
                return 0;
            }
            if (wParam == kSystemTimerId && tool_ == BuiltInTool::SystemGlance && IsVisible()) {
                UpdateSystemGlance();
                return 0;
            }
            if (wParam == kCaffeineRetryTimerId && !caffeineAvailable_ && caffeineActive_) {
                SetCaffeineAvailable(false);
                return 0;
            }
            if (wParam == kMicCleanupTimerId && waveIn_ != nullptr) {
                (void)StopMicCheck();
                return 0;
            }
            break;
        case MM_WIM_DATA:
            HandleMicData(reinterpret_cast<HWAVEIN>(wParam), reinterpret_cast<WAVEHDR*>(lParam));
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK)
                PrimaryAction();
            else if (LOWORD(wParam) == kSecondaryId)
                SecondaryAction();
            else if (LOWORD(wParam) == kPinId) {
                SetPinned(::SendMessageW(pin_, BM_GETCHECK, 0, 0) == BST_CHECKED);
            } else if (LOWORD(wParam) == IDCANCEL) {
                Hide();
            } else if (LOWORD(wParam) == kPresetId && HIWORD(wParam) == CBN_SELCHANGE &&
                       tool_ == BuiltInTool::Timer) {
                timerRunning_ = false;
                (void)::KillTimer(window_, kCountdownTimerId);
                (void)::SetWindowTextW(primary_, L"Start");
                const LRESULT selected = ::SendMessageW(preset_, CB_GETCURSEL, 0, 0);
                const ULONGLONG minutes[] = {1, 5, 10, 25};
                if (selected >= 0 && selected < 4) {
                    timerRemainingMs_ = minutes[selected] * 60ULL * 1000ULL;
                }
                UpdateTimer();
            } else if (LOWORD(wParam) == kInputId && HIWORD(wParam) == EN_MAXTEXT) {
                (void)::SetWindowTextW(display_,
                                       L"The text reached this tool's supported size limit");
            }
            return 0;
        case WM_CLOSE:
            Hide();
            return 0;
        case WM_DPICHANGED: {
            const UINT newDpi = HIWORD(wParam);
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested != nullptr) {
                (void)::SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                                     suggested->right - suggested->left,
                                     suggested->bottom - suggested->top,
                                     SWP_NOACTIVATE | SWP_NOZORDER);
            }
            ScaleChildren(dpi_, newDpi);
            dpi_ = newDpi;
            UpdateFont(dpi_);
            return 0;
        }
        default:
            break;
    }
    return ::DefWindowProcW(window_, message, wParam, lParam);
}

bool MiniAppWindow::CreateControls() noexcept {
    const auto control = [&](const wchar_t* className, const wchar_t* text, const DWORD style,
                             const int id, const int x, const int y, const int width,
                             const int height) {
        const auto scaled = [&](const int value) {
            return ::MulDiv(value, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI);
        };
        const HWND created = ::CreateWindowExW(
            0, className, text, WS_CHILD | WS_VISIBLE | style, scaled(x), scaled(y), scaled(width),
            scaled(height), window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_,
            nullptr);
        if (created != nullptr)
            SetFont(created, font_);
        return created;
    };
    display_ = control(L"STATIC", L"", SS_CENTER, 0, 20, 20, 372, 64);
    input_ =
        control(L"EDIT", L"",
                WS_BORDER | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
                kInputId, 20, 94, 372, 124);
    preset_ =
        control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, kPresetId, 20, 228, 372, 160);
    for (const wchar_t* preset : {L"1 minute", L"5 minutes", L"10 minutes", L"25 minutes"}) {
        (void)::SendMessageW(preset_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(preset));
    }
    (void)::SendMessageW(preset_, CB_SETCURSEL, 1, 0);
    primary_ = control(L"BUTTON", L"", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, 20, 266, 180, 32);
    secondary_ =
        control(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, kSecondaryId, 212, 266, 180, 32);
    pin_ = control(L"BUTTON", L"Pin above other windows", BS_AUTOCHECKBOX | WS_TABSTOP, kPinId, 20,
                   320, 220, 24);
    close_ = control(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 312, 316, 80, 30);
    cameraPreview_ = ::capCreateCaptureWindowW(
        L"OrbitDeck Camera Preview", WS_CHILD | WS_CLIPSIBLINGS,
        ::MulDiv(20, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI),
        ::MulDiv(20, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI),
        ::MulDiv(372, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI),
        ::MulDiv(240, static_cast<int>(dpi_), USER_DEFAULT_SCREEN_DPI), window_, kCameraPreviewId);
    return display_ != nullptr && input_ != nullptr && preset_ != nullptr && primary_ != nullptr &&
           secondary_ != nullptr && pin_ != nullptr && close_ != nullptr;
}

void MiniAppWindow::UpdateFont(const UINT dpi) noexcept {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    HFONT replacement = nullptr;
    if (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi)) {
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

void MiniAppWindow::ScaleChildren(const UINT oldDpi, const UINT newDpi) noexcept {
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

void MiniAppWindow::ConfigureTool(const BuiltInTool tool) noexcept {
    if (tool_ == BuiltInTool::Mirror)
        StopMirror();
    if (tool_ == BuiltInTool::MicCheck)
        (void)StopMicCheck();
    if (tool_ == BuiltInTool::Scratchpad && scratchpadLoaded_) {
        if (!SaveScratchpad()) {
            (void)::SetWindowTextW(display_, L"Save failed");
            return;
        }
    }
    (void)::KillTimer(window_, kSystemTimerId);
    if (!timerRunning_) {
        (void)::KillTimer(window_, kCountdownTimerId);
    }
    tool_ = tool;
    scratchpadLoaded_ = false;
    ShowControl(input_, false);
    ShowControl(preset_, false);
    ShowControl(secondary_, false);
    ShowControl(primary_, true);
    ShowControl(cameraPreview_, false);
    (void)::EnableWindow(input_, TRUE);
    (void)::EnableWindow(primary_, TRUE);
    (void)::EnableWindow(secondary_, TRUE);
    (void)::SetWindowTextW(input_, L"");

    switch (tool_) {
        case BuiltInTool::Timer:
            (void)::SetWindowTextW(window_, L"Weapon Cycle Timer");
            ShowControl(preset_, true);
            ShowControl(secondary_, true);
            (void)::SetWindowTextW(primary_, timerRunning_ ? L"Pause" : L"Start");
            (void)::SetWindowTextW(secondary_, L"Reset");
            UpdateTimer();
            break;
        case BuiltInTool::Caffeine:
            if (!caffeineAvailable_ && !caffeineActive_) {
                Hide();
                break;
            }
            (void)::SetWindowTextW(window_, L"Weapon Cycle Keep Awake");
            (void)::SetWindowTextW(display_, caffeineActive_ ? L"Keep Awake is active"
                                                             : L"Your PC can sleep normally");
            (void)::SetWindowTextW(primary_, caffeineActive_ ? L"Allow Sleep" : L"Keep Awake");
            break;
        case BuiltInTool::Calculator:
            (void)::SetWindowTextW(window_, L"Weapon Cycle Calculator");
            ShowControl(input_, true);
            (void)::SendMessageW(input_, EM_SETLIMITTEXT, 4096, 0);
            SetWantReturn(input_, false);
            ShowControl(secondary_, true);
            (void)::SetWindowTextW(display_, L"Enter an expression");
            (void)::SetWindowTextW(primary_, L"Calculate");
            (void)::SetWindowTextW(secondary_, L"Clear");
            (void)::SetFocus(input_);
            break;
        case BuiltInTool::Scratchpad:
            (void)::SetWindowTextW(window_, L"Weapon Cycle Scratchpad");
            ShowControl(input_, true);
            (void)::SendMessageW(input_, EM_SETLIMITTEXT, kMaximumScratchpadCharacters, 0);
            SetWantReturn(input_, true);
            ShowControl(secondary_, true);
            (void)::SetWindowTextW(display_, L"Saved locally");
            (void)::SetWindowTextW(primary_, L"Save");
            (void)::SetWindowTextW(secondary_, L"Clear");
            scratchpadLoaded_ = LoadScratchpad();
            if (!scratchpadLoaded_) {
                (void)::SetWindowTextW(display_,
                                       L"Could not load the note; it will not be overwritten");
                (void)::EnableWindow(input_, FALSE);
                (void)::EnableWindow(primary_, FALSE);
                (void)::EnableWindow(secondary_, FALSE);
            } else {
                (void)::EnableWindow(input_, TRUE);
                (void)::EnableWindow(primary_, TRUE);
                (void)::EnableWindow(secondary_, TRUE);
            }
            (void)::SetFocus(input_);
            break;
        case BuiltInTool::SystemGlance:
            (void)::SetWindowTextW(window_, L"Weapon Cycle System Glance");
            (void)::SetWindowTextW(primary_, L"Refresh");
            UpdateSystemGlance();
            break;
        case BuiltInTool::Mirror:
            (void)::SetWindowTextW(window_, L"Weapon Cycle Mirror");
            ShowControl(primary_, false);
            ShowControl(display_, false);
            if (StartMirror()) {
                ShowControl(cameraPreview_, true);
            } else {
                ShowControl(display_, true);
                (void)::SetWindowTextW(display_, L"Camera unavailable or permission denied");
            }
            break;
        case BuiltInTool::MicCheck:
            (void)::SetWindowTextW(window_, L"Weapon Cycle Mic Check");
            ShowControl(primary_, false);
            if (!StartMicCheck()) {
                (void)::SetWindowTextW(display_, L"Microphone unavailable or permission denied");
            } else {
                (void)::SetWindowTextW(display_, L"Listening for microphone input...");
            }
            break;
        case BuiltInTool::None:
            Hide();
            break;
    }
    if (timerRunning_ || tool_ == BuiltInTool::SystemGlance) {
        if (timerRunning_) {
            (void)::SetTimer(window_, kCountdownTimerId, 200, nullptr);
        }
        if (tool_ == BuiltInTool::SystemGlance) {
            (void)::SetTimer(window_, kSystemTimerId, 1000, nullptr);
        }
    }
}

void MiniAppWindow::PrimaryAction() noexcept {
    switch (tool_) {
        case BuiltInTool::Timer:
            if (timerRunning_) {
                const ULONGLONG now = ::GetTickCount64();
                timerRemainingMs_ = timerEnd_ > now ? timerEnd_ - now : 0;
                timerRunning_ = false;
                (void)::KillTimer(window_, kCountdownTimerId);
            } else if (timerRemainingMs_ > 0) {
                timerEnd_ = ::GetTickCount64() + timerRemainingMs_;
                timerRunning_ = true;
                (void)::SetTimer(window_, kCountdownTimerId, 200, nullptr);
            }
            (void)::SetWindowTextW(primary_, timerRunning_ ? L"Pause" : L"Start");
            UpdateTimer();
            break;
        case BuiltInTool::Caffeine:
            if (!caffeineAvailable_ && !caffeineActive_)
                break;
            {
                const bool requested = !caffeineActive_;
                const EXECUTION_STATE state = ::SetThreadExecutionState(
                    requested ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED
                              : ES_CONTINUOUS);
                if (state != 0) {
                    caffeineActive_ = requested;
                    ConfigureTool(BuiltInTool::Caffeine);
                } else {
                    (void)::SetWindowTextW(display_, L"Windows rejected the power-state request");
                }
            }
            break;
        case BuiltInTool::Calculator: {
            double value = 0.0;
            if (EvaluateExpression(WindowText(input_), value)) {
                wchar_t text[64];
                (void)swprintf_s(text, L"%.12g", value);
                (void)::SetWindowTextW(display_, text);
            } else {
                (void)::SetWindowTextW(display_, L"Invalid expression");
            }
            break;
        }
        case BuiltInTool::Scratchpad:
            if (scratchpadLoaded_ && SaveScratchpad()) {
                (void)::SetWindowTextW(display_, L"Saved");
            } else {
                (void)::SetWindowTextW(display_, L"Save failed");
            }
            break;
        case BuiltInTool::SystemGlance:
            UpdateSystemGlance();
            break;
        case BuiltInTool::Mirror:
        case BuiltInTool::MicCheck:
        case BuiltInTool::None:
            break;
    }
}

void MiniAppWindow::SecondaryAction() noexcept {
    if (tool_ == BuiltInTool::Timer) {
        timerRunning_ = false;
        (void)::KillTimer(window_, kCountdownTimerId);
        const LRESULT selected = ::SendMessageW(preset_, CB_GETCURSEL, 0, 0);
        const ULONGLONG minutes[] = {1, 5, 10, 25};
        timerRemainingMs_ = minutes[selected >= 0 && selected < 4 ? selected : 1] * 60ULL * 1000ULL;
        (void)::SetWindowTextW(primary_, L"Start");
        UpdateTimer();
    } else if (tool_ == BuiltInTool::Calculator || tool_ == BuiltInTool::Scratchpad) {
        (void)::SetWindowTextW(input_, L"");
        (void)::SetWindowTextW(
            display_, tool_ == BuiltInTool::Calculator ? L"Enter an expression" : L"Cleared");
        if (tool_ == BuiltInTool::Scratchpad && scratchpadLoaded_ && !SaveScratchpad()) {
            if (!SaveScratchpadRecovery()) {
                (void)::SetWindowTextW(display_, L"Save failed; the note remains open");
                (void)::ShowWindow(window_, SW_SHOWNORMAL);
                (void)::SetForegroundWindow(window_);
                return;
            }
            return;
        }
    }
}

void MiniAppWindow::UpdateTimer() noexcept {
    if (timerRunning_) {
        const ULONGLONG now = ::GetTickCount64();
        timerRemainingMs_ = timerEnd_ > now ? timerEnd_ - now : 0;
        if (timerRemainingMs_ == 0) {
            timerRunning_ = false;
            if (tool_ == BuiltInTool::Timer) {
                (void)::SetWindowTextW(primary_, L"Start");
            }
            (void)::MessageBeep(MB_ICONINFORMATION);
            (void)::KillTimer(window_, kCountdownTimerId);
        }
    }
    const ULONGLONG seconds = (timerRemainingMs_ + 999) / 1000;
    wchar_t text[32];
    (void)swprintf_s(text, L"%02llu:%02llu", seconds / 60, seconds % 60);
    if (tool_ == BuiltInTool::Timer) {
        (void)::SetWindowTextW(display_, text);
    }
}

void MiniAppWindow::UpdateSystemGlance() noexcept {
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    const bool haveMemory = ::GlobalMemoryStatusEx(&memory) != FALSE;
    SYSTEM_POWER_STATUS power{};
    const bool havePower = ::GetSystemPowerStatus(&power) != FALSE;
    const ULONGLONG uptimeMinutes = ::GetTickCount64() / 60000ULL;
    const std::wstring memoryText =
        haveMemory ? std::to_wstring(memory.dwMemoryLoad) + L"%" : L"unavailable";
    std::wstring batteryText = L"unavailable";
    if (havePower) {
        if (power.BatteryFlag == 128) {
            batteryText = L"not present";
        } else if (power.BatteryFlag != 255 && power.BatteryLifePercent != 255) {
            batteryText = std::to_wstring(power.BatteryLifePercent) + L"%";
        }
    }
    const std::wstring text = L"Memory load: " + memoryText + L"\r\nBattery: " + batteryText +
                              L"\r\nUptime: " + std::to_wstring(uptimeMinutes / 60) + L" h " +
                              std::to_wstring(uptimeMinutes % 60) + L" m";
    (void)::SetWindowTextW(display_, text.c_str());
}

bool MiniAppWindow::StartMirror() noexcept {
    if (cameraPreview_ == nullptr)
        return false;
    if (::SendMessageW(cameraPreview_, WM_CAP_DRIVER_CONNECT, 0, 0) == FALSE) {
        return false;
    }
    (void)::SendMessageW(cameraPreview_, WM_CAP_SET_SCALE, TRUE, 0);
    (void)::SendMessageW(cameraPreview_, WM_CAP_SET_PREVIEWRATE, 33, 0);
    if (::SendMessageW(cameraPreview_, WM_CAP_SET_PREVIEW, TRUE, 0) == FALSE) {
        StopMirror();
        return false;
    }
    return true;
}

void MiniAppWindow::StopMirror() noexcept {
    if (cameraPreview_ == nullptr)
        return;
    (void)::SendMessageW(cameraPreview_, WM_CAP_SET_PREVIEW, FALSE, 0);
    (void)::SendMessageW(cameraPreview_, WM_CAP_DRIVER_DISCONNECT, 0, 0);
    ShowControl(cameraPreview_, false);
    ShowControl(display_, true);
}

bool MiniAppWindow::StartMicCheck() noexcept {
    if (waveIn_ != nullptr) {
        (void)StopMicCheck();
        if (waveIn_ != nullptr)
            return false;
    }
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 16000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    if (::waveInOpen(&waveIn_, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(window_), 0,
                     CALLBACK_WINDOW) != MMSYSERR_NOERROR) {
        waveIn_ = nullptr;
        return false;
    }
    ++micSessionGeneration_;
    micBuffers_.reset(new (std::nothrow) MicBuffers{});
    if (micBuffers_ == nullptr) {
        (void)::waveInClose(waveIn_);
        waveIn_ = nullptr;
        return false;
    }
    micCleanupAttempts_ = 0;
    for (std::size_t index = 0; index < micBuffers_->headers.size(); ++index) {
        WAVEHDR& header = micBuffers_->headers[index];
        header = {};
        header.lpData = reinterpret_cast<LPSTR>(micBuffers_->samples[index].data());
        header.dwUser = static_cast<DWORD_PTR>(micSessionGeneration_);
        header.dwBufferLength =
            static_cast<DWORD>(micBuffers_->samples[index].size() * sizeof(short));
        if (::waveInPrepareHeader(waveIn_, &header, sizeof(header)) != MMSYSERR_NOERROR ||
            ::waveInAddBuffer(waveIn_, &header, sizeof(header)) != MMSYSERR_NOERROR) {
            (void)StopMicCheck();
            return false;
        }
    }
    if (::waveInStart(waveIn_) != MMSYSERR_NOERROR) {
        (void)StopMicCheck();
        return false;
    }
    micCapturing_ = true;
    return true;
}

bool MiniAppWindow::StopMicCheck() noexcept {
    micCapturing_ = false;
    if (waveIn_ == nullptr)
        return true;
    (void)::waveInStop(waveIn_);
    (void)::waveInReset(waveIn_);
    bool headersReleased = true;
    bool terminalError = false;
    if (micBuffers_ == nullptr) {
        return false;
    }
    for (WAVEHDR& header : micBuffers_->headers) {
        if ((header.dwFlags & WHDR_PREPARED) != 0) {
            const MMRESULT result = ::waveInUnprepareHeader(waveIn_, &header, sizeof(header));
            if (result == WAVERR_STILLPLAYING) {
                headersReleased = false;
            } else if (result != MMSYSERR_NOERROR) {
                terminalError = true;
            }
        }
        if ((header.dwFlags & WHDR_PREPARED) == 0) {
            header = {};
        }
    }
    MMRESULT closeResult = MMSYSERR_NOERROR;
    if (headersReleased && !terminalError) {
        closeResult = ::waveInClose(waveIn_);
        if (closeResult != MMSYSERR_NOERROR && closeResult != WAVERR_STILLPLAYING) {
            terminalError = true;
        }
    }
    if (terminalError) {
        (void)::KillTimer(window_, kMicCleanupTimerId);
        (void)::SetWindowTextW(display_,
                               L"Microphone cleanup failed; restart OrbitDeck before "
                               L"using Mic Check again");
        return false;
    }
    if (!headersReleased || closeResult == WAVERR_STILLPLAYING) {
        ++micCleanupAttempts_;
        if (micCleanupAttempts_ <= 20) {
            if (::SetTimer(window_, kMicCleanupTimerId, 250, nullptr) == 0 &&
                !::PostMessageW(window_, WM_TIMER, kMicCleanupTimerId, 0)) {
                ::Sleep(25);
                return StopMicCheck();
            }
        } else {
            (void)::KillTimer(window_, kMicCleanupTimerId);
            (void)::SetWindowTextW(display_,
                                   L"Microphone cleanup timed out; restart OrbitDeck "
                                   L"before using Mic Check again");
        }
        return false;
    }
    waveIn_ = nullptr;
    micBuffers_.reset();
    micCleanupAttempts_ = 0;
    (void)::KillTimer(window_, kMicCleanupTimerId);
    MSG message{};
    while (::PeekMessageW(&message, window_, MM_WIM_DATA, MM_WIM_DATA, PM_REMOVE)) {
    }
    return true;
}

void MiniAppWindow::HandleMicData(const HWAVEIN source, WAVEHDR* const header) noexcept {
    if (waveIn_ == nullptr || source != waveIn_ || header == nullptr ||
        header->dwUser != static_cast<DWORD_PTR>(micSessionGeneration_) ||
        tool_ != BuiltInTool::MicCheck || !micCapturing_) {
        return;
    }
    const std::size_t sampleCount = header->dwBytesRecorded / sizeof(short);
    const auto* const samples = reinterpret_cast<const short*>(header->lpData);
    int peak = 0;
    for (std::size_t index = 0; index < sampleCount; ++index) {
        const int value =
            samples[index] == SHRT_MIN ? SHRT_MAX : std::abs(static_cast<int>(samples[index]));
        peak = std::max(peak, value);
    }
    const int percent = static_cast<int>(
        std::sqrt(static_cast<double>(peak) / static_cast<double>(SHRT_MAX)) * 100.0);
    const std::wstring text =
        L"Microphone level: " + std::to_wstring(std::clamp(percent, 0, 100)) + L"%";
    (void)::SetWindowTextW(display_, text.c_str());
    header->dwBytesRecorded = 0;
    if (::waveInAddBuffer(waveIn_, header, sizeof(*header)) != MMSYSERR_NOERROR) {
        (void)::SetWindowTextW(display_, L"Microphone disconnected or unavailable");
        (void)StopMicCheck();
    }
}

bool MiniAppWindow::LoadScratchpad() noexcept {
    const std::wstring folder = ConfigFolderPath();
    if (folder.empty())
        return false;
    const std::wstring saved = folder + L"\\scratchpad.txt";
    const std::wstring recovery[]{saved + L".recovery0", saved + L".recovery1"};
    scratchpadRecoverySlot_ = -1;
    std::wstring selectedText;
    const ScratchpadReadStatus primaryStatus =
        ReadScratchpadFile(saved, selectedText, scratchpadGeneration_);
    if (primaryStatus == ScratchpadReadStatus::Unsupported)
        return false;
    bool haveCandidate = primaryStatus != ScratchpadReadStatus::Invalid;
    if (primaryStatus == ScratchpadReadStatus::Missing) {
        scratchpadGeneration_ = 0;
        selectedText.clear();
    }
    for (int slot = 0; slot < 2; ++slot) {
        const std::wstring ready = recovery[slot] + L".ready";
        if (::GetFileAttributesW(ready.c_str()) == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                continue;
            }
            return false;
        }
        const std::uint64_t markerGeneration = ReadGeneration(ready);
        if (markerGeneration == 0)
            return false;
        std::wstring recoveredText;
        std::uint64_t generation = 0;
        const ScratchpadReadStatus recoveryStatus =
            ReadScratchpadFile(recovery[slot], recoveredText, generation);
        if (recoveryStatus != ScratchpadReadStatus::Loaded) {
            if (recoveryStatus == ScratchpadReadStatus::Missing &&
                primaryStatus == ScratchpadReadStatus::Loaded &&
                markerGeneration <= scratchpadGeneration_ && ::DeleteFileW(ready.c_str())) {
                continue;
            }
            return false;
        }
        if (generation != markerGeneration) {
            return false;
        }
        if (generation > scratchpadGeneration_) {
            scratchpadGeneration_ = generation;
            scratchpadRecoverySlot_ = slot;
            selectedText = std::move(recoveredText);
            haveCandidate = true;
        }
    }
    std::wstring fileEmergency;
    std::uint64_t fileEmergencyGeneration = 0;
    std::wstring registryEmergency;
    std::uint64_t registryEmergencyGeneration = 0;
    const std::wstring emergencyPath = ScratchpadEmergencyPath();
    ScratchpadReadStatus fileEmergencyStatus = ScratchpadReadStatus::Missing;
    if (!emergencyPath.empty()) {
        fileEmergencyStatus =
            ReadScratchpadFile(emergencyPath, fileEmergency, fileEmergencyGeneration);
    }
    const ScratchpadReadStatus registryEmergencyStatus =
        ReadRegistryScratchpadRecovery(registryEmergency, registryEmergencyGeneration);
    if (fileEmergencyStatus == ScratchpadReadStatus::Unsupported ||
        registryEmergencyStatus == ScratchpadReadStatus::Unsupported ||
        fileEmergencyStatus == ScratchpadReadStatus::Invalid ||
        registryEmergencyStatus == ScratchpadReadStatus::Invalid) {
        return false;
    }
    if (fileEmergencyStatus == ScratchpadReadStatus::Loaded &&
        fileEmergencyGeneration > scratchpadGeneration_) {
        selectedText = std::move(fileEmergency);
        scratchpadGeneration_ = fileEmergencyGeneration;
        scratchpadRecoverySlot_ = -1;
        haveCandidate = true;
    }
    if (registryEmergencyStatus == ScratchpadReadStatus::Loaded &&
        registryEmergencyGeneration >= scratchpadGeneration_) {
        selectedText = std::move(registryEmergency);
        scratchpadGeneration_ = registryEmergencyGeneration;
        scratchpadRecoverySlot_ = -1;
        haveCandidate = true;
    }
    if (!haveCandidate)
        return false;

    std::wstring normalized;
    normalized.reserve(selectedText.size() + 16);
    for (std::size_t index = 0; index < selectedText.size(); ++index) {
        if (selectedText[index] == L'\r') {
            if (index + 1 < selectedText.size() && selectedText[index + 1] == L'\n') {
                ++index;
            }
            normalized += L"\r\n";
        } else if (selectedText[index] == L'\n') {
            normalized += L"\r\n";
        } else {
            normalized.push_back(selectedText[index]);
        }
        if (normalized.size() > kMaximumScratchpadCharacters) {
            return false;
        }
    }
    const bool loaded =
        ::SetWindowTextW(input_, normalized.c_str()) != FALSE && WindowText(input_) == normalized;
    if (loaded && scratchpadRecoverySlot_ < 0) {
        for (const std::wstring& item : recovery) {
            if (::GetFileAttributesW((item + L".ready").c_str()) == INVALID_FILE_ATTRIBUTES) {
                (void)::DeleteFileW(item.c_str());
            }
        }
    }
    return loaded;
}

bool MiniAppWindow::SaveScratchpad() noexcept {
    const std::wstring folder = ConfigFolderPath();
    if (folder.empty())
        return false;
    const int createFolder = ::SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    if (createFolder != ERROR_SUCCESS && createFolder != ERROR_ALREADY_EXISTS &&
        createFolder != ERROR_FILE_EXISTS) {
        return false;
    }
    const std::wstring text = WindowText(input_);
    const std::wstring path = folder + L"\\scratchpad.txt";
    const int nextSlot = scratchpadRecoverySlot_ == 0 ? 1 : 0;
    const std::uint64_t nextGeneration = scratchpadGeneration_ + 1;
    const std::wstring temporary = path + L".recovery" + std::to_wstring(nextSlot);
    const std::wstring ready = temporary + L".ready";
    if (GetFileAttributesW(ready.c_str()) != INVALID_FILE_ATTRIBUTES &&
        !::DeleteFileW(ready.c_str())) {
        return false;
    }
    const HANDLE file = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const std::size_t textByteCount = text.size() * sizeof(wchar_t);
    const std::size_t byteCount = sizeof(ScratchpadHeader) + textByteCount;
    if (textByteCount > kMaximumScratchpadBytes || byteCount > MAXDWORD) {
        (void)::CloseHandle(file);
        (void)::DeleteFileW(temporary.c_str());
        return false;
    }
    const ScratchpadHeader header{kScratchpadMagic, 1, nextGeneration};
    DWORD headerWritten = 0;
    DWORD textWritten = 0;
    const bool writtenOk =
        ::WriteFile(file, &header, sizeof(header), &headerWritten, nullptr) != FALSE &&
        headerWritten == static_cast<DWORD>(sizeof(header)) &&
        (text.empty() || (::WriteFile(file, text.data(), static_cast<DWORD>(textByteCount),
                                      &textWritten, nullptr) != FALSE &&
                          static_cast<std::size_t>(textWritten) == textByteCount)) &&
        ::FlushFileBuffers(file) != FALSE;
    (void)::CloseHandle(file);
    if (!writtenOk) {
        (void)::DeleteFileW(temporary.c_str());
        return false;
    }

    if (!WriteGeneration(ready, nextGeneration)) {
        (void)::DeleteFileW(ready.c_str());
        return false;
    }
    scratchpadRecoverySlot_ = nextSlot;
    scratchpadGeneration_ = nextGeneration;

    const bool replaced =
        ::MoveFileExW(temporary.c_str(), path.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (replaced) {
        for (int slot = 0; slot < 2; ++slot) {
            const std::wstring recovery = path + L".recovery" + std::to_wstring(slot);
            (void)::DeleteFileW(recovery.c_str());
            (void)::DeleteFileW((recovery + L".ready").c_str());
        }
        scratchpadRecoverySlot_ = -1;
        const std::wstring emergencyPath = ScratchpadEmergencyPath();
        if (!emergencyPath.empty()) {
            (void)::DeleteFileW(emergencyPath.c_str());
        }
        HKEY key = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\OrbitDeck", 0, KEY_SET_VALUE, &key) ==
            ERROR_SUCCESS) {
            (void)::RegDeleteValueW(key, L"ScratchpadRecovery");
            (void)::RegCloseKey(key);
        }
    }
    return replaced;
}

bool MiniAppWindow::SaveScratchpadRecovery() noexcept {
    const std::wstring text = WindowText(input_);
    const std::uint64_t generation = scratchpadGeneration_ + 1;
    const std::wstring path = ScratchpadEmergencyPath();
    if (!path.empty()) {
        (void)::SHCreateDirectoryExW(nullptr, ConfigFolderPath().c_str(), nullptr);
    }
    if (!path.empty() && WriteScratchpadFileAtomic(path, text, generation)) {
        scratchpadGeneration_ = generation;
        return true;
    }

    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\OrbitDeck", 0, nullptr, 0, KEY_SET_VALUE,
                          nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const ScratchpadHeader header{kScratchpadMagic, 1, generation};
    const std::size_t textBytes = text.size() * sizeof(wchar_t);
    std::vector<BYTE> value(sizeof(header) + textBytes);
    std::memcpy(value.data(), &header, sizeof(header));
    if (textBytes != 0) {
        std::memcpy(value.data() + sizeof(header), text.data(), textBytes);
    }
    const bool saved = ::RegSetValueExW(key, L"ScratchpadRecovery", 0, REG_BINARY, value.data(),
                                        static_cast<DWORD>(value.size())) == ERROR_SUCCESS;
    (void)::RegCloseKey(key);
    if (saved)
        scratchpadGeneration_ = generation;
    return saved;
}

void MiniAppWindow::Hide() noexcept {
    if (window_ == nullptr)
        return;
    if (tool_ == BuiltInTool::Scratchpad && scratchpadLoaded_ && !SaveScratchpad()) {
        if (!SaveScratchpadRecovery()) {
            (void)::SetWindowTextW(display_, L"Save failed; the note remains open");
            (void)::ShowWindow(window_, SW_SHOWNORMAL);
            (void)::SetForegroundWindow(window_);
            return;
        }
    }
    if (tool_ == BuiltInTool::Mirror)
        StopMirror();
    if (tool_ == BuiltInTool::MicCheck)
        (void)StopMicCheck();
    (void)::KillTimer(window_, kSystemTimerId);
    (void)::ShowWindow(window_, SW_HIDE);
}

void MiniAppWindow::SetPinned(const bool pinned) noexcept {
    pinned_ = pinned;
    (void)::SetWindowPos(window_, pinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

}  // namespace orbitdeck
