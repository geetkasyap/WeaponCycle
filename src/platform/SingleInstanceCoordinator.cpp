#include "platform/SingleInstanceCoordinator.h"

namespace orbitdeck {

SingleInstanceCoordinator::~SingleInstanceCoordinator() noexcept {
    Stop();
}

void SingleInstanceCoordinator::Stop() noexcept {
    if (mutex_ != nullptr) {
        ::CloseHandle(mutex_);
        mutex_ = nullptr;
    }
}

SingleInstanceCoordinator::StartResult SingleInstanceCoordinator::Start(
    const bool signalExisting) noexcept {
    if (mutex_ != nullptr) {
        return StartResult::Primary;
    }

    const UINT activationMessage = ActivationMessage();
    if (activationMessage == 0) {
        return StartResult::Error;
    }

    ::SetLastError(ERROR_SUCCESS);
    const HANDLE mutex = ::CreateMutexW(nullptr, FALSE, L"Local\\OrbitDeck.SingleInstance.v1");
    const DWORD creationStatus = ::GetLastError();
    if (mutex == nullptr) {
        return StartResult::Error;
    }

    if (creationStatus == ERROR_ALREADY_EXISTS) {
        const BOOL posted =
            !signalExisting || ::PostMessageW(HWND_BROADCAST, activationMessage, 0, 0);
        const DWORD postError = posted ? ERROR_SUCCESS : ::GetLastError();
        ::CloseHandle(mutex);

        if (!posted) {
            ::SetLastError(postError);
            return StartResult::Error;
        }
        return StartResult::ExistingSignaled;
    }

    mutex_ = mutex;
    return StartResult::Primary;
}

UINT SingleInstanceCoordinator::ActivationMessage() noexcept {
    return ::RegisterWindowMessageW(L"OrbitDeck.Activate.v1");
}

bool SingleInstanceCoordinator::ForwardFile(const std::wstring& path) noexcept {
    HWND receiver{};
    for (HWND window =
             ::FindWindowExW(nullptr, nullptr, L"OrbitDeck.MainMessageWindow.v1", nullptr);
         window != nullptr;
         window = ::FindWindowExW(nullptr, window, L"OrbitDeck.MainMessageWindow.v1", nullptr)) {
        DWORD process{};
        (void)::GetWindowThreadProcessId(window, &process);
        if (process != ::GetCurrentProcessId()) {
            receiver = window;
            break;
        }
    }
    if (receiver == nullptr || path.empty() || path.size() >= 32768)
        return false;
    DWORD process{};
    (void)::GetWindowThreadProcessId(receiver, &process);
    (void)::AllowSetForegroundWindow(process);
    COPYDATASTRUCT data{};
    data.dwData = kConvertFileTag;
    data.cbData = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
    data.lpData = const_cast<wchar_t*>(path.c_str());
    DWORD_PTR accepted{};
    return ::SendMessageTimeoutW(receiver, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &accepted) != 0 &&
           accepted != 0;
}

}  // namespace orbitdeck
