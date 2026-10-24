#include "tools/DeveloperTools.h"

#include <windows.h>

#include <bcrypt.h>
#include <objbase.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <new>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace orbitdeck {
namespace {

using json = nlohmann::json;
constexpr std::size_t kMaximumTextCharacters{4 * 1024 * 1024};
constexpr std::size_t kMaximumBinaryBytes{3 * 1024 * 1024};
constexpr std::size_t kMaximumJsonBytes{512 * 1024};
constexpr int kMaximumJsonDepth{64};
constexpr std::size_t kMaximumJsonEvents{50000};
constexpr std::size_t kMaximumDiffLines{1200};
constexpr std::size_t kMaximumDiffCharacters{256 * 1024};
constexpr std::size_t kMaximumDiffLineCharacters{4096};
constexpr std::size_t kMaximumDiffComparisonWork{20 * 1024 * 1024};

class JsonComplexityError final {};

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

std::string ToUtf8(std::wstring_view value) {
    if (value.empty())
        return {};
    const int size =
        ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                              static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                              static_cast<int>(value.size()), result.data(), size, nullptr,
                              nullptr) != size) {
        return {};
    }
    return result;
}

std::wstring FromUtf8(std::string_view value) {
    if (value.empty())
        return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                              static_cast<int>(value.size()), result.data(), size) != size) {
        return {};
    }
    return result;
}

bool ParseJsonBounded(const std::string_view input, json& document, std::wstring& error) {
    if (input.size() > kMaximumJsonBytes) {
        error = L"JSON input exceeds the 512 KiB interactive limit.";
        return false;
    }
    std::size_t events = 0;
    try {
        document = json::parse(input, [&](const int depth, const json::parse_event_t, json&) {
            if (depth > kMaximumJsonDepth || ++events > kMaximumJsonEvents) {
                throw JsonComplexityError{};
            }
            return true;
        });
        return true;
    } catch (const JsonComplexityError&) {
        error = L"JSON is too deeply nested or contains too many values.";
    } catch (const json::exception&) {
        error = L"Input is not valid JSON.";
    }
    return false;
}

bool RenderJsonBounded(const json& document, const int indent, std::wstring& output,
                       std::wstring& error) {
    const std::string rendered = document.dump(indent);
    if (rendered.size() > kMaximumTextCharacters) {
        error = L"Formatted JSON exceeds the 4 MiB output limit.";
        return false;
    }
    output = FromUtf8(rendered);
    if (output.empty() && !rendered.empty()) {
        error = L"Formatted JSON is not valid UTF-8.";
        return false;
    }
    return true;
}

std::string Base64Encode(const std::vector<unsigned char>& bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const unsigned value =
            static_cast<unsigned>(bytes[index]) << 16 |
            (index + 1 < bytes.size() ? static_cast<unsigned>(bytes[index + 1]) << 8 : 0U) |
            (index + 2 < bytes.size() ? static_cast<unsigned>(bytes[index + 2]) : 0U);
        output.push_back(alphabet[(value >> 18) & 63U]);
        output.push_back(alphabet[(value >> 12) & 63U]);
        output.push_back(index + 1 < bytes.size() ? alphabet[(value >> 6) & 63U] : '=');
        output.push_back(index + 2 < bytes.size() ? alphabet[value & 63U] : '=');
    }
    return output;
}

enum class Base64Variant {
    Standard,
    Url,
};

