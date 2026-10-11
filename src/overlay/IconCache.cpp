#include "overlay/IconCache.h"

#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace orbitdeck {
namespace {

constexpr UINT kFallbackSize{96};
constexpr UINT kMaximumCachedIconSize{256};

std::wstring ExecutableIconPath(const std::wstring& source) {
    if (source.size() < 4 || source.find_first_of(L"\\/:") != std::wstring::npos ||
        _wcsicmp(source.c_str() + source.size() - 4, L".exe") != 0)
        return source;
    const std::wstring key = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + source;
    for (const HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        DWORD bytes{};
        constexpr DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
        if (::RegGetValueW(root, key.c_str(), nullptr, flags, nullptr, nullptr, &bytes) !=
                ERROR_SUCCESS ||
            bytes < sizeof(wchar_t) || bytes > 65536 || bytes % sizeof(wchar_t) != 0)
            continue;
        std::wstring path(bytes / sizeof(wchar_t), L'\0');
        if (::RegGetValueW(root, key.c_str(), nullptr, flags, nullptr, path.data(), &bytes) !=
            ERROR_SUCCESS)
            continue;
        path.resize(wcsnlen_s(path.c_str(), path.size()));
        if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') {
            path = path.substr(1, path.size() - 2);
        }
        if (!path.empty())
            return path;
    }
    const DWORD needed = ::SearchPathW(nullptr, source.c_str(), nullptr, 0, nullptr, nullptr);
    if (needed != 0 && needed < 32768) {
        std::wstring path(needed + 1, L'\0');
        const DWORD length = ::SearchPathW(nullptr, source.c_str(), nullptr,
                                           static_cast<DWORD>(path.size()), path.data(), nullptr);
        if (length > 0 && length < path.size()) {
            path.resize(length);
            return path;
        }
    }
    return source;
}

}  // namespace

struct IconCache::PendingIcons {
    struct Pixels {
        std::wstring source;
        UINT width{};
        UINT height{};
        std::vector<BYTE> bytes;
    };
    std::mutex mutex;
    std::vector<std::wstring> requested;
    std::vector<Pixels> completed;
    std::uint64_t generation{};
    HWND target{};
    UINT message{};
    bool running{};
};

IconCache::~IconCache() noexcept {
    Clear();
}

bool IconCache::Initialize() noexcept {
    if (factory_ != nullptr)
        return true;
    const HRESULT result =
        ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(factory_.ReleaseAndGetAddressOf()));
    return SUCCEEDED(result);
}

IWICBitmapSource* IconCache::Get(const std::wstring& source) noexcept {
    if (factory_ == nullptr) {
        return nullptr;
    }
    const auto existing = cache_.find(source);
    if (existing != cache_.end()) {
        return existing->second.Get();
    }
    bool fallback = false;
    BitmapPtr bitmap = Decode(source, fallback);
    IWICBitmapSource* const raw = bitmap.Get();
    cache_.emplace(source, std::move(bitmap));
    fallbacks_[source] = fallback;
    return raw;
}

bool IconCache::IsFallback(const std::wstring& source) const noexcept {
    const auto found = fallbacks_.find(source);
    return found != fallbacks_.end() && found->second;
}

IWICBitmapSource* IconCache::GetCached(const std::wstring& source) const noexcept {
    const auto found = cache_.find(source);
    return found == cache_.end() ? nullptr : found->second.Get();
}

bool IconCache::Prepare(std::vector<std::wstring> sources, const HWND target,
                        const UINT readyMessage) noexcept {
    std::erase(sources, std::wstring{});
    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    if (sources == preparedSources_)
        return true;
    if (pending_ == nullptr)
        pending_ = std::make_shared<PendingIcons>();
    const auto state = pending_;
    {
        std::lock_guard lock(state->mutex);
        ++state->generation;
        state->requested.clear();
        state->completed.clear();
        state->target = target;
        state->message = readyMessage;
        for (const auto& source : sources) {
            if (GetCached(source) == nullptr) {
                state->requested.push_back(source);
            }
        }
        preparedSources_ = std::move(sources);
        if (state->running || state->requested.empty())
            return true;
        state->running = true;
    }
    try {
        std::thread([state] {
            const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            {
                IconCache loader;
                const bool initialized = SUCCEEDED(com) && loader.Initialize();
                (void)::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                for (;;) {
                    std::vector<std::wstring> requested;
                    std::uint64_t generation{};
                    {
                        std::lock_guard lock(state->mutex);
                        if (state->target == nullptr || state->requested.empty()) {
                            state->running = false;
                            break;
                        }
                        requested.swap(state->requested);
                        generation = state->generation;
                    }
                    for (const auto& source : requested) {
                        {
                            std::lock_guard lock(state->mutex);
                            if (state->target == nullptr || state->generation != generation)
                                break;
                        }
                        PendingIcons::Pixels pixels{source};
                        bool fallback = true;
                        BitmapPtr bitmap =
                            initialized ? loader.Decode(source, fallback) : BitmapPtr{};
                        if (!fallback && bitmap != nullptr &&
                            SUCCEEDED(bitmap->GetSize(&pixels.width, &pixels.height))) {
                            const UINT stride = pixels.width * 4;
                            pixels.bytes.resize(static_cast<std::size_t>(stride) * pixels.height);
                            if (FAILED(bitmap->CopyPixels(nullptr, stride,
                                                          static_cast<UINT>(pixels.bytes.size()),
                                                          pixels.bytes.data()))) {
                                pixels.bytes.clear();
                            }
                        }
                        std::lock_guard lock(state->mutex);
                        if (state->target != nullptr && state->generation == generation) {
                            state->completed.push_back(std::move(pixels));
                            (void)::PostMessageW(state->target, state->message, 0, 0);
                        }
                    }
                }
            }
            if (SUCCEEDED(com))
                ::CoUninitialize();
        }).detach();
    } catch (const std::system_error&) {
        std::lock_guard lock(state->mutex);
        state->running = false;
        preparedSources_.clear();
        ::OutputDebugStringW(L"Weapon Cycle: icon worker could not start.\n");
        return false;
    }
    return true;
}

