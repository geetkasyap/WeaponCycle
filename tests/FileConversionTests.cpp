#include "tools/FileConversion.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

struct TemporaryFolder {
    std::filesystem::path path;
    TemporaryFolder() {
        wchar_t root[MAX_PATH]{}, name[MAX_PATH]{};
        if (::GetTempPathW(MAX_PATH, root) != 0 && ::GetTempFileNameW(root, L"wcf", 0, name) != 0 &&
            ::DeleteFileW(name) && ::CreateDirectoryW(name, nullptr)) {
            path = name;
        }
    }
    ~TemporaryFolder() {
        if (!path.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }
};

bool WriteFixture(IWICImagingFactory* factory, const std::filesystem::path& path,
                  const GUID& container, const unsigned frames = 1, const USHORT orientation = 1) {
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.GetAddressOf())) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))
        return false;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(container, nullptr, encoder.GetAddressOf())) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return false;
    for (unsigned index = 0; index < frames; ++index) {
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> options;
        if (FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), options.GetAddressOf())) ||
            FAILED(frame->Initialize(options.Get())) || FAILED(frame->SetSize(2, 1)))
            return false;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&format)) ||
            !::IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA))
            return false;
        if (orientation != 1) {
            ComPtr<IWICMetadataQueryWriter> metadata;
            if (FAILED(frame->GetMetadataQueryWriter(metadata.GetAddressOf())))
                return false;
            PROPVARIANT value{};
            value.vt = VT_UI2;
            value.uiVal = orientation;
            if (FAILED(metadata->SetMetadataByName(L"/ifd/{ushort=274}", &value)))
                return false;
        }
        BYTE pixels[]{0, 0, 255, 128, 0, 0, 0, 0};
        if (FAILED(frame->WritePixels(1, 8, 8, pixels)) || FAILED(frame->Commit()))
            return false;
    }
    return SUCCEEDED(encoder->Commit());
}

bool Read(IWICImagingFactory* factory, const std::wstring& path, UINT& width, UINT& height,
          std::vector<BYTE>& pixels) {
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converted;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnLoad,
                                                  decoder.GetAddressOf())) ||
        FAILED(decoder->GetFrame(0, frame.GetAddressOf())) ||
        FAILED(frame->GetSize(&width, &height)) ||
        FAILED(factory->CreateFormatConverter(converted.GetAddressOf())) ||
        FAILED(converted->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return false;
    pixels.resize(static_cast<std::size_t>(width) * height * 4);
    return SUCCEEDED(
        converted->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
}

int Check() {
    using orbitdeck::ConvertImageFile;
    using orbitdeck::ImageFormat;
    TemporaryFolder folder;
    if (folder.path.empty())
        return 1;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()))))
        return 2;
    const auto source = folder.path / L"sample image.png";
    if (!WriteFixture(factory.Get(), source, GUID_ContainerFormatPng))
        return 3;
    const auto originalSize = std::filesystem::file_size(source);
    const auto existing = folder.path / L"sample image (converted).png";
    {
        std::ofstream marker(existing, std::ios::binary);
        marker << "existing file";
    }
    std::atomic_bool cancel{};
    const auto png = ConvertImageFile(source.wstring(), ImageFormat::Png, cancel);
    if (!png.succeeded) {
        std::fwprintf(stderr, L"%ls\n", png.error.c_str());
        return 4;
    }
    if (std::filesystem::path(png.outputPath) == existing)
        return 5;
    {
        std::ifstream marker(existing, std::ios::binary);
        const std::string content{std::istreambuf_iterator<char>(marker), {}};
        if (content != "existing file")
            return 6;
    }
    const auto again = ConvertImageFile(source.wstring(), ImageFormat::Png, cancel);
    if (!again.succeeded || again.outputPath == png.outputPath)
        return 7;
    UINT width{}, height{};
    std::vector<BYTE> pixels;
    if (!Read(factory.Get(), png.outputPath, width, height, pixels) || width != 2 || height != 1 ||
        pixels[2] != 255 || pixels[3] != 128 || pixels[7] != 0)
        return 8;

    for (const ImageFormat format : {ImageFormat::Jpeg, ImageFormat::Bmp, ImageFormat::Tiff}) {
        const auto result = ConvertImageFile(source.wstring(), format, cancel);
        if (!result.succeeded) {
            std::fwprintf(stderr, L"%ls\n", result.error.c_str());
            return 9;
        }
        if (!Read(factory.Get(), result.outputPath, width, height, pixels) || width != 2 ||
            height != 1)
            return 10;
        if (format == ImageFormat::Bmp &&
            (pixels[4] != 255 || pixels[5] != 255 || pixels[6] != 255 || pixels[0] < 126 ||
             pixels[0] > 128 || pixels[2] != 255))
            return 11;
        if (format == ImageFormat::Tiff && pixels[3] != 128)
            return 12;
    }
    if (std::filesystem::file_size(source) != originalSize)
        return 13;
    if (!Read(factory.Get(), source.wstring(), width, height, pixels) || pixels[3] != 128 ||
        pixels[7] != 0)
        return 14;

    const auto rotated = folder.path / L"portrait.tiff";
    if (!WriteFixture(factory.Get(), rotated, GUID_ContainerFormatTiff, 1, 6))
        return 15;
    const auto upright = ConvertImageFile(rotated.wstring(), ImageFormat::Png, cancel);
    if (!upright.succeeded || !Read(factory.Get(), upright.outputPath, width, height, pixels) ||
        width != 1 || height != 2)
        return 16;

    const auto multipage = folder.path / L"two pages.tiff";
    if (!WriteFixture(factory.Get(), multipage, GUID_ContainerFormatTiff, 2))
        return 17;
    const auto refused = ConvertImageFile(multipage.wstring(), ImageFormat::Png, cancel);
    if (refused.succeeded || refused.error.empty() || !refused.outputPath.empty())
        return 18;

    cancel = true;
    const auto cancelled = ConvertImageFile(source.wstring(), ImageFormat::Jpeg, cancel);
    if (!cancelled.cancelled || cancelled.succeeded || !cancelled.outputPath.empty())
        return 19;
    cancel = false;
    if (ConvertImageFile(L"relative.png", ImageFormat::Png, cancel).error.empty())
        return 20;
    if (ConvertImageFile(folder.path.wstring(), ImageFormat::Png, cancel).error.empty())
        return 21;
    if (ConvertImageFile(source.wstring(), static_cast<ImageFormat>(99), cancel).error.empty())
        return 22;
    if (ConvertImageFile((folder.path / L"missing.png").wstring(), ImageFormat::Png, cancel)
            .error.empty())
        return 23;
    if (ConvertImageFile(existing.wstring(), ImageFormat::Png, cancel).error.empty())
        return 24;
    for (const auto& entry : std::filesystem::directory_iterator(folder.path)) {
        if (entry.path().extension() == L".partial")
            return 25;
    }
    return 0;
}

}  // namespace

int main() {
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com))
        return 100;
    const int result = Check();
    ::CoUninitialize();
    if (result == 0)
        std::puts("File conversion tests passed");
    return result;
}