bool Base64Decode(const std::string_view text, std::vector<unsigned char>& output,
                  const Base64Variant variant) {
    std::array<int, 256> values{};
    values.fill(-1);
    const std::string alphabet =
        variant == Base64Variant::Standard
            ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
            : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (std::size_t index = 0; index < alphabet.size(); ++index) {
        values[static_cast<unsigned char>(alphabet[index])] = static_cast<int>(index);
    }
    std::string normalized;
    normalized.reserve(text.size() + 3);
    for (char character : text) {
        if (std::isspace(static_cast<unsigned char>(character)))
            return false;
        if (variant == Base64Variant::Url && character == '=')
            return false;
        normalized.push_back(character);
    }
    if (normalized.size() % 4 == 1) {
        return false;
    }
    if (variant == Base64Variant::Standard) {
        if (normalized.size() % 4 != 0)
            return false;
    } else {
        while (normalized.size() % 4 != 0)
            normalized.push_back('=');
    }

    output.clear();
    output.reserve(normalized.size() / 4 * 3);
    for (std::size_t index = 0; index < normalized.size(); index += 4) {
        unsigned value = 0;
        int padding = 0;
        std::array<int, 4> sextets{};
        if (normalized[index] == '=' || normalized[index + 1] == '=' ||
            (normalized[index + 2] == '=' && normalized[index + 3] != '=') ||
            ((normalized[index + 2] == '=' || normalized[index + 3] == '=') &&
             index + 4 != normalized.size())) {
            return false;
        }
        for (int offset = 0; offset < 4; ++offset) {
            const unsigned char character =
                static_cast<unsigned char>(normalized[index + static_cast<std::size_t>(offset)]);
            if (character == '=') {
                ++padding;
                sextets[static_cast<std::size_t>(offset)] = 0;
                value <<= 6;
            } else {
                const int decoded = values[character];
                if (decoded < 0 || padding != 0)
                    return false;
                sextets[static_cast<std::size_t>(offset)] = decoded;
                value = (value << 6) | static_cast<unsigned>(decoded);
            }
        }
        if ((padding == 2 && (sextets[1] & 0x0F) != 0) ||
            (padding == 1 && (sextets[2] & 0x03) != 0)) {
            return false;
        }
        output.push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
        if (padding < 2) {
            output.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
        }
        if (padding < 1) {
            output.push_back(static_cast<unsigned char>(value & 0xFF));
        }
    }
    return true;
}

bool ReadBinaryFile(const std::wstring& path, std::vector<unsigned char>& bytes) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        return false;
    const std::streamoff size = stream.tellg();
    if (size < 0 || static_cast<unsigned long long>(size) > kMaximumBinaryBytes) {
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    stream.seekg(0);
    return bytes.empty() ||
           static_cast<bool>(stream.read(reinterpret_cast<char*>(bytes.data()),
                                         static_cast<std::streamsize>(bytes.size())));
}

bool WriteBinaryFileAtomic(const std::wstring& path, const std::vector<unsigned char>& bytes) {
    if (path.empty())
        return false;
    GUID identifier{};
    wchar_t identifierText[40]{};
    if (FAILED(::CoCreateGuid(&identifier)) ||
        ::StringFromGUID2(identifier, identifierText,
                          static_cast<int>(std::size(identifierText))) <= 0) {
        return false;
    }
    const std::wstring temporary = path + L"." + identifierText + L".tmp";
    const std::wstring backup = path + L"." + identifierText + L".backup";
    const HANDLE file = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool stored =
        (bytes.empty() || (::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                                       &written, nullptr) != FALSE &&
                           written == bytes.size())) &&
        ::FlushFileBuffers(file) != FALSE;
    (void)::CloseHandle(file);
    if (!stored) {
        (void)::DeleteFileW(temporary.c_str());
        return false;
    }
    const bool exists = ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!exists) {
        return ::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (::ReplaceFileW(path.c_str(), temporary.c_str(), backup.c_str(), REPLACEFILE_WRITE_THROUGH,
                       nullptr, nullptr)) {
        (void)::DeleteFileW(backup.c_str());
        return true;
    }

    // ReplaceFile can rename the original before failing. Never delete either
    // recovery artifact on failure; restore the backup if the destination
    // disappeared, or publish the complete temporary file when no backup exists.
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (::GetFileAttributesW(backup.c_str()) != INVALID_FILE_ATTRIBUTES) {
            (void)::MoveFileExW(backup.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH);
        } else if (::GetFileAttributesW(temporary.c_str()) != INVALID_FILE_ATTRIBUTES &&
                   ::MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
    }
    return false;
}

std::wstring MimeForPath(const std::wstring& path) {
    const std::size_t dot = path.find_last_of(L'.');
    const std::wstring extension = dot == std::wstring::npos ? L"" : Lower(path.substr(dot));
    if (extension == L".png")
        return L"image/png";
    if (extension == L".jpg" || extension == L".jpeg")
        return L"image/jpeg";
    if (extension == L".gif")
        return L"image/gif";
    if (extension == L".webp")
        return L"image/webp";
    if (extension == L".svg")
        return L"image/svg+xml";
    return L"application/octet-stream";
}

