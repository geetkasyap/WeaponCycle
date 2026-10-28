#include "tools/ConversionProtocol.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kSuccess =
    R"({"type":"result","succeeded":true,"cancelled":false,)"
    R"("outputs":["C:\\test\\file.png"],"error":"","message":"Original kept."})";
constexpr std::string_view kFormats =
    R"({"type":"formats","sourceFormat":"JPG","category":"Image","targets":[)"
    R"({"id":"png","label":"PNG","available":true,"packages":[],"downloadBytes":0}]})";

bool IsRejected(const std::string_view message) {
    orbitdeck::ConversionProtocol protocol;
    const bool parsed = protocol.Append(message) && protocol.Finish(0);
    return !parsed && !protocol.Response().error.empty() &&
           std::holds_alternative<std::monostate>(protocol.Response().payload);
}

}  // namespace

int main() {
    orbitdeck::ConversionProtocol result;
    for (char byte : kSuccess) {
        if (!result.Append(std::string_view(&byte, 1))) {
            return 1;
        }
    }
    if (!result.Finish(0)) {
        return 2;
    }
    const auto* converted =
        std::get_if<orbitdeck::FileConversionResult>(&result.Response().payload);
    if (converted == nullptr || !converted->succeeded ||
        converted->outputPath != L"C:\\test\\file.png" || converted->outputPaths.size() != 1 ||
        converted->notice != L"Original kept.") {
        return 3;
    }

    orbitdeck::ConversionProtocol formats;
    if (!formats.Append(std::string(kFormats) + "\r\n") || !formats.Finish(0)) {
        return 4;
    }
    const auto* choices = std::get_if<orbitdeck::ConversionFormats>(&formats.Response().payload);
    if (choices == nullptr || choices->targets.size() != 1 || choices->targets[0].id != L"png" ||
        !choices->targets[0].available) {
        return 5;
    }
    int progressValue{-1};
    std::wstring progressText;
    orbitdeck::ConversionProtocol progress;
    if (!progress.Append("\nEngine diagnostic\r\n"
                         R"({"type":"progress","progress":1.5,"message":"Working"})"
                         "\n",
                         [&](const int value, const std::wstring& text) {
                             progressValue = value;
                             progressText = text;
                         }) ||
        progressValue != 100 || progressText != L"Working" ||
        !progress.Append(std::string(kSuccess) + "\n") || !progress.Finish(0)) {
        return 6;
    }
    orbitdeck::ConversionProtocol failed;
    if (!failed.Append(
            R"({"type":"result","succeeded":false,"outputs":[],"error":"Unreadable file."})") ||
        !failed.Finish(1)) {
        return 7;
    }
    const auto* failure = std::get_if<orbitdeck::FileConversionResult>(&failed.Response().payload);
    if (failure == nullptr || failure->succeeded || failure->error != L"Unreadable file.") {
        return 8;
    }

    for (
        const std::string_view invalid :
        {R"({"type":"result",)", R"({"type":"result","succeeded":"yes","outputs":[]})",
         R"({"type":"result","succeeded":true,"cancelled":true,"outputs":[]})",
         R"({"type":"result","succeeded":false,"outputs":[]})",
         R"({"type":"result","succeeded":false,"outputs":["C:\\bad"],"error":"Failed"})",
         R"({"type":"result","succeeded":true,"outputs":[""]})",
         R"({"type":"result","succeeded":true,"outputs":["bad\u0000name"]})",
         R"({"type":"unknown"})", R"({"type":"progress","progress":"half","message":"Working"})",
         R"({"type":"formats","sourceFormat":"JPG","category":"Image","targets":"png"})",
         R"({"type":"formats","sourceFormat":"JPG","category":"Image","targets":[{"id":"png","label":"PNG","available":true,"packages":[],"downloadBytes":-1}]})"}) {
        if (!IsRejected(invalid)) {
            return 9;
        }
    }
    if (!IsRejected(std::string(kSuccess) + "\n" + std::string(kSuccess))) {
        return 10;
    }
    orbitdeck::ConversionProtocol exitStatus;
    if (!exitStatus.Append(kSuccess) || exitStatus.Finish(1) ||
        exitStatus.Response().error.empty()) {
        return 11;
    }
    orbitdeck::ConversionProtocol missing;
    if (!missing.Append("Missing engine dependency.\n") || missing.Finish(1) ||
        missing.Response().error.find(L"Missing engine dependency.") == std::wstring::npos) {
        return 12;
    }
    if (!IsRejected(std::string(2 * 1024 * 1024 + 1, 'x'))) {
        return 13;
    }
    const std::string invalidUtf8 =
        "{\"type\":\"result\",\"succeeded\":true,\"outputs\":[\"bad\xff\"]}";
    if (!IsRejected(invalidUtf8)) {
        return 14;
    }
    if (!IsRejected("{\"type\":\"result\",\"nested\":" + std::string(80, '[') + "0" +
                    std::string(80, ']') + "}")) {
        return 16;
    }
    orbitdeck::ConversionProtocol unicode;
    if (!unicode.Append(
            R"({"type":"result","succeeded":true,"outputs":["C:\\\u65e5\u672c.png"]})") ||
        !unicode.Finish(0) ||
        std::get<orbitdeck::FileConversionResult>(unicode.Response().payload).outputPath !=
            L"C:\\\u65e5\u672c.png") {
        return 15;
    }
    std::puts("Conversion framing, UTF-8, schema and exit-status validation passed");
    return 0;
}
