#include "config/ConfigLoader.h"

#include <windows.h>
#include <shlobj.h>

#include <array>
#include <cmath>
#include <cwctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// nlohmann/json is warning-clean under /W4 /permissive-; no suppression needed.
#include <nlohmann/json.hpp>

namespace orbitdeck {
namespace {

using json = nlohmann::json;
constexpr std::size_t kMaximumConfigBytes{4 * 1024 * 1024};
constexpr LONGLONG kMaximumResetHashBytes{64LL * 1024 * 1024};
constexpr int kMaximumJsonDepth{64};
constexpr std::size_t kMaximumJsonEvents{50000};

class JsonComplexityError final {};
bool ReadFileUtf8(const std::wstring& path, std::string& out);

bool ParseJsonBounded(const std::string& text, json& document, std::wstring& error) {
    if (text.size() > kMaximumConfigBytes) {
        error = L"The configuration is larger than the supported 4 MiB limit.";
        return false;
    }
    std::size_t events = 0;
    try {
        document = json::parse(text, [&](const int depth, const json::parse_event_t, json&) {
            if (depth > kMaximumJsonDepth || ++events > kMaximumJsonEvents) {
                throw JsonComplexityError{};
            }

            return true;
        });
        return true;
    } catch (const JsonComplexityError&) {
        error =
            L"The configuration is too deeply nested or contains too many "
            L"JSON values.";
        return false;
    } catch (const json::exception&) {
        error = L"The configuration file is not valid JSON.";
        return false;
    }
}

class ConfigTransactionLock {
public:
    explicit ConfigTransactionLock(const std::wstring& path) noexcept {
        const ULONGLONG deadline = ::GetTickCount64() + 5000;
        do {
            file_ = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
            if (file_ != INVALID_HANDLE_VALUE)
                return;
            ::Sleep(25);
        } while (::GetTickCount64() < deadline);
    }

    ~ConfigTransactionLock() noexcept {
        if (file_ != INVALID_HANDLE_VALUE) {
            (void)::CloseHandle(file_);
        }
    }

    [[nodiscard]] bool Acquired() const noexcept {
        return file_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE file_{INVALID_HANDLE_VALUE};
};

std::uint64_t HashBytes(const std::string_view bytes) noexcept {
    std::uint64_t hash{14695981039346656037ULL};
    for (const unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool ParseHash(const std::string& text, std::uint64_t& hash) noexcept {
    if (text.empty())
        return false;
    std::uint64_t value = 0;
    for (const char character : text) {
        if (character == '\r' || character == '\n')
            break;
        if (character < '0' || character > '9')
            return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    hash = value;
    return true;
}

bool ReadCommitMarker(const std::wstring& path, std::uint64_t& storedHash) {
    std::string marker;
    return ReadFileUtf8(path, marker) && ParseHash(marker, storedHash);
}

bool CommitMarkerMatches(const std::wstring& path, const std::uint64_t expectedHash) {
    std::uint64_t storedHash = 0;
    return ReadCommitMarker(path, storedHash) && storedHash == expectedHash;
}

bool PinnedGenerationCommitted(const std::wstring& generationPin,
                               const std::wstring& commitMarker) {
    std::string pinned;
    return ReadFileUtf8(generationPin, pinned) &&
           CommitMarkerMatches(commitMarker, HashBytes(pinned));
}

class LockedConfigFile {
public:
    ~LockedConfigFile() noexcept {
        Close();
    }

    void Close() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE) {
            (void)::UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &lock_);
            (void)::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            lock_ = {};
        }
    }

    [[nodiscard]] bool Open(const std::wstring& path, const bool allowDelete) noexcept {
        handle_ = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | (allowDelete ? FILE_SHARE_DELETE : 0), nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            return false;
        return ::LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                            MAXDWORD, MAXDWORD, &lock_) != FALSE;
    }

    [[nodiscard]] bool Read(std::string& bytes) const {
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(handle_, &size) || size.QuadPart < 0 ||
            size.QuadPart > static_cast<LONGLONG>(kMaximumConfigBytes)) {
            return false;
        }
        bytes.assign(static_cast<std::size_t>(size.QuadPart), '\0');
        if (bytes.empty())
            return true;
        LARGE_INTEGER start{};
        if (!::SetFilePointerEx(handle_, start, nullptr, FILE_BEGIN)) {
            return false;
        }
        DWORD read = 0;
        return ::ReadFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &read,
                          nullptr) != FALSE &&
               static_cast<std::size_t>(read) == bytes.size();
    }

    [[nodiscard]] bool Flush() const noexcept {
        return ::FlushFileBuffers(handle_) != FALSE;
    }

    [[nodiscard]] bool Hash(std::uint64_t& hash) const noexcept {
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(handle_, &size) || size.QuadPart < 0 ||
            size.QuadPart > kMaximumResetHashBytes) {
            return false;
        }
        LARGE_INTEGER start{};
        if (!::SetFilePointerEx(handle_, start, nullptr, FILE_BEGIN)) {
            return false;
        }
        hash = 14695981039346656037ULL;
        std::array<BYTE, 64 * 1024> buffer{};
        for (;;) {
            DWORD read = 0;
            if (!::ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                            nullptr)) {
                return false;
            }
            for (DWORD index = 0; index < read; ++index) {
                hash ^= buffer[index];
                hash *= 1099511628211ULL;
            }
            if (read == 0)
                return true;
        }
    }

    [[nodiscard]] bool Write(const std::string_view bytes) const noexcept {
        if (bytes.size() > kMaximumConfigBytes)
            return false;
        LARGE_INTEGER start{};
        if (!::SetFilePointerEx(handle_, start, nullptr, FILE_BEGIN)) {
            return false;
        }
        DWORD written = 0;
        if ((!bytes.empty() && (!::WriteFile(handle_, bytes.data(),
                                             static_cast<DWORD>(bytes.size()), &written, nullptr) ||
                                static_cast<std::size_t>(written) != bytes.size())) ||
            !::SetEndOfFile(handle_)) {
            return false;
        }
        return Flush();
    }

    [[nodiscard]] long long LastWrite() const noexcept {
        FILETIME write{};
        if (!::GetFileTime(handle_, nullptr, nullptr, &write))
            return 0;
        ULARGE_INTEGER packed{};
        packed.LowPart = write.dwLowDateTime;
        packed.HighPart = write.dwHighDateTime;
        return static_cast<long long>(packed.QuadPart);
    }

    [[nodiscard]] bool StillNames(const std::wstring& path) const noexcept {
        BY_HANDLE_FILE_INFORMATION locked{};
        if (!::GetFileInformationByHandle(handle_, &locked))
            return false;
        const HANDLE current =
            ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (current == INVALID_HANDLE_VALUE)
            return false;
        BY_HANDLE_FILE_INFORMATION named{};
        const bool ok = ::GetFileInformationByHandle(current, &named) != FALSE;
        (void)::CloseHandle(current);
        return ok && locked.dwVolumeSerialNumber == named.dwVolumeSerialNumber &&
               locked.nFileIndexHigh == named.nFileIndexHigh &&
               locked.nFileIndexLow == named.nFileIndexLow;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
    OVERLAPPED lock_{};
};

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        return {};
    }
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Expands %VAR% references. Returns the input unchanged when it contains no '%'
// or the expansion fails, so a malformed reference never loses the original.
std::wstring ExpandEnv(const std::wstring& s) {
    if (s.find(L'%') == std::wstring::npos) {
        return s;
    }
    const DWORD needed = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
    if (needed == 0) {
        return s;
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    const DWORD written = ExpandEnvironmentStringsW(s.c_str(), out.data(), needed);
    if (written == 0 || written > needed) {
        return s;
    }
    out.resize(static_cast<size_t>(written) - 1);  // drop trailing NUL
    return out;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && iswspace(s[b])) {
        ++b;
    }
    while (e > b && iswspace(s[e - 1])) {
        --e;
    }
    return s.substr(b, e - b);
}

std::wstring ToLower(std::wstring s) {
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(towlower(c));
    }
    return s;
}

bool HasEmbeddedNull(const std::wstring& value) noexcept {
    return value.find(L'\0') != std::wstring::npos;
}

BuiltInTool ParseBuiltInTool(const std::wstring& name) {
    const std::wstring lower = ToLower(name);
    if (lower == L"timer")
        return BuiltInTool::Timer;
    if (lower == L"caffeine")
        return BuiltInTool::Caffeine;
    if (lower == L"calculator")
        return BuiltInTool::Calculator;
    if (lower == L"scratchpad")
        return BuiltInTool::Scratchpad;
    if (lower == L"systemglance" || lower == L"system") {
        return BuiltInTool::SystemGlance;
    }
    if (lower == L"mirror" || lower == L"camera") {
        return BuiltInTool::Mirror;
    }
    if (lower == L"miccheck" || lower == L"microphone") {
        return BuiltInTool::MicCheck;
    }
    if (lower == L"colorpicker")
        return BuiltInTool::ColorPicker;
    if (lower == L"jsoneditor" || lower == L"json") {
        return BuiltInTool::JsonEditor;
    }
    if (lower == L"base64text" || lower == L"base64") {
        return BuiltInTool::Base64Text;
    }
    if (lower == L"guidgenerator" || lower == L"guid") {
        return BuiltInTool::GuidGenerator;
    }
    if (lower == L"jwtdecoder" || lower == L"jwt") {
        return BuiltInTool::JwtDecoder;
    }
    if (lower == L"urlcodec")
        return BuiltInTool::UrlCodec;
    if (lower == L"timestampconverter" || lower == L"timestamp") {
        return BuiltInTool::TimestampConverter;
    }
    if (lower == L"texttransform")
        return BuiltInTool::TextTransform;
    if (lower == L"base64image")
        return BuiltInTool::Base64Image;
    if (lower == L"numberbase")
        return BuiltInTool::NumberBase;
    if (lower == L"colorconverter")
        return BuiltInTool::ColorConverter;
    if (lower == L"hashcalculator" || lower == L"hash") {
        return BuiltInTool::HashCalculator;
    }
    if (lower == L"regextester" || lower == L"regex") {
        return BuiltInTool::RegexTester;
    }
    if (lower == L"textdiff" || lower == L"diff") {
        return BuiltInTool::TextDiff;
    }
    if (lower == L"fileconverter")
        return BuiltInTool::FileConverter;
    return BuiltInTool::None;
}

std::wstring BuiltInToolName(const BuiltInTool tool) {
    switch (tool) {
        case BuiltInTool::Timer:
            return L"timer";
        case BuiltInTool::Caffeine:
            return L"caffeine";
        case BuiltInTool::Calculator:
            return L"calculator";
        case BuiltInTool::Scratchpad:
            return L"scratchpad";
        case BuiltInTool::SystemGlance:
            return L"systemGlance";
        case BuiltInTool::Mirror:
            return L"mirror";
        case BuiltInTool::MicCheck:
            return L"micCheck";
        case BuiltInTool::ColorPicker:
            return L"colorPicker";
        case BuiltInTool::JsonEditor:
            return L"jsonEditor";
        case BuiltInTool::Base64Text:
            return L"base64Text";
        case BuiltInTool::GuidGenerator:
            return L"guidGenerator";
        case BuiltInTool::JwtDecoder:
            return L"jwtDecoder";
        case BuiltInTool::UrlCodec:
            return L"urlCodec";
        case BuiltInTool::TimestampConverter:
            return L"timestampConverter";
        case BuiltInTool::TextTransform:
            return L"textTransform";
        case BuiltInTool::Base64Image:
            return L"base64Image";
        case BuiltInTool::NumberBase:
            return L"numberBase";
        case BuiltInTool::ColorConverter:
            return L"colorConverter";
        case BuiltInTool::HashCalculator:
            return L"hashCalculator";
        case BuiltInTool::RegexTester:
            return L"regexTester";
        case BuiltInTool::TextDiff:
            return L"textDiff";
        case BuiltInTool::FileConverter:
            return L"fileConverter";
        case BuiltInTool::None:
            return {};
    }
    return {};
}

