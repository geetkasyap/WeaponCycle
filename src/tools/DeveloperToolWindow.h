#pragma once

#include "config/Configuration.h"

#include <windows.h>

namespace orbitdeck {

class DeveloperToolWindow {
public:
    DeveloperToolWindow() = default;
    ~DeveloperToolWindow() noexcept;

    DeveloperToolWindow(const DeveloperToolWindow&) = delete;
    DeveloperToolWindow& operator=(const DeveloperToolWindow&) = delete;

    [[nodiscard]] bool Create(HINSTANCE instance, HWND owner) noexcept;
    void Show(BuiltInTool tool, POINT anchor) noexcept;
    void SetToolAvailable(BuiltInTool tool, bool available) noexcept;
    void Destroy() noexcept;
    [[nodiscard]] bool IsVisible() const noexcept;

private:
    [[nodiscard]] static constexpr const wchar_t* ClassName() noexcept {
        return L"WeaponCycle.DeveloperToolWindow.v1";
    }
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam,
                                    LPARAM lParam) noexcept;
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept;
    [[nodiscard]] bool CreateControls() noexcept;
    void ConfigureTool(BuiltInTool tool) noexcept;
    void RunTool() noexcept;
    void CopyOutput() noexcept;
    void UpdateColorPicker() noexcept;
    void FitToMonitor(HMONITOR monitor) noexcept;
    void Hide() noexcept;

    HINSTANCE instance_{};
    HWND owner_{};
    HWND window_{};
    HWND toolPicker_{};
    HWND description_{};
    HWND modeLabel_{};
    HWND mode_{};
    HWND inputLabel_{};
    HWND input_{};
    HWND secondaryLabel_{};
    HWND secondaryInput_{};
    HWND outputLabel_{};
    HWND output_{};
    HWND run_{};
    HWND copy_{};
    HWND clear_{};
    HWND pin_{};
    HWND close_{};
    HFONT font_{};
    HBRUSH backgroundBrush_{};
    HBRUSH fieldBrush_{};
    float layoutScale_{1.0F};
    BuiltInTool tool_{BuiltInTool::None};
    bool pinned_{false};
    bool updatingToolPicker_{false};
    bool ownsClassRegistration_{false};
};

}  // namespace orbitdeck
