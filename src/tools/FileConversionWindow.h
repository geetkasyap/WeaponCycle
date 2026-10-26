#pragma once

#include <windows.h>
#include <shellapi.h>
#include "tools/ConversionEngine.h"

#include <memory>
#include <string>

namespace orbitdeck {

class FileConversionWindow {
public:
    FileConversionWindow() = default;
    ~FileConversionWindow() noexcept;
    FileConversionWindow(const FileConversionWindow&) = delete;
    FileConversionWindow& operator=(const FileConversionWindow&) = delete;

    [[nodiscard]] bool Create(HINSTANCE instance, HWND owner) noexcept;
    void Show(POINT anchor, const std::wstring& source = {}, HWND explorer = nullptr) noexcept;
    void Destroy() noexcept;
    [[nodiscard]] bool IsVisible() const noexcept;
    [[nodiscard]] HWND Handle() const noexcept {
        return window_;
    }

private:
    struct Job;
    enum class Operation { Inspect, Convert, Install, Selection };
    static constexpr UINT kCompletedMessage{WM_APP + 80};
    static constexpr UINT_PTR kJobTimerId{81};
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM) noexcept;
    LRESULT HandleMessage(UINT, WPARAM, LPARAM) noexcept;
    [[nodiscard]] bool CreateControls() noexcept;
    void Layout() noexcept;
    void ChooseSource() noexcept;
    void SetSource(std::wstring path) noexcept;
    void Convert() noexcept;
    void Complete() noexcept;
    void Cancel() noexcept;
    void InstallRequired() noexcept;
    void StartJob(Operation operation, HWND explorer = nullptr) noexcept;
    void UpdateTarget() noexcept;
    void ShowProgress() noexcept;
    void SetBusy(bool busy) noexcept;
    void DropFiles(HDROP drop) noexcept;

    HINSTANCE instance_{};
    HWND window_{};
    HWND heading_{};
    HWND path_{};
    HWND format_{};
    HWND status_{};
    HWND progress_{};
    HFONT font_{};
    HFONT titleFont_{};
    HBRUSH background_{};
    HBRUSH field_{};
    bool ownsClass_{};
    std::wstring sourcePath_;
    std::wstring outputPath_;
    std::wstring desiredFormat_;
    std::vector<ConversionTarget> targets_;
    bool fallbackImaging_{};
    int shownProgress_{-1};
    std::wstring shownProgressMessage_;
    std::shared_ptr<Job> job_;
};

}  // namespace orbitdeck