// Maps a single key token (already lowercased) to a virtual-key code, or 0 if
// it names no supported key.
UINT KeyFromToken(const std::wstring& l) {
    if (l == L"space")
        return VK_SPACE;
    if (l == L"escape" || l == L"esc")
        return VK_ESCAPE;
    if (l == L"enter" || l == L"return")
        return VK_RETURN;
    if (l == L"tab")
        return VK_TAB;
    if (l == L"backspace")
        return VK_BACK;
    if (l == L"delete" || l == L"del")
        return VK_DELETE;
    if (l.size() == 1) {
        const wchar_t c = l[0];
        if (c >= L'a' && c <= L'z')
            return static_cast<UINT>(L'A' + (c - L'a'));
        if (c >= L'0' && c <= L'9')
            return static_cast<UINT>(c);
    }
    if (l.size() >= 2 && l[0] == L'f') {
        int num = 0;
        for (size_t i = 1; i < l.size(); ++i) {
            if (l[i] < L'0' || l[i] > L'9') {
                return 0;
            }
            num = num * 10 + (l[i] - L'0');
        }
        if (num >= 1 && num <= 24) {
            return static_cast<UINT>(VK_F1 + (num - 1));
        }
    }
    return 0;
}

// Parses "Shift+Space", "Ctrl+Alt+K", "F13", etc. into an ActivationConfig.
// Returns false on an empty token, an unknown key, or more than one base key.
bool ParseHotkey(const std::wstring& text, ActivationConfig& out) {
    UINT mods = MOD_NOREPEAT;
    UINT vk = 0;
    std::vector<std::wstring> tokens;
    std::wstring cur;
    for (const wchar_t c : text) {
        if (c == L'+') {
            tokens.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    tokens.push_back(cur);

    for (const std::wstring& raw : tokens) {
        const std::wstring t = Trim(raw);
        if (t.empty()) {
            return false;
        }
        const std::wstring l = ToLower(t);
        if (l == L"ctrl" || l == L"control") {
            mods |= MOD_CONTROL;
        } else if (l == L"alt") {
            mods |= MOD_ALT;
        } else if (l == L"shift") {
            mods |= MOD_SHIFT;
        } else if (l == L"win" || l == L"windows") {
            mods |= MOD_WIN;
        } else {
            if (vk != 0) {
                return false;  // two base keys
            }
            vk = KeyFromToken(l);
            if (vk == 0) {
                return false;
            }
        }
    }
    if (vk == 0) {
        return false;
    }
    out.modifiers = mods;
    out.virtualKey = vk;
    return true;
}

// A '#'-prefixed theme string is treated as a colour and must be 3/4/6/8 hex
// digits; any other non-empty string is an accepted theme name.
// ponytail: named themes currently share orbitDark; add a palette catalog only
// when a second named palette is designed.
bool IsValidThemeString(const std::wstring& s) {
    if (s.empty()) {
        return false;
    }
    if (s[0] != L'#') {
        return true;  // named theme
    }
    const std::wstring hex = s.substr(1);
    if (!(hex.size() == 3 || hex.size() == 4 || hex.size() == 6 || hex.size() == 8)) {
        return false;
    }
    for (const wchar_t c : hex) {
        const bool isHex =
            (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
        if (!isHex) {
            return false;
        }
    }
    return true;
}

std::wstring OptString(const json& obj, const char* key) {
    if (obj.contains(key) && obj[key].is_string()) {
        return Utf8ToWide(obj[key].get<std::string>());
    }
    return {};
}

bool ValidateAppearance(const json& ap, Configuration& cfg, std::wstring& error) {
    if (ap.contains("reducedMotion")) {
        if (!ap["reducedMotion"].is_boolean()) {
            error = L"appearance.reducedMotion must be true or false.";
            return false;
        }
        cfg.appearance.reducedMotion = ap["reducedMotion"].get<bool>();
    }

    const auto readDimension = [&](const char* key, float& destination) -> bool {
        if (!ap.contains(key)) {
            return true;
        }
        if (!ap[key].is_number()) {
            error = L"appearance." + Utf8ToWide(key) + L" must be a number.";
            return false;
        }
        const double value = ap[key].get<double>();
        if (!std::isfinite(value) || value <= 0.0 || value > 2000.0) {
            error = L"appearance." + Utf8ToWide(key) + L" must be between 0 and 2000.";
            return false;
        }
        destination = static_cast<float>(value);
        return true;
    };
    if (!readDimension("outerRadiusDip", cfg.appearance.outerRadiusDip) ||
        !readDimension("innerRadiusDip", cfg.appearance.innerRadiusDip) ||
        !readDimension("selectionThresholdDip", cfg.appearance.selectionThresholdDip) ||
        !readDimension("neutralThresholdDip", cfg.appearance.neutralThresholdDip) ||
        !readDimension("wheelDiameterDip", cfg.appearance.wheelDiameterDip)) {
        return false;
    }
    if (cfg.appearance.wheelDiameterDip < 240.0F) {
        error = L"appearance.wheelDiameterDip must be at least 240.";
        return false;
    }

    if (cfg.appearance.innerRadiusDip >= cfg.appearance.outerRadiusDip) {
        error = L"appearance.innerRadiusDip must be smaller than outerRadiusDip.";
        return false;
    }
    if (cfg.appearance.outerRadiusDip - cfg.appearance.innerRadiusDip < 24.0F) {
        error = L"appearance outer and inner radii must be at least 24 DIPs apart.";
        return false;
    }
    if (cfg.appearance.outerRadiusDip > cfg.appearance.wheelDiameterDip * 0.5F - 8.0F) {
        error = L"appearance.outerRadiusDip must fit inside wheelDiameterDip.";
        return false;
    }
    if (cfg.appearance.neutralThresholdDip > cfg.appearance.selectionThresholdDip) {
        error =
            L"appearance.neutralThresholdDip must not exceed "
            L"selectionThresholdDip.";
        return false;
    }
    if (cfg.appearance.selectionThresholdDip > cfg.appearance.outerRadiusDip) {
        error =
            L"appearance.selectionThresholdDip must not exceed "
            L"outerRadiusDip.";
        return false;
    }

    if (ap.contains("scrimOpacity")) {
        if (!ap["scrimOpacity"].is_number()) {
            error = L"appearance.scrimOpacity must be a number.";
            return false;
        }
        const double scrim = ap["scrimOpacity"].get<double>();
        if (scrim < 0.0 || scrim > 1.0) {
            error = L"appearance.scrimOpacity must be between 0 and 1.";
            return false;
        }
        cfg.appearance.scrimOpacity = static_cast<float>(scrim);
    }
    if (ap.contains("wheelOpacity")) {
        if (!ap["wheelOpacity"].is_number()) {
            error = L"appearance.wheelOpacity must be a number.";
            return false;
        }
        const double opacity = ap["wheelOpacity"].get<double>();
        if (opacity < 0.2 || opacity > 1.0) {
            error = L"appearance.wheelOpacity must be between 0.2 and 1.";
            return false;
        }
        cfg.appearance.wheelOpacity = static_cast<float>(opacity);
    }

    if (ap.contains("theme")) {
        if (!ap["theme"].is_string()) {
            error = L"appearance.theme must be a string.";
            return false;
        }
        cfg.appearance.theme = Utf8ToWide(ap["theme"].get<std::string>());
        if (!IsValidThemeString(cfg.appearance.theme)) {
            error = L"appearance.theme is not a valid theme name or colour.";
            return false;
        }
    }
    return true;
}

bool MapItems(const json& items, std::vector<WheelItem>& out, std::set<std::wstring>& seenIds,
              const std::size_t depth, std::size_t& totalItems, std::wstring& error);

// Maps one leaf or submenu recursively. A submenu has children and no launch
// action; a leaf has one launch action.
bool MapItem(const json& it, WheelItem& out, std::set<std::wstring>& seenIds,
             const std::size_t depth, std::size_t& totalItems, std::wstring& error) {
    WheelItem w;
    if (it.contains("label") && !it["label"].is_string()) {
        error = L"An item's label must be a string.";
        return false;
    }
    w.label = OptString(it, "label");
    if (HasEmbeddedNull(w.label)) {
        error = L"An item's label contains an embedded null character.";
        return false;
    }
    if (it.contains("quickKey")) {
        if (!it["quickKey"].is_string()) {
            error = L"An item's quickKey must be a string.";
            return false;
        }
        const std::wstring quickKey = Utf8ToWide(it["quickKey"].get<std::string>());
        if (quickKey.size() != 1 || !std::iswalnum(quickKey.front())) {
            error = L"An item's quickKey must be one letter or number.";
            return false;
        }
        w.quickKey = static_cast<wchar_t>(std::towupper(quickKey.front()));
    }

    if (it.contains("enabled")) {
        if (!it["enabled"].is_boolean()) {
            error = L"An item's enabled must be true or false.";
            return false;
        }
        w.enabled = it["enabled"].get<bool>();
    }

    std::wstring iconPath;
    if (it.contains("icon") && !it["icon"].is_object()) {
        error = L"An item's icon must be an object.";
        return false;
    }
    if (it.contains("icon")) {
        if (it["icon"].contains("path") && !it["icon"]["path"].is_string()) {
            error = L"An item's icon.path must be a string.";
            return false;
        }
        if (it["icon"].contains("type") && !it["icon"]["type"].is_string()) {
            error = L"An item's icon.type must be a string.";
            return false;
        }
        iconPath = OptString(it["icon"], "path");
        if (HasEmbeddedNull(iconPath)) {
            error = L"An item's icon.path contains an embedded null character.";
            return false;
        }
    }
    w.configuredIconSource = iconPath;
    w.iconAutomatic = iconPath.empty();

    if (it.contains("children")) {
        if (it.contains("launch") || it.contains("builtin")) {
            error = L"Item '" + (w.label.empty() ? L"?" : w.label) +
                    L"' cannot have both launch and children.";
            return false;
        }
        if (!MapItems(it["children"], w.children, seenIds, depth + 1, totalItems, error)) {
            return false;
        }
        w.iconSource = ExpandEnv(iconPath);
        out = std::move(w);
        return true;
    }

    if (it.contains("builtin")) {
        if (it.contains("launch")) {
            error = L"Item '" + (w.label.empty() ? L"?" : w.label) +
                    L"' cannot have both launch and builtin.";
            return false;
        }
        if (!it["builtin"].is_string()) {
            error = L"An item's builtin must be a string.";
            return false;
        }
        w.builtInTool = ParseBuiltInTool(Utf8ToWide(it["builtin"].get<std::string>()));
        if (w.builtInTool == BuiltInTool::None) {
            error = L"An item names an unsupported built-in tool.";
            return false;
        }
        w.iconSource = ExpandEnv(iconPath);
        out = std::move(w);
        return true;
    }

    if (!it.contains("launch") || !it["launch"].is_object()) {
        error =
            L"Item '" + (w.label.empty() ? L"?" : w.label) + L"' has neither launch nor children.";
        return false;
    }
    const json& launch = it["launch"];
    if (!launch.contains("candidates") || !launch["candidates"].is_array() ||
        launch["candidates"].empty()) {
        error = L"Item '" + (w.label.empty() ? L"?" : w.label) + L"' has no launch candidates.";
        return false;
    }

    if (launch["candidates"].size() > kMaximumLaunchCandidates) {
        error = L"An item has more than " + std::to_wstring(kMaximumLaunchCandidates) +
                L" launch candidates.";
        return false;
    }
    w.launchCandidates.reserve(launch["candidates"].size());
    for (const json& cand : launch["candidates"]) {
        if (!cand.is_object()) {
            error = L"Each launch candidate must be an object.";
            return false;
        }
        if (!cand.contains("target") || !cand["target"].is_string()) {
            error = L"An item's launch target must be a string.";
            return false;
        }
        if (cand.contains("type") && !cand["type"].is_string()) {
            error = L"An item's launch type must be a string.";
            return false;
        }
        LaunchCandidate mapped;
        mapped.configuredTarget = OptString(cand, "target");
        if (mapped.configuredTarget.empty() || HasEmbeddedNull(mapped.configuredTarget)) {
            error = L"Item '" + (w.label.empty() ? L"?" : w.label) +
                    L"' has a launch candidate with no target.";
            return false;
        }
        mapped.type = OptString(cand, "type");
        if (HasEmbeddedNull(mapped.type)) {
            error = L"An item's launch type contains an embedded null character.";
            return false;
        }
        if (mapped.type.empty())
            mapped.type = L"shell";
        const std::wstring normalizedType = ToLower(mapped.type);
        mapped.target = normalizedType == L"clipboard" || normalizedType == L"shellcommand" ||
                                normalizedType == L"shortcut" || normalizedType == L"system" ||
                                normalizedType == L"url" || normalizedType == L"uri"
                            ? mapped.configuredTarget
                            : ExpandEnv(mapped.configuredTarget);
        mapped.kind = normalizedType == L"aumid" ? LaunchKind::Aumid : LaunchKind::Shell;
        w.launchCandidates.push_back(std::move(mapped));
    }
    const LaunchCandidate& first = w.launchCandidates.front();
    w.configuredTarget = first.configuredTarget;
    w.target = first.target;
    w.launchType = first.type;
    w.launchKind = first.kind;

    // An empty icon path auto-derives the icon from the launch target.
    w.iconSource = iconPath.empty() ? w.target : ExpandEnv(iconPath);

    if (it.contains("existingWindowAction") && !it["existingWindowAction"].is_string()) {
        error = L"An item's existingWindowAction must be a string.";
        return false;
    }
    w.existingWindowAction = OptString(it, "existingWindowAction");
    if (HasEmbeddedNull(w.existingWindowAction)) {
        error =
            L"An item's existingWindowAction contains an embedded null "
            L"character.";
        return false;
    }
    if (w.existingWindowAction.empty()) {
        w.existingWindowAction = L"focus";
    }
    const std::wstring windowAction = ToLower(w.existingWindowAction);
    if (windowAction == L"focus") {
        w.windowAction = ExistingWindowAction::Focus;
    } else if (windowAction == L"restoreandfocus") {
        w.windowAction = ExistingWindowAction::RestoreAndFocus;
    } else if (windowAction == L"maximizeandfocus") {
        w.windowAction = ExistingWindowAction::MaximizeAndFocus;
    } else if (windowAction == L"opennewinstance") {
        w.windowAction = ExistingWindowAction::OpenNewInstance;
    } else if (windowAction == L"donothing") {
        w.windowAction = ExistingWindowAction::DoNothing;
    } else if (windowAction == L"toggleminimize") {
        w.windowAction = ExistingWindowAction::ToggleMinimize;
    } else {
        // Schema v1 historically treated unknown values as focus. Keep that
        // compatibility while preserving the original string on round-trip.
        w.windowAction = ExistingWindowAction::Focus;
    }
    w.maximizeOnActivate = w.windowAction == ExistingWindowAction::MaximizeAndFocus;

    out = std::move(w);
    return true;
}

bool MapItems(const json& items, std::vector<WheelItem>& out, std::set<std::wstring>& seenIds,
              const std::size_t depth, std::size_t& totalItems, std::wstring& error) {
    if (!items.is_array() || items.empty()) {
        error = L"Each wheel level must contain at least one item.";
        return false;
    }
    if (items.size() > kMaximumWheelItems) {
        error = L"A wheel level has more than " + std::to_wstring(kMaximumWheelItems) + L" items.";
        return false;
    }
    if (depth > kMaximumWheelDepth) {
        error = L"The configuration has more than " + std::to_wstring(kMaximumWheelDepth) +
                L" submenu levels.";
        return false;
    }

    out.reserve(items.size());
    for (const json& it : items) {
        if (!it.is_object()) {
            error = L"Each item must be an object.";
            return false;
        }
        const std::wstring id = OptString(it, "id");
        if (id.empty() || HasEmbeddedNull(id)) {
            error = L"An item is missing its id.";
            return false;
        }
        if (!seenIds.insert(id).second) {
            error = L"Duplicate item id: " + id;
            return false;
        }
        if (++totalItems > kMaximumTotalWheelItems) {
            error = L"The configuration has more than " + std::to_wstring(kMaximumTotalWheelItems) +
                    L" total items.";
            return false;
        }

        WheelItem mapped;
        if (!MapItem(it, mapped, seenIds, depth, totalItems, error)) {
            return false;
        }
        mapped.id = id;
        out.push_back(std::move(mapped));
    }
    return true;
}

}  // namespace

ConfigParseResult ParseConfiguration(const std::string& utf8Json) {
    ConfigParseResult result;

    json doc;
    if (!ParseJsonBounded(utf8Json, doc, result.error)) {
        return result;
    }

    try {
        if (!doc.is_object()) {
            result.error = L"The configuration root must be an object.";
            return result;
        }

        if (!doc.contains("schemaVersion") || !doc["schemaVersion"].is_number_integer() ||
            doc["schemaVersion"].get<json::number_integer_t>() != 1) {
            result.error = L"Unsupported schemaVersion (expected 1).";
            return result;
        }

        Configuration cfg;

        if (doc.contains("activation") && !doc["activation"].is_object()) {
            result.error = L"activation must be an object.";
            return result;
        }
        if (doc.contains("activation")) {
            const json& act = doc["activation"];
            if (act.contains("hotkey")) {
                if (!act["hotkey"].is_string()) {
                    result.error = L"activation.hotkey must be a string.";
                    return result;
                }
                ActivationConfig parsed;
                if (!ParseHotkey(Utf8ToWide(act["hotkey"].get<std::string>()), parsed)) {
                    result.error = L"activation.hotkey is not a valid key combination.";
                    return result;
                }
                cfg.activation = parsed;
            }
            if (act.contains("experimentalLeftShiftOnly")) {
                if (!act["experimentalLeftShiftOnly"].is_boolean()) {
                    result.error = L"activation.experimentalLeftShiftOnly must be true or false.";
                    return result;
                }
                cfg.activation.experimentalLeftShiftOnly =
                    act["experimentalLeftShiftOnly"].get<bool>();
            }
            if (act.contains("leftShiftHoldThresholdMs")) {
                if (!act["leftShiftHoldThresholdMs"].is_number_unsigned()) {
                    result.error =
                        L"activation.leftShiftHoldThresholdMs must be a non-negative integer.";
                    return result;
                }
                const auto threshold = act["leftShiftHoldThresholdMs"].get<unsigned long long>();
                if (threshold > std::numeric_limits<UINT>::max()) {
                    result.error = L"activation.leftShiftHoldThresholdMs is too large.";
                    return result;
                }
                cfg.activation.leftShiftHoldThresholdMs = static_cast<UINT>(threshold);
            }
        }

        if (doc.contains("placement")) {
            if (!doc["placement"].is_object()) {
                result.error = L"placement must be an object.";
                return result;
            }
            if (doc["placement"].contains("mode")) {
                if (!doc["placement"]["mode"].is_string()) {
                    result.error = L"placement.mode must be a string.";
                    return result;
                }
                const std::wstring mode =
                    ToLower(Trim(Utf8ToWide(doc["placement"]["mode"].get<std::string>())));
                if (mode != L"cursor" && mode != L"monitorcentre" && mode != L"monitorcenter") {
                    result.error = L"placement.mode must be cursor or monitorCenter.";
                    return result;
                }
                if (mode == L"monitorcentre" || mode == L"monitorcenter") {
                    cfg.placement.mode = PlacementMode::MonitorCentre;
                }
            }
        }

        if (doc.contains("appearance") && !doc["appearance"].is_object()) {
            result.error = L"appearance must be an object.";
            return result;
        }
        if (doc.contains("appearance")) {
            if (!ValidateAppearance(doc["appearance"], cfg, result.error)) {
                return result;
            }
        }

        if (doc.contains("interaction")) {
            if (!doc["interaction"].is_object()) {
                result.error = L"interaction must be an object.";
                return result;
            }
            const json& interaction = doc["interaction"];
            const auto readBoolean = [&](const char* key, bool& destination) -> bool {
                if (!interaction.contains(key)) {
                    return true;
                }
                if (!interaction[key].is_boolean()) {
                    result.error = L"interaction." + Utf8ToWide(key) + L" must be true or false.";
                    return false;
                }
                destination = interaction[key].get<bool>();
                return true;
            };
            if (!readBoolean("cancelOnRightClick", cfg.interaction.cancelOnRightClick) ||
                !readBoolean("cancelOnOuterExit", cfg.interaction.cancelOnOuterExit) ||
                !readBoolean("numberQuickKeys", cfg.interaction.numberQuickKeys)) {
                return result;
            }
            if (interaction.contains("outerExitRadiusDip")) {
                if (!interaction["outerExitRadiusDip"].is_number()) {
                    result.error = L"interaction.outerExitRadiusDip must be a number.";
                    return result;
                }
                const double radius = interaction["outerExitRadiusDip"].get<double>();
                if (radius < kMinimumOuterExitRadiusDip || radius > 2000.0) {
                    result.error =
                        L"interaction.outerExitRadiusDip must be between 120 "
                        L"and 2000.";
                    return result;
                }
                cfg.interaction.outerExitRadiusDip = static_cast<float>(radius);
            }
            if (interaction.contains("submenuDwellMs")) {
                if (!interaction["submenuDwellMs"].is_number_unsigned()) {
                    result.error =
                        L"interaction.submenuDwellMs must be an unsigned "
                        L"integer.";
                    return result;
                }
                const auto dwell = interaction["submenuDwellMs"].get<unsigned long long>();
                if (dwell < 100 || dwell > 2000) {
                    result.error =
                        L"interaction.submenuDwellMs must be between 100 and "
                        L"2000.";
                    return result;
                }
                cfg.interaction.submenuDwellMs = static_cast<UINT>(dwell);
            }
        }
        if (cfg.interaction.cancelOnOuterExit &&
            cfg.interaction.outerExitRadiusDip <= cfg.appearance.selectionThresholdDip) {
            result.error =
                L"interaction.outerExitRadiusDip must be greater than "
                L"appearance.selectionThresholdDip when outer-exit "
                L"cancellation is enabled.";
            return result;
        }

        if (!doc.contains("items") || !doc["items"].is_array()) {
            result.error = L"The configuration has no items array.";
            return result;
        }
        std::set<std::wstring> seenIds;
        std::size_t totalItems = 0;
        if (!MapItems(doc["items"], cfg.items, seenIds, 1, totalItems, result.error)) {
            return result;
        }

        result.ok = true;
        result.config = std::move(cfg);
        return result;
    } catch (const json::exception&) {
        result.ok = false;
        result.error = L"The configuration has a field of the wrong type.";
        return result;
    }
}

std::wstring ExpandConfigurationValue(const std::wstring& value) {
    return ExpandEnv(value);
}

bool ParseHotkeyText(const std::wstring& text, ActivationConfig& activation) {
    return ParseHotkey(text, activation);
}

ConfigParseResult LoadConfigurationFile(const std::wstring& path, std::string* const sourceJson) {
    ConfigParseResult result;
    std::string bytes;
    if (path.empty() || !ReadFileUtf8(path, bytes)) {
        result.error = L"The selected configuration could not be read or exceeds 4 MiB.";
        return result;
    }
    result = ParseConfiguration(bytes);
    if (result.ok && sourceJson != nullptr) {
        *sourceJson = std::move(bytes);
    }
    return result;
}

std::string DefaultConfigJson() {
    // Eight example applications. Each item lists its full documented shape;
    // candidates are tried in order, most reliable first.
    return R"JSON({
  "schemaVersion": 1,
  "activation": {
    "hotkey": "Shift+Space",
    "cancelKey": "Escape",
    "experimentalLeftShiftOnly": false,
    "leftShiftHoldThresholdMs": 240
  },
  "placement": {
    "mode": "cursor"
  },
  "appearance": {
    "theme": "orbitDark",
    "reducedMotion": false,
    "wheelOpacity": 0.9,
    "scrimOpacity": 0.0,
    "wheelDiameterDip": 400,
    "outerRadiusDip": 156,
    "innerRadiusDip": 66,
    "selectionThresholdDip": 82,
    "neutralThresholdDip": 50
  },
  "interaction": {
    "cancelOnRightClick": true,
    "cancelOnOuterExit": true,
    "numberQuickKeys": true,
    "outerExitRadiusDip": 300,
    "submenuDwellMs": 350
  },
  "items": [
    {
      "id": "chrome",
      "label": "Chrome",
      "description": "Google Chrome web browser",
      "enabled": true,
      "icon": { "type": "auto", "path": "" },
      "launch": {
        "candidates": [
          { "type": "appPath", "target": "chrome.exe" },
          { "type": "shell", "target": "chrome" }
        ]
      },
      "windowMatch": { "processName": "chrome.exe", "className": "Chrome_WidgetWin_1" },
      "existingWindowAction": "maximizeAndFocus",
      "multipleWindowPolicy": "mostRecent",
      "allowNewInstance": true
    },
    {
      "id": "spotify",
      "label": "Spotify",
      "description": "Spotify music player",
      "enabled": true,
      "icon": { "type": "auto", "path": "" },
      "launch": {
        "candidates": [
          { "type": "appPath", "target": "Spotify.exe" },
          { "type": "uri", "target": "spotify:" }
        ]
      },
      "windowMatch": { "processName": "Spotify.exe", "className": "" },
      "existingWindowAction": "focus",
      "multipleWindowPolicy": "mostRecent",
      "allowNewInstance": true
    },
    {
      "id": "chatgpt",
      "label": "ChatGPT",
      "description": "ChatGPT in the default browser",
      "enabled": true,
      "icon": { "type": "auto", "path": "" },
      "launch": {
        "candidates": [
          { "type": "url", "target": "https://chatgpt.com" }
        ]
      },
      "windowMatch": { "processName": "", "className": "" },
      "existingWindowAction": "focus",
      "multipleWindowPolicy": "mostRecent",
      "allowNewInstance": false
    },
    {
      "id": "work",
      "label": "Work",
      "enabled": true,
      "icon": { "type": "auto", "path": "" },
      "children": [
    {
      "id": "notepad",
      "label": "Notepad",
      "description": "Windows Notepad",
      "enabled": true,
      "icon": { "type": "auto", "path": "" },
      "launch": {
        "candidates": [
          { "type": "appPath", "target": "notepad.exe" }
        ]
      },
      "windowMatch": { "processName": "notepad.exe", "className": "Notepad" },
      "existingWindowAction": "maximizeAndFocus",
      "multipleWindowPolicy": "mostRecent",
      "allowNewInstance": true
    },
    {
      "id": "teams",
      "label": "Teams",
      "description": "Microsoft Teams",
      "enabled": true,
      "icon": { "type": "auto", "path": "" },
      "launch": {
        "candidates": [
          { "type": "appPath", "target": "ms-teams.exe" },
          { "type": "uri", "target": "msteams:" }
        ]
      },
      "windowMatch": { "processName": "ms-teams.exe", "className": "" },
      "existingWindowAction": "focus",
      "multipleWindowPolicy": "mostRecent",
      "allowNewInstance": false
    },
    {
      "id": "explorer",
      "label": "File Explorer",
      "description": "Windows File Explorer",
      "enabled": true,
      "icon": { "type": "auto", "path": "" },
      "launch": {
        "candidates": [
          { "type": "shell", "target": "explorer.exe" }
        ]
      },
      "windowMatch": { "processName": "explorer.exe", "className": "CabinetWClass" },
      "existingWindowAction": "focus",
      "multipleWindowPolicy": "mostRecent",
      "allowNewInstance": true
    },
    {
      "id": "outlook",
      "label": "Outlook",
      "description": "Microsoft Outlook",
      "enabled": true,
      "icon": { "type": "auto", "path": "" },
      "launch": {
        "candidates": [
          { "type": "appPath", "target": "outlook.exe" },
          { "type": "uri", "target": "outlookmail:" }
        ]
      },
      "windowMatch": { "processName": "outlook.exe", "className": "" },
      "existingWindowAction": "maximizeAndFocus",
      "multipleWindowPolicy": "mostRecent",
      "allowNewInstance": false
    },
    {
      "id": "vscode",
      "label": "VS Code",
      "description": "Visual Studio Code",
      "enabled": true,
      "icon": { "type": "auto", "path": "" },
      "launch": {
        "candidates": [
          { "type": "path", "target": "%LOCALAPPDATA%\\Programs\\Microsoft VS Code\\Code.exe" },
          { "type": "shell", "target": "code" }
        ]
      },
      "windowMatch": { "processName": "Code.exe", "className": "Chrome_WidgetWin_1" },
      "existingWindowAction": "maximizeAndFocus",
      "multipleWindowPolicy": "mostRecent",
      "allowNewInstance": true
    }
      ]
    },
    {
      "id": "windows-tools",
      "label": "Windows",
      "enabled": true,
      "icon": { "type": "auto", "path": "shell:AppsFolder\\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App" },
      "children": [
        { "id": "clock", "label": "Clock / Timer", "enabled": true, "launch": { "candidates": [{ "type": "system", "target": "timer" }] } },
        { "id": "focus", "label": "Focus", "enabled": true, "launch": { "candidates": [{ "type": "system", "target": "focus" }] } },
        { "id": "windows-calculator", "label": "Calculator", "enabled": true, "launch": { "candidates": [{ "type": "system", "target": "calculator" }] } },
        { "id": "magnifier", "label": "Magnifier", "enabled": true, "launch": { "candidates": [{ "type": "system", "target": "magnifier" }] } },
        { "id": "windows-notepad", "label": "Notepad", "enabled": true, "launch": { "candidates": [{ "type": "system", "target": "notepad" }] } },
        { "id": "screenshots", "label": "Screenshots", "enabled": true, "launch": { "candidates": [{ "type": "system", "target": "screenshots" }] } },
        { "id": "windows-settings", "label": "Settings", "enabled": true, "launch": { "candidates": [{ "type": "system", "target": "settings" }] } }
      ]
    },
    {
      "id": "utilities",
      "label": "Utilities",
      "enabled": true,
      "icon": { "type": "auto", "path": "" },
      "children": [
        { "id": "file-converter", "label": "Convert a file", "enabled": true, "builtin": "fileConverter" },
        { "id": "color-picker", "label": "Color Picker", "enabled": true, "builtin": "colorPicker" },
        { "id": "keep-awake", "label": "Keep Awake", "enabled": true, "builtin": "caffeine" },
        { "id": "scratchpad", "label": "Scratchpad", "enabled": true, "builtin": "scratchpad" },
        { "id": "mirror", "label": "Mirror", "enabled": true, "builtin": "mirror" },
        { "id": "system-glance", "label": "System Glance", "enabled": true, "builtin": "systemGlance" }
      ]
    },
    {
      "id": "developer",
      "label": "Developer",
      "enabled": true,
      "icon": { "type": "auto", "path": "" },
      "children": [
        {
          "id": "developer-data",
          "label": "Data",
          "enabled": true,
          "children": [
            { "id": "json-editor", "label": "JSON Editor", "enabled": true, "builtin": "jsonEditor" },
            { "id": "jwt-decoder", "label": "JWT Decoder", "enabled": true, "builtin": "jwtDecoder" },
            { "id": "timestamp", "label": "Timestamp", "enabled": true, "builtin": "timestampConverter" }
          ]
        },
        {
          "id": "developer-encoding",
          "label": "Encoding",
          "enabled": true,
          "children": [
            { "id": "base64-text", "label": "Base64", "enabled": true, "builtin": "base64Text" },
            { "id": "url-codec", "label": "URL Codec", "enabled": true, "builtin": "urlCodec" },
            { "id": "base64-image", "label": "Base64 Image", "enabled": true, "builtin": "base64Image" }
          ]
        },
        {
          "id": "developer-text",
          "label": "Text",
          "enabled": true,
          "children": [
            { "id": "text-transform", "label": "Transform", "enabled": true, "builtin": "textTransform" },
            { "id": "regex-tester", "label": "Regex Tester", "enabled": true, "builtin": "regexTester" },
            { "id": "text-diff", "label": "Text Diff", "enabled": true, "builtin": "textDiff" }
          ]
        },
        {
          "id": "developer-generators",
          "label": "Generators",
          "enabled": true,
          "children": [
            { "id": "guid-generator", "label": "GUID", "enabled": true, "builtin": "guidGenerator" },
            { "id": "hash-calculator", "label": "Hash", "enabled": true, "builtin": "hashCalculator" },
            { "id": "number-base", "label": "Number Base", "enabled": true, "builtin": "numberBase" },
            { "id": "color-converter", "label": "Color", "enabled": true, "builtin": "colorConverter" }
          ]
        }
      ]
    },
    {
      "id": "powertoys",
      "label": "PowerToys",
      "enabled": true,
      "icon": { "type": "auto", "path": "%LOCALAPPDATA%\\PowerToys\\PowerToys.exe" },
      "children": [
        { "id": "pt-command-palette", "label": "Command Palette", "enabled": true, "launch": { "candidates": [{ "type": "shortcut", "target": "Win+Alt+Space" }] } },
        { "id": "pt-advanced-paste", "label": "Advanced Paste", "enabled": true, "launch": { "candidates": [{ "type": "shortcut", "target": "Win+Shift+V" }] } },
        { "id": "pt-always-on-top", "label": "Always On Top", "enabled": true, "launch": { "candidates": [{ "type": "shortcut", "target": "Win+Ctrl+T" }] } },
        { "id": "pt-screen-ruler", "label": "Screen Ruler", "enabled": true, "launch": { "candidates": [{ "type": "shortcut", "target": "Win+Ctrl+Shift+M" }] } },
        { "id": "pt-text-extractor", "label": "Text Extractor", "enabled": true, "launch": { "candidates": [{ "type": "shortcut", "target": "Win+Shift+T" }] } },
        { "id": "pt-shortcut-guide", "label": "Shortcut Guide", "enabled": true, "launch": { "candidates": [{ "type": "shortcut", "target": "Win+Shift+/" }] } },
        { "id": "pt-peek", "label": "Peek Selected File", "enabled": true, "launch": { "candidates": [{ "type": "shortcut", "target": "Ctrl+Space" }] } },
        { "id": "pt-run", "label": "PowerToys Run", "enabled": true, "launch": { "candidates": [{ "type": "shortcut", "target": "Alt+Space" }] } }
      ]
    }
  ]
})JSON";
}

