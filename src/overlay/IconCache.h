#pragma once

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <string>
#include <memory>
#include <unordered_map>
#include <vector>

namespace orbitdeck {

// Decodes item icon sources to device-independent premultiplied BGRA WIC
// bitmaps, caching each source once so the disk/shell work stays off the hot
// draw path. Any empty, missing, or unreadable source yields a generated
// fallback icon, so callers never get a null result and never crash on bad
// input.
class IconCache {
public:
    IconCache() = default;
    ~IconCache() noexcept;
    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;

    [[nodiscard]] bool Initialize() noexcept;

    // Returns a cached WIC bitmap for the source, generating a fallback on
    // empty/failure. Never returns null once Initialize() has succeeded.
    [[nodiscard]] IWICBitmapSource* Get(const std::wstring& source) noexcept;
    [[nodiscard]] bool IsFallback(const std::wstring& source) const noexcept;
    [[nodiscard]] IWICBitmapSource* GetCached(const std::wstring& source) const noexcept;
    [[nodiscard]] bool Prepare(std::vector<std::wstring> sources, HWND target,
                               UINT readyMessage) noexcept;
    [[nodiscard]] bool Collect() noexcept;

    void ClearEntries() noexcept;
    void Clear() noexcept;

private:
    struct PendingIcons;
    using BitmapPtr = Microsoft::WRL::ComPtr<IWICBitmapSource>;

    [[nodiscard]] BitmapPtr Decode(const std::wstring& source, bool& fallback) const noexcept;
    [[nodiscard]] BitmapPtr LoadImageFile(const std::wstring& path) const noexcept;
    [[nodiscard]] BitmapPtr ExtractExecutableIcon(const std::wstring& path) const noexcept;
    [[nodiscard]] BitmapPtr LoadShellItemIcon(const std::wstring& parsingName) const noexcept;
    [[nodiscard]] BitmapPtr BuildFallback() const noexcept;
    [[nodiscard]] BitmapPtr ToPremultiplied(const BitmapPtr& source) const noexcept;

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory_{};
    std::unordered_map<std::wstring, BitmapPtr> cache_{};
    std::unordered_map<std::wstring, bool> fallbacks_{};
    std::vector<std::wstring> preparedSources_{};
    std::shared_ptr<PendingIcons> pending_{};
};

}  // namespace orbitdeck
