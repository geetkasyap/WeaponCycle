#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace orbitdeck {

enum class ImageFormat { Png, Jpeg, Bmp, Tiff };

struct FileConversionResult {
    bool succeeded{};
    bool cancelled{};
    std::wstring outputPath;
    std::wstring error;
    std::vector<std::wstring> outputPaths;
    std::wstring notice;
};

[[nodiscard]] const wchar_t* ImageFormatExtension(ImageFormat format) noexcept;
[[nodiscard]] FileConversionResult ConvertImageFile(const std::wstring& sourcePath,
                                                    ImageFormat format,
                                                    const std::atomic_bool& cancel) noexcept;

}  // namespace orbitdeck