namespace {

std::wstring HotkeyKeyName(const UINT virtualKey) {
    if (virtualKey == VK_SPACE)
        return L"Space";
    if (virtualKey == VK_ESCAPE)
        return L"Escape";
    if (virtualKey == VK_RETURN)
        return L"Enter";
    if (virtualKey == VK_TAB)
        return L"Tab";
    if (virtualKey == VK_BACK)
        return L"Backspace";
    if (virtualKey == VK_DELETE)
        return L"Delete";
    if (virtualKey >= L'A' && virtualKey <= L'Z') {
        return std::wstring(1, static_cast<wchar_t>(virtualKey));
    }
    if (virtualKey >= L'0' && virtualKey <= L'9') {
        return std::wstring(1, static_cast<wchar_t>(virtualKey));
    }
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        return L"F" + std::to_wstring(virtualKey - VK_F1 + 1);
    }
    return L"Space";
}

std::wstring HotkeyText(const ActivationConfig& activation) {
    std::wstring text;
    const auto append = [&](const wchar_t* token) {
        if (!text.empty())
            text += L"+";
        text += token;
    };
    if ((activation.modifiers & MOD_CONTROL) != 0)
        append(L"Ctrl");
    if ((activation.modifiers & MOD_ALT) != 0)
        append(L"Alt");
    if ((activation.modifiers & MOD_SHIFT) != 0)
        append(L"Shift");
    if ((activation.modifiers & MOD_WIN) != 0)
        append(L"Win");
    append(HotkeyKeyName(activation.virtualKey).c_str());
    return text;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                              result.data(), size, nullptr, nullptr);
    return result;
}

