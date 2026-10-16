#include "actions/ActionExecutor.h"

#include <windows.h>

#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace orbitdeck {
namespace {

std::wstring Lower(std::wstring_view value) {
    std::wstring result(value);
    for (wchar_t& character : result) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    return result;
}

std::wstring Trim(std::wstring_view value) {
    std::size_t first = 0;
    std::size_t last = value.size();
    while (first < last && std::iswspace(value[first]))
        ++first;
    while (last > first && std::iswspace(value[last - 1]))
        --last;
    return std::wstring(value.substr(first, last - first));
}

unsigned short ShortcutKey(std::wstring_view token) noexcept {
    const std::wstring lower = Lower(token);
    if (lower == L"space")
        return VK_SPACE;
    if (lower == L"enter" || lower == L"return")
        return VK_RETURN;
    if (lower == L"tab")
        return VK_TAB;
    if (lower == L"escape" || lower == L"esc")
        return VK_ESCAPE;
    if (lower == L"backspace")
        return VK_BACK;
    if (lower == L"delete" || lower == L"del")
        return VK_DELETE;
    if (lower == L"up")
        return VK_UP;
    if (lower == L"down")
        return VK_DOWN;
    if (lower == L"left")
        return VK_LEFT;
    if (lower == L"right")
        return VK_RIGHT;
    if (lower == L"/" || lower == L"?")
        return VK_OEM_2;
    if (lower.size() == 1) {
        const wchar_t value = lower.front();
        if (value >= L'a' && value <= L'z') {
            return static_cast<unsigned short>(L'A' + value - L'a');
        }
        if (value >= L'0' && value <= L'9') {
            return static_cast<unsigned short>(value);
        }
    }
    if (lower.size() >= 2 && lower.front() == L'f') {
        unsigned number = 0;
        for (std::size_t index = 1; index < lower.size(); ++index) {
            if (lower[index] < L'0' || lower[index] > L'9')
                return 0;
            number = number * 10 + static_cast<unsigned>(lower[index] - L'0');
        }
        if (number >= 1 && number <= 24) {
            return static_cast<unsigned short>(VK_F1 + number - 1);
        }
    }
    return 0;
}

LaunchResult CopyClipboardText(const std::wstring& value, const HWND owner) noexcept {
    const SIZE_T bytes = (value.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        return {false, L"Not enough memory to copy the snippet."};
    }
    void* destination = ::GlobalLock(memory);
    if (destination == nullptr) {
        (void)::GlobalFree(memory);
        return {false, L"Windows could not prepare the clipboard text."};
    }
    std::memcpy(destination, value.c_str(), bytes);
    (void)::GlobalUnlock(memory);
    if (owner == nullptr || !::OpenClipboard(owner)) {
        (void)::GlobalFree(memory);
        return {false, L"Windows could not open the clipboard."};
    }
    struct ClipboardCloser {
        ~ClipboardCloser() noexcept {
            (void)::CloseClipboard();
        }
    } closer;
    if (!::EmptyClipboard()) {
        (void)::GlobalFree(memory);
        return {false, L"Windows could not clear the clipboard."};
    }
    if (::SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        (void)::GlobalFree(memory);
        return {false, L"Windows rejected the clipboard text."};
    }
    return {true, {}};
}

LaunchResult RunShellCommand(const std::wstring& command) noexcept {
    if (Trim(command).empty()) {
        return {false, L"The shell command is empty."};
    }
    wchar_t comspec[32768]{};
    const DWORD length =
        ::GetEnvironmentVariableW(L"ComSpec", comspec, static_cast<DWORD>(std::size(comspec)));
    const std::wstring executable =
        length > 0 && length < std::size(comspec) ? std::wstring(comspec, length) : L"cmd.exe";
    const std::wstring parameters = L"/d /s /c \"" + command + L"\"";
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_FLAG_NO_UI;
    execute.lpVerb = L"open";
    execute.lpFile = executable.c_str();
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_SHOWNORMAL;
    if (!::ShellExecuteExW(&execute)) {
        return {false, L"Windows could not start the shell command."};
    }
    return {true, {}};
}

LaunchResult SendShortcut(const std::wstring& value) noexcept {
    ShortcutStroke stroke;
    if (!ParseShortcut(value, stroke)) {
        return {false,
                L"Shortcut must contain one key and optional Ctrl, Alt, Shift, "
                L"or Win modifiers."};
    }
    std::vector<INPUT> inputs;
    std::vector<std::pair<WORD, bool>> events;
    const auto extended = [](const WORD virtualKey) noexcept {
        switch (virtualKey) {
            case VK_INSERT:
            case VK_DELETE:
            case VK_HOME:
            case VK_END:
            case VK_PRIOR:
            case VK_NEXT:
            case VK_LEFT:
            case VK_RIGHT:
            case VK_UP:
            case VK_DOWN:
            case VK_DIVIDE:
            case VK_NUMLOCK:
                return true;
            default:
                return false;
        }
    };
    const auto key = [&](const WORD virtualKey, const bool up) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = virtualKey;
        input.ki.dwFlags =
            (up ? KEYEVENTF_KEYUP : 0) | (extended(virtualKey) ? KEYEVENTF_EXTENDEDKEY : 0);
        inputs.push_back(input);
        events.emplace_back(virtualKey, up);
    };
    if (stroke.control)
        key(VK_CONTROL, false);
    if (stroke.alt)
        key(VK_MENU, false);
    if (stroke.shift)
        key(VK_SHIFT, false);
    if (stroke.windows)
        key(VK_LWIN, false);
    key(stroke.virtualKey, false);
    key(stroke.virtualKey, true);
    if (stroke.windows)
        key(VK_LWIN, true);
    if (stroke.shift)
        key(VK_SHIFT, true);
    if (stroke.alt)
        key(VK_MENU, true);
    if (stroke.control)
        key(VK_CONTROL, true);
    const UINT sent = ::SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    if (sent < inputs.size()) {
        std::vector<INPUT> cleanup;
        for (std::size_t offset = sent; offset-- > 0;) {
            const auto [virtualKey, up] = events[offset];
            if (up)
                continue;
            bool released = false;
            for (std::size_t later = offset + 1; later < sent; ++later) {
                if (events[later].first == virtualKey && events[later].second) {
                    released = true;
                    break;
                }
            }
            if (released)
                continue;
            INPUT release{};
            release.type = INPUT_KEYBOARD;
            release.ki.wVk = virtualKey;
            release.ki.dwFlags =
                KEYEVENTF_KEYUP | (extended(virtualKey) ? KEYEVENTF_EXTENDEDKEY : 0);
            cleanup.push_back(release);
        }
        if (!cleanup.empty()) {
            (void)::SendInput(static_cast<UINT>(cleanup.size()), cleanup.data(), sizeof(INPUT));
        }
    }
    return sent == inputs.size()
               ? LaunchResult{true, {}}
               : LaunchResult{false, L"Windows could not send the configured shortcut."};
}

}  // namespace

