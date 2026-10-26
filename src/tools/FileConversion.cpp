#include "tools/FileConversion.h"
#include "platform/UniqueHandle.h"

#include <windows.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <new>
#include <vector>

namespace orbitdeck {
namespace {

using Microsoft::WRL::ComPtr;
constexpr std::uint64_t kMaximumPixels{32'000'000};
constexpr std::uint64_t kMaximumInputBytes{512ULL * 1024 * 1024};
constexpr UINT kRowsPerWrite{32};

struct OutputFile {
    std::wstring path;
    UniqueHandle reservation;
    bool keep{};
    ~OutputFile() {
        reservation.Reset();
        if (!keep && !path.empty() && !::DeleteFileW(path.c_str())) {
            ::OutputDebugStringW(L"Weapon Cycle: could not remove an incomplete converted file.\n");
        }
    }
};

std::wstring Failure(const wchar_t* operation, const HRESULT result) {
    wchar_t code[24]{};
    (void)swprintf_s(code, L" (0x%08lX).", static_cast<unsigned long>(result));
    return std::wstring(operation) + code;
}

WICBitmapTransformOptions Orientation(IWICBitmapFrameDecode* frame) noexcept {
    ComPtr<IWICMetadataQueryReader> metadata;
    if (FAILED(frame->GetMetadataQueryReader(metadata.GetAddressOf()))) {
        return WICBitmapTransformRotate0;
    }
    for (const wchar_t* query :
         {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}", L"/eXIf/ifd/{ushort=274}"}) {
        PROPVARIANT value{};
        const HRESULT read = metadata->GetMetadataByName(query, &value);
        const unsigned orientation = SUCCEEDED(read) && value.vt == VT_UI2 ? value.uiVal : 1;
        (void)::PropVariantClear(&value);
        if (FAILED(read))
            continue;
        switch (orientation) {
            case 2:
                return WICBitmapTransformFlipHorizontal;
            case 3:
                return WICBitmapTransformRotate180;
            case 4:
                return WICBitmapTransformFlipVertical;
            case 5:
                return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 |
                                                              WICBitmapTransformFlipHorizontal);
            case 6:
                return WICBitmapTransformRotate90;
            case 7:
                return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 |
                                                              WICBitmapTransformFlipHorizontal);
            case 8:
                return WICBitmapTransformRotate270;
            default:
                return WICBitmapTransformRotate0;
        }
    }
    return WICBitmapTransformRotate0;
}

