#pragma once

#include "config/Configuration.h"

#include <windows.h>

#include <mmsystem.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

namespace orbitdeck {

[[nodiscard]] bool EvaluateExpression(std::wstring_view expression, double& result) noexcept;

class MiniAppWindow {
public:
    MiniAppWindow() = default;
    ~MiniAppWindow() noexcept;

    MiniAppWindow(const MiniAppWindow&) = delete;
    MiniAppWindow& operator=(const MiniAppWindow&) = delete;

    [[nodiscard]] bool Create(HINSTANCE instance, HWND owner) noexcept;
    void Show(BuiltInTool tool, POINT anchor) noexcept;
    [[nodiscard]] bool PrepareForShutdown() noexcept;
    void Destroy() noexcept;
    void SetToolAvailable(BuiltInTool tool, bool available) noexcept;
    void SetCaffeineAvailable(bool available) noexcept;
    [[nodiscard]] bool IsVisible() const noexcept;
    [[nodiscard]] HWND Handle() const noexcept;

private:
    struct MicBuffers {
        std::array<WAVEHDR, 2> headers{};
        std::array<std::array<short, 1024>, 2> samples{};
    };

    [[nodiscard]] static constexpr const wchar_t* ClassName() noexcept {
        return L"OrbitDeck.MiniAppWindow.v1";
    }

    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam,
                                    LPARAM lParam) noexcept;
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept;
    [[nodiscard]] bool CreateControls() noexcept;
    void UpdateFont(UINT dpi) noexcept;
    void ScaleChildren(UINT oldDpi, UINT newDpi) noexcept;
    void ConfigureTool(BuiltInTool tool) noexcept;
    void PrimaryAction() noexcept;
    void SecondaryAction() noexcept;
    void UpdateTimer() noexcept;
    void UpdateSystemGlance() noexcept;
    [[nodiscard]] bool StartMirror() noexcept;
    void StopMirror() noexcept;
    [[nodiscard]] bool StartMicCheck() noexcept;
    [[nodiscard]] bool StopMicCheck() noexcept;
    void HandleMicData(HWAVEIN source, WAVEHDR* header) noexcept;
    [[nodiscard]] bool LoadScratchpad() noexcept;
    [[nodiscard]] bool SaveScratchpad() noexcept;
    [[nodiscard]] bool SaveScratchpadRecovery() noexcept;
    void Hide() noexcept;
    void SetPinned(bool pinned) noexcept;

    HINSTANCE instance_{};
    HWND owner_{};
    HWND window_{};
    HWND display_{};
    HWND input_{};
    HWND preset_{};
    HWND primary_{};
    HWND secondary_{};
    HWND pin_{};
    HWND close_{};
    HWND cameraPreview_{};
    HFONT font_{};
    UINT dpi_{USER_DEFAULT_SCREEN_DPI};
    bool ownsClassRegistration_{false};
    BuiltInTool tool_{BuiltInTool::None};
    bool pinned_{false};
    bool timerRunning_{false};
    bool caffeineActive_{false};
    bool caffeineAvailable_{true};
    bool scratchpadLoaded_{false};
    int scratchpadRecoverySlot_{-1};
    std::uint64_t scratchpadGeneration_{0};
    HWAVEIN waveIn_{};
    bool micCapturing_{false};
    int micCleanupAttempts_{0};
    std::unique_ptr<MicBuffers> micBuffers_{};
    std::uint64_t micSessionGeneration_{0};
    ULONGLONG timerEnd_{0};
    ULONGLONG timerRemainingMs_{5 * 60 * 1000};
};

}  // namespace orbitdeck