bool IsDirectActionType(const std::wstring_view type) noexcept {
    const std::wstring lower = Lower(type);
    return lower == L"clipboard" || lower == L"shellcommand" || lower == L"shortcut" ||
           lower == L"system";
}

bool ParseShortcut(const std::wstring_view text, ShortcutStroke& stroke) noexcept {
    ShortcutStroke parsed;
    bool haveKey = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t separator = text.find(L'+', start);
        const std::size_t end = separator == std::wstring_view::npos ? text.size() : separator;
        const std::wstring token = Lower(Trim(text.substr(start, end - start)));
        if (token.empty())
            return false;
        if (token == L"ctrl" || token == L"control")
            parsed.control = true;
        else if (token == L"alt")
            parsed.alt = true;
        else if (token == L"shift")
            parsed.shift = true;
        else if (token == L"win" || token == L"windows")
            parsed.windows = true;
        else {
            if (haveKey)
                return false;
            parsed.virtualKey = ShortcutKey(token);
            if (parsed.virtualKey == 0)
                return false;
            haveKey = true;
        }
        if (separator == std::wstring_view::npos)
            break;
        start = separator + 1;
    }
    if (!haveKey)
        return false;
    stroke = parsed;
    return true;
}

std::wstring ResolveSystemActionTarget(const std::wstring_view action) {
    const std::wstring lower = Lower(Trim(action));
    if (lower == L"clock" || lower == L"timer" || lower == L"focus") {
        return L"ms-clock:";
    }
    if (lower == L"calculator")
        return L"calculator:";
    if (lower == L"magnifier")
        return L"magnify.exe";
    if (lower == L"notepad")
        return L"notepad.exe";
    if (lower == L"settings")
        return L"ms-settings:";
    if (lower == L"screenclip" || lower == L"snippingtool") {
        return L"ms-screenclip:";
    }
    if (lower == L"screenshots") {
        PWSTR raw = nullptr;
        const HRESULT result =
            ::SHGetKnownFolderPath(FOLDERID_Screenshots, KF_FLAG_DEFAULT, nullptr, &raw);
        std::wstring path;
        if (SUCCEEDED(result) && raw != nullptr)
            path.assign(raw);
        if (raw != nullptr)
            ::CoTaskMemFree(raw);
        return path;
    }
    return {};
}

LaunchResult ExecuteDirectAction(const std::wstring_view type, const std::wstring& value,
                                 const HWND clipboardOwner) noexcept {
    const std::wstring lower = Lower(type);
    if (lower == L"clipboard") {
        return CopyClipboardText(value, clipboardOwner);
    }
    if (lower == L"shellcommand")
        return RunShellCommand(value);
    if (lower == L"shortcut")
        return SendShortcut(value);
    if (lower == L"system") {
        const std::wstring action = Lower(Trim(value));
        if (action == L"lock") {
            return ::LockWorkStation()
                       ? LaunchResult{true, {}}
                       : LaunchResult{false, L"Windows could not lock the workstation."};
        }
        const std::wstring target = ResolveSystemActionTarget(action);
        return target.empty() ? LaunchResult{false, L"The configured system action is unknown."}
                              : Launch(target, LaunchKind::Shell);
    }
    return {false, L"The configured action type is unsupported."};
}

}  // namespace orbitdeck