FileConversionResult Convert(const std::wstring& sourcePath, const ImageFormat format,
                             const std::atomic_bool& cancel) {
    FileConversionResult result;
    if (cancel.load()) {
        result.cancelled = true;
        return result;
    }
    const wchar_t* extension = ImageFormatExtension(format);
    if (sourcePath.empty() || sourcePath.find(L'\0') != std::wstring::npos ||
        !std::filesystem::path(sourcePath).is_absolute() || extension == nullptr) {
        result.error = L"Choose a local image and a supported output format.";
        return result;
    }
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!::GetFileAttributesExW(sourcePath.c_str(), GetFileExInfoStandard, &attributes) ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        result.error = L"The selected file is missing, inaccessible, or a folder.";
        return result;
    }
    const std::uint64_t bytes =
        (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    if (bytes == 0 || bytes > kMaximumInputBytes) {
        result.error = L"Choose a non-empty image no larger than 512 MiB.";
        return result;
    }

    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        result.error = Failure(L"Windows imaging could not start", com);
        return result;
    }
    struct ComScope {
        bool initialized;
        ~ComScope() {
            if (initialized)
                ::CoUninitialize();
        }
    } comScope{SUCCEEDED(com)};
    OutputFile output;
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        result.error = Failure(L"Windows imaging is unavailable", hr);
        return result;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(sourcePath.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
    if (FAILED(hr)) {
        result.error = L"Windows cannot decode this image. Use Filee for additional formats. " +
                       Failure(L"Decoder error", hr);
        return result;
    }
    UINT frameCount{};
    hr = decoder->GetFrameCount(&frameCount);
    if (FAILED(hr) || frameCount != 1) {
        result.error =
            L"Animated or multipage images need Filee; this tool never silently "
            L"discards frames or pages.";
        return result;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.GetAddressOf());
    UINT width{}, height{};
    if (SUCCEEDED(hr))
        hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0 ||
        static_cast<std::uint64_t>(width) * height > kMaximumPixels) {
        result.error = L"The image is invalid or exceeds the 32-megapixel limit.";
        return result;
    }
    ComPtr<IWICBitmapSource> source;
    (void)frame.As(&source);
    const WICBitmapTransformOptions orientation = Orientation(frame.Get());
    ComPtr<IWICBitmapFlipRotator> rotated;
    if (orientation != WICBitmapTransformRotate0) {
        hr = factory->CreateBitmapFlipRotator(rotated.GetAddressOf());
        if (SUCCEEDED(hr))
            hr = rotated->Initialize(source.Get(), orientation);
        if (SUCCEEDED(hr))
            hr = rotated.As(&source);
        if (SUCCEEDED(hr))
            hr = source->GetSize(&width, &height);
        if (FAILED(hr)) {
            result.error = Failure(L"The image orientation could not be applied", hr);
            return result;
        }
    }

    ComPtr<IWICFormatConverter> pixels;
    hr = factory->CreateFormatConverter(pixels.GetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = pixels->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) {
        result.error = Failure(L"The image pixels could not be read", hr);
        return result;
    }
    if (cancel.load()) {
        result.cancelled = true;
        return result;
    }

    ComPtr<IStream> stream;
    const std::filesystem::path original(sourcePath);
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        GUID id{};
        hr = ::CoCreateGuid(&id);
        if (FAILED(hr))
            break;
        wchar_t identifier[40]{};
        if (::StringFromGUID2(id, identifier, 40) == 0) {
            hr = E_FAIL;
            break;
        }
        const std::wstring path =
            (original.parent_path() / (L".weaponcycle-" + std::wstring(identifier) + L".partial"))
                .wstring();
        const HANDLE reserved = ::CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                              nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (reserved != INVALID_HANDLE_VALUE) {
            output.path = path;
            output.reservation.Reset(reserved);
            hr = ::SHCreateStreamOnFileEx(path.c_str(), STGM_READWRITE | STGM_SHARE_EXCLUSIVE,
                                          FILE_ATTRIBUTE_NORMAL, FALSE, nullptr,
                                          stream.GetAddressOf());
            break;
        }
        const DWORD error = ::GetLastError();
        hr = HRESULT_FROM_WIN32(error);
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
            break;
    }
    if (stream == nullptr) {
        result.error = Failure(L"A new output file could not be created next to the original", hr);
        return result;
    }

    const GUID container = format == ImageFormat::Png    ? GUID_ContainerFormatPng
                           : format == ImageFormat::Jpeg ? GUID_ContainerFormatJpeg
                           : format == ImageFormat::Bmp  ? GUID_ContainerFormatBmp
                                                         : GUID_ContainerFormatTiff;
    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(container, nullptr, encoder.GetAddressOf());
    if (SUCCEEDED(hr))
        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> encoded;
    ComPtr<IPropertyBag2> properties;
    if (SUCCEEDED(hr)) {
        hr = encoder->CreateNewFrame(encoded.GetAddressOf(), properties.GetAddressOf());
    }
    if (SUCCEEDED(hr) && format == ImageFormat::Jpeg && properties != nullptr) {
        PROPBAG2 option{};
        option.pstrName = const_cast<wchar_t*>(L"ImageQuality");
        VARIANT value{};
        value.vt = VT_R4;
        value.fltVal = 0.92F;
        hr = properties->Write(1, &option, &value);
    }
    if (SUCCEEDED(hr))
        hr = encoded->Initialize(properties.Get());
    if (SUCCEEDED(hr))
        hr = encoded->SetSize(width, height);
    double dpiX{}, dpiY{};
    if (SUCCEEDED(hr) && SUCCEEDED(frame->GetResolution(&dpiX, &dpiY)) && dpiX > 0.0 &&
        dpiY > 0.0) {
        if (orientation == WICBitmapTransformRotate90 ||
            orientation == WICBitmapTransformRotate270 ||
            (static_cast<unsigned>(orientation) & 1U) != 0) {
            std::swap(dpiX, dpiY);
        }
        hr = encoded->SetResolution(dpiX, dpiY);
    }
    const bool opaque = format == ImageFormat::Jpeg || format == ImageFormat::Bmp;
    WICPixelFormatGUID pixelFormat =
        opaque ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr))
        hr = encoded->SetPixelFormat(&pixelFormat);
    if (SUCCEEDED(hr) && !::IsEqualGUID(pixelFormat, opaque ? GUID_WICPixelFormat24bppBGR
                                                            : GUID_WICPixelFormat32bppBGRA)) {
        hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    }
    if (FAILED(hr)) {
        result.error = Failure(L"The output encoder could not be prepared", hr);
        return result;
    }

    const UINT readStride = width * 4;
    const UINT writeStride = width * (opaque ? 3U : 4U);
    const UINT bufferRows = std::min(kRowsPerWrite, height);
    std::vector<BYTE> buffer(static_cast<std::size_t>(readStride) * bufferRows);
    std::vector<BYTE> flattened;
    if (opaque)
        flattened.resize(static_cast<std::size_t>(writeStride) * bufferRows);
    for (UINT row = 0; row < height; row += kRowsPerWrite) {
        if (cancel.load()) {
            result.cancelled = true;
            return result;
        }
        const UINT rows = std::min(kRowsPerWrite, height - row);
        const WICRect rect{0, static_cast<INT>(row), static_cast<INT>(width),
                           static_cast<INT>(rows)};
        hr = pixels->CopyPixels(&rect, readStride, static_cast<UINT>(buffer.size()), buffer.data());
        if (FAILED(hr))
            break;
        if (opaque) {
            for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(width) * rows; ++pixel) {
                const unsigned alpha = buffer[pixel * 4 + 3];
                for (unsigned channel = 0; channel < 3; ++channel) {
                    flattened[pixel * 3 + channel] = static_cast<BYTE>(
                        (buffer[pixel * 4 + channel] * alpha + 255U * (255U - alpha) + 127U) /
                        255U);
                }
            }
        }
        hr = encoded->WritePixels(rows, writeStride, writeStride * rows,
                                  opaque ? flattened.data() : buffer.data());
        if (FAILED(hr))
            break;
    }
    if (SUCCEEDED(hr))
        hr = encoded->Commit();
    if (SUCCEEDED(hr))
        hr = encoder->Commit();
    if (SUCCEEDED(hr))
        hr = stream->Commit(STGC_DEFAULT);
    if (FAILED(hr)) {
        result.error = Failure(L"The image could not be converted or saved", hr);
        return result;
    }
    if (cancel.load()) {
        result.cancelled = true;
        return result;
    }
    encoded.Reset();
    encoder.Reset();
    stream.Reset();
    output.reservation.Reset();
    for (unsigned number = 1; number <= 9999; ++number) {
        if (cancel.load()) {
            result.cancelled = true;
            return result;
        }
        const std::wstring name =
            original.stem().wstring() +
            (number == 1 ? L" (converted)" : L" (converted " + std::to_wstring(number) + L")") +
            extension;
        const std::wstring destination = (original.parent_path() / name).wstring();
        if (::MoveFileExW(output.path.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            output.keep = true;
            result.succeeded = true;
            result.outputPath = destination;
            return result;
        }
        const DWORD error = ::GetLastError();
        hr = HRESULT_FROM_WIN32(error);
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
            break;
    }
    result.error = Failure(L"The finished image could not be published under a new name", hr);
    return result;
}

}  // namespace

const wchar_t* ImageFormatExtension(const ImageFormat format) noexcept {
    switch (format) {
        case ImageFormat::Png:
            return L".png";
        case ImageFormat::Jpeg:
            return L".jpg";
        case ImageFormat::Bmp:
            return L".bmp";
        case ImageFormat::Tiff:
            return L".tiff";
    }
    return nullptr;
}

FileConversionResult ConvertImageFile(const std::wstring& sourcePath, const ImageFormat format,
                                      const std::atomic_bool& cancel) noexcept {
    try {
        return Convert(sourcePath, format, cancel);
    } catch (const std::bad_alloc&) {
        return {false, false, {}, L"Not enough memory to convert this image."};
    } catch (const std::filesystem::filesystem_error&) {
        return {false, false, {}, L"The selected image path is not valid."};
    }
}

}  // namespace orbitdeck
