#include "app/OrbitDeckApp.h"

#include <objbase.h>
#include <shellapi.h>
#include <windows.h>

#include <string_view>
namespace {

class ComApartment {
public:
    ComApartment() noexcept
        : result_(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}

    ~ComApartment() noexcept {
        if (SUCCEEDED(result_)) {
            ::CoUninitialize();
        }
    }

    [[nodiscard]] bool Ready() const noexcept {
        return SUCCEEDED(result_);
    }

private:
    HRESULT result_{};
};

void ShowStartupError(const wchar_t* message) noexcept {
    (void)::MessageBoxW(nullptr, message, L"Weapon Cycle", MB_OK | MB_ICONERROR | MB_TASKMODAL);
}

}  // namespace

int RunOrbitDeck(const HINSTANCE instance) noexcept {
    int count{};
    wchar_t** arguments = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (arguments == nullptr) {
        ShowStartupError(L"Weapon Cycle could not read its launch arguments.");
        return 1;
    }
    const bool background = count == 2 && std::wstring_view(arguments[1]) == L"--background";
    const bool convert = count == 3 && std::wstring_view(arguments[1]) == L"--convert";
    const bool droppedFile = count == 2 && !background && arguments[1][0] != L'-';
    const std::wstring source = convert ? arguments[2] : droppedFile ? arguments[1] : L"";
    const bool supported = count == 1 || background || convert || droppedFile;
    (void)::LocalFree(arguments);
    if (!supported) {
        ShowStartupError(
            L"Open Weapon Cycle without arguments for Preferences, or use "
            L"--background for quiet tray startup, or --convert \"file path\".");
        return 1;
    }
    if (!::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
        ::GetLastError() != ERROR_ACCESS_DENIED) {
        ShowStartupError(L"Weapon Cycle could not enable per-monitor DPI awareness.");
        return 1;
    }

    const ComApartment com;
    if (!com.Ready()) {
        ShowStartupError(L"Weapon Cycle could not initialize Windows COM support.");
        return 1;
    }

    orbitdeck::OrbitDeckApp app;
    return app.Run(instance, background, source);
}

#ifdef ORBITDECK_DIAGNOSTIC_CONSOLE
int wmain() {
    return RunOrbitDeck(::GetModuleHandleW(nullptr));
}
#else
int WINAPI wWinMain(const HINSTANCE instance, HINSTANCE, wchar_t*, int) {
    return RunOrbitDeck(instance);
}
#endif
