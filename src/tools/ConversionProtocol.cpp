#include "tools/ConversionProtocol.h"

#include <windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace orbitdeck {
namespace {

using Json = nlohmann::json;
constexpr std::size_t kMaximumMessageBytes{2 * 1024 * 1024};
constexpr std::size_t kMaximumDiagnosticsBytes{8192};
constexpr int kMaximumMessageDepth{64};

struct InvalidProtocolMessage {};

std::wstring ReadText(const Json& value) {
    const auto& text = value.get_ref<const Json::string_t&>();
    if (text.find('\0') != std::string::npos ||
        text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw InvalidProtocolMessage{};
    }
    if (text.empty()) {
        return {};
    }
    const int count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0);
    if (count == 0) {
        throw InvalidProtocolMessage{};
    }
    std::wstring converted(static_cast<std::size_t>(count), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                              static_cast<int>(text.size()), converted.data(), count) != count) {
        throw InvalidProtocolMessage{};
    }
    return converted;
}

std::wstring OptionalText(const Json& message, const char* field) {
    const auto found = message.find(field);
    return found == message.end() ? std::wstring{} : ReadText(*found);
}

FileConversionResult ReadResult(const Json& message) {
    FileConversionResult result;
    result.succeeded = message.at("succeeded").get<bool>();
    result.cancelled = message.value("cancelled", false);
    result.error = OptionalText(message, "error");
    result.notice = OptionalText(message, "message");
    for (const Json& output : message.at("outputs").get_ref<const Json::array_t&>()) {
        std::wstring path = ReadText(output);
        if (path.empty()) {
            throw InvalidProtocolMessage{};
        }
        result.outputPaths.push_back(std::move(path));
    }
    if (result.succeeded && (result.cancelled || !result.error.empty())) {
        throw InvalidProtocolMessage{};
    }
    if (!result.succeeded && !result.cancelled && result.error.empty()) {
        throw InvalidProtocolMessage{};
    }
    if (!result.succeeded && !result.outputPaths.empty()) {
        throw InvalidProtocolMessage{};
    }
    if (!result.outputPaths.empty()) {
        result.outputPath = result.outputPaths.front();
    }
    return result;
}

ConversionFormats ReadFormats(const Json& message) {
    ConversionFormats formats;
    formats.sourceFormat = ReadText(message.at("sourceFormat"));
    formats.category = ReadText(message.at("category"));
    for (const Json& entry : message.at("targets").get_ref<const Json::array_t&>()) {
        ConversionTarget target;
        target.id = ReadText(entry.at("id"));
        target.label = ReadText(entry.at("label"));
        target.available = entry.at("available").get<bool>();
        if (!entry.at("downloadBytes").is_number_unsigned()) {
            throw InvalidProtocolMessage{};
        }
        target.downloadBytes = entry.at("downloadBytes").get<std::uint64_t>();
        for (const Json& package : entry.at("packages").get_ref<const Json::array_t&>()) {
            std::wstring id = ReadText(package);
            if (id.empty()) {
                throw InvalidProtocolMessage{};
            }
            target.packages.push_back(std::move(id));
        }
        if (target.id.empty() || target.label.empty() ||
            (!target.available && target.packages.empty())) {
            throw InvalidProtocolMessage{};
        }
        formats.targets.push_back(std::move(target));
    }
    return formats;
}

}  // namespace

bool ConversionProtocol::Append(const std::string_view bytes, const ConversionProgress& progress) {
    if (finished_ || !response_.error.empty()) {
        return false;
    }
    std::size_t position{};
    while (position < bytes.size()) {
        const std::size_t newline = bytes.find('\n', position);
        const std::size_t end = newline == std::string_view::npos ? bytes.size() : newline;
        const std::size_t length = end - position;
        if (length > kMaximumMessageBytes - pending_.size()) {
            return Fail(L"The conversion engine response exceeded its safety limit.");
        }
        pending_.append(bytes.substr(position, length));
        if (newline == std::string_view::npos) {
            return true;
        }
        if (!ReadLine(pending_, progress)) {
            return false;
        }
        pending_.clear();
        position = newline + 1;
    }
    return true;
}

bool ConversionProtocol::ReadLine(std::string_view line, const ConversionProgress& progress) {
    const std::size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return true;
    }
    line.remove_prefix(first);
    if (line.front() != '{') {
        const std::size_t remaining = kMaximumDiagnosticsBytes - diagnostics_.size();
        diagnostics_.append(line.substr(0, remaining));
        if (diagnostics_.size() < kMaximumDiagnosticsBytes) {
            diagnostics_ += '\n';
        }
        return true;
    }

    try {
        const Json message = Json::parse(line, [](const int depth, Json::parse_event_t, Json&) {
            if (depth > kMaximumMessageDepth) {
                throw InvalidProtocolMessage{};
            }
            return true;
        });
        const std::string type = message.at("type").get<std::string>();
        if (!std::holds_alternative<std::monostate>(response_.payload)) {
            return Fail(L"The conversion engine returned more than one terminal response.");
        }
        if (type == "progress") {
            const double fraction = message.at("progress").get<double>();
            const std::wstring text = ReadText(message.at("message"));
            if (!std::isfinite(fraction)) {
                throw InvalidProtocolMessage{};
            }
            if (progress) {
                progress(static_cast<int>(std::clamp(fraction, 0.0, 1.0) * 100), text);
            }
        } else if (type == "formats") {
            response_.payload = ReadFormats(message);
        } else if (type == "result") {
            response_.payload = ReadResult(message);
        } else {
            throw InvalidProtocolMessage{};
        }
    } catch (const Json::exception&) {
        return Fail(L"The conversion engine returned a malformed response.");
    } catch (const InvalidProtocolMessage&) {
        return Fail(L"The conversion engine returned an invalid response value.");
    }
    return true;
}

bool ConversionProtocol::Finish(const std::uint32_t exitCode, const ConversionProgress& progress) {
    if (finished_ || !response_.error.empty()) {
        return false;
    }
    if (!pending_.empty() && !ReadLine(pending_, progress)) {
        return false;
    }
    pending_.clear();
    finished_ = true;
    if (std::holds_alternative<std::monostate>(response_.payload)) {
        std::wstring error = L"The conversion engine exited without a result.";
        if (!diagnostics_.empty()) {
            try {
                error += L"\n" + ReadText(Json(diagnostics_));
            } catch (const InvalidProtocolMessage&) {
                error += L"\nThe engine diagnostics were not valid UTF-8.";
            }
        }
        return Fail(std::move(error));
    }
    const auto* result = std::get_if<FileConversionResult>(&response_.payload);
    if (exitCode != 0 && (result == nullptr || result->succeeded)) {
        return Fail(L"The conversion engine exited with an error after reporting success.");
    }
    return true;
}

const ConversionResponse& ConversionProtocol::Response() const noexcept {
    return response_;
}

bool ConversionProtocol::Fail(std::wstring error) {
    response_.payload = std::monostate{};
    response_.error = std::move(error);
    return false;
}

}  // namespace orbitdeck
