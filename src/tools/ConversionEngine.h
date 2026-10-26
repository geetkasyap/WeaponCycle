#pragma once

#include "tools/FileConversion.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace orbitdeck {

struct ConversionTarget {
    std::wstring id;
    std::wstring label;
    std::vector<std::wstring> packages;
    std::uint64_t downloadBytes{};
    bool available{};
};

struct ConversionFormats {
    std::vector<ConversionTarget> targets;
    std::wstring sourceFormat;
    std::wstring category;
    std::wstring error;
};

using ConversionProgress = std::function<void(int, const std::wstring&)>;

[[nodiscard]] std::wstring ConversionEnginePath();
[[nodiscard]] std::wstring QuoteConversionArgument(const std::wstring& value);
[[nodiscard]] ConversionFormats DiscoverConversionFormats(const std::wstring& source,
                                                          const std::atomic_bool& cancel,
                                                          const ConversionProgress& progress = {});
[[nodiscard]] FileConversionResult RunFullConversion(const std::wstring& source,
                                                     const std::wstring& format,
                                                     const std::atomic_bool& cancel,
                                                     const ConversionProgress& progress);
[[nodiscard]] FileConversionResult InstallConversionEngine(
    const std::vector<std::wstring>& packages, const std::atomic_bool& cancel,
    const ConversionProgress& progress);

}  // namespace orbitdeck
