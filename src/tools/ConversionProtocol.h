#pragma once

#include "tools/ConversionEngine.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace orbitdeck {

using ConversionPayload = std::variant<std::monostate, ConversionFormats, FileConversionResult>;

struct ConversionResponse {
    ConversionPayload payload;
    std::wstring error;
    bool cancelled{};
};

class ConversionProtocol {
public:
    [[nodiscard]] bool Append(std::string_view bytes, const ConversionProgress& progress = {});
    [[nodiscard]] bool Finish(std::uint32_t exitCode, const ConversionProgress& progress = {});
    [[nodiscard]] const ConversionResponse& Response() const noexcept;

private:
    [[nodiscard]] bool ReadLine(std::string_view line, const ConversionProgress& progress);
    [[nodiscard]] bool Fail(std::wstring error);

    ConversionResponse response_;
    std::string pending_;
    std::string diagnostics_;
    bool finished_{};
};

}  // namespace orbitdeck