const json* FindItemJson(const json& items, const std::wstring& id) {
    if (!items.is_array())
        return nullptr;
    const std::string utf8Id = WideToUtf8(id);
    for (const json& item : items) {
        if (item.is_object() && item.contains("id") && item["id"].is_string() &&
            item["id"].get<std::string>() == utf8Id) {
            return &item;
        }
    }
    return nullptr;
}

json ItemJson(const WheelItem& item, const json* const existing = nullptr) {
    json value = existing != nullptr && existing->is_object() ? *existing : json::object();
    value["id"] = WideToUtf8(item.id);
    value["label"] = WideToUtf8(item.label);
    value["enabled"] = item.enabled;
    if (item.quickKey != L'\0') {
        value["quickKey"] = WideToUtf8(std::wstring(1, item.quickKey));
    } else {
        value.erase("quickKey");
    }
    if (!value.contains("icon") || !value["icon"].is_object()) {
        value["icon"] = json::object();
    }
    if (!value["icon"].contains("type")) {
        value["icon"]["type"] = "auto";
    }
    value["icon"]["path"] =
        item.iconAutomatic ? std::string{} : WideToUtf8(item.configuredIconSource);
    if (item.IsSubmenu()) {
        const json existingChildren = value.contains("children") && value["children"].is_array()
                                          ? value["children"]
                                          : json::array();
        value.erase("launch");
        value["children"] = json::array();
        for (const WheelItem& child : item.children) {
            value["children"].push_back(ItemJson(child, FindItemJson(existingChildren, child.id)));
        }
        value.erase("builtin");
    } else if (item.IsBuiltIn()) {
        value.erase("launch");
        value.erase("children");
        value["builtin"] = WideToUtf8(BuiltInToolName(item.builtInTool));
    } else {
        value.erase("builtin");
        value.erase("children");
        if (!value.contains("launch") || !value["launch"].is_object()) {
            value["launch"] = json::object();
        }
        const json existingCandidates =
            value["launch"].contains("candidates") && value["launch"]["candidates"].is_array()
                ? value["launch"]["candidates"]
                : json::array();
        std::vector<LaunchCandidate> candidates = item.launchCandidates;
        if (candidates.empty()) {
            LaunchCandidate candidate;
            candidate.type = item.launchType.empty() ? L"shell" : item.launchType;
            candidate.configuredTarget =
                item.configuredTarget.empty() ? item.target : item.configuredTarget;
            candidate.target = item.target;
            candidate.kind = item.launchKind;
            candidates.push_back(std::move(candidate));
        }
        value["launch"]["candidates"] = json::array();
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            json candidate =
                index < existingCandidates.size() && existingCandidates[index].is_object()
                    ? existingCandidates[index]
                    : json::object();
            candidate["type"] =
                WideToUtf8(candidates[index].type.empty() ? L"shell" : candidates[index].type);
            candidate["target"] = WideToUtf8(candidates[index].configuredTarget.empty()
                                                 ? candidates[index].target
                                                 : candidates[index].configuredTarget);
            value["launch"]["candidates"].push_back(std::move(candidate));
        }
        value["existingWindowAction"] =
            WideToUtf8(item.existingWindowAction.empty()
                           ? (item.maximizeOnActivate ? L"maximizeAndFocus" : L"focus")
                           : item.existingWindowAction);
    }
    return value;
}