bool IconCache::Collect() noexcept {
    if (pending_ == nullptr || factory_ == nullptr)
        return false;
    std::vector<PendingIcons::Pixels> completed;
    {
        std::lock_guard lock(pending_->mutex);
        completed.swap(pending_->completed);
    }
    for (auto& pixels : completed) {
        BitmapPtr source;
        if (!pixels.bytes.empty()) {
            Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
            if (SUCCEEDED(factory_->CreateBitmapFromMemory(
                    pixels.width, pixels.height, GUID_WICPixelFormat32bppPBGRA, pixels.width * 4,
                    static_cast<UINT>(pixels.bytes.size()), pixels.bytes.data(),
                    bitmap.GetAddressOf()))) {
                (void)bitmap.As(&source);
            }
        }
        fallbacks_[pixels.source] = source == nullptr;
        if (source == nullptr)
            source = BuildFallback();
        cache_[pixels.source] = std::move(source);
    }
    return !completed.empty();
}

void IconCache::Clear() noexcept {
    ClearEntries();
    if (pending_ != nullptr) {
        std::lock_guard lock(pending_->mutex);
        pending_->target = nullptr;
    }
    factory_.Reset();
}

void IconCache::ClearEntries() noexcept {
    cache_.clear();
    fallbacks_.clear();
    preparedSources_.clear();
    if (pending_ != nullptr) {
        std::lock_guard lock(pending_->mutex);
        ++pending_->generation;
        pending_->requested.clear();
        pending_->completed.clear();
    }
}

IconCache::BitmapPtr IconCache::Decode(const std::wstring& source, bool& fallback) const noexcept {
    fallback = false;
    const auto colon = source.find(L':');
    const bool filePath = colon == 1 || source.find_first_of(L"\\/") < colon;
    constexpr std::wstring_view appFolder{L"shell:AppsFolder\\"};
    const bool shellApp =
        source.size() >= appFolder.size() &&
        ::CompareStringOrdinal(source.data(), static_cast<int>(appFolder.size()), appFolder.data(),
                               static_cast<int>(appFolder.size()), TRUE) == CSTR_EQUAL;
    if (colon != std::wstring::npos && !filePath && !shellApp) {
        fallback = true;
        return BuildFallback();
    }
    if (!source.empty()) {
        const std::wstring path = ExecutableIconPath(source);
        BitmapPtr bitmap = LoadImageFile(path);
        if (bitmap == nullptr) {
            bitmap = ExtractExecutableIcon(path);
        }
        if (bitmap == nullptr) {
            bitmap = LoadShellItemIcon(path);
        }
        if (bitmap != nullptr) {
            return bitmap;
        }
    }
    fallback = true;
    return BuildFallback();
}

IconCache::BitmapPtr IconCache::LoadImageFile(const std::wstring& path) const noexcept {
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    HRESULT result = factory_->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf());
    if (FAILED(result)) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    result = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(result)) {
        return nullptr;
    }
    return ToPremultiplied(frame);
}

IconCache::BitmapPtr IconCache::ExtractExecutableIcon(const std::wstring& path) const noexcept {
    HICON icon = nullptr;
    const UINT extracted = ::ExtractIconExW(path.c_str(), 0, &icon, nullptr, 1);
    if (extracted == 0 || icon == nullptr) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    const HRESULT result = factory_->CreateBitmapFromHICON(icon, bitmap.GetAddressOf());
    ::DestroyIcon(icon);
    if (FAILED(result)) {
        return nullptr;
    }
    return ToPremultiplied(bitmap);
}

