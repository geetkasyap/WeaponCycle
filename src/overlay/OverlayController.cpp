#include "overlay/OverlayController.h"

#include "overlay/WheelPlacement.h"

#include <d2d1helper.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <numbers>
#include <string_view>
#include <utility>

namespace orbitdeck {
namespace {

constexpr float kSelectedExtensionDip{10.0F};
constexpr float kSeparatorWidthDip{0.7F};
constexpr float kSelectedEdgeWidthDip{1.0F};
constexpr float kRootOutlineWidthDip{0.75F};

constexpr UINT kScrimSurfaceSize{8};
constexpr UINT kVignetteSurfaceSize{256};
constexpr float kVignetteCoverageFactor{2.2F};

constexpr float kLabelFontDip{11.0F};
constexpr float kCentreFontDip{16.0F};
constexpr float kCentreMetaFontDip{9.5F};
constexpr float kIconGlyphFontDip{22.0F};
constexpr float kRootIconSizeDip{30.0F};
constexpr float kChildIconSizeDip{26.0F};
constexpr float kUnselectedIconOpacity{0.90F};
constexpr float kDisabledItemOpacity{0.25F};
constexpr float kBadgeRadiusSizeDip{6.5F};

[[nodiscard]] constexpr D2D1_COLOR_F Rgba(const unsigned red, const unsigned green,
                                          const unsigned blue, const unsigned alpha) noexcept {
    constexpr float divisor{255.0F};
    return {static_cast<float>(red) / divisor, static_cast<float>(green) / divisor,
            static_cast<float>(blue) / divisor, static_cast<float>(alpha) / divisor};
}

struct WheelTheme {
    D2D1_COLOR_F unselected;
    D2D1_COLOR_F secondary;
    D2D1_COLOR_F selected;
    D2D1_COLOR_F selectedEdge;
    D2D1_COLOR_F separator;
    D2D1_COLOR_F outerRing;
    D2D1_COLOR_F innerRing;
    D2D1_COLOR_F scrim;
    D2D1_COLOR_F vignetteEdge;
    D2D1_COLOR_F label;
    D2D1_COLOR_F centreText;
    D2D1_COLOR_F selectedContent;
};

constexpr WheelTheme kTheme{
    Rgba(22, 27, 34, 250),    Rgba(30, 36, 44, 248),    Rgba(216, 240, 230, 255),
    Rgba(255, 255, 255, 255), Rgba(237, 240, 242, 28),  Rgba(244, 246, 247, 48),
    Rgba(237, 240, 242, 38),  Rgba(6, 7, 9, 96),        Rgba(2, 3, 5, 62),
    Rgba(236, 239, 242, 232), Rgba(244, 246, 248, 248), Rgba(20, 22, 25, 255),
};

[[nodiscard]] unsigned HexDigit(const wchar_t value) noexcept {
    if (value >= L'0' && value <= L'9') {
        return static_cast<unsigned>(value - L'0');
    }
    const wchar_t lower = static_cast<wchar_t>(std::towlower(value));
    return static_cast<unsigned>(lower - L'a' + 10);
}

[[nodiscard]] D2D1_COLOR_F AccentColor(const std::wstring_view theme) noexcept {
    if (theme.empty() || theme.front() != L'#') {
        return kTheme.selected;
    }
    const std::wstring_view hex = theme.substr(1);
    if ((hex.size() != 3 && hex.size() != 4 && hex.size() != 6 && hex.size() != 8) ||
        !std::all_of(hex.begin(), hex.end(), [](const wchar_t value) {
            return (value >= L'0' && value <= L'9') || (value >= L'a' && value <= L'f') ||
                   (value >= L'A' && value <= L'F');
        })) {
        return kTheme.selected;
    }
    unsigned red = 242;
    unsigned green = 244;
    unsigned blue = 247;
    unsigned alpha = 255;
    if (hex.size() == 3 || hex.size() == 4) {
        red = HexDigit(hex[0]) * 17;
        green = HexDigit(hex[1]) * 17;
        blue = HexDigit(hex[2]) * 17;
        if (hex.size() == 4)
            alpha = HexDigit(hex[3]) * 17;
    } else if (hex.size() == 6 || hex.size() == 8) {
        red = HexDigit(hex[0]) * 16 + HexDigit(hex[1]);
        green = HexDigit(hex[2]) * 16 + HexDigit(hex[3]);
        blue = HexDigit(hex[4]) * 16 + HexDigit(hex[5]);
        if (hex.size() == 8) {
            alpha = HexDigit(hex[6]) * 16 + HexDigit(hex[7]);
        }
    }
    return Rgba(red, green, blue, alpha);
}

[[nodiscard]] D2D1_COLOR_F ContentColor(const D2D1_COLOR_F accent) noexcept {
    const D2D1_COLOR_F composite{accent.r * accent.a + kTheme.unselected.r * (1.0F - accent.a),
                                 accent.g * accent.a + kTheme.unselected.g * (1.0F - accent.a),
                                 accent.b * accent.a + kTheme.unselected.b * (1.0F - accent.a),
                                 1.0F};
    const auto linear = [](const float channel) {
        return channel <= 0.04045F
                   ? channel / 12.92F
                   : static_cast<float>(std::pow((channel + 0.055F) / 1.055F, 2.4F));
    };
    const float luminance = 0.2126F * linear(composite.r) + 0.7152F * linear(composite.g) +
                            0.0722F * linear(composite.b);
    const float darkContrast = (luminance + 0.05F) / 0.05F;
    const float lightContrast = 1.05F / (luminance + 0.05F);
    return darkContrast >= lightContrast ? Rgba(18, 21, 26, 255) : Rgba(248, 250, 252, 255);
}

[[nodiscard]] constexpr float Clamp01(const float value) noexcept {
    if (value < 0.0F) {
        return 0.0F;
    }
    return value > 1.0F ? 1.0F : value;
}

[[nodiscard]] constexpr D2D1_ARC_SIZE ArcSizeForSpan(const float radians) noexcept {
    return radians > std::numbers::pi_v<float> ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
}

static_assert(ArcSizeForSpan(std::numbers::pi_v<float>) == D2D1_ARC_SIZE_SMALL);
static_assert(ArcSizeForSpan(2.0F * std::numbers::pi_v<float> - 0.1F) == D2D1_ARC_SIZE_LARGE);

[[nodiscard]] DWORD CurrentWin32Error() noexcept {
    const DWORD error = ::GetLastError();
    return error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error;
}

[[nodiscard]] D2D1_POINT_2F PointOnCircle(const float centre, const float radius,
                                          const float angle) noexcept {
    return D2D1::Point2F(centre + std::cos(angle) * radius, centre + std::sin(angle) * radius);
}

[[nodiscard]] D2D1_ROUNDED_RECT TileRectangle(const WheelLayoutSegment& segment, const float scale,
                                              const D2D1_POINT_2F centre,
                                              const float expansion = 0.0F) noexcept {
    const WheelTileGeometry tile = TileForWheelSegment(segment);
    const float halfW = (tile.width * 0.5F + expansion) * scale;
    const float halfH = (tile.height * 0.5F + expansion) * scale;
    const float x = centre.x + tile.centreX * scale;
    const float y = centre.y + tile.centreY * scale;
    const float radius = std::min(14.0F, tile.height * 0.22F) * scale;
    return D2D1::RoundedRect(D2D1::RectF(x - halfW, y - halfH, x + halfW, y + halfH), radius,
                             radius);
}

[[nodiscard]] const WheelItem* ItemAtPath(const std::vector<WheelItem>& rootItems,
                                          const std::vector<std::size_t>& path) noexcept {
    const std::vector<WheelItem>* items = &rootItems;
    const WheelItem* item = nullptr;
    for (const std::size_t index : path) {
        if (index >= items->size()) {
            return nullptr;
        }
        item = &(*items)[index];
        items = &item->children;
    }
    return item;
}

[[nodiscard]] bool SameGeometry(const WheelLayout& left, const WheelLayout& right) noexcept {
    if (left.segments.size() != right.segments.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.segments.size(); ++index) {
        const WheelLayoutSegment& a = left.segments[index];
        const WheelLayoutSegment& b = right.segments[index];
        if (a.centreAngleRadians != b.centreAngleRadians ||
            a.halfSpanRadians != b.halfSpanRadians || a.innerRadiusDip != b.innerRadiusDip ||
            a.outerRadiusDip != b.outerRadiusDip) {
            return false;
        }
    }
    return true;
}

std::wstring FallbackGlyph(const WheelItem& item) {
    if (item.IsSubmenu())
        return L"›";
    switch (item.builtInTool) {
        case BuiltInTool::Caffeine:
            return L"☕";
        case BuiltInTool::Scratchpad:
            return L"✎";
        case BuiltInTool::SystemGlance:
            return L"◫";
        case BuiltInTool::Mirror:
            return L"◉";
        case BuiltInTool::MicCheck:
            return L"◖";
        case BuiltInTool::ColorPicker:
            return L"●";
        case BuiltInTool::JsonEditor:
            return L"{}";
        case BuiltInTool::Base64Text:
            return L"64";
        case BuiltInTool::GuidGenerator:
            return L"#";
        case BuiltInTool::JwtDecoder:
            return L"J";
        case BuiltInTool::UrlCodec:
            return L"↗";
        case BuiltInTool::TimestampConverter:
            return L"◷";
        case BuiltInTool::TextTransform:
            return L"Aa";
        case BuiltInTool::Base64Image:
            return L"▧";
        case BuiltInTool::NumberBase:
            return L"10";
        case BuiltInTool::ColorConverter:
            return L"◐";
        case BuiltInTool::HashCalculator:
            return L"#";
        case BuiltInTool::RegexTester:
            return L".*";
        case BuiltInTool::TextDiff:
            return L"±";
        case BuiltInTool::FileConverter:
            return L"⇄";
        case BuiltInTool::Timer:
            return L"◷";
        case BuiltInTool::Calculator:
            return L"=";
        case BuiltInTool::None:
            break;
    }
    const std::wstring type = item.launchType;
    if (type == L"url" || type == L"uri")
        return L"↗";
    if (type == L"clipboard")
        return L"⧉";
    if (type == L"shellCommand")
        return L">_";
    if (type == L"shortcut")
        return L"⌨";
    if (type == L"system")
        return L"⊞";
    std::wstring label = item.label;
    std::transform(label.begin(), label.end(), label.begin(),
                   [](const wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    if (label == L"spotify")
        return L"♫";
    if (label == L"chatgpt")
        return L"✦";
    if (label == L"powertoys")
        return L"⚡";
    if (label == L"developer")
        return L"<>";
    if (label == L"utilities")
        return L"⌘";
    if (label == L"work")
        return L"▣";
    if (label == L"windows")
        return L"⊞";
    return item.label.empty()
               ? L"•"
               : std::wstring(1, static_cast<wchar_t>(std::towupper(item.label.front())));
}

}  // namespace

OverlayController::~OverlayController() noexcept {
    Destroy();
}

bool OverlayController::Create(const HINSTANCE instance) noexcept {
    if (instance == nullptr) {
        lastError_ = ERROR_INVALID_PARAMETER;
        return false;
    }
    if (window_ != nullptr) {
        lastError_ = ERROR_ALREADY_EXISTS;
        return false;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &OverlayController::WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = ClassName();

    const ATOM classAtom = ::RegisterClassExW(&windowClass);
    if (classAtom == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        lastError_ = CurrentWin32Error();
        return false;
    }

    instance_ = instance;
    ownsClassRegistration_ = classAtom != 0;
    const HWND window = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP,
        ClassName(), L"Weapon Cycle", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (window == nullptr) {
        const DWORD error = CurrentWin32Error();
        Destroy();
        lastError_ = error;
        return false;
    }
    window_ = window;

    if (!InitializeGraphics()) {
        const DWORD error = lastError_;
        Destroy();
        lastError_ = error;
        return false;
    }

    lastError_ = ERROR_SUCCESS;
    return true;
}

bool OverlayController::SetLayout(const std::vector<WheelItem>& rootItems,
                                  WheelLayout layout) noexcept {
    const bool wasVisible = IsVisible();
    const UINT previousDpi = surfaceDpi_;
    const float previousPixelsPerDip = pixelsPerDip_;
    const float centreXClient = wheelOffsetXClient_ + static_cast<float>(surfaceSize_) * 0.5F;
    const float centreYClient = wheelOffsetYClient_ + static_cast<float>(surfaceSize_) * 0.5F;
    std::vector<const WheelItem*> layoutItems;
    layoutItems.reserve(layout.segments.size());
    for (const WheelLayoutSegment& segment : layout.segments) {
        const WheelItem* const item = ItemAtPath(rootItems, segment.path);
        if (item == nullptr) {
            lastError_ = ERROR_INVALID_DATA;
            return false;
        }
        layoutItems.push_back(item);
    }
    const float nextSurfaceDiameter =
        std::max(appearance_.wheelDiameterDip, layout.surfaceRadiusDip * 2.0F);
    const bool geometryChanged =
        !SameGeometry(layout_, layout) || surfaceDiameterDip_ != nextSurfaceDiameter;
    layout_ = std::move(layout);
    layoutItems_ = std::move(layoutItems);
    fallbackGlyphs_.clear();
    fallbackGlyphs_.reserve(layoutItems_.size());
    for (const WheelItem* item : layoutItems_)
        fallbackGlyphs_.push_back(FallbackGlyph(*item));
    itemTextLayouts_.resize(layoutItems_.size());
    surfaceDiameterDip_ = nextSurfaceDiameter;
    if (geometryChanged) {
        geometryItemCount_ = 0;
        segmentGeometries_.clear();
        accentSegmentGeometries_.clear();
        wheelSurface_.Reset();
        surfaceSize_ = 0;
    }

    if (!wasVisible || previousDpi == 0) {
        return true;
    }
    const long desiredSize =
        std::max(1L, std::lround(surfaceDiameterDip_ * std::max(0.1F, previousPixelsPerDip)));
    const int workWidth = workAreaClient_.right - workAreaClient_.left;
    const int workHeight = workAreaClient_.bottom - workAreaClient_.top;
    const int maximumSize = workWidth > 0 && workHeight > 0 ? std::min(workWidth, workHeight)
                                                            : static_cast<int>(desiredSize);
    const UINT pixelSize = static_cast<UINT>(std::clamp<long>(desiredSize, 1L, maximumSize));
    if (!EnsureWheelSurface(pixelSize, previousDpi)) {
        return false;
    }
    const float half = static_cast<float>(pixelSize) * 0.5F;
    const float nextCentreX =
        workWidth > 0 ? std::clamp(centreXClient, static_cast<float>(workAreaClient_.left) + half,
                                   static_cast<float>(workAreaClient_.right) - half)
                      : centreXClient;
    const float nextCentreY =
        workHeight > 0 ? std::clamp(centreYClient, static_cast<float>(workAreaClient_.top) + half,
                                    static_cast<float>(workAreaClient_.bottom) - half)
                       : centreYClient;
    wheelCentre_.x += nextCentreX - centreXClient;
    wheelCentre_.y += nextCentreY - centreYClient;
    pixelsPerDip_ = static_cast<float>(pixelSize) / surfaceDiameterDip_;
    wheelOffsetXClient_ = nextCentreX - half;
    wheelOffsetYClient_ = nextCentreY - half;
    HRESULT result = wheelVisual_->SetOffsetX(wheelOffsetXClient_);
    if (SUCCEEDED(result)) {
        result = wheelVisual_->SetOffsetY(wheelOffsetYClient_);
    }
    return SUCCEEDED(result) && Commit();
}

void OverlayController::InvalidateItems() noexcept {
    iconCache_.ClearEntries();
    iconBitmaps_.clear();
    itemTextLayouts_.clear();
}

void OverlayController::PrepareItems(const std::vector<WheelItem>& items) noexcept {
    std::vector<std::wstring> sources;
    const auto prepare = [&](const auto& self, const std::vector<WheelItem>& current) -> void {
        for (const WheelItem& item : current) {
            sources.push_back(item.iconSource);
            self(self, item.children);
        }
    };
    prepare(prepare, items);
    (void)iconCache_.Prepare(std::move(sources), window_, kIconsReadyMessage);
}

void OverlayController::ConfigureAppearance(const AppearanceConfig& appearance,
                                            const bool numberQuickKeys,
                                            std::wstring centreTitle) noexcept {
    const bool geometryChanged = appearance_.wheelDiameterDip != appearance.wheelDiameterDip ||
                                 appearance_.outerRadiusDip != appearance.outerRadiusDip ||
                                 appearance_.innerRadiusDip != appearance.innerRadiusDip;
    appearance_ = appearance;
    appearance_.scrimOpacity = Clamp01(appearance_.scrimOpacity);
    selectedColor_ = AccentColor(appearance_.theme);
    selectedContentColor_ = ContentColor(selectedColor_);
    numberQuickKeys_ = numberQuickKeys;
    centreTitle_ = centreTitle.empty() ? L"Choose" : std::move(centreTitle);
    if (geometryChanged) {
        geometryItemCount_ = 0;
        segmentGeometries_.clear();
        accentSegmentGeometries_.clear();
        wheelSurface_.Reset();
        surfaceSize_ = 0;
        surfaceDpi_ = 0;
    }
}

bool OverlayController::ShowAt(const POINT cursor, const PlacementMode placementMode,
                               const bool holdToCommit) noexcept {
    if (window_ == nullptr || compositionDesktopDevice_ == nullptr || rootVisual_ == nullptr) {
        lastError_ = ERROR_INVALID_STATE;
        return false;
    }
    cancellationRequested_ = false;
    previewMode_ = false;
    holdToCommit_ = holdToCommit;
    (void)::SetWindowLongPtrW(window_, GWLP_HWNDPARENT, 0);

    const HMONITOR monitor = ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr) {
        lastError_ = ERROR_INVALID_MONITOR_HANDLE;
        return false;
    }

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!::GetMonitorInfoW(monitor, &monitorInfo)) {
        lastError_ = CurrentWin32Error();
        return false;
    }

    const RECT monitorBounds = monitorInfo.rcMonitor;
    const int monitorWidth = monitorBounds.right - monitorBounds.left;
    const int monitorHeight = monitorBounds.bottom - monitorBounds.top;
    if (monitorWidth <= 0 || monitorHeight <= 0) {
        lastError_ = ERROR_INVALID_DATA;
        return false;
    }

    if (!::SetWindowPos(window_, nullptr, monitorBounds.left, monitorBounds.top, monitorWidth,
                        monitorHeight, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER)) {
        lastError_ = CurrentWin32Error();
        return false;
    }

    const UINT dpi = ::GetDpiForWindow(window_);
    if (dpi == 0) {
        lastError_ = ERROR_INVALID_HANDLE;
        Hide();
        return false;
    }

    const int requestedSize = ::MulDiv(static_cast<int>(std::lround(surfaceDiameterDip_)),
                                       static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
    const RECT usableBounds = monitorInfo.rcWork;
    workAreaClient_ = {usableBounds.left - monitorBounds.left, usableBounds.top - monitorBounds.top,
                       usableBounds.right - monitorBounds.left,
                       usableBounds.bottom - monitorBounds.top};
    POINT placementCentre = cursor;
    if (placementMode == PlacementMode::MonitorCentre) {
        placementCentre.x = usableBounds.left + (usableBounds.right - usableBounds.left) / 2;
        placementCentre.y = usableBounds.top + (usableBounds.bottom - usableBounds.top) / 2;
    }
    const SquarePlacement placement = CalculateSquarePlacement(
        {placementCentre.x, placementCentre.y},
        {usableBounds.left, usableBounds.top, usableBounds.right, usableBounds.bottom},
        requestedSize);
    if (placement.size <= 0) {
        lastError_ = ERROR_INVALID_DATA;
        Hide();
        return false;
    }
    if (!EnsureWheelSurface(static_cast<UINT>(placement.size), dpi)) {
        const DWORD error = lastError_;
        Hide();
        lastError_ = error;
        return false;
    }

    monitorWidthPx_ = monitorWidth;
    monitorHeightPx_ = monitorHeight;
    wheelCentre_ = D2D1::Point2F(
        static_cast<float>(placement.left) + static_cast<float>(placement.size) * 0.5F,
        static_cast<float>(placement.top) + static_cast<float>(placement.size) * 0.5F);
    pixelsPerDip_ = static_cast<float>(placement.size) / surfaceDiameterDip_;
    wheelOffsetXClient_ = static_cast<float>(placement.left - monitorBounds.left);
    wheelOffsetYClient_ = static_cast<float>(placement.top - monitorBounds.top);

    HRESULT result = wheelVisual_->SetOffsetX(wheelOffsetXClient_);
    if (SUCCEEDED(result)) {
        result = wheelVisual_->SetOffsetY(wheelOffsetYClient_);
    }
    if (SUCCEEDED(result)) {
        const float scrimScaleX =
            static_cast<float>(monitorWidth) / static_cast<float>(kScrimSurfaceSize);
        const float scrimScaleY =
            static_cast<float>(monitorHeight) / static_cast<float>(kScrimSurfaceSize);
        result = static_cast<IDCompositionVisual*>(scrimVisual_.Get())
                     ->SetTransform(D2D1::Matrix3x2F::Scale(scrimScaleX, scrimScaleY));
    }
    if (SUCCEEDED(result) && vignetteVisual_ != nullptr) {
        const float centreX = wheelOffsetXClient_ + static_cast<float>(placement.size) * 0.5F;
        const float centreY = wheelOffsetYClient_ + static_cast<float>(placement.size) * 0.5F;
        const float diagonal =
            std::sqrt(static_cast<float>(monitorWidth) * static_cast<float>(monitorWidth) +
                      static_cast<float>(monitorHeight) * static_cast<float>(monitorHeight));
        const float coverage = kVignetteCoverageFactor * diagonal;
        const float vignetteScale = coverage / static_cast<float>(kVignetteSurfaceSize);
        const D2D1_MATRIX_3X2_F vignetteTransform =
            D2D1::Matrix3x2F::Scale(vignetteScale, vignetteScale) *
            D2D1::Matrix3x2F::Translation(centreX - coverage * 0.5F, centreY - coverage * 0.5F);
        result = static_cast<IDCompositionVisual*>(vignetteVisual_.Get())
                     ->SetTransform(vignetteTransform);
    }
    if (FAILED(result)) {
        const DWORD error = static_cast<DWORD>(result);
        Hide();
        lastError_ = error;
        return false;
    }

    ApplyVisualParams(WheelVisualParams{});
    if (!Commit()) {
        const DWORD error = lastError_;
        Hide();
        lastError_ = error;
        return false;
    }

    if (!::SetWindowPos(window_, HWND_TOPMOST, monitorBounds.left, monitorBounds.top, monitorWidth,
                        monitorHeight, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW)) {
        const DWORD error = CurrentWin32Error();
        Hide();
        lastError_ = error;
        return false;
    }

    lastError_ = ERROR_SUCCESS;
    return true;
}

bool OverlayController::ShowPreview(const HWND anchorWindow) noexcept {
    if (window_ == nullptr || anchorWindow == nullptr || compositionDesktopDevice_ == nullptr ||
        wheelVisual_ == nullptr) {
        lastError_ = ERROR_INVALID_PARAMETER;
        return false;
    }

    RECT anchor{};
    if (!::GetWindowRect(anchorWindow, &anchor)) {
        lastError_ = CurrentWin32Error();
        return false;
    }
    const bool embedded = (::GetWindowLongPtrW(anchorWindow, GWL_STYLE) & WS_CHILD) != 0;
    const HMONITOR monitor = ::MonitorFromWindow(anchorWindow, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor == nullptr || !::GetMonitorInfoW(monitor, &monitorInfo)) {
        lastError_ = ERROR_INVALID_MONITOR_HANDLE;
        return false;
    }

    UINT dpi = ::GetDpiForWindow(anchorWindow);
    if (dpi == 0)
        dpi = USER_DEFAULT_SCREEN_DPI;
    constexpr int previewDiameterDip{360};
    const int requested =
        embedded ? std::max(1, std::min(static_cast<int>(anchor.right - anchor.left),
                                        static_cast<int>(anchor.bottom - anchor.top)))
                 : ::MulDiv(previewDiameterDip, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
    const int workWidth = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
    const int workHeight = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
    const int size = std::max(1, std::min({requested, workWidth, workHeight}));
    const int gap = ::MulDiv(16, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
    int left =
        embedded ? anchor.left + (anchor.right - anchor.left - size) / 2 : anchor.right + gap;
    if (!embedded && left + size > monitorInfo.rcWork.right) {
        left = anchor.left - gap - size;
    }
    left = std::clamp<int>(left, static_cast<int>(monitorInfo.rcWork.left),
                           static_cast<int>(monitorInfo.rcWork.right) - size);
    const int top = std::clamp<int>(
        embedded ? anchor.top + (anchor.bottom - anchor.top - size) / 2 : anchor.top,
        static_cast<int>(monitorInfo.rcWork.top),
        static_cast<int>(monitorInfo.rcWork.bottom) - size);

    previewMode_ = true;
    holdToCommit_ = true;
    cancellationRequested_ = false;
    (void)::SetWindowLongPtrW(
        window_, GWLP_HWNDPARENT,
        reinterpret_cast<LONG_PTR>(embedded ? ::GetAncestor(anchorWindow, GA_ROOT) : anchorWindow));
    if (!EnsureWheelSurface(static_cast<UINT>(size), dpi)) {
        return false;
    }
    wheelOffsetXClient_ = 0.0F;
    wheelOffsetYClient_ = 0.0F;
    wheelCentre_ =
        D2D1::Point2F(static_cast<float>(left + size / 2), static_cast<float>(top + size / 2));
    pixelsPerDip_ = static_cast<float>(size) / surfaceDiameterDip_;
    HRESULT result = wheelVisual_->SetOffsetX(0.0F);
    if (SUCCEEDED(result))
        result = wheelVisual_->SetOffsetY(0.0F);
    if (FAILED(result))
        return Fail(result);

    ApplyVisualParams(OverlayAnimation::kOpenParams);
    if (!Commit()) {
        return false;
    }
    if (!::SetWindowPos(window_, HWND_TOP, left, top, size, size,
                        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW)) {
        lastError_ = CurrentWin32Error();
        return false;
    }
    lastError_ = ERROR_SUCCESS;
    return true;
}

void OverlayController::ApplyVisualParams(const WheelVisualParams& params) noexcept {
    if (scrimVisual_ != nullptr) {
        scrimVisual_->SetOpacity(
            previewMode_ ? 0.0F : Clamp01(params.scrimOpacity * appearance_.scrimOpacity));
    }
    if (vignetteVisual_ != nullptr) {
        vignetteVisual_->SetOpacity(
            previewMode_ ? 0.0F : Clamp01(params.scrimOpacity * appearance_.scrimOpacity));
    }
    if (wheelVisual_ == nullptr) {
        return;
    }

    wheelVisual_->SetOpacity(Clamp01(params.wheelOpacity));

    const float centre = static_cast<float>(surfaceSize_) * 0.5F;
    const D2D1_MATRIX_3X2_F transform =
        D2D1::Matrix3x2F::Rotation(params.wheelRotationDegrees, D2D1::Point2F(centre, centre)) *
        D2D1::Matrix3x2F::Scale(params.wheelScale, params.wheelScale,
                                D2D1::Point2F(centre, centre));
    static_cast<IDCompositionVisual*>(wheelVisual_.Get())->SetTransform(transform);
}

bool OverlayController::DrawSelection(const std::optional<std::size_t> previous,
                                      const std::optional<std::size_t> current,
                                      const float strength) noexcept {
    if ((previous.has_value() && *previous >= layoutItems_.size()) ||
        (current.has_value() && *current >= layoutItems_.size())) {
        lastError_ = ERROR_INVALID_PARAMETER;
        return false;
    }
    if (wheelSurface_ == nullptr) {
        lastError_ = ERROR_INVALID_STATE;
        return false;
    }

    const HRESULT result =
        DrawWheelSurface(wheelSurface_.Get(), surfaceSize_, previous, current, Clamp01(strength));
    if (FAILED(result)) {
        return Fail(result);
    }

    lastError_ = ERROR_SUCCESS;
    return true;
}

void OverlayController::SetSubmenuReveal(const float progress) noexcept {
    submenuReveal_ = Clamp01(progress);
}

bool OverlayController::Commit() noexcept {
    if (compositionDesktopDevice_ == nullptr) {
        lastError_ = ERROR_INVALID_STATE;
        return false;
    }

    const HRESULT result = compositionDesktopDevice_->Commit();
    if (FAILED(result)) {
        return Fail(result);
    }
    lastError_ = ERROR_SUCCESS;
    return true;
}

bool OverlayController::RecoverGraphics() noexcept {
    Hide();
    ReleaseGraphics();
    if (!InitializeGraphics()) {
        return false;
    }
    lastError_ = ERROR_SUCCESS;
    return true;
}

void OverlayController::Hide() noexcept {
    if (window_ == nullptr) {
        return;
    }

    if (!::SetWindowPos(window_, HWND_NOTOPMOST, 0, 0, 0, 0,
                        SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
        lastError_ = CurrentWin32Error();
    }
    cancellationRequested_ = false;
}

void OverlayController::Destroy() noexcept {
    Hide();

    ReleaseGraphics();
    iconCache_.Clear();
    layout_ = {};
    layoutItems_.clear();
    surfaceDiameterDip_ = appearance_.wheelDiameterDip;
    monitorWidthPx_ = 0;
    monitorHeightPx_ = 0;
    workAreaClient_ = {};
    wheelOffsetXClient_ = 0.0F;
    wheelOffsetYClient_ = 0.0F;
    cancellationRequested_ = false;
    previewMode_ = false;
    wheelCentre_ = {};
    pixelsPerDip_ = 1.0F;

    if (window_ != nullptr) {
        const HWND window = window_;
        if (!::DestroyWindow(window)) {
            lastError_ = CurrentWin32Error();
            (void)::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            (void)::PostMessageW(window, WM_CLOSE, 0, 0);
        }

        window_ = nullptr;
    }
    if (ownsClassRegistration_) {
        if (!::UnregisterClassW(ClassName(), instance_)) {
            lastError_ = CurrentWin32Error();
        }
    }

    instance_ = nullptr;
    ownsClassRegistration_ = false;
}

void OverlayController::ReleaseGraphics() noexcept {
    itemTextLayouts_.clear();
    neutralTitleLayout_ = {};
    neutralHintLayout_ = {};
    iconBitmaps_.clear();
    solidBrush_.Reset();
    labelFormat_.Reset();
    centreFormat_.Reset();
    centreMetaFormat_.Reset();
    iconFormat_.Reset();
    dwriteFactory_.Reset();
    geometryItemCount_ = 0;
    segmentGeometries_.clear();
    accentSegmentGeometries_.clear();
    wheelSurface_.Reset();
    vignetteSurface_.Reset();
    scrimSurface_.Reset();
    wheelVisual_.Reset();
    vignetteVisual_.Reset();
    scrimVisual_.Reset();
    rootVisual_.Reset();
    compositionTarget_.Reset();
    compositionDesktopDevice_.Reset();
    d2dDevice_.Reset();
    d2dFactory_.Reset();
    dxgiDevice_.Reset();
    d3dDevice_.Reset();
    surfaceSize_ = 0;
    surfaceDpi_ = 0;
}

bool OverlayController::IsVisible() const noexcept {
    return window_ != nullptr && ::IsWindowVisible(window_) != FALSE;
}

bool OverlayController::CancellationRequested() const noexcept {
    return cancellationRequested_;
}

D2D1_POINT_2F OverlayController::WheelCentre() const noexcept {
    return wheelCentre_;
}

float OverlayController::PixelsPerDip() const noexcept {
    return pixelsPerDip_;
}

DWORD OverlayController::LastError() const noexcept {
    return lastError_;
}

const WheelLayout& OverlayController::Layout() const noexcept {
    return layout_;
}

bool OverlayController::InitializeGraphics() noexcept {
    constexpr UINT flags{D3D11_CREATE_DEVICE_BGRA_SUPPORT};
    HRESULT result = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr,
                                         0, D3D11_SDK_VERSION, d3dDevice_.ReleaseAndGetAddressOf(),
                                         nullptr, nullptr);
    if (FAILED(result)) {
        result = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
                                     D3D11_SDK_VERSION, d3dDevice_.ReleaseAndGetAddressOf(),
                                     nullptr, nullptr);
    }
    if (FAILED(result)) {
        return Fail(result);
    }

    result = d3dDevice_.As(&dxgiDevice_);
    if (FAILED(result)) {
        return Fail(result);
    }

    result = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 d2dFactory_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        return Fail(result);
    }

    result = d2dFactory_->CreateDevice(dxgiDevice_.Get(), d2dDevice_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        return Fail(result);
    }

    result = ::DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwriteFactory_.ReleaseAndGetAddressOf()));
    if (FAILED(result)) {
        return Fail(result);
    }