bool HasImageSignature(const std::wstring_view mime, const std::vector<unsigned char>& bytes) {
    if (mime == L"image/png") {
        static constexpr unsigned char signature[]{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        return bytes.size() >= std::size(signature) &&
               std::equal(std::begin(signature), std::end(signature), bytes.begin());
    }
    if (mime == L"image/jpeg") {
        return bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF;
    }
    if (mime == L"image/gif") {
        return bytes.size() >= 6 && (std::equal(bytes.begin(), bytes.begin() + 6, "GIF87a") ||
                                     std::equal(bytes.begin(), bytes.begin() + 6, "GIF89a"));
    }
    if (mime == L"image/webp") {
        return bytes.size() >= 12 && std::equal(bytes.begin(), bytes.begin() + 4, "RIFF") &&
               std::equal(bytes.begin() + 8, bytes.begin() + 12, "WEBP");
    }
    if (mime == L"image/svg+xml") {
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        return first != std::string::npos && (text.compare(first, 4, "<svg") == 0 ||
                                              (text.compare(first, 5, "<?xml") == 0 &&
                                               text.find("<svg", first + 5) != std::string::npos));
    }
    return false;
}

bool ParseUnsigned(std::string_view value, const int base, unsigned long long& output) {
    const auto result = std::from_chars(value.data(), value.data() + value.size(), output, base);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}

bool LinesBounded(const std::wstring_view text, std::vector<std::wstring>& lines,
                  std::wstring& error) {
    if (text.size() > kMaximumDiffCharacters) {
        error = L"Text diff supports up to 256 KiB per side.";
        return false;
    }
    lines.clear();
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find(L'\n', start);
        const std::size_t length =
            end == std::wstring_view::npos ? text.size() - start : end - start;
        if (length > kMaximumDiffLineCharacters) {
            error = L"Text diff supports lines up to 4,096 characters.";
            return false;
        }
        if (lines.size() >= kMaximumDiffLines) {
            error = L"Text diff supports up to 1,200 lines per side.";
            return false;
        }
        std::wstring line(text.substr(start, length));
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();
        lines.push_back(std::move(line));
        if (end == std::wstring_view::npos)
            break;
        start = end + 1;
    }
    return true;
}

bool SafeRegexPattern(std::wstring_view pattern) noexcept {
    if (pattern.empty() || pattern.size() > 256)
        return false;
    bool escaped = false;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        const wchar_t character = pattern[index];
        if (escaped) {
            if (character >= L'1' && character <= L'9')
                return false;
            escaped = false;
            continue;
        }
        if (character == L'\\') {
            escaped = true;
            continue;
        }
        if (character == L'{' || character == L'}' || character == L'(' || character == L')') {
            return false;
        }
        if (character == L'*' || character == L'+' || character == L'?') {
            return false;
        }
    }
    return !escaped;
}

std::wstring DiffText(std::wstring_view leftText, std::wstring_view rightText,
                      std::wstring& error) {
    std::vector<std::wstring> left;
    std::vector<std::wstring> right;
    if (!LinesBounded(leftText, left, error) || !LinesBounded(rightText, right, error)) {
        return {};
    }
    const std::size_t comparisonWork =
        leftText.size() * right.size() + rightText.size() * left.size();
    if (comparisonWork > kMaximumDiffComparisonWork) {
        error = L"Text diff is too large for the bounded interactive comparison.";
        return {};
    }
    const std::size_t columns = right.size() + 1;
    std::vector<unsigned short> lengths((left.size() + 1) * columns, 0);
    for (std::size_t i = left.size(); i-- > 0;) {
        for (std::size_t j = right.size(); j-- > 0;) {
            lengths[i * columns + j] =
                left[i] == right[j]
                    ? static_cast<unsigned short>(lengths[(i + 1) * columns + j + 1] + 1)
                    : std::max(lengths[(i + 1) * columns + j], lengths[i * columns + j + 1]);
        }
    }
    std::wstring output;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < left.size() || j < right.size()) {
        if (i < left.size() && j < right.size() && left[i] == right[j]) {
            output += L"  " + left[i] + L"\r\n";
            ++i;
            ++j;
        } else if (j < right.size() && (i == left.size() || lengths[i * columns + j + 1] >=
                                                                lengths[(i + 1) * columns + j])) {
            output += L"+ " + right[j++] + L"\r\n";
        } else {
            output += L"- " + left[i++] + L"\r\n";
        }
    }
    return output;
}

