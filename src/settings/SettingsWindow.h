#pragma once

#include "config/Configuration.h"
#include "shell/AppCatalog.h"

#include <windows.h>
#include <shellapi.h>

#include <cstddef>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace orbitdeck {

struct AppDiscoveryState {
    std::atomic_bool cancel{false};
    std::atomic_bool running{true};
    HWND target{};
};

class SettingsWindow {
public:
    using SaveCallback = bool (*)(void* context, const Configuration& configuration,
                                  bool replaceInvalidExisting, const std::string* replacementJson,
                                  std::wstring& error) noexcept;
    using PreviewCallback = void (*)(void* context, const Configuration* configuration) noexcept;
    using TestCallback = void (*)(void* context, const WheelItem* item) noexcept;

    SettingsWindow() = default;
    ~SettingsWindow() noexcept;

    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    [[nodiscard]] bool Create(HINSTANCE instance, HWND owner, SaveCallback save,
                              PreviewCallback preview, TestCallback test, void* context) noexcept;
    void Show(const Configuration& configuration) noexcept;
    void Destroy() noexcept;
    [[nodiscard]] bool IsVisible() const noexcept;
    [[nodiscard]] HWND Handle() const noexcept;
    [[nodiscard]] HWND PreviewHandle() const noexcept;
    void UpdatePreview() noexcept;

private:
    static constexpr UINT kAppsDiscoveredMessage{WM_APP + 41};
    static constexpr UINT_PTR kPreviewTimerId{42};
    [[nodiscard]] static constexpr const wchar_t* ClassName() noexcept {
        return L"OrbitDeck.SettingsWindow.v1";
    }

    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam,
                                    LPARAM lParam) noexcept;

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept;
    [[nodiscard]] bool CreateControls() noexcept;
    void UpdateFont(UINT dpi) noexcept;
    void ScaleChildren(UINT oldDpi, UINT newDpi) noexcept;
    void FitToWorkArea() noexcept;
    void RebuildList(const std::vector<std::size_t>& selectPath = {}) noexcept;
    void AppendItems(const std::vector<WheelItem>& items, std::vector<std::size_t>& path,
                     std::size_t depth) noexcept;
    void SaveSelectedFields() noexcept;
    void LoadSelectedFields() noexcept;
    [[nodiscard]] bool SaveAppearanceFields(bool showValidationError) noexcept;
    [[nodiscard]] bool SaveHotkeyField(bool showValidationError) noexcept;
    void LoadGlobalFields() noexcept;
    void RenderPreview() noexcept;
    void ShowPage(int page) noexcept;
    void ImportConfiguration() noexcept;
    void ExportConfiguration() noexcept;
    void AddItem(bool submenu) noexcept;
    void RefreshInstalledApps() noexcept;
    void CancelInstalledAppDiscovery() noexcept;
    void AddCheckedApps() noexcept;
    void AddStarterPacks() noexcept;
    void ChooseAccentColor() noexcept;
    void AddDroppedFiles(HDROP drop) noexcept;
    void DuplicateSelectedItem() noexcept;
    void TestSelectedItem() noexcept;
    void FilterInstalledApps() noexcept;
    void MoveItemToFlatIndex(int flatIndex, bool after) noexcept;
    void RemoveItem() noexcept;
    void MoveItem(int direction) noexcept;
    void Save() noexcept;
    void ResetToDefaults() noexcept;
    [[nodiscard]] WheelItem* ItemAt(const std::vector<std::size_t>& path) noexcept;
    [[nodiscard]] std::vector<WheelItem>* SiblingsAt(const std::vector<std::size_t>& path) noexcept;
    [[nodiscard]] std::wstring NextId() noexcept;
    [[nodiscard]] int FlatIndexOf(const std::vector<std::size_t>& path) const noexcept;

    HINSTANCE instance_{};
    HWND owner_{};
    HWND window_{};
    HWND itemList_{};
    HWND brandTitle_{};
    HWND previewHost_{};
    HWND appFilter_{};
    HWND appList_{};
    HWND catalogStatus_{};
    HWND labelEdit_{};
    HWND targetEdit_{};
    HWND iconEdit_{};
    HWND enabledCheck_{};
    HWND quickKeyEdit_{};
    HWND actionCombo_{};
    HWND windowActionCombo_{};
    HWND placementCombo_{};
    HWND hotkeyEdit_{};
    HWND rightClickCheck_{};
    HWND outerExitCheck_{};
    HWND quickKeysCheck_{};
    HWND wheelSizeEdit_{};
    HWND outerRadiusEdit_{};
    HWND innerRadiusEdit_{};
    HWND neutralRadiusEdit_{};
    HWND selectionRadiusEdit_{};
    HWND wheelOpacityEdit_{};
    HWND scrimEdit_{};
    HWND accentEdit_{};
    HWND reducedMotionCheck_{};
    HFONT font_{};
    HFONT titleFont_{};
    HBRUSH backgroundBrush_{};
    HBRUSH fieldBrush_{};
    UINT dpi_{USER_DEFAULT_SCREEN_DPI};
    SaveCallback save_{};
    PreviewCallback preview_{};
    TestCallback test_{};
    void* context_{};
    bool ownsClassRegistration_{false};
    bool updatingControls_{false};
    bool actionEdited_{false};
    bool windowActionEdited_{false};
    Configuration working_{};
    std::vector<std::vector<std::size_t>> flatPaths_{};
    std::vector<InstalledApp> installedApps_{};
    std::vector<std::size_t> visibleAppIndexes_{};
    std::vector<std::size_t> selectedPath_{};
    std::vector<std::size_t> dragSourcePath_{};
    std::size_t nextId_{1};
    UINT dragListMessage_{};
    std::shared_ptr<AppDiscoveryState> appDiscoveryState_{};
    std::vector<std::pair<HWND, int>> pageControls_{};
    int currentPage_{};
};

}  // namespace orbitdeck