void UpdateConfigurationJson(json& doc, const Configuration& configuration) {
    if (!doc.is_object())
        doc = json::object();
    doc["schemaVersion"] = 1;
    if (!doc.contains("activation") || !doc["activation"].is_object()) {
        doc["activation"] = json::object();
    }
    doc["activation"]["hotkey"] = WideToUtf8(HotkeyText(configuration.activation));
    doc["activation"]["experimentalLeftShiftOnly"] =
        configuration.activation.experimentalLeftShiftOnly;
    doc["activation"]["leftShiftHoldThresholdMs"] =
        configuration.activation.leftShiftHoldThresholdMs;

    if (!doc.contains("placement") || !doc["placement"].is_object()) {
        doc["placement"] = json::object();
    }
    doc["placement"]["mode"] =
        configuration.placement.mode == PlacementMode::MonitorCentre ? "monitorCenter" : "cursor";

    if (!doc.contains("appearance") || !doc["appearance"].is_object()) {
        doc["appearance"] = json::object();
    }
    doc["appearance"]["theme"] = WideToUtf8(configuration.appearance.theme);
    doc["appearance"]["reducedMotion"] = configuration.appearance.reducedMotion;
    doc["appearance"]["scrimOpacity"] = configuration.appearance.scrimOpacity;
    doc["appearance"]["wheelOpacity"] = configuration.appearance.wheelOpacity;
    doc["appearance"]["wheelDiameterDip"] = configuration.appearance.wheelDiameterDip;
    doc["appearance"]["outerRadiusDip"] = configuration.appearance.outerRadiusDip;
    doc["appearance"]["innerRadiusDip"] = configuration.appearance.innerRadiusDip;
    doc["appearance"]["selectionThresholdDip"] = configuration.appearance.selectionThresholdDip;
    doc["appearance"]["neutralThresholdDip"] = configuration.appearance.neutralThresholdDip;

    if (!doc.contains("interaction") || !doc["interaction"].is_object()) {
        doc["interaction"] = json::object();
    }
    doc["interaction"]["cancelOnRightClick"] = configuration.interaction.cancelOnRightClick;
    doc["interaction"]["cancelOnOuterExit"] = configuration.interaction.cancelOnOuterExit;
    doc["interaction"]["numberQuickKeys"] = configuration.interaction.numberQuickKeys;
    doc["interaction"]["outerExitRadiusDip"] = configuration.interaction.outerExitRadiusDip;
    doc["interaction"]["submenuDwellMs"] = configuration.interaction.submenuDwellMs;

    const json existingItems =
        doc.contains("items") && doc["items"].is_array() ? doc["items"] : json::array();
    doc["items"] = json::array();
    for (const WheelItem& item : configuration.items) {
        doc["items"].push_back(ItemJson(item, FindItemJson(existingItems, item.id)));
    }
}

}  // namespace

std::wstring FormatHotkey(const ActivationConfig& activation) {
    return HotkeyText(activation);
}

std::string SerializeConfiguration(const Configuration& configuration) {
    json doc;
    UpdateConfigurationJson(doc, configuration);
    return doc.dump(2) + "\n";
}

ConfigSerializeResult SerializeConfigurationPreserving(const Configuration& configuration,
                                                       const std::string& existingJson) {
    ConfigSerializeResult result;
    json doc;
    if (!ParseJsonBounded(existingJson, doc, result.error)) {
        return result;
    }
    try {
        if (!doc.is_object()) {
            result.error = L"The existing configuration root is not an object.";
            return result;
        }
        if (!doc.contains("schemaVersion") || !doc["schemaVersion"].is_number_integer() ||
            doc["schemaVersion"].get<json::number_integer_t>() != 1) {
            result.error =
                L"The existing configuration uses an unsupported schema and "
                L"was not overwritten.";
            return result;
        }
        const ConfigParseResult existingValidation = ParseConfiguration(existingJson);
        if (!existingValidation.ok) {
            result.error =
                L"The existing configuration is invalid and was not "
                L"overwritten. Fix it or explicitly reset it first.";
            return result;
        }
        UpdateConfigurationJson(doc, configuration);
        result.json = doc.dump(2) + "\n";
    } catch (const json::exception&) {
        result.error =
            L"The configuration could not be merged safely and was not "
            L"overwritten.";
        return result;
    }

    const ConfigParseResult validation = ParseConfiguration(result.json);
    if (!validation.ok) {
        result.error =
            validation.error.empty() ? L"The merged configuration is invalid." : validation.error;
        result.json.clear();
        return result;
    }
    result.ok = true;
    return result;
}

std::wstring ConfigFolderPath() {
    PWSTR raw = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) {
        folder.assign(raw);
        folder += L"\\OrbitDeck";
    }
    if (raw) {
        CoTaskMemFree(raw);
    }
    return folder;
}

std::wstring ConfigFilePath() {
    const std::wstring folder = ConfigFolderPath();
    return folder.empty() ? std::wstring{} : folder + L"\\config.json";
}

long long ConfigLastWrite() {
    const std::wstring path = ConfigFilePath();
    if (path.empty()) {
        return 0;
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    ULARGE_INTEGER packed;
    packed.LowPart = data.ftLastWriteTime.dwLowDateTime;
    packed.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return static_cast<long long>(packed.QuadPart);
}

namespace {

bool ReadFileUtf8(const std::wstring& path, std::string& out) {
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file)
        return false;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0 || size > static_cast<std::streamoff>(kMaximumConfigBytes)) {
        return false;
    }
    file.seekg(0, std::ios::beg);
    out.assign(static_cast<std::size_t>(size), '\0');
    if (out.empty())
        return true;
    file.read(out.data(), static_cast<std::streamsize>(size));
    return file.good() || static_cast<std::size_t>(file.gcount()) == out.size();
}