    // Icons are a best-effort embellishment: if WIC is unavailable the wheel
    // still renders with labels and centre text, just without glyphs.
    (void)iconCache_.Initialize();

    result = ::DCompositionCreateDevice2(
        d2dDevice_.Get(), __uuidof(IDCompositionDesktopDevice),
        reinterpret_cast<void**>(compositionDesktopDevice_.ReleaseAndGetAddressOf()));
    if (FAILED(result)) {
        return Fail(result);
    }

    result = compositionDesktopDevice_->CreateTargetForHwnd(
        window_, TRUE, compositionTarget_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        return Fail(result);
    }

    result = compositionDesktopDevice_->CreateVisual(rootVisual_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        return Fail(result);
    }
    result = rootVisual_->SetOffsetX(0.0F);
    if (SUCCEEDED(result)) {
        result = rootVisual_->SetOffsetY(0.0F);
    }
    if (FAILED(result)) {
        return Fail(result);
    }

    result = CreateChildVisual(scrimVisual_);
    if (SUCCEEDED(result)) {
        result = CreateChildVisual(vignetteVisual_);
    }
    if (SUCCEEDED(result)) {
        result = CreateChildVisual(wheelVisual_);
    }
    if (FAILED(result)) {
        return Fail(result);
    }

    result = CreateScrimSurface();
    if (FAILED(result)) {
        return Fail(result);
    }
    // The vignette is a best-effort embellishment. If the radial-gradient
    // surface cannot be created, drop the visual and continue: the overlay
    // still renders correctly with just the scrim.
    if (FAILED(CreateVignetteSurface())) {
        vignetteVisual_.Reset();
        vignetteSurface_.Reset();
    }

