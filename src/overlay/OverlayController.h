#pragma once

#include <windows.h>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

#include "config/Configuration.h"
#include "overlay/IconCache.h"
#include "overlay/CachedTextLayout.h"
#include "overlay/OverlayAnimation.h"
#include "overlay/WheelLayout.h"

namespace orbitdeck {

class OverlayController {
public:
    OverlayController() noexcept = default;
    ~OverlayController() noexcept;

    OverlayController(const OverlayController&) = delete;
    OverlayController& operator=(const OverlayController&) = delete;

    [[nodiscard]] bool Create(HINSTANCE instance) noexcept;

    // Points the overlay at the caller-owned item tree and a pure, precomputed
    // layout. Both remain stable until the next call.
    [[nodiscard]] bool SetLayout(const std::vector<WheelItem>& rootItems,
                                 WheelLayout layout) noexcept;
    void InvalidateItems() noexcept;
    void PrepareItems(const std::vector<WheelItem>& items) noexcept;
    void ConfigureAppearance(const AppearanceConfig& appearance, bool numberQuickKeys,
                             std::wstring centreTitle) noexcept;

    [[nodiscard]] bool ShowAt(POINT cursor, PlacementMode placementMode,
                              bool holdToCommit = true) noexcept;
    [[nodiscard]] bool ShowPreview(HWND anchorWindow) noexcept;

    // Applies one animation frame's opacity/transform to the composition tree.
    // Does not commit; callers batch a single Commit per frame.
    void ApplyVisualParams(const WheelVisualParams& params) noexcept;

    // Redraws the wheel surface, cross-fading the accent between the previously
    // and currently selected wedges by strength in [0, 1]. Does not commit.
    [[nodiscard]] bool DrawSelection(std::optional<std::size_t> previous,
                                     std::optional<std::size_t> current, float strength) noexcept;
    void SetSubmenuReveal(float progress) noexcept;

    [[nodiscard]] bool Commit() noexcept;
    [[nodiscard]] bool RecoverGraphics() noexcept;

    void Hide() noexcept;
    void Destroy() noexcept;

    [[nodiscard]] bool IsVisible() const noexcept;
    [[nodiscard]] bool CancellationRequested() const noexcept;
    [[nodiscard]] D2D1_POINT_2F WheelCentre() const noexcept;
    [[nodiscard]] float PixelsPerDip() const noexcept;
    [[nodiscard]] DWORD LastError() const noexcept;
    [[nodiscard]] const WheelLayout& Layout() const noexcept;

private:
    static constexpr UINT kIconsReadyMessage{WM_APP + 72};
    [[nodiscard]] static constexpr const wchar_t* ClassName() noexcept {
        return L"OrbitDeck.OverlayWindow.v1";
    }

    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam,
                                    LPARAM lParam) noexcept;

    [[nodiscard]] bool InitializeGraphics() noexcept;
    void ReleaseGraphics() noexcept;
    [[nodiscard]] HRESULT CreateChildVisual(
        Microsoft::WRL::ComPtr<IDCompositionVisual3>& visual) noexcept;
    [[nodiscard]] HRESULT CreateScrimSurface() noexcept;
    [[nodiscard]] HRESULT CreateVignetteSurface() noexcept;
    [[nodiscard]] bool EnsureWheelSurface(UINT pixelSize, UINT dpi) noexcept;
    [[nodiscard]] HRESULT BuildSegmentGeometries(UINT pixelSize) noexcept;
    [[nodiscard]] HRESULT CreateSegmentGeometry(
        float centre, float innerRadius, float outerRadius, float startAngle, float endAngle,
        Microsoft::WRL::ComPtr<ID2D1PathGeometry1>& geometry) noexcept;
    [[nodiscard]] HRESULT DrawWheelSurface(IDCompositionSurface* surface, UINT pixelSize,
                                           std::optional<std::size_t> previous,
                                           std::optional<std::size_t> current,
                                           float strength) noexcept;
    [[nodiscard]] HRESULT EnsureTextFormats(float scale) noexcept;
    void DrawItemsAndCentre(ID2D1DeviceContext* context, float scale, D2D1_POINT_2F centre,
                            std::optional<std::size_t> previous, std::optional<std::size_t> current,
                            float strength) noexcept;
    void FillAccentWedge(ID2D1DeviceContext* context, std::size_t index, float scale,
                         float strength) noexcept;
    void DrawCachedText(ID2D1DeviceContext* context, CachedTextLayout& cache,
                        std::wstring_view text, IDWriteTextFormat* format, D2D1_RECT_F bounds,
                        D2D1_DRAW_TEXT_OPTIONS options = D2D1_DRAW_TEXT_OPTIONS_NONE) noexcept;
    [[nodiscard]] bool Fail(HRESULT result) noexcept;

