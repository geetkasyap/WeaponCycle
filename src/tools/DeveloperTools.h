#pragma once

#include "config/Configuration.h"

#include <string>
#include <string_view>
#include <vector>

namespace orbitdeck {

[[nodiscard]] bool IsDeveloperTool(BuiltInTool tool) noexcept;
[[nodiscard]] std::wstring DeveloperToolName(BuiltInTool tool);
[[nodiscard]] std::vector<std::wstring> DeveloperToolModes(BuiltInTool tool);
[[nodiscard]] bool RunDeveloperTool(BuiltInTool tool, std::wstring_view input,
                                    std::wstring_view secondaryInput, std::wstring_view mode,
                                    std::wstring& output, std::wstring& error) noexcept;

}  // namespace orbitdeck