    // Z-order via successive inserts (each goes above the previous child):
    // scrim (bottom), vignette, wheel (top).
    result = rootVisual_->AddVisual(static_cast<IDCompositionVisual*>(scrimVisual_.Get()), TRUE,
                                    nullptr);
    if (SUCCEEDED(result) && vignetteVisual_ != nullptr) {
        result = rootVisual_->AddVisual(static_cast<IDCompositionVisual*>(vignetteVisual_.Get()),
                                        TRUE, nullptr);
    }
    if (SUCCEEDED(result)) {
        result = rootVisual_->AddVisual(static_cast<IDCompositionVisual*>(wheelVisual_.Get()), TRUE,
                                        nullptr);
    }
    if (FAILED(result)) {
        return Fail(result);
    }

    // Seed a fully transparent tree so nothing flashes before the first ShowAt.
    ApplyVisualParams(WheelVisualParams{});

    result = compositionTarget_->SetRoot(rootVisual_.Get());
    if (SUCCEEDED(result)) {
        result = compositionDesktopDevice_->Commit();
    }
    return SUCCEEDED(result) ? true : Fail(result);
}

HRESULT OverlayController::CreateChildVisual(
    Microsoft::WRL::ComPtr<IDCompositionVisual3>& visual) noexcept {
    if (compositionDesktopDevice_ == nullptr) {
        return E_INVALIDARG;
    }
    Microsoft::WRL::ComPtr<IDCompositionVisual2> created;
    HRESULT result = compositionDesktopDevice_->CreateVisual(created.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        return result;
    }
    Microsoft::WRL::ComPtr<IDCompositionVisual3> asVisual3;
    result = created.As(&asVisual3);
    if (FAILED(result)) {
        return result;
    }
    result = asVisual3->SetOffsetX(0.0F);
    if (SUCCEEDED(result)) {
        result = asVisual3->SetOffsetY(0.0F);
    }
    if (FAILED(result)) {
        return result;
    }
    visual = std::move(asVisual3);
    return S_OK;
}

HRESULT OverlayController::CreateScrimSurface() noexcept {
    if (compositionDesktopDevice_ == nullptr || scrimVisual_ == nullptr) {
        return E_INVALIDARG;
    }

    Microsoft::WRL::ComPtr<IDCompositionSurface> surface;
    HRESULT result = compositionDesktopDevice_->CreateSurface(
        kScrimSurfaceSize, kScrimSurfaceSize, DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED, surface.GetAddressOf());
    if (FAILED(result)) {
        return result;
    }

    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
    POINT offset{};
    result = surface->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
                                reinterpret_cast<void**>(context.GetAddressOf()), &offset);
    if (FAILED(result)) {
        return result;
    }

