#pragma once

#include <windows.h>

namespace orbitdeck {

class MainMessageWindow {
public:
    using MessageCallback = LRESULT (*)(void* context, HWND window, UINT message, WPARAM wParam,
                                        LPARAM lParam);

    MainMessageWindow() = default;
    ~MainMessageWindow() noexcept;

    MainMessageWindow(const MainMessageWindow&) = delete;
    MainMessageWindow& operator=(const MainMessageWindow&) = delete;

    [[nodiscard]] bool Create(HINSTANCE instance, MessageCallback callback, void* context) noexcept;
    void Destroy() noexcept;

    [[nodiscard]] HWND Handle() const noexcept;

private:
    [[nodiscard]] static constexpr const wchar_t* ClassName() noexcept {
        return L"OrbitDeck.MainMessageWindow.v1";
    }

    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam,
                                    LPARAM lParam) noexcept;

    HINSTANCE instance_{};
    HWND window_{};
    MessageCallback callback_{};
    void* context_{};
    bool ownsClassRegistration_{false};
};

}  // namespace orbitdeck