bool ReadConfigSnapshot(const std::wstring& path, std::string& bytes, long long& lastWrite,
                        std::uint64_t& contentHash) {
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    FILETIME write{};
    bool ok = ::GetFileSizeEx(file, &size) != FALSE &&
              ::GetFileTime(file, nullptr, nullptr, &write) != FALSE && size.QuadPart >= 0 &&
              size.QuadPart <= static_cast<LONGLONG>(kMaximumConfigBytes);
    if (ok) {
        bytes.assign(static_cast<std::size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        ok = bytes.empty() || (::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                                          &read, nullptr) != FALSE &&
                               static_cast<std::size_t>(read) == bytes.size());
    }
    (void)::CloseHandle(file);
    if (!ok)
        return false;
    ULARGE_INTEGER packed{};
    packed.LowPart = write.dwLowDateTime;
    packed.HighPart = write.dwHighDateTime;
    lastWrite = static_cast<long long>(packed.QuadPart);
    contentHash = HashBytes(bytes);
    return true;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& bytes) {
    if (bytes.size() > MAXDWORD)
        return false;
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = (::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                                 nullptr) != FALSE) &&
                    static_cast<std::size_t>(written) == bytes.size() &&
                    ::FlushFileBuffers(file) != FALSE;
    (void)::CloseHandle(file);
    return ok;
}

bool WriteFileUtf8Atomic(const std::wstring& path, const std::string& bytes) {
    const std::wstring temporary = path + L".new";
    (void)::DeleteFileW(temporary.c_str());
    if (!WriteFileUtf8(temporary, bytes)) {
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

bool WriteExportFileAtomic(const std::wstring& path, const std::string& bytes) {
    if (bytes.size() > MAXDWORD)
        return false;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        const std::wstring temporary =
            path + L".orbitdeck-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
            std::to_wstring(::GetTickCount64()) + L"-" + std::to_wstring(attempt) + L".tmp";
        const HANDLE file = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                          FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (::GetLastError() == ERROR_FILE_EXISTS)
                continue;
            return false;
        }
        DWORD written = 0;
        const bool complete =
            (bytes.empty() || (::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                                           &written, nullptr) != FALSE &&
                               static_cast<std::size_t>(written) == bytes.size())) &&
            ::FlushFileBuffers(file) != FALSE;
        (void)::CloseHandle(file);
        if (!complete || !::MoveFileExW(temporary.c_str(), path.c_str(),
                                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            (void)::DeleteFileW(temporary.c_str());
            return false;
        }
        return true;
    }
    return false;
}

