#pragma once

#include <dwrite.h>
#include <wrl/client.h>

#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace orbitdeck {

class CachedTextLayout {
public:
    [[nodiscard]] HRESULT Ensure(IDWriteFactory* factory, IDWriteTextFormat* format,
                                 const std::wstring_view text, const float width,
                                 const float height) noexcept {
        if (factory == nullptr || format == nullptr || !std::isfinite(width) ||
            !std::isfinite(height) || width <= 0 || height <= 0 ||
            text.size() > std::numeric_limits<UINT32>::max())
            return E_INVALIDARG;
        if (layout_ != nullptr && format_.Get() == format && text_ == text && width_ == width &&
            height_ == height)
            return S_OK;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        const HRESULT result =
            factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, width,
                                      height, layout.GetAddressOf());
        if (FAILED(result))
            return result;
        text_ = text;
        format_ = format;
        width_ = width;
        height_ = height;
        layout_ = std::move(layout);
        return S_OK;
    }
    [[nodiscard]] IDWriteTextLayout* Get() const noexcept {
        return layout_.Get();
    }

private:
    std::wstring text_;
    float width_{};
    float height_{};
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format_;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout_;
};

}  // namespace orbitdeck