std::wstring Hash(std::wstring_view input, std::wstring_view mode, std::wstring& error) {
    LPCWSTR algorithm = BCRYPT_SHA256_ALGORITHM;
    if (mode == L"SHA-1")
        algorithm = BCRYPT_SHA1_ALGORITHM;
    else if (mode == L"MD5")
        algorithm = BCRYPT_MD5_ALGORITHM;
    BCRYPT_ALG_HANDLE provider = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD hashBytes = 0;
    DWORD copied = 0;
    const std::string utf8 = ToUtf8(input);
    if (BCryptOpenAlgorithmProvider(&provider, algorithm, nullptr, 0) < 0 ||
        BCryptGetProperty(provider, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashBytes),
                          sizeof(hashBytes), &copied, 0) < 0 ||
        BCryptCreateHash(provider, &hash, nullptr, 0, nullptr, 0, 0) < 0 ||
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(utf8.data())),
                       static_cast<ULONG>(utf8.size()), 0) < 0) {
        error = L"Windows could not initialize the hash operation.";
    }
    std::vector<unsigned char> digest(hashBytes);
    if (error.empty() &&
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        error = L"Windows could not finish the hash operation.";
    }
    if (hash != nullptr)
        BCryptDestroyHash(hash);
    if (provider != nullptr)
        BCryptCloseAlgorithmProvider(provider, 0);
    if (!error.empty())
        return {};
    std::wostringstream output;
    output << std::hex << std::setfill(L'0');
    for (const unsigned char byte : digest) {
        output << std::setw(2) << static_cast<unsigned>(byte);
    }
    return output.str();
}

}  // namespace

bool IsDeveloperTool(const BuiltInTool tool) noexcept {
    return tool >= BuiltInTool::ColorPicker && tool <= BuiltInTool::TextDiff;
}

std::wstring DeveloperToolName(const BuiltInTool tool) {
    switch (tool) {
        case BuiltInTool::ColorPicker:
            return L"Color Picker";
        case BuiltInTool::JsonEditor:
            return L"JSON Editor";
        case BuiltInTool::Base64Text:
            return L"Base64 Encoder / Decoder";
        case BuiltInTool::GuidGenerator:
            return L"GUID Generator";
        case BuiltInTool::JwtDecoder:
            return L"JWT Decoder";
        case BuiltInTool::UrlCodec:
            return L"URL Encoder / Decoder";
        case BuiltInTool::TimestampConverter:
            return L"Timestamp Converter";
        case BuiltInTool::TextTransform:
            return L"Text Transformation";
        case BuiltInTool::Base64Image:
            return L"Base64 Image Converter";
        case BuiltInTool::NumberBase:
            return L"Number Base Converter";
        case BuiltInTool::ColorConverter:
            return L"Color Converter";
        case BuiltInTool::HashCalculator:
            return L"Hash Calculator";
        case BuiltInTool::RegexTester:
            return L"Regex Tester";
        case BuiltInTool::TextDiff:
            return L"Text Diff";
        default:
            return L"Developer Tool";
    }
}

std::vector<std::wstring> DeveloperToolModes(const BuiltInTool tool) {
    switch (tool) {
        case BuiltInTool::JsonEditor:
            return {L"Format", L"Minify", L"Validate"};
        case BuiltInTool::Base64Text:
            return {L"Encode", L"Decode"};
        case BuiltInTool::UrlCodec:
            return {L"Encode", L"Decode"};
        case BuiltInTool::TimestampConverter:
            return {L"Unix seconds", L"Unix milliseconds"};
        case BuiltInTool::TextTransform:
            return {L"UPPERCASE", L"lowercase", L"Title Case", L"snake_case", L"kebab-case"};
        case BuiltInTool::Base64Image:
            return {L"Encode file", L"Decode data URL"};
        case BuiltInTool::NumberBase:
            return {L"To binary", L"To octal", L"To decimal", L"To hex"};
        case BuiltInTool::HashCalculator:
            return {L"SHA-256", L"SHA-1", L"MD5"};
        default:
            return {L"Run"};
    }
}