    context->SetDpi(96.0F, 96.0F);
    context->SetTransform(
        D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
    context->Clear(kTheme.scrim);

    const HRESULT endResult = surface->EndDraw();
    if (FAILED(endResult)) {
        return endResult;
    }

    result = scrimVisual_->SetContent(surface.Get());
    if (FAILED(result)) {
        return result;
    }

    scrimSurface_.Swap(surface);
    return S_OK;
}

HRESULT OverlayController::CreateVignetteSurface() noexcept {
    if (compositionDesktopDevice_ == nullptr || vignetteVisual_ == nullptr) {
        return E_INVALIDARG;
    }

    Microsoft::WRL::ComPtr<IDCompositionSurface> surface;
    HRESULT result = compositionDesktopDevice_->CreateSurface(
        kVignetteSurfaceSize, kVignetteSurfaceSize, DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED, surface.GetAddressOf());
    if (FAILED(result)) {
        return result;
    }

    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
    POINT offset{};
    result = surface->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
                                reinterpret_cast<void**>(context.GetAddressOf()), &offset);
    if (FAILED(result)) {
        return result;
    }

    context->SetDpi(96.0F, 96.0F);
    context->SetTransform(
        D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x), static_cast<float>(offset.y)));
    context->Clear(D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F));

    const float radius = static_cast<float>(kVignetteSurfaceSize) * 0.5F;
    const D2D1_GRADIENT_STOP stops[]{
        {0.0F, D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F)},
        {0.62F, D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F)},
        {1.0F, kTheme.vignetteEdge},
    };

    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> stopCollection;
    result = context->CreateGradientStopCollection(stops, static_cast<UINT32>(std::size(stops)),
                                                   D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP,
                                                   stopCollection.GetAddressOf());
    if (FAILED(result)) {
        (void)surface->EndDraw();
        return result;
    }

    Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> brush;
    result = context->CreateRadialGradientBrush(
        D2D1::RadialGradientBrushProperties(D2D1::Point2F(radius, radius),
                                            D2D1::Point2F(0.0F, 0.0F), radius, radius),
        stopCollection.Get(), brush.GetAddressOf());
    if (FAILED(result)) {
        (void)surface->EndDraw();
        return result;
    }

    context->FillRectangle(D2D1::RectF(0.0F, 0.0F, static_cast<float>(kVignetteSurfaceSize),
                                       static_cast<float>(kVignetteSurfaceSize)),
                           brush.Get());

    result = surface->EndDraw();
    if (FAILED(result)) {
        return result;
    }

    result = vignetteVisual_->SetContent(surface.Get());
    if (FAILED(result)) {
        return result;
    }

    vignetteSurface_.Swap(surface);
    return S_OK;
}

