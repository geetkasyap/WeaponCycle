#pragma once

#include <windows.h>
#include <string>

namespace orbitdeck {

struct SelectedFile {
    std::wstring path;
    std::wstring error;
};

[[nodiscard]] SelectedFile ExplorerSelectedFile(HWND explorer) noexcept;
[[nodiscard]] bool ConversionContextMenuEnabled() noexcept;
[[nodiscard]] bool SetConversionContextMenu(bool enabled, std::wstring& error) noexcept;

}  // namespace orbitdeck