bool RestorePreviousIfMissing(const std::wstring& path) noexcept {
    if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    const std::wstring backup = path + L".previous";
    if (::GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return ::MoveFileExW(backup.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool FlushFilePath(const std::wstring& path) noexcept {
    const HANDLE file =
        ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const bool flushed = ::FlushFileBuffers(file) != FALSE;
    (void)::CloseHandle(file);
    return flushed;
}

bool SameFileIdentity(const std::wstring& leftPath, const std::wstring& rightPath) noexcept {
    const auto information = [](const std::wstring& path, BY_HANDLE_FILE_INFORMATION& info) {
        const HANDLE file = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        const bool ok = ::GetFileInformationByHandle(file, &info) != FALSE;
        (void)::CloseHandle(file);
        return ok;
    };

    BY_HANDLE_FILE_INFORMATION left{};
    BY_HANDLE_FILE_INFORMATION right{};
    return information(leftPath, left) && information(rightPath, right) &&
           left.dwVolumeSerialNumber == right.dwVolumeSerialNumber &&
           left.nFileIndexHigh == right.nFileIndexHigh && left.nFileIndexLow == right.nFileIndexLow;
}

bool RestoreBackupDurably(const std::wstring& path, const std::wstring& backup,
                          const std::uint64_t expectedHash) {
    if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    const std::wstring restore = path + L".restore.tmp";
    (void)::DeleteFileW(restore.c_str());
    if (!::CopyFileW(backup.c_str(), restore.c_str(), FALSE) || !FlushFilePath(restore)) {
        (void)::DeleteFileW(restore.c_str());
        return false;
    }

    const bool published =
        ::MoveFileExW(restore.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!published) {
        (void)::DeleteFileW(restore.c_str());
        return false;
    }

    std::string restored;
    if (!FlushFilePath(path) || !ReadFileUtf8(path, restored) ||
        HashBytes(restored) != expectedHash) {
        return false;
    }
    return ::DeleteFileW(backup.c_str()) != FALSE;
}

bool RetireBackup(const std::wstring& path, const std::wstring& backup) {
    if (::GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    std::string active;
    std::string previous;
    if (!ReadFileUtf8(path, active) || !ReadFileUtf8(backup, previous)) {
        return false;
    }
    if (HashBytes(active) == HashBytes(previous)) {
        return ::DeleteFileW(backup.c_str()) != FALSE;
    }
    const std::wstring archive =
        path + L".conflict." + std::to_wstring(::GetTickCount64()) + L".bak";
    return ::MoveFileExW(backup.c_str(), archive.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool ResolveGenerationConflict(const std::wstring& path, const std::wstring& backup,
                               const std::wstring& generationPin,
                               const std::uint64_t expectedGenerationHash) {
    const std::wstring displaced = path + L".conflict.tmp";
    (void)::DeleteFileW(displaced.c_str());
    if (SameFileIdentity(path, generationPin)) {
        if (!::MoveFileExW(path.c_str(), displaced.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return false;
        }
        if (SameFileIdentity(displaced, generationPin)) {
            std::string displacedBytes;
            if (!ReadFileUtf8(displaced, displacedBytes)) {
                return false;
            }
            if (HashBytes(displacedBytes) != expectedGenerationHash) {
                const bool restored =
                    ::MoveFileExW(displaced.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
                return restored && RetireBackup(path, backup);
            }
            std::string previous;
            const bool restored = ReadFileUtf8(backup, previous) &&
                                  RestoreBackupDurably(path, backup, HashBytes(previous));
            if (restored) {
                (void)::DeleteFileW(displaced.c_str());
            }
            return restored && RetireBackup(path, backup);
        } else if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            const bool restored =
                ::MoveFileExW(displaced.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
            return restored && RetireBackup(path, backup);
        }
        return RetireBackup(path, backup);
    } else if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::string previous;
        return ReadFileUtf8(backup, previous) &&
               RestoreBackupDurably(path, backup, HashBytes(previous));
    }
    return RetireBackup(path, backup);
}

bool PublishDefaultConfiguration(const std::wstring& path) {
    const std::wstring temporary = path + L".seed.tmp";
    (void)::DeleteFileW(temporary.c_str());
    if (!WriteFileUtf8(temporary, DefaultConfigJson())) {
        (void)::DeleteFileW(temporary.c_str());
        return false;
    }
    if (!::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        (void)::DeleteFileW(temporary.c_str());
        return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    return FlushFilePath(path);
}

}  // namespace

ConfigLoadResult LoadConfiguration() {
    ConfigLoadResult result;
    const std::wstring path = ConfigFilePath();
    if (path.empty()) {
        result.error = L"Could not resolve the %LOCALAPPDATA% folder.";
        return result;
    }

    const std::wstring folder = ConfigFolderPath();
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    const ConfigTransactionLock transaction(folder + L"\\config.lock");
    if (!transaction.Acquired()) {
        result.error = L"Timed out waiting for another configuration operation.";
        return result;
    }

    const std::wstring generationPin = path + L".tmp.pin";
    const std::wstring commitMarker = path + L".tmp.committed";
    const std::wstring conflict = path + L".conflict.tmp";
    bool recoveredConflict = false;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (GetFileAttributesW(conflict.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const std::wstring backup = path + L".previous";
            std::string conflictBytes;
            std::string markerBytes;
            std::uint64_t expectedHash = 0;
            const bool markerKnown =
                ReadFileUtf8(commitMarker, markerBytes) && ParseHash(markerBytes, expectedHash);
            const bool haveConflict = ReadFileUtf8(conflict, conflictBytes);
            const bool externalEdit =
                markerKnown && haveConflict && HashBytes(conflictBytes) != expectedHash;
            bool restored = false;
            if (!markerKnown || !haveConflict) {
                const std::wstring archive =
                    path + L".conflict.unknown." + std::to_wstring(::GetTickCount64()) + L".json";
                if (::MoveFileExW(conflict.c_str(), archive.c_str(), MOVEFILE_WRITE_THROUGH)) {
                    if (GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        restored = RestorePreviousIfMissing(path);
                    } else {
                        restored = ::MoveFileExW(archive.c_str(), path.c_str(),
                                                 MOVEFILE_WRITE_THROUGH) != FALSE;
                    }
                }
            } else if (externalEdit) {
                restored = ::MoveFileExW(conflict.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) !=
                               FALSE &&
                           RetireBackup(path, backup);
            } else if (GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES) {
                restored = RestorePreviousIfMissing(path);
            }
            if (!restored) {
                result.error =
                    L"An interrupted configuration conflict could not be "
                    L"restored. Recovery files were preserved.";
                return result;
            }
            recoveredConflict = true;
        }
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (GetFileAttributesW(generationPin.c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(commitMarker.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::string pinned;
            if (!ReadFileUtf8(generationPin, pinned) || !ParseConfiguration(pinned).ok ||
                !CommitMarkerMatches(commitMarker, HashBytes(pinned))) {
                result.error =
                    L"The pending configuration generation is invalid or "
                    L"uncommitted. It and config.json.previous were preserved.";
                return result;
            }
            if (!::CreateHardLinkW(path.c_str(), generationPin.c_str(), nullptr)) {
                result.error =
                    L"The pending configuration generation could not be "
                    L"restored. No fallback was written.";
                return result;
            }
        }
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(generationPin.c_str()) != INVALID_FILE_ATTRIBUTES &&
        (GetFileAttributesW(commitMarker.c_str()) == INVALID_FILE_ATTRIBUTES ||
         !CommitMarkerMatches(commitMarker, [&]() {
             std::string pinned;
             return ReadFileUtf8(generationPin, pinned) ? HashBytes(pinned) : 0;
         }()))) {
        const std::wstring backup = path + L".previous";
        if (GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (!RestorePreviousIfMissing(path)) {
                result.error =
                    L"An uncommitted generation was found and the previous "
                    L"configuration could not be restored.";
                return result;
            }
        } else {
            (void)::DeleteFileW((path + L".tmp").c_str());
            (void)::DeleteFileW(generationPin.c_str());
        }
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const std::wstring backup = path + L".previous";
        if (GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES &&
            !RestorePreviousIfMissing(path)) {
            result.error =
                L"config.json.previous could not be restored. Defaults were "
                L"not written; close applications using the file and retry.";
            return result;
        }
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (!PublishDefaultConfiguration(path)) {
            result.error = L"Could not create the default config.json.";
            return result;
        }
    }
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(generationPin.c_str()) != INVALID_FILE_ATTRIBUTES &&
        SameFileIdentity(path, generationPin)) {
        std::uint64_t markerHash = 0;
        if (!ReadCommitMarker(commitMarker, markerHash)) {
            const std::wstring backup = path + L".previous";
            if (GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES) {
                const std::wstring uncommitted = path + L".uncommitted";
                (void)::DeleteFileW(uncommitted.c_str());
                if (!::MoveFileExW(path.c_str(), uncommitted.c_str(), MOVEFILE_WRITE_THROUGH)) {
                    result.error =
                        L"An uncommitted configuration generation could not "
                        L"be isolated. Recovery files were preserved.";
                    return result;
                }
                if (!SameFileIdentity(uncommitted, generationPin)) {
                    (void)::MoveFileExW(uncommitted.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH);
                    result.error =
                        L"config.json changed during recovery. The external "
                        L"edit was preserved.";
                    return result;
                }
                if (!RestorePreviousIfMissing(path)) {
                    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                        (void)::MoveFileExW(uncommitted.c_str(), path.c_str(),
                                            MOVEFILE_WRITE_THROUGH);
                    }
                    result.error =
                        L"An uncommitted configuration generation could not "
                        L"be rolled back. Recovery files were preserved.";
                    return result;
                }
                const std::wstring archive =
                    path + L".uncommitted." + std::to_wstring(::GetTickCount64()) + L".json";
                (void)::MoveFileExW(uncommitted.c_str(), archive.c_str(), MOVEFILE_WRITE_THROUGH);
                (void)::DeleteFileW((path + L".tmp").c_str());
                (void)::DeleteFileW(generationPin.c_str());
            }
        }
    }

    std::string bytes;
    if (!ReadConfigSnapshot(path, bytes, result.lastWrite, result.contentHash)) {
        LockedConfigFile snapshot;
        if (snapshot.Open(path, false)) {
            (void)snapshot.Hash(result.contentHash);
            result.lastWrite = snapshot.LastWrite();
        }
        result.error = L"Could not read config.json.";
        return result;
    }

    ConfigParseResult parsed = ParseConfiguration(bytes);
    result.ok = parsed.ok;
    result.config = std::move(parsed.config);
    result.error = std::move(parsed.error);
    const std::wstring backup = path + L".previous";
    if (result.ok && GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::string backupBytes;
        if (ReadFileUtf8(backup, backupBytes) && HashBytes(backupBytes) == result.contentHash) {
            if (!::DeleteFileW(backup.c_str())) {
                result.error =
                    L"The configuration loaded, but its stale backup could "
                    L"not be removed. Restart OrbitDeck to retry cleanup.";
            }
        }
    }
    if (result.ok && GetFileAttributesW(generationPin.c_str()) != INVALID_FILE_ATTRIBUTES &&
        SameFileIdentity(path, generationPin) &&
        !CommitMarkerMatches(commitMarker, result.contentHash)) {
        const std::wstring temporary = path + L".tmp";
        const bool cleaned =
            RetireBackup(path, backup) &&
            (GetFileAttributesW(temporary.c_str()) == INVALID_FILE_ATTRIBUTES ||
             ::DeleteFileW(temporary.c_str())) &&
            (GetFileAttributesW(generationPin.c_str()) == INVALID_FILE_ATTRIBUTES ||
             ::DeleteFileW(generationPin.c_str())) &&
            (GetFileAttributesW(commitMarker.c_str()) == INVALID_FILE_ATTRIBUTES ||
             ::DeleteFileW(commitMarker.c_str()));
        if (!cleaned) {
            result.error =
                L"The external configuration loaded, but stale recovery "
                L"artifacts could not be retired.";
        }
    } else if (result.ok && GetFileAttributesW(generationPin.c_str()) != INVALID_FILE_ATTRIBUTES &&
               SameFileIdentity(path, generationPin)) {
        const std::wstring temporary = path + L".tmp";
        LockedConfigFile active;
        std::string activeBytes;
        const bool locked = active.Open(path, true) && active.Read(activeBytes) &&
                            HashBytes(activeBytes) == result.contentHash;
        const bool aliasesRemoved =
            locked &&
            (GetFileAttributesW(temporary.c_str()) == INVALID_FILE_ATTRIBUTES ||
             ::DeleteFileW(temporary.c_str())) &&
            (GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES ||
             ::DeleteFileW(backup.c_str()));
        active.Close();
        const bool markersRemoved =
            aliasesRemoved && SameFileIdentity(path, generationPin) &&
            (GetFileAttributesW(generationPin.c_str()) == INVALID_FILE_ATTRIBUTES ||
             ::DeleteFileW(generationPin.c_str())) &&
            (GetFileAttributesW(commitMarker.c_str()) == INVALID_FILE_ATTRIBUTES ||
             ::DeleteFileW(commitMarker.c_str()));
        if (!markersRemoved) {
            result.error =
                L"The recovered configuration is valid, but its recovery "
                L"files could not be retired. Close applications using the "
                L"files and restart OrbitDeck.";
        }
    } else if (result.ok && recoveredConflict) {
        const auto removeIfPresent = [](const std::wstring& file) {
            return ::GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES ||
                   ::DeleteFileW(file.c_str()) != FALSE;
        };
        if (!removeIfPresent(conflict) || !removeIfPresent(path + L".tmp") ||
            !removeIfPresent(generationPin) || !removeIfPresent(commitMarker)) {
            result.error =
                L"The configuration conflict was recovered, but its recovery "
                L"files could not be retired. Restart OrbitDeck to retry.";
        }
    } else if (result.ok && GetFileAttributesW(generationPin.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const std::wstring temporary = path + L".tmp";
        const bool temporaryRemoved =
            GetFileAttributesW(temporary.c_str()) == INVALID_FILE_ATTRIBUTES ||
            ::DeleteFileW(temporary.c_str()) != FALSE;
        if (!temporaryRemoved || !::DeleteFileW(generationPin.c_str()) ||
            (GetFileAttributesW(commitMarker.c_str()) != INVALID_FILE_ATTRIBUTES &&
             !::DeleteFileW(commitMarker.c_str()))) {
            result.error =
                L"An interrupted, uncommitted configuration generation could "
                L"not be cleaned up.";
        }
    } else if (result.ok && GetFileAttributesW(commitMarker.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!::DeleteFileW(commitMarker.c_str())) {
            result.error =
                L"The configuration is valid, but a stale commit marker "
                L"could not be removed.";
        }
    }
    if (result.ok && GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES &&
        !RetireBackup(path, backup)) {
        result.error =
            L"The configuration loaded, but its orphaned backup could not be "
            L"retired. Restart OrbitDeck to retry cleanup.";
    }
    return result;
}

ConfigSaveResult SaveConfiguration(const Configuration& configuration,
                                   const long long expectedLastWrite,
                                   const std::uint64_t expectedContentHash,
                                   const bool replaceInvalidExisting,
                                   const std::string* const replacementJson) {
    ConfigSaveResult result;
    const std::wstring path = ConfigFilePath();
    const std::wstring folder = ConfigFolderPath();
    if (path.empty() || folder.empty()) {
        result.error = L"Could not resolve the %LOCALAPPDATA% folder.";
        return result;
    }
    const int createFolder = ::SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    if (createFolder != ERROR_SUCCESS && createFolder != ERROR_ALREADY_EXISTS &&
        createFolder != ERROR_FILE_EXISTS) {
        result.error = L"Could not create the configuration folder.";
        return result;
    }
    const ConfigTransactionLock transaction(folder + L"\\config.lock");
    if (!transaction.Acquired()) {
        result.error = L"Timed out waiting for another configuration operation.";
        return result;
    }
    if (::GetFileAttributesW((path + L".previous").c_str()) != INVALID_FILE_ATTRIBUTES ||
        ::GetFileAttributesW((path + L".tmp.pin").c_str()) != INVALID_FILE_ATTRIBUTES ||
        ::GetFileAttributesW((path + L".tmp.committed").c_str()) != INVALID_FILE_ATTRIBUTES) {
        result.error =
            L"Unresolved configuration recovery files exist. Restart "
            L"Weapon Cycle or resolve config.json.previous/config.json.tmp.pin "
            L"before saving.";
        return result;
    }
    if (ConfigLastWrite() != expectedLastWrite) {
        result.error =
            L"config.json changed before it could be saved. Reopen "
            L"Preferences and try again.";
        return result;
    }
    const bool fileExists = ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    LockedConfigFile lockedFile;
    std::string existing;
    if (fileExists) {
        if (!lockedFile.Open(path, true)) {
            result.error =
                L"config.json is being edited by another application. "
                L"Close it there and try again.";
            return result;
        }
        if (lockedFile.LastWrite() != expectedLastWrite) {
            result.error = L"config.json changed before it could be locked for saving.";
            return result;
        }
        if (!replaceInvalidExisting) {
            if (!lockedFile.Read(existing)) {
                result.error = L"Could not read config.json for saving.";
                return result;
            }
            if (HashBytes(existing) != expectedContentHash) {
                result.error =
                    L"config.json content changed before it could be saved. "
                    L"Reopen Preferences and try again.";
                return result;
            }
        } else if (expectedContentHash != 0) {
            std::uint64_t currentHash = 0;
            if (!lockedFile.Hash(currentHash) || currentHash != expectedContentHash) {
                result.error =
                    L"config.json content changed before reset. Reopen "
                    L"Preferences and try again.";
                return result;
            }
        } else {
            result.error =
                L"No verified config.json baseline is available for reset. "
                L"Restart OrbitDeck and try again.";
            return result;
        }
    } else if (expectedContentHash != 0) {
        result.error =
            L"config.json was deleted before it could be saved. Reopen "
            L"Preferences and try again.";
        return result;
    }
    ConfigSerializeResult serialized;
    if (replacementJson != nullptr) {
        serialized.ok = true;
        serialized.json = *replacementJson;
    } else if (fileExists) {
        if (replaceInvalidExisting) {
            serialized.ok = true;
            serialized.json = SerializeConfiguration(configuration);
        } else {
            serialized = SerializeConfigurationPreserving(configuration, existing);
        }
    } else {
        serialized.ok = true;
        serialized.json = SerializeConfiguration(configuration);
    }
    if (!serialized.ok) {
        result.error = serialized.error;
        return result;
    }
    if (serialized.json.size() > kMaximumConfigBytes) {
        result.error = L"The configuration is larger than the supported 4 MiB limit.";
        return result;
    }
    const ConfigParseResult serializedValidation = ParseConfiguration(serialized.json);
    if (!serializedValidation.ok) {
        result.error = serializedValidation.error.empty()
                           ? L"The serialized configuration is invalid."
                           : serializedValidation.error;
        return result;
    }

    const std::wstring temporary = path + L".tmp";
    const std::wstring commitMarker = path + L".tmp.committed";
    if (!WriteFileUtf8(temporary, serialized.json)) {
        (void)::DeleteFileW(temporary.c_str());
        result.error = L"Could not write the temporary configuration file.";
        return result;
    }
    const std::wstring generationPin = temporary + L".pin";
    if (GetFileAttributesW(generationPin.c_str()) != INVALID_FILE_ATTRIBUTES &&
        !::DeleteFileW(generationPin.c_str())) {
        (void)::DeleteFileW(temporary.c_str());
        result.error = L"A stale configuration recovery marker could not be removed.";
        return result;
    }
    if (!::CreateHardLinkW(generationPin.c_str(), temporary.c_str(), nullptr)) {
        (void)::DeleteFileW(temporary.c_str());
        result.error = L"Could not pin the new configuration generation for saving.";
        return result;
    }
    if (fileExists) {
        if (!lockedFile.StillNames(path) || lockedFile.LastWrite() != expectedLastWrite) {
            (void)::DeleteFileW(generationPin.c_str());
            (void)::DeleteFileW(temporary.c_str());
            result.error =
                L"config.json was replaced while it was being saved. The "
                L"newer file was not overwritten.";
            return result;
        }
    } else if (ConfigLastWrite() != 0) {
        (void)::DeleteFileW(generationPin.c_str());
        (void)::DeleteFileW(temporary.c_str());
        result.error =
            L"config.json was created by another application while it was "
            L"being saved. The newer file was not overwritten.";
        return result;
    }
    if (fileExists) {
        const std::wstring backup = path + L".previous";
        (void)::DeleteFileW(backup.c_str());
        if (!::ReplaceFileW(path.c_str(), temporary.c_str(), backup.c_str(), 0, nullptr, nullptr)) {
            const DWORD replaceError = ::GetLastError();
            if (replaceError == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
                lockedFile.Close();
                if (!RestoreBackupDurably(path, backup, expectedContentHash)) {
                    (void)::DeleteFileW(generationPin.c_str());
                    result.error =
                        L"Saving was interrupted. The previous configuration "
                        L"is preserved as config.json.previous and the new "
                        L"version as config.json.tmp.";
                    return result;
                }
            }
            (void)::DeleteFileW(generationPin.c_str());
            (void)::DeleteFileW(temporary.c_str());
            if (replaceError == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
                result.error =
                    L"Could not atomically replace config.json; the previous "
                    L"configuration was restored.";
            } else {
                result.error = L"Could not atomically replace config.json.";
            }
            return result;
        }
        if (!lockedFile.StillNames(backup) || !SameFileIdentity(path, generationPin)) {
            lockedFile.Close();
            const bool resolved =
                ResolveGenerationConflict(path, backup, generationPin, HashBytes(serialized.json));
            if (resolved)
                (void)::DeleteFileW(generationPin.c_str());
            result.error = resolved ? L"config.json changed during save. The external edit "
                                      L"was kept; reopen Preferences and try again."
                                    : L"config.json changed during save and automatic "
                                      L"recovery did not complete. Recovery files were kept.";
            return result;
        }
        if (!WriteFileUtf8Atomic(commitMarker, std::to_string(HashBytes(serialized.json)) + "\n")) {
            lockedFile.Close();
            const bool resolved =
                ResolveGenerationConflict(path, backup, generationPin, HashBytes(serialized.json));
            if (resolved) {
                if (::DeleteFileW(generationPin.c_str())) {
                    (void)::DeleteFileW(commitMarker.c_str());
                }
            }
            result.error =
                L"The configuration replacement could not be committed "
                L"durably; recovery files were preserved.";
            return result;
        }
        lockedFile.Close();

        LockedConfigFile installed;
        std::string installedBytes;
        if (!installed.Open(path, false) || !installed.Read(installedBytes) ||
            HashBytes(installedBytes) != HashBytes(serialized.json) || !installed.Flush()) {
            installed.Close();
            const bool resolved =
                ResolveGenerationConflict(path, backup, generationPin, HashBytes(serialized.json));
            if (resolved) {
                (void)::DeleteFileW(generationPin.c_str());
                (void)::DeleteFileW(commitMarker.c_str());
            }
            result.error = resolved ? L"The installed configuration could not be verified; "
                                      L"the previous generation was restored."
                                    : L"The installed configuration could not be verified. "
                                      L"Recovery files were kept.";
            return result;
        }
        if (!::DeleteFileW(backup.c_str())) {
            result.warning =
                L"The new configuration was installed, but its previous "
                L"backup could not be retired. Restart OrbitDeck to retry "
                L"cleanup.";
            result.lastWrite = installed.LastWrite();
            result.contentHash = HashBytes(installedBytes);
            installed.Close();
            result.ok = true;
            return result;
        }
        result.lastWrite = installed.LastWrite();
        result.contentHash = HashBytes(installedBytes);
        installed.Close();
        const bool pinRemoved = ::DeleteFileW(generationPin.c_str()) != FALSE;
        if (!pinRemoved) {
            result.warning =
                L"The configuration was saved, but its recovery marker could "
                L"not be removed. Restart OrbitDeck to finish cleanup.";
        } else if (!::DeleteFileW(commitMarker.c_str())) {
            result.warning =
                L"The configuration was saved, but its commit marker could "
                L"not be removed. Restart OrbitDeck to finish cleanup.";
        }
    } else if (!WriteFileUtf8Atomic(commitMarker,
                                    std::to_string(HashBytes(serialized.json)) + "\n")) {
        (void)::DeleteFileW(generationPin.c_str());
        (void)::DeleteFileW(temporary.c_str());
        result.error = L"Could not durably commit the first configuration generation.";
        return result;
    } else if (!::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        if (::DeleteFileW(generationPin.c_str())) {
            (void)::DeleteFileW(commitMarker.c_str());
        }
        (void)::DeleteFileW(temporary.c_str());
        result.error = L"Could not create config.json.";
        return result;
    } else {
        if (!SameFileIdentity(path, generationPin)) {
            if (::DeleteFileW(generationPin.c_str())) {
                (void)::DeleteFileW(commitMarker.c_str());
            }
            result.error =
                L"config.json was created concurrently. The external file "
                L"was kept; reopen Preferences and try again.";
            return result;
        }
        LockedConfigFile installed;
        std::string installedBytes;
        if (!installed.Open(path, false) || !installed.Read(installedBytes) ||
            HashBytes(installedBytes) != HashBytes(serialized.json) || !installed.Flush()) {
            installed.Close();
            if (::DeleteFileW(generationPin.c_str())) {
                (void)::DeleteFileW(commitMarker.c_str());
            }
            result.error =
                L"config.json was created but could not be verified. It was "
                L"left in place to avoid deleting another writer's file.";
            return result;
        }
        result.lastWrite = installed.LastWrite();
        result.contentHash = HashBytes(installedBytes);
        installed.Close();
        if (!::DeleteFileW(generationPin.c_str())) {
            result.warning =
                L"The configuration was saved, but its recovery marker could "
                L"not be removed. Restart OrbitDeck to finish cleanup.";
        } else if (!::DeleteFileW(commitMarker.c_str())) {
            result.warning =
                L"The configuration was saved, but its commit marker could "
                L"not be removed. Restart OrbitDeck to finish cleanup.";
        }
    }

    result.ok = true;
    return result;
}

ConfigExportResult ExportConfigurationFile(const Configuration& configuration,
                                           const std::wstring& path) {
    ConfigExportResult result;
    const std::wstring activePath = ConfigFilePath();
    const auto absolutePath = [](const std::wstring& value) {
        const DWORD needed = ::GetFullPathNameW(value.c_str(), 0, nullptr, nullptr);
        if (needed == 0)
            return value;
        std::wstring absolute(needed, L'\0');
        const DWORD length = ::GetFullPathNameW(value.c_str(), needed, absolute.data(), nullptr);
        if (length == 0 || length >= needed)
            return value;
        absolute.resize(length);
        return absolute;
    };
    const auto startsWithInsensitive = [](const std::wstring& value, const std::wstring& prefix) {
        return value.size() >= prefix.size() &&
               ::CompareStringOrdinal(value.data(), static_cast<int>(prefix.size()), prefix.data(),
                                      static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
    };
    const auto finalDirectoryPath = [&](const std::wstring& filePath) {
        const std::wstring absolute = absolutePath(filePath);
        const std::size_t separator = absolute.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
            return std::wstring{};
        const std::wstring directory = absolute.substr(0, separator);
        const HANDLE handle =
            ::CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return std::wstring{};
        constexpr DWORD flags{FILE_NAME_NORMALIZED | VOLUME_NAME_GUID};
        const DWORD needed = ::GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
        std::wstring finalPath(needed == 0 ? 0 : static_cast<std::size_t>(needed), L'\0');
        const DWORD length =
            needed == 0 ? 0 : ::GetFinalPathNameByHandleW(handle, finalPath.data(), needed, flags);
        (void)::CloseHandle(handle);
        if (length == 0 || length >= needed)
            return std::wstring{};
        finalPath.resize(length);
        return finalPath;
    };
    const std::wstring destination = absolutePath(path);
    const std::wstring active = absolutePath(activePath);
    const std::wstring configFolder = ConfigFolderPath();
    const std::wstring lock = absolutePath(configFolder + L"\\config.lock");
    const bool activeArtifact =
        !active.empty() &&
        (destination.size() == active.size()
             ? startsWithInsensitive(destination, active)
             : destination.size() > active.size() && destination[active.size()] == L'.' &&
                   startsWithInsensitive(destination, active));
    const bool lockArtifact = !configFolder.empty() && startsWithInsensitive(destination, lock);
    const bool activeAlias = ::GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES &&
                             ::GetFileAttributesW(active.c_str()) != INVALID_FILE_ATTRIBUTES &&
                             SameFileIdentity(destination, active);
    const std::wstring destinationDirectory = finalDirectoryPath(destination);
    const std::wstring configDirectory = finalDirectoryPath(active);
    const bool insideConfigDirectory =
        !destinationDirectory.empty() && !configDirectory.empty() &&
        ::CompareStringOrdinal(destinationDirectory.c_str(), -1, configDirectory.c_str(), -1,
                               TRUE) == CSTR_EQUAL;
    const std::array<std::wstring, 8> reservedPaths{
        active,
        active.empty() ? std::wstring{} : active + L".previous",
        active.empty() ? std::wstring{} : active + L".tmp",
        active.empty() ? std::wstring{} : active + L".tmp.pin",
        active.empty() ? std::wstring{} : active + L".tmp.committed",
        active.empty() ? std::wstring{} : active + L".conflict.tmp",
        active.empty() ? std::wstring{} : active + L".seed.tmp",
        configFolder.empty() ? std::wstring{} : lock};
    const bool reservedAlias =
        std::any_of(reservedPaths.begin(), reservedPaths.end(), [&](const std::wstring& reserved) {
            return !reserved.empty() &&
                   ::GetFileAttributesW(reserved.c_str()) != INVALID_FILE_ATTRIBUTES &&
                   ::GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES &&
                   SameFileIdentity(destination, reserved);
        });
    if (path.empty() || activeArtifact || lockArtifact || activeAlias || insideConfigDirectory ||
        reservedAlias) {
        result.error =
            L"Choose a destination outside OrbitDeck's active configuration "
            L"and recovery files.";
        return result;
    }

    std::string serialized;
    std::string existing;
    const std::wstring configPath = ConfigFilePath();
    if (ReadFileUtf8(configPath, existing)) {
        const ConfigSerializeResult preserving =
            SerializeConfigurationPreserving(configuration, existing);
        serialized = preserving.ok ? preserving.json : SerializeConfiguration(configuration);
    } else {
        serialized = SerializeConfiguration(configuration);
    }
    const ConfigParseResult validation = ParseConfiguration(serialized);
    if (!validation.ok) {
        result.error =
            validation.error.empty() ? L"The exported configuration is invalid." : validation.error;
        return result;
    }
    if (!WriteExportFileAtomic(path, serialized)) {
        result.error = L"The selected export file could not be written.";
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace orbitdeck