bool OverlayController::EnsureWheelSurface(const UINT pixelSize, const UINT dpi) noexcept {
    if (wheelSurface_ != nullptr && surfaceSize_ == pixelSize && surfaceDpi_ == dpi &&
        geometryItemCount_ == layout_.segments.size()) {
        const HRESULT result =
            DrawWheelSurface(wheelSurface_.Get(), pixelSize, std::nullopt, std::nullopt, 0.0F);
        return SUCCEEDED(result) ? true : Fail(result);
    }

    Microsoft::WRL::ComPtr<IDCompositionSurface> surface;
    HRESULT result = compositionDesktopDevice_->CreateSurface(
        pixelSize, pixelSize, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED,
        surface.GetAddressOf());
    if (FAILED(result)) {
        return Fail(result);
    }

    result = BuildSegmentGeometries(pixelSize);
    if (FAILED(result)) {
        return Fail(result);
    }

    result = EnsureTextFormats(static_cast<float>(pixelSize) / surfaceDiameterDip_);
    if (FAILED(result)) {
        return Fail(result);
    }

    result = DrawWheelSurface(surface.Get(), pixelSize, std::nullopt, std::nullopt, 0.0F);
    if (FAILED(result)) {
        return Fail(result);
    }

    result = wheelVisual_->SetContent(surface.Get());
    if (FAILED(result)) {
        return Fail(result);
    }

    wheelSurface_.Swap(surface);
    surfaceSize_ = pixelSize;
    surfaceDpi_ = dpi;
    return true;
}