IconCache::BitmapPtr IconCache::LoadShellItemIcon(const std::wstring& parsingName) const noexcept {
    Microsoft::WRL::ComPtr<IShellItemImageFactory> imageFactory;
    HRESULT result = ::SHCreateItemFromParsingName(parsingName.c_str(), nullptr,
                                                   IID_PPV_ARGS(imageFactory.GetAddressOf()));
    if (FAILED(result))
        return nullptr;

    HBITMAP bitmapHandle = nullptr;
    result = imageFactory->GetImage(
        SIZE{static_cast<LONG>(kMaximumCachedIconSize), static_cast<LONG>(kMaximumCachedIconSize)},
        SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY, &bitmapHandle);
    if (FAILED(result) || bitmapHandle == nullptr)
        return nullptr;

    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    result = factory_->CreateBitmapFromHBITMAP(
        bitmapHandle, nullptr, WICBitmapUsePremultipliedAlpha, bitmap.GetAddressOf());
    (void)::DeleteObject(bitmapHandle);
    if (FAILED(result))
        return nullptr;
    return ToPremultiplied(bitmap);
}

IconCache::BitmapPtr IconCache::BuildFallback() const noexcept {
    constexpr UINT size = kFallbackSize;
    constexpr UINT stride = size * 4;
    const float radius = static_cast<float>(size) * 0.46F;
    const float innerRadius = static_cast<float>(size) * 0.30F;
    const float centre = static_cast<float>(size - 1) * 0.5F;

    std::vector<BYTE> pixels(static_cast<std::size_t>(stride) * size, 0);
    for (UINT y = 0; y < size; ++y) {
        for (UINT x = 0; x < size; ++x) {
            const float dx = static_cast<float>(x) - centre;
            const float dy = static_cast<float>(y) - centre;
            const float distance = std::sqrt(dx * dx + dy * dy);
            float alpha = radius + 0.5F - distance;
            alpha = alpha < 0.0F ? 0.0F : (alpha > 1.0F ? 1.0F : alpha);
            if (alpha <= 0.0F) {
                continue;
            }
            const bool inner = distance <= innerRadius;
            const float red = inner ? 168.0F : 78.0F;
            const float green = inner ? 190.0F : 100.0F;
            const float blue = inner ? 214.0F : 122.0F;
            BYTE* const pixel = &pixels[y * stride + x * 4];
            pixel[0] = static_cast<BYTE>(blue * alpha);
            pixel[1] = static_cast<BYTE>(green * alpha);
            pixel[2] = static_cast<BYTE>(red * alpha);
            pixel[3] = static_cast<BYTE>(255.0F * alpha);
        }
    }

    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    const HRESULT result = factory_->CreateBitmapFromMemory(
        size, size, GUID_WICPixelFormat32bppPBGRA, stride, static_cast<UINT>(pixels.size()),
        pixels.data(), bitmap.GetAddressOf());
    if (FAILED(result)) {
        return nullptr;
    }
    BitmapPtr source;
    (void)bitmap.As(&source);
    return source;
}

IconCache::BitmapPtr IconCache::ToPremultiplied(const BitmapPtr& source) const noexcept {
    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    if (FAILED(source->GetSize(&sourceWidth, &sourceHeight)) || sourceWidth == 0 ||
        sourceHeight == 0) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
    IWICBitmapSource* conversionSource = source.Get();
    if (sourceWidth > kMaximumCachedIconSize || sourceHeight > kMaximumCachedIconSize) {
        const double scale = std::min(static_cast<double>(kMaximumCachedIconSize) / sourceWidth,
                                      static_cast<double>(kMaximumCachedIconSize) / sourceHeight);
        const UINT width = std::max(1U, static_cast<UINT>(std::lround(sourceWidth * scale)));
        const UINT height = std::max(1U, static_cast<UINT>(std::lround(sourceHeight * scale)));
        HRESULT result = factory_->CreateBitmapScaler(scaler.GetAddressOf());
        if (FAILED(result) || FAILED(scaler->Initialize(source.Get(), width, height,
                                                        WICBitmapInterpolationModeFant))) {
            return nullptr;
        }
        conversionSource = scaler.Get();
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    HRESULT result = factory_->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(result)) {
        return nullptr;
    }
    result =
        converter->Initialize(conversionSource, GUID_WICPixelFormat32bppPBGRA,
                              WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(result)) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<IWICBitmap> detached;
    result = factory_->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad,
                                              detached.GetAddressOf());
    if (FAILED(result)) {
        return nullptr;
    }
    BitmapPtr converted;
    (void)detached.As(&converted);
    return converted;
}

}  // namespace orbitdeck