    HINSTANCE instance_{};
    HWND window_{};
    bool ownsClassRegistration_{false};
    DWORD lastError_{ERROR_SUCCESS};
    UINT surfaceSize_{};
    UINT surfaceDpi_{};
    int monitorWidthPx_{};
    int monitorHeightPx_{};
    RECT workAreaClient_{};
    float wheelOffsetXClient_{};
    float wheelOffsetYClient_{};
    float surfaceDiameterDip_{600.0F};
    bool cancellationRequested_{false};
    bool previewMode_{false};
    bool holdToCommit_{true};
    D2D1_POINT_2F wheelCentre_{};
    float pixelsPerDip_{1.0F};
    AppearanceConfig appearance_{};
    D2D1_COLOR_F selectedColor_{};
    D2D1_COLOR_F selectedContentColor_{};
    bool numberQuickKeys_{true};
    float submenuReveal_{1.0F};
    std::wstring centreTitle_{L"Choose"};

    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice_{};
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice_{};
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory_{};
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_{};
    Microsoft::WRL::ComPtr<IDCompositionDesktopDevice> compositionDesktopDevice_{};
    Microsoft::WRL::ComPtr<IDCompositionTarget> compositionTarget_{};
    Microsoft::WRL::ComPtr<IDCompositionVisual2> rootVisual_{};
    Microsoft::WRL::ComPtr<IDCompositionVisual3> scrimVisual_{};
    Microsoft::WRL::ComPtr<IDCompositionVisual3> vignetteVisual_{};
    Microsoft::WRL::ComPtr<IDCompositionVisual3> wheelVisual_{};
    Microsoft::WRL::ComPtr<IDCompositionSurface> scrimSurface_{};
    Microsoft::WRL::ComPtr<IDCompositionSurface> vignetteSurface_{};
    Microsoft::WRL::ComPtr<IDCompositionSurface> wheelSurface_{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> solidBrush_{};
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_{};
    Microsoft::WRL::ComPtr<IDWriteTextFormat> labelFormat_{};
    Microsoft::WRL::ComPtr<IDWriteTextFormat> centreFormat_{};
    Microsoft::WRL::ComPtr<IDWriteTextFormat> centreMetaFormat_{};
    Microsoft::WRL::ComPtr<IDWriteTextFormat> iconFormat_{};
    IconCache iconCache_{};
    std::unordered_map<std::wstring, Microsoft::WRL::ComPtr<ID2D1Bitmap>> iconBitmaps_{};
    std::optional<std::size_t> lastPrevious_{};
    std::optional<std::size_t> lastCurrent_{};
    float lastStrength_{};
    struct ItemTextLayouts {
        CachedTextLayout glyph;
        CachedTextLayout label;
        CachedTextLayout badge;
        CachedTextLayout title;
        CachedTextLayout hint;
    };
    std::vector<ItemTextLayouts> itemTextLayouts_;
    std::vector<std::wstring> fallbackGlyphs_;
    CachedTextLayout neutralTitleLayout_;
    CachedTextLayout neutralHintLayout_;
    HRESULT drawTextError_{S_OK};
    WheelLayout layout_{};
    std::vector<const WheelItem*> layoutItems_;
    std::size_t geometryItemCount_{};
    std::vector<Microsoft::WRL::ComPtr<ID2D1PathGeometry1>> segmentGeometries_;
    std::vector<Microsoft::WRL::ComPtr<ID2D1PathGeometry1>> accentSegmentGeometries_;
};

}  // namespace orbitdeck