HRESULT OverlayController::BuildSegmentGeometries(const UINT pixelSize) noexcept {
    if (pixelSize == 0 || d2dFactory_ == nullptr || layout_.segments.empty() ||
        surfaceDiameterDip_ <= 0.0F) {
        return E_INVALIDARG;
    }

    std::vector<Microsoft::WRL::ComPtr<ID2D1PathGeometry1>> geometries(layout_.segments.size());
    std::vector<Microsoft::WRL::ComPtr<ID2D1PathGeometry1>> accentGeometries(
        layout_.segments.size());
    const float scale = static_cast<float>(pixelSize) / surfaceDiameterDip_;
    const float centre = static_cast<float>(pixelSize) * 0.5F;

    for (std::size_t index = 0; index < layout_.segments.size(); ++index) {
        const WheelLayoutSegment& segment = layout_.segments[index];
        const HRESULT result = CreateSegmentGeometry(
            centre, segment.innerRadiusDip * scale, segment.outerRadiusDip * scale,
            segment.centreAngleRadians - segment.halfSpanRadians,
            segment.centreAngleRadians + segment.halfSpanRadians, geometries[index]);
        if (FAILED(result)) {
            return result;
        }
        const HRESULT accentResult = CreateSegmentGeometry(
            centre, segment.innerRadiusDip * scale,
            (segment.outerRadiusDip + kSelectedExtensionDip) * scale,
            segment.centreAngleRadians - segment.halfSpanRadians,
            segment.centreAngleRadians + segment.halfSpanRadians, accentGeometries[index]);
        if (FAILED(accentResult)) {
            return accentResult;
        }
    }

    segmentGeometries_ = std::move(geometries);
    accentSegmentGeometries_ = std::move(accentGeometries);
    geometryItemCount_ = layout_.segments.size();
    return S_OK;
}

HRESULT OverlayController::CreateSegmentGeometry(
    const float centre, const float innerRadius, const float outerRadius, const float startAngle,
    const float endAngle, Microsoft::WRL::ComPtr<ID2D1PathGeometry1>& geometry) noexcept {
    Microsoft::WRL::ComPtr<ID2D1PathGeometry1> path;
    HRESULT result = d2dFactory_->CreatePathGeometry(path.GetAddressOf());
    if (FAILED(result)) {
        return result;
    }

    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    result = path->Open(sink.GetAddressOf());
    if (FAILED(result)) {
        return result;
    }

    const D2D1_ARC_SIZE arcSize = ArcSizeForSpan(endAngle - startAngle);
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    sink->BeginFigure(PointOnCircle(centre, outerRadius, startAngle), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddArc(D2D1::ArcSegment(PointOnCircle(centre, outerRadius, endAngle),
                                  D2D1::SizeF(outerRadius, outerRadius), 0.0F,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, arcSize));
    sink->AddLine(PointOnCircle(centre, innerRadius, endAngle));
    sink->AddArc(D2D1::ArcSegment(PointOnCircle(centre, innerRadius, startAngle),
                                  D2D1::SizeF(innerRadius, innerRadius), 0.0F,
                                  D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, arcSize));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);

    result = sink->Close();
    if (SUCCEEDED(result)) {
        geometry.Swap(path);
    }
    return result;
}

HRESULT OverlayController::DrawWheelSurface(IDCompositionSurface* const surface,
                                            const UINT pixelSize,
                                            const std::optional<std::size_t> previous,
                                            const std::optional<std::size_t> current,
                                            const float strength) noexcept {
    if (surface == nullptr || pixelSize == 0 || layoutItems_.empty() ||
        layout_.segments.size() != layoutItems_.size() ||
        segmentGeometries_.size() != layoutItems_.size() ||
        accentSegmentGeometries_.size() != layoutItems_.size() ||
        (previous.has_value() && *previous >= layoutItems_.size()) ||
        (current.has_value() && *current >= layoutItems_.size())) {
        return E_INVALIDARG;
    }
    lastPrevious_ = previous;
    lastCurrent_ = current;
    lastStrength_ = strength;
    drawTextError_ = S_OK;
    const float displayStrength = current.has_value() && previous == current ? 1.0F : strength;

    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
    POINT updateOffset{};
    HRESULT result =
        surface->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
                           reinterpret_cast<void**>(context.GetAddressOf()), &updateOffset);
    if (FAILED(result)) {
        return result;
    }

    context->SetDpi(static_cast<float>(USER_DEFAULT_SCREEN_DPI),
                    static_cast<float>(USER_DEFAULT_SCREEN_DPI));
    context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const D2D1_MATRIX_3X2_F baseTransform = D2D1::Matrix3x2F::Translation(
        static_cast<float>(updateOffset.x), static_cast<float>(updateOffset.y));
    context->SetTransform(baseTransform);
    context->Clear(D2D1::ColorF(0.0F, 0.0F, 0.0F, 0.0F));

    if (solidBrush_ == nullptr) {
        result =
            context->CreateSolidColorBrush(kTheme.unselected, solidBrush_.ReleaseAndGetAddressOf());
    }

    if (SUCCEEDED(result)) {
        const float size = static_cast<float>(pixelSize);
        const float scale = size / surfaceDiameterDip_;
        const D2D1_POINT_2F centre = D2D1::Point2F(size * 0.5F, size * 0.5F);

        for (std::size_t index = 0; index < segmentGeometries_.size(); ++index) {
            D2D1_COLOR_F fill =
                layout_.segments[index].layer == 0 ? kTheme.unselected : kTheme.secondary;
            if (layout_.segments[index].expandedParent) {
                fill.r += (1.0F - fill.r) * 0.08F;
                fill.g += (1.0F - fill.g) * 0.08F;
                fill.b += (1.0F - fill.b) * 0.08F;
            }
            if (layout_.segments[index].layer > 0) {
                fill.a *= submenuReveal_;
            }
            fill.a *= appearance_.wheelOpacity;
            solidBrush_->SetColor(fill);
            const auto tile = TileRectangle(layout_.segments[index], scale, centre);
            if (layout_.segments[index].layer > 0) {
                D2D1_COLOR_F rail = fill;
                rail.a *= 0.28F;
                solidBrush_->SetColor(rail);
                context->FillGeometry(segmentGeometries_[index].Get(), solidBrush_.Get());
                solidBrush_->SetColor(fill);
            }
            context->FillRoundedRectangle(tile, solidBrush_.Get());
            D2D1_COLOR_F separator = Rgba(120, 138, 156, 92);
            if (layout_.segments[index].layer > 0) {
                separator.a *= submenuReveal_;
            }
            solidBrush_->SetColor(separator);
            context->DrawRoundedRectangle(tile, solidBrush_.Get(), kSeparatorWidthDip * scale);
        }

        // The selected segment uses a second cached geometry whose inner edge
        // is unchanged and whose outer edge alone projects beyond the ring.
        if (previous.has_value() && previous != current) {
            FillAccentWedge(context.Get(), *previous, scale, 1.0F - displayStrength);
        }
        if (current.has_value()) {
            FillAccentWedge(context.Get(), *current, scale, displayStrength);
        }
        context->SetTransform(baseTransform);

        solidBrush_->SetColor(Rgba(16, 21, 27, 250));
        const float centreRadius = std::max(12.0F, appearance_.innerRadiusDip - 12.0F) * scale;
        context->FillEllipse(D2D1::Ellipse(centre, centreRadius, centreRadius), solidBrush_.Get());

        solidBrush_->SetColor(kTheme.innerRing);
        context->DrawEllipse(D2D1::Ellipse(centre, centreRadius, centreRadius), solidBrush_.Get(),
                             kRootOutlineWidthDip * scale);
        DrawItemsAndCentre(context.Get(), scale, centre, previous, current, displayStrength);
        if (FAILED(drawTextError_))
            result = drawTextError_;
    }

    context.Reset();
    const HRESULT endResult = surface->EndDraw();
    return FAILED(result) ? result : endResult;
}

