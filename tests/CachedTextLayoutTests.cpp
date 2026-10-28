#include "overlay/CachedTextLayout.h"

#include <cstdio>
#include <chrono>
#include <limits>

int main() {
    Microsoft::WRL::ComPtr<IDWriteFactory> factory;
    if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
                                     reinterpret_cast<IUnknown**>(factory.GetAddressOf()))))
        return 1;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if (FAILED(factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         12.0F, L"", format.GetAddressOf())))
        return 2;
    orbitdeck::CachedTextLayout cache;
    if (FAILED(cache.Ensure(factory.Get(), format.Get(), L"Choose an action", 120.0F, 30.0F)))
        return 3;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> first = cache.Get();
    const auto cachedStart = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 1000; ++frame) {
        if (FAILED(cache.Ensure(factory.Get(), format.Get(), L"Choose an action", 120.0F, 30.0F)) ||
            cache.Get() != first.Get())
            return 4;
    }
    const double cachedUs =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - cachedStart)
            .count() /
        1000.0;
    const auto uncachedStart = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 1000; ++frame) {
        orbitdeck::CachedTextLayout fresh;
        if (FAILED(fresh.Ensure(factory.Get(), format.Get(), L"Choose an action", 120.0F, 30.0F)))
            return 10;
    }
    const double uncachedUs =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - uncachedStart)
            .count() /
        1000.0;
    std::printf("{\"cachedTextMicroseconds\":%.4f,\"freshLayoutMicroseconds\":%.4f}\n", cachedUs,
                uncachedUs);
    if (FAILED(cache.Ensure(factory.Get(), format.Get(), L"A different title", 120.0F, 30.0F)) ||
        cache.Get() == first.Get())
        return 5;
    auto* changed = cache.Get();
    if (FAILED(cache.Ensure(factory.Get(), format.Get(), L"A different title", 80.0F, 30.0F)) ||
        cache.Get() == changed)
        return 6;
    if (cache.Ensure(factory.Get(), format.Get(), L"Invalid", 0.0F, 30.0F) != E_INVALIDARG)
        return 7;
    if (cache.Ensure(factory.Get(), format.Get(), L"Invalid",
                     std::numeric_limits<float>::quiet_NaN(), 30.0F) != E_INVALIDARG)
        return 8;
    if (cache.Ensure(nullptr, format.Get(), L"Invalid", 80.0F, 30.0F) != E_INVALIDARG)
        return 9;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> larger;
    if (FAILED(factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         18.0F, L"", larger.GetAddressOf())))
        return 11;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> beforeFontChange = cache.Get();
    if (FAILED(cache.Ensure(factory.Get(), larger.Get(), L"A different title", 80.0F, 30.0F)) ||
        cache.Get() == beforeFontChange.Get())
        return 12;
    std::puts("Cached text layout reuse and invalidation passed without a graphics device");
    return 0;
}