bool RunDeveloperTool(const BuiltInTool tool, const std::wstring_view input,
                      const std::wstring_view secondaryInput, const std::wstring_view mode,
                      std::wstring& output, std::wstring& error) noexcept {
    output.clear();
    error.clear();
    if (input.size() > kMaximumTextCharacters || secondaryInput.size() > kMaximumTextCharacters) {
        error = L"Input exceeds the 4 MiB text limit.";
        return false;
    }
    try {
        if (tool == BuiltInTool::JsonEditor) {
            json document;
            if (!ParseJsonBounded(ToUtf8(input), document, error)) {
                return false;
            }
            if (mode == L"Validate") {
                output = L"Valid JSON";
            } else if (!RenderJsonBounded(document, mode == L"Minify" ? -1 : 2, output, error)) {
                return false;
            }
        } else if (tool == BuiltInTool::Base64Text) {
            if (mode == L"Decode") {
                std::vector<unsigned char> decoded;
                if (!Base64Decode(ToUtf8(input), decoded, Base64Variant::Standard)) {
                    error = L"Input is not valid Base64.";
                    return false;
                }
                output = FromUtf8(
                    std::string(reinterpret_cast<const char*>(decoded.data()), decoded.size()));
                if (output.empty() && !decoded.empty()) {
                    error = L"Decoded bytes are not valid UTF-8 text.";
                    return false;
                }
            } else {
                const std::string utf8 = ToUtf8(input);
                output =
                    FromUtf8(Base64Encode(std::vector<unsigned char>(utf8.begin(), utf8.end())));
            }
        } else if (tool == BuiltInTool::GuidGenerator) {
            GUID guid{};
            if (FAILED(::CoCreateGuid(&guid))) {
                error = L"Windows could not create a GUID.";
                return false;
            }
            wchar_t text[40]{};
            if (::StringFromGUID2(guid, text, static_cast<int>(std::size(text))) <= 0) {
                error = L"Windows could not format the GUID.";
                return false;
            }
            output = text;
        } else if (tool == BuiltInTool::JwtDecoder) {
            const std::wstring token = Trim(input);
            const std::size_t first = token.find(L'.');
            const std::size_t second =
                first == std::wstring::npos ? first : token.find(L'.', first + 1);
            if (first == std::wstring::npos || second == std::wstring::npos ||
                token.find(L'.', second + 1) != std::wstring::npos) {
                error = L"JWT must contain header, payload, and signature parts.";
                return false;
            }
            const auto decodeJsonPart = [&](std::wstring_view part, json& document) {
                std::vector<unsigned char> bytes;
                if (!Base64Decode(ToUtf8(part), bytes, Base64Variant::Url)) {
                    return false;
                }
                const std::string utf8(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                std::wstring parseError;
                return ParseJsonBounded(utf8, document, parseError) && document.is_object();
            };
            json headerDocument;
            json payloadDocument;
            if (!decodeJsonPart(token.substr(0, first), headerDocument) ||
                !decodeJsonPart(token.substr(first + 1, second - first - 1), payloadDocument)) {
                error = L"JWT header and payload must be Base64URL JSON objects.";
                return false;
            }
            const std::wstring_view signature = std::wstring_view(token).substr(second + 1);
            const bool unsignedToken = headerDocument.value("alg", std::string{}) == "none";
            std::vector<unsigned char> signatureBytes;
            if ((unsignedToken && !signature.empty()) || (!unsignedToken && signature.empty()) ||
                (!signature.empty() &&
                 !Base64Decode(ToUtf8(signature), signatureBytes, Base64Variant::Url))) {
                error = L"JWT signature is not valid Base64URL.";
                return false;
            }
            std::wstring header;
            std::wstring payload;
            if (!RenderJsonBounded(headerDocument, 2, header, error) ||
                !RenderJsonBounded(payloadDocument, 2, payload, error)) {
                return false;
            }
            output = L"Header\r\n" + header + L"\r\n\r\nPayload\r\n" + payload +
                     L"\r\n\r\nSignature is displayed but not verified.";
        } else if (tool == BuiltInTool::UrlCodec) {
            if (mode == L"Decode") {
                const std::string encoded = ToUtf8(input);
                std::string decoded;
                for (std::size_t index = 0; index < encoded.size(); ++index) {
                    if (encoded[index] == '%') {
                        if (index + 2 >= encoded.size()) {
                            error = L"URL contains an incomplete percent escape.";
                            return false;
                        }
                        unsigned value = 0;
                        const auto converted = std::from_chars(
                            encoded.data() + index + 1, encoded.data() + index + 3, value, 16);
                        if (converted.ec != std::errc{} ||
                            converted.ptr != encoded.data() + index + 3) {
                            error = L"URL contains an invalid percent escape.";
                            return false;
                        }
                        decoded.push_back(static_cast<char>(value));
                        index += 2;
                    } else {
                        decoded.push_back(encoded[index]);
                    }
                }
                output = FromUtf8(decoded);
                if (output.empty() && !decoded.empty()) {
                    error = L"Decoded URL bytes are not valid UTF-8.";
                    return false;
                }
            } else {
                static constexpr char hex[] = "0123456789ABCDEF";
                const std::string utf8 = ToUtf8(input);
                std::string encoded;
                for (const unsigned char byte : utf8) {
                    if (std::isalnum(byte) || byte == '-' || byte == '_' || byte == '.' ||
                        byte == '~') {
                        encoded.push_back(static_cast<char>(byte));
                    } else {
                        encoded.push_back('%');
                        encoded.push_back(hex[byte >> 4]);
                        encoded.push_back(hex[byte & 15]);
                    }
                }
                output = FromUtf8(encoded);
            }
        } else if (tool == BuiltInTool::TimestampConverter) {
            const std::string value = ToUtf8(Trim(input));
            long long raw = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), raw);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
                error = L"Enter a Unix timestamp using the selected unit.";
                return false;
            }
            const bool milliseconds = mode == L"Unix milliseconds";
            constexpr long long minimumSeconds{-11644473600LL};
            constexpr long long maximumSeconds{253402300799LL};
            constexpr long long minimumMilliseconds{minimumSeconds * 1000LL};
            constexpr long long maximumMilliseconds{maximumSeconds * 1000LL + 999LL};
            if ((milliseconds && (raw < minimumMilliseconds || raw > maximumMilliseconds)) ||
                (!milliseconds && (raw < minimumSeconds || raw > maximumSeconds))) {
                error = L"Timestamp must be between years 1601 and 9999.";
                return false;
            }
            const long long valueMilliseconds = milliseconds ? raw : raw * 1000LL;
            long long unixSeconds = valueMilliseconds / 1000LL;
            long long remainderMilliseconds = valueMilliseconds % 1000LL;
            if (remainderMilliseconds < 0) {
                --unixSeconds;
                remainderMilliseconds += 1000LL;
            }
            constexpr unsigned long long windowsEpochSeconds{11644473600ULL};
            const unsigned long long windowsSeconds = static_cast<unsigned long long>(
                unixSeconds + static_cast<long long>(windowsEpochSeconds));
            const unsigned long long ticks =
                windowsSeconds * 10000000ULL +
                static_cast<unsigned long long>(remainderMilliseconds) * 10000ULL;
            ULARGE_INTEGER packed{};
            packed.QuadPart = ticks;
            FILETIME fileTime{packed.LowPart, packed.HighPart};
            SYSTEMTIME utc{};
            SYSTEMTIME local{};
            if (!::FileTimeToSystemTime(&fileTime, &utc) ||
                !::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
                error = L"Timestamp is outside the supported date range.";
                return false;
            }
            wchar_t utcText[80]{};
            wchar_t localText[80]{};
            (void)swprintf_s(utcText, L"%04hu-%02hu-%02hu %02hu:%02hu:%02hu.%03hu UTC", utc.wYear,
                             utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond,
                             utc.wMilliseconds);
            (void)swprintf_s(localText, L"%04hu-%02hu-%02hu %02hu:%02hu:%02hu.%03hu local",
                             local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute,
                             local.wSecond, local.wMilliseconds);
            output = std::wstring(utcText) + L"\r\n" + localText;
        } else if (tool == BuiltInTool::TextTransform) {
            output = input;
            if (mode == L"UPPERCASE" || mode == L"lowercase") {
                for (wchar_t& character : output) {
                    character = mode == L"UPPERCASE"
                                    ? static_cast<wchar_t>(std::towupper(character))
                                    : static_cast<wchar_t>(std::towlower(character));
                }
            } else if (mode == L"Title Case") {
                bool word = true;
                for (wchar_t& character : output) {
                    character = word ? static_cast<wchar_t>(std::towupper(character))
                                     : static_cast<wchar_t>(std::towlower(character));
                    word = !std::iswalnum(character);
                }
            } else {
                std::wstring normalized;
                bool separator = false;
                const wchar_t delimiter = mode == L"snake_case" ? L'_' : L'-';
                for (const wchar_t character : output) {
                    if (std::iswalnum(character)) {
                        if (separator && !normalized.empty()) {
                            normalized.push_back(delimiter);
                        }
                        normalized.push_back(static_cast<wchar_t>(std::towlower(character)));
                        separator = false;
                    } else {
                        separator = true;
                    }
                }
                output = std::move(normalized);
            }
        } else if (tool == BuiltInTool::Base64Image) {
            if (mode == L"Decode data URL") {
                const std::wstring source = Trim(input);
                constexpr std::wstring_view prefix{L"data:"};
                constexpr std::wstring_view marker{L";base64,"};
                const std::size_t markerIndex = source.find(marker);
                if (!source.starts_with(prefix) || markerIndex == std::wstring::npos ||
                    markerIndex <= prefix.size() || markerIndex + marker.size() >= source.size() ||
                    secondaryInput.empty()) {
                    error =
                        L"Provide a supported image data URL and an output "
                        L"file path.";
                    return false;
                }
                const std::wstring mime =
                    Lower(source.substr(prefix.size(), markerIndex - prefix.size()));
                std::vector<unsigned char> bytes;
                if (!Base64Decode(ToUtf8(source.substr(markerIndex + marker.size())), bytes,
                                  Base64Variant::Standard) ||
                    bytes.empty() || !HasImageSignature(mime, bytes)) {
                    error = L"Data URL is not a supported valid image.";
                    return false;
                }
                if (!WriteBinaryFileAtomic(std::wstring(secondaryInput), bytes)) {
                    error = L"Image data could not be written atomically.";
                    return false;
                }
                output = L"Wrote " + std::to_wstring(bytes.size()) + L" bytes to " +
                         std::wstring(secondaryInput);
            } else {
                const std::wstring path = Trim(input);
                std::vector<unsigned char> bytes;
                if (!ReadBinaryFile(path, bytes)) {
                    error = L"Image file could not be read or exceeds 3 MiB.";
                    return false;
                }
                output = L"data:" + MimeForPath(path) + L";base64," + FromUtf8(Base64Encode(bytes));
            }
        } else if (tool == BuiltInTool::NumberBase) {
            std::string value = ToUtf8(Lower(Trim(input)));
            int inputBase = 10;
            if (value.starts_with("0x")) {
                inputBase = 16;
                value.erase(0, 2);
            } else if (value.starts_with("0b")) {
                inputBase = 2;
                value.erase(0, 2);
            } else if (value.starts_with("0o")) {
                inputBase = 8;
                value.erase(0, 2);
            }
            unsigned long long number = 0;
            if (!ParseUnsigned(value, inputBase, number)) {
                error = L"Enter an unsigned integer, optionally prefixed 0x, 0b, or 0o.";
                return false;
            }
            const int outputBase = mode == L"To binary"  ? 2
                                   : mode == L"To octal" ? 8
                                   : mode == L"To hex"   ? 16
                                                         : 10;
            char buffer[70]{};
            const auto converted =
                std::to_chars(buffer, buffer + std::size(buffer), number, outputBase);
            output = FromUtf8(std::string(buffer, converted.ptr));
            if (outputBase == 16)
                output = L"0x" + output;
            else if (outputBase == 2)
                output = L"0b" + output;
            else if (outputBase == 8)
                output = L"0o" + output;
        } else if (tool == BuiltInTool::ColorConverter) {
            std::wstring value = Trim(input);
            unsigned red = 0;
            unsigned green = 0;
            unsigned blue = 0;
            if (!value.empty() && value.front() == L'#') {
                value.erase(value.begin());
                const std::string hex = ToUtf8(value);
                unsigned packed = 0;
                const auto parsed =
                    std::from_chars(hex.data(), hex.data() + hex.size(), packed, 16);
                if (parsed.ec != std::errc{} || parsed.ptr != hex.data() + hex.size() ||
                    (hex.size() != 3 && hex.size() != 6)) {
                    error = L"Enter #RGB, #RRGGBB, or R,G,B.";
                    return false;
                }
                if (hex.size() == 3) {
                    red = ((packed >> 8) & 15U) * 17U;
                    green = ((packed >> 4) & 15U) * 17U;
                    blue = (packed & 15U) * 17U;
                } else {
                    red = (packed >> 16) & 255U;
                    green = (packed >> 8) & 255U;
                    blue = packed & 255U;
                }
            } else if (swscanf_s(value.c_str(), L"%u,%u,%u", &red, &green, &blue) != 3 ||
                       red > 255 || green > 255 || blue > 255) {
                error = L"Enter #RGB, #RRGGBB, or R,G,B.";
                return false;
            }
            const double r = red / 255.0;
            const double g = green / 255.0;
            const double b = blue / 255.0;
            const double maximum = std::max({r, g, b});
            const double minimum = std::min({r, g, b});
            const double lightness = (maximum + minimum) * 0.5;
            double hue = 0.0;
            double saturation = 0.0;
            if (maximum != minimum) {
                const double delta = maximum - minimum;
                saturation = lightness > 0.5 ? delta / (2.0 - maximum - minimum)
                                             : delta / (maximum + minimum);
                if (maximum == r) {
                    hue = (g - b) / delta + (g < b ? 6.0 : 0.0);
                } else if (maximum == g) {
                    hue = (b - r) / delta + 2.0;
                } else {
                    hue = (r - g) / delta + 4.0;
                }
                hue *= 60.0;
            }
            wchar_t text[160]{};
            (void)swprintf_s(
                text, L"HEX #%02X%02X%02X\r\nRGB %u, %u, %u\r\nHSL %.0f°, %.0f%%, %.0f%%", red,
                green, blue, red, green, blue, hue, saturation * 100.0, lightness * 100.0);
            output = text;
        } else if (tool == BuiltInTool::HashCalculator) {
            output = Hash(input, mode, error);
        } else if (tool == BuiltInTool::RegexTester) {
            if (secondaryInput.size() > 32 * 1024 || !SafeRegexPattern(input)) {
                error =
                    L"Regex supports safe ECMAScript patterns up to 256 "
                    L"characters and 32 KiB of text; lookarounds, backreferences, "
                    L"groups, counted repetition, and unbounded quantifiers are "
                    L"rejected.";
                return false;
            }
            const std::wregex expression(std::wstring(input), std::regex_constants::ECMAScript);
            const std::wstring text(secondaryInput);
            std::wstring matches;
            std::size_t count = 0;
            for (std::wsregex_iterator it(text.begin(), text.end(), expression), end;
                 it != end && count < 1000; ++it, ++count) {
                matches += L"[" + std::to_wstring(count + 1) + L"] " + it->str() + L"\r\n";
            }
            output = matches.empty() ? L"No matches" : std::move(matches);
        } else if (tool == BuiltInTool::TextDiff) {
            output = DiffText(input, secondaryInput, error);
        } else {
            error = L"This developer tool is not implemented.";
            return false;
        }
    } catch (const json::exception&) {
        error = L"Input is not valid JSON.";
    } catch (const std::regex_error&) {
        error = L"The regular expression is invalid.";
    } catch (const std::bad_alloc&) {
        error = L"Not enough memory to process this input.";
    } catch (const std::length_error&) {
        error = L"The utility output exceeds the supported size.";
    }
    if (error.empty() && output.find(L'\0') != std::wstring::npos) {
        output.clear();
        error = L"Decoded text contains binary NUL bytes; use a binary tool.";
    }
    return error.empty();
}

}  // namespace orbitdeck