void OverlayController::FillAccentWedge(ID2D1DeviceContext* const context, const std::size_t index,
                                        const float scale, const float strength) noexcept {
    if (context == nullptr || index >= accentSegmentGeometries_.size() || solidBrush_ == nullptr ||
        strength <= 0.0F) {
        return;
    }

    const float clamped = Clamp01(strength);
    const D2D1_POINT_2F centre =
        D2D1::Point2F(surfaceDiameterDip_ * scale * 0.5F, surfaceDiameterDip_ * scale * 0.5F);
    const auto tile = TileRectangle(layout_.segments[index], scale, centre, clamped * 2.0F);

    D2D1_COLOR_F accent = selectedColor_;
    accent.a *= clamped;
    if (layout_.segments[index].layer > 0) {
        accent.a *= submenuReveal_;
    }
    solidBrush_->SetColor(accent);
    context->FillRoundedRectangle(tile, solidBrush_.Get());

    D2D1_COLOR_F edge = selectedContentColor_;
    edge.a = 0.32F * clamped;
    if (layout_.segments[index].layer > 0) {
        edge.a *= submenuReveal_;
    }
    solidBrush_->SetColor(edge);
    context->DrawRoundedRectangle(tile, solidBrush_.Get(), kSelectedEdgeWidthDip * scale);
}

HRESULT OverlayController::EnsureTextFormats(const float scale) noexcept {
    if (dwriteFactory_ == nullptr) {
        return E_FAIL;
    }

    // Font sizes are expressed in the surface's pixel domain (the draw context
    // DPI is pinned to 96), so multiplying the DIP size by scale keeps text a
    // constant physical size and pixel-sharp at every monitor DPI.
    HRESULT result = dwriteFactory_->CreateTextFormat(
        L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, kLabelFontDip * scale, L"",
        labelFormat_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        return result;
    }
    (void)labelFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    (void)labelFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    (void)labelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
    result =
        dwriteFactory_->CreateEllipsisTrimmingSign(labelFormat_.Get(), ellipsis.GetAddressOf());
    if (FAILED(result))
        return result;
    const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    (void)labelFormat_->SetTrimming(&trimming, ellipsis.Get());

    result = dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              kCentreFontDip * scale, L"",
                                              centreFormat_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        return result;
    }
    (void)centreFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    (void)centreFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    (void)centreFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    ellipsis.Reset();
    result =
        dwriteFactory_->CreateEllipsisTrimmingSign(centreFormat_.Get(), ellipsis.GetAddressOf());
    if (FAILED(result))
        return result;
    (void)centreFormat_->SetTrimming(&trimming, ellipsis.Get());
    result = dwriteFactory_->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, kCentreMetaFontDip * scale, L"",
        centreMetaFormat_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        return result;
    }
    (void)centreMetaFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    (void)centreMetaFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    (void)centreMetaFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    result = dwriteFactory_->CreateTextFormat(
        L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, kIconGlyphFontDip * scale, L"",
        iconFormat_.ReleaseAndGetAddressOf());
    if (FAILED(result)) {
        return result;
    }
    (void)iconFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    (void)iconFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    (void)iconFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return S_OK;
}

