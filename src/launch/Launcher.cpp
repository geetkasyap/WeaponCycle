#include "launch/Launcher.h"

#include <windows.h>

#include <shellapi.h>

namespace orbitdeck {
namespace {

[[nodiscard]] std::wstring Trim(const std::wstring& value) {
    constexpr wchar_t whitespace[] = L" \t\r\n";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::wstring::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

// Expands %VAR% references using the process environment. Falls back to the
// input unchanged if expansion fails, so a literal target still launches.
[[nodiscard]] std::wstring ExpandEnvironment(const std::wstring& value) {
    const DWORD needed = ::ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (needed == 0) {
        return value;
    }
    std::wstring expanded(needed, L'\0');
    const DWORD written = ::ExpandEnvironmentStringsW(value.c_str(), expanded.data(), needed);
    if (written == 0 || written > needed) {
        return value;
    }
    expanded.resize(written - 1);  // drop the counted null terminator
    return expanded;
}

[[nodiscard]] std::wstring DescribeError(const std::wstring& target, const DWORD code) {
    std::wstring message = L"Weapon Cycle could not open \"" + target + L"\".";

    wchar_t* system = nullptr;
    const DWORD chars = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&system), 0, nullptr);
    if (chars != 0 && system != nullptr) {
        std::wstring detail(system, chars);
        while (!detail.empty() && (detail.back() == L'\r' || detail.back() == L'\n' ||
                                   detail.back() == L' ' || detail.back() == L'.')) {
            detail.pop_back();
        }
        if (!detail.empty()) {
            message += L" " + detail + L".";
        }
    }
    if (system != nullptr) {
        (void)::LocalFree(system);
    }
    return message;
}

}  // namespace

std::wstring ResolveShellTarget(const std::wstring& target, const LaunchKind kind) {
    const std::wstring trimmed = Trim(target);
    if (trimmed.empty()) {
        return {};
    }
    const std::wstring expanded = ExpandEnvironment(trimmed);
    if (kind == LaunchKind::Aumid) {
        return L"shell:AppsFolder\\" + expanded;
    }
    return expanded;
}

LaunchResult Launch(const std::wstring& target, const LaunchKind kind) noexcept {
    const std::wstring file = ResolveShellTarget(target, kind);
    if (file.empty()) {
        return {false, L"No launch target was configured for this item."};
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    // NO_UI: report failures through OrbitDeck, not the shell's own dialog.
    // The resident STA keeps pumping messages while the Shell completes handoff.
    info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_ASYNCOK;
    info.lpVerb = L"open";
    info.lpFile = file.c_str();
    info.nShow = SW_SHOWNORMAL;

    if (::ShellExecuteExW(&info) != FALSE) {
        return {true, {}};
    }
    return {false, DescribeError(target, ::GetLastError())};
}

}  // namespace orbitdeck
