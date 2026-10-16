#include "window/WindowActivator.h"

#include <windows.h>

#include <dwmapi.h>

#include <cwctype>

namespace orbitdeck {
namespace {

std::wstring ToLower(std::wstring value) {
    for (wchar_t& c : value) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return value;
}

std::wstring Trim(const std::wstring& value) {
    constexpr wchar_t whitespace[] = L" \t\r\n";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::wstring::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

std::wstring BaseName(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Lowercased image basename of the window's owning process, or empty on any
// failure (access denied, exited process). Never elevates or reads memory.
std::wstring ProcessBaseName(HWND window) {
    DWORD processId = 0;
    (void)::GetWindowThreadProcessId(window, &processId);
    if (processId == 0) {
        return {};
    }
    const HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return {};
    }
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    const BOOL ok = ::QueryFullProcessImageNameW(process, 0, path, &size);
    (void)::CloseHandle(process);
    if (!ok || size == 0) {
        return {};
    }
    return ToLower(BaseName(std::wstring(path, size)));
}

bool IsCloaked(HWND window) {
    DWORD cloaked = 0;
    const HRESULT hr = ::DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return SUCCEEDED(hr) && cloaked != 0;
}

struct EnumState {
    const std::wstring* matchName;  // already lowercase
    DWORD ownProcessId;
    HWND best;
    long bestArea;
};

BOOL CALLBACK EnumProc(HWND window, LPARAM param) {
    auto& state = *reinterpret_cast<EnumState*>(param);

    // A minimized window keeps WS_VISIBLE, so this still matches it for restore;
    // it only rejects genuinely hidden windows.
    if (!::IsWindowVisible(window)) {
        return TRUE;
    }
    // Owned windows are dialogs/tool windows, not the app's main window.
    if (::GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    // Cloaked windows live on another virtual desktop or are suspended UWP.
    if (IsCloaked(window)) {
        return TRUE;
    }

    DWORD processId = 0;
    (void)::GetWindowThreadProcessId(window, &processId);
    if (processId == state.ownProcessId) {
        return TRUE;
    }
    if (ProcessBaseName(window) != *state.matchName) {
        return TRUE;
    }

    // ponytail: multiple-window policy is "largest client area wins" as the main
    // window; upgrade to the weighted scoring model if it mis-picks a
    // multi-window app. A minimized match has area 0 and still beats the -1 seed,
    // so a minimized-only app is restored.
    RECT rect{};
    (void)::GetClientRect(window, &rect);
    const long area = (rect.right - rect.left) * (rect.bottom - rect.top);
    if (area > state.bestArea) {
        state.best = window;
        state.bestArea = area;
    }
    return TRUE;
}

HWND FindExistingWindow(const std::wstring& matchName) noexcept {
    if (matchName.empty())
        return nullptr;
    EnumState state{&matchName, ::GetCurrentProcessId(), nullptr, -1};
    (void)::EnumWindows(&EnumProc, reinterpret_cast<LPARAM>(&state));
    return state.best;
}

void FocusWindow(const HWND target, const bool maximize) noexcept {
    if (::IsIconic(target)) {
        (void)::ShowWindowAsync(target, SW_RESTORE);
    }
    if (maximize) {
        (void)::ShowWindowAsync(target, SW_MAXIMIZE);
    }
    (void)::SetForegroundWindow(target);

    if (::GetForegroundWindow() != target) {
        FLASHWINFO flash{};
        flash.cbSize = sizeof(flash);
        flash.hwnd = target;
        flash.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
        (void)::FlashWindowEx(&flash);
    }
}

}  // namespace

std::wstring ExecutableMatchName(const std::wstring& target) {
    const std::wstring trimmed = Trim(target);
    if (trimmed.find(L"://") != std::wstring::npos)
        return {};
    const auto colon = trimmed.find(L':');
    const bool drivePath = colon == 1 && std::iswalpha(trimmed.front()) && trimmed.size() > 2 &&
                           (trimmed[2] == L'\\' || trimmed[2] == L'/');
    if (colon != std::wstring::npos && !drivePath)
        return {};
    const std::wstring base = ToLower(BaseName(trimmed));
    constexpr wchar_t ext[] = L".exe";
    const bool isExe = base.size() > 4 && base.compare(base.size() - 4, 4, ext) == 0;
    return isExe ? base : std::wstring{};
}

bool ActivateExisting(const std::wstring& matchName, const bool maximize) noexcept {
    HWND target = FindExistingWindow(matchName);
    if (target == nullptr) {
        return false;
    }

    FocusWindow(target, maximize);
    return true;
}

bool HasExistingWindow(const std::wstring& matchName) noexcept {
    return FindExistingWindow(matchName) != nullptr;
}

bool ToggleExistingWindow(const std::wstring& matchName) noexcept {
    HWND target = FindExistingWindow(matchName);
    if (target == nullptr)
        return false;
    if (::GetForegroundWindow() == target && !::IsIconic(target)) {
        (void)::ShowWindowAsync(target, SW_MINIMIZE);
    } else {
        FocusWindow(target, false);
    }
    return true;
}

}  // namespace orbitdeck