void OverlayController::DrawItemsAndCentre(ID2D1DeviceContext* const context, const float scale,
                                           const D2D1_POINT_2F centre,
                                           const std::optional<std::size_t> previous,
                                           const std::optional<std::size_t> current,
                                           const float strength) noexcept {
    if (context == nullptr || solidBrush_ == nullptr || layoutItems_.empty() ||
        layout_.segments.size() != layoutItems_.size()) {
        return;
    }

    const float clamped = Clamp01(strength);
    const float badgeSize = kBadgeRadiusSizeDip * scale;

    for (std::size_t index = 0; index < layoutItems_.size(); ++index) {
        const WheelLayoutSegment& segment = layout_.segments[index];
        const WheelItem& item = *layoutItems_[index];
        ItemTextLayouts& textLayouts = itemTextLayouts_[index];
        const WheelTileGeometry tile = TileForWheelSegment(segment);
        const auto bounds = TileRectangle(segment, scale, centre).rect;
        const bool showLabel = tile.height >= 48.0F;
        const float iconHalf =
            std::min((segment.layer == 0 ? kRootIconSizeDip : kChildIconSizeDip) * 0.5F,
                     tile.height * 0.28F) *
            scale;

        // Selected wedge shows its icon/label at full strength; the outgoing
        // wedge fades back to the unselected level as the cross-fade advances.
        float selectionStrength = 0.0F;
        if (item.enabled && current.has_value() && *current == index) {
            selectionStrength = clamped;
        } else if (item.enabled && previous.has_value() && *previous == index) {
            selectionStrength = 1.0F - clamped;
        }

        float emphasis = item.enabled ? kUnselectedIconOpacity : kDisabledItemOpacity;
        emphasis += (1.0F - kUnselectedIconOpacity) * selectionStrength;
        if (segment.layer > 0) {
            emphasis *= submenuReveal_;
        }

        const float iconX = centre.x + tile.centreX * scale;
        const float iconY = centre.y + (tile.centreY - (showLabel ? 9.0F : 0.0F)) * scale;
        IWICBitmapSource* const wic = iconCache_.GetCached(item.iconSource);
        if (wic != nullptr && !iconCache_.IsFallback(item.iconSource)) {
            auto& bitmap = iconBitmaps_[item.iconSource];
            if (bitmap == nullptr) {
                (void)context->CreateBitmapFromWicBitmap(wic, nullptr, bitmap.GetAddressOf());
            }
            if (bitmap != nullptr) {
                context->DrawBitmap(bitmap.Get(),
                                    D2D1::RectF(iconX - iconHalf, iconY - iconHalf,
                                                iconX + iconHalf, iconY + iconHalf),
                                    emphasis, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
            }
        } else if (iconFormat_ != nullptr && !item.label.empty()) {
            D2D1_COLOR_F glyphColor =
                selectionStrength >= 0.42F ? selectedContentColor_ : kTheme.label;
            glyphColor.a *= emphasis;
            solidBrush_->SetColor(glyphColor);
            DrawCachedText(
                context, textLayouts.glyph, fallbackGlyphs_[index], iconFormat_.Get(),
                D2D1::RectF(iconX - iconHalf, iconY - iconHalf, iconX + iconHalf, iconY + iconHalf),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        const std::wstring& label = item.label;
        if (showLabel && labelFormat_ != nullptr && !label.empty()) {
            D2D1_COLOR_F color = selectionStrength >= 0.42F ? selectedContentColor_ : kTheme.label;
            color.a *= emphasis;
            solidBrush_->SetColor(color);
            DrawCachedText(context, textLayouts.label, label, labelFormat_.Get(),
                           D2D1::RectF(bounds.left + 4.0F * scale, bounds.bottom - 22.0F * scale,
                                       bounds.right - 4.0F * scale, bounds.bottom - 3.0F * scale),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        const bool numberedQuickKey =
            numberQuickKeys_ && segment.layer == layout_.activeLayer && segment.index < 9;
        if (segment.layer == layout_.activeLayer && (item.quickKey != L'\0' || numberedQuickKey) &&
            centreMetaFormat_ != nullptr) {
            const float badgeX = bounds.left + 9.0F * scale;
            const float badgeY = bounds.top + 9.0F * scale;
            D2D1_COLOR_F badgeBackground =
                selectionStrength >= 0.42F ? selectedContentColor_ : kTheme.secondary;
            badgeBackground.a *= item.enabled ? 0.72F : 0.32F;
            solidBrush_->SetColor(badgeBackground);
            context->FillEllipse(D2D1::Ellipse(D2D1::Point2F(badgeX, badgeY), badgeSize, badgeSize),
                                 solidBrush_.Get());

            D2D1_COLOR_F badgeText = selectionStrength >= 0.42F ? (selectedContentColor_.r > 0.5F
                                                                       ? Rgba(18, 21, 26, 255)
                                                                       : Rgba(248, 250, 252, 255))
                                                                : kTheme.label;
            badgeText.a *= emphasis;
            solidBrush_->SetColor(badgeText);
            const wchar_t key[] = {
                item.quickKey != L'\0' ? item.quickKey : static_cast<wchar_t>(L'1' + segment.index),
                L'\0'};
            DrawCachedText(context, textLayouts.badge, std::wstring_view(key, 1),
                           centreMetaFormat_.Get(),
                           D2D1::RectF(badgeX - badgeSize, badgeY - badgeSize, badgeX + badgeSize,
                                       badgeY + badgeSize));
        }
    }

    // Centre text: the focused item's label, cross-fading with the outgoing one.
    if (centreFormat_ == nullptr) {
        return;
    }
    const float half = std::max(0.0F, appearance_.innerRadiusDip - 18.0F) * scale;
    const D2D1_RECT_F centreRect =
        D2D1::RectF(centre.x - half, centre.y - half, centre.x + half, centre.y + half);
    const auto drawCentre = [&](const std::size_t index, const float alpha) {
        if (index >= layoutItems_.size() || layoutItems_[index]->label.empty()) {
            return;
        }
        D2D1_COLOR_F color = kTheme.centreText;
        color.a *= alpha;
        solidBrush_->SetColor(color);
        const std::wstring& label = layoutItems_[index]->label;
        const D2D1_RECT_F titleRect = D2D1::RectF(centreRect.left, centre.y - 36.0F * scale,
                                                  centreRect.right, centre.y + 10.0F * scale);
        DrawCachedText(context, itemTextLayouts_[index].title, label, centreFormat_.Get(),
                       titleRect, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (centreMetaFormat_ != nullptr && index < layout_.segments.size()) {
            const std::wstring_view meta = layoutItems_[index]->IsSubmenu() ? L"Hold for more"
                                           : holdToCommit_                  ? L"Release to open"
                                                                            : L"Enter to open";
            D2D1_COLOR_F metaColor = kTheme.label;
            metaColor.a *= alpha * 0.78F;
            solidBrush_->SetColor(metaColor);
            DrawCachedText(context, itemTextLayouts_[index].hint, meta, centreMetaFormat_.Get(),
                           D2D1::RectF(centreRect.left, centre.y + 10.0F * scale, centreRect.right,
                                       centre.y + 32.0F * scale));
        }
    };
    if (previous.has_value() && previous != current) {
        drawCentre(*previous, 1.0F - clamped);
    }
    if (current.has_value()) {
        drawCentre(*current, clamped);
    }
    if (!current.has_value() || !previous.has_value()) {
        const float neutralAlpha =
            current.has_value() ? 1.0F - clamped : (previous.has_value() ? clamped : 1.0F);
        if (!centreTitle_.empty() && neutralAlpha > 0.0F) {
            D2D1_COLOR_F color = kTheme.centreText;
            color.a *= neutralAlpha;
            solidBrush_->SetColor(color);
            DrawCachedText(context, neutralTitleLayout_, centreTitle_, centreFormat_.Get(),
                           D2D1::RectF(centreRect.left, centre.y - 30.0F * scale, centreRect.right,
                                       centre.y + 8.0F * scale),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (centreMetaFormat_ != nullptr) {
                color.a *= 0.65F;
                solidBrush_->SetColor(color);
                const std::wstring_view hint = holdToCommit_ ? L"Point / release\nEsc to cancel"
                                                             : L"Enter to open\nEsc to cancel";
                DrawCachedText(context, neutralHintLayout_, hint, centreMetaFormat_.Get(),
                               D2D1::RectF(centreRect.left, centre.y + 8.0F * scale,
                                           centreRect.right, centre.y + 36.0F * scale));
            }
        }
    }
}

void OverlayController::DrawCachedText(ID2D1DeviceContext* const context, CachedTextLayout& cache,
                                       const std::wstring_view text,
                                       IDWriteTextFormat* const format, const D2D1_RECT_F bounds,
                                       const D2D1_DRAW_TEXT_OPTIONS options) noexcept {
    if (FAILED(drawTextError_) || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return;
    drawTextError_ = cache.Ensure(dwriteFactory_.Get(), format, text, bounds.right - bounds.left,
                                  bounds.bottom - bounds.top);
    if (SUCCEEDED(drawTextError_)) {
        context->DrawTextLayout(D2D1::Point2F(bounds.left, bounds.top), cache.Get(),
                                solidBrush_.Get(), options);
    }
}

bool OverlayController::Fail(const HRESULT result) noexcept {
    lastError_ = static_cast<DWORD>(result);
    return false;
}

LRESULT CALLBACK OverlayController::WndProc(const HWND window, const UINT message,
                                            const WPARAM wParam, const LPARAM lParam) noexcept {
    auto* owner = reinterpret_cast<OverlayController*>(::GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE && owner == nullptr) {
        if (lParam == 0) {
            return FALSE;
        }
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        owner = static_cast<OverlayController*>(create->lpCreateParams);
        if (owner == nullptr) {
            return FALSE;
        }

        ::SetLastError(ERROR_SUCCESS);
        if (::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner)) == 0 &&
            ::GetLastError() != ERROR_SUCCESS) {
            return FALSE;
        }
        owner->window_ = window;
    }

    switch (message) {
        case kIconsReadyMessage:
            if (owner != nullptr && owner->iconCache_.Collect()) {
                owner->iconBitmaps_.clear();
                if (owner->IsVisible() && owner->wheelSurface_ != nullptr) {
                    if (!owner->DrawSelection(owner->lastPrevious_, owner->lastCurrent_,
                                              owner->lastStrength_) ||
                        !owner->Commit()) {
                        owner->cancellationRequested_ = true;
                    }
                }
            }
            return 0;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_KEYDOWN:
            if (wParam != VK_ESCAPE)
                break;
            [[fallthrough]];
        case WM_CANCELMODE:
        case WM_CLOSE:
            if (owner != nullptr)
                owner->cancellationRequested_ = true;
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_DPICHANGED:
            if (owner != nullptr && ::IsWindowVisible(window)) {
                owner->cancellationRequested_ = true;
            }
            return 0;
        case WM_ERASEBKGND:
            return TRUE;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            ::BeginPaint(window, &paint);
            ::EndPaint(window, &paint);
            return 0;
        }
        case WM_NCDESTROY:
            if (owner != nullptr) {
                owner->window_ = nullptr;
            }
            ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        default:
            break;
    }

    return ::DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace orbitdeck
