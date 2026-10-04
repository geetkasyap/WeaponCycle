#include "platform/MainMessageWindow.h"

namespace orbitdeck {

MainMessageWindow::~MainMessageWindow() noexcept {
    Destroy();
}

bool MainMessageWindow::Create(const HINSTANCE instance, const MessageCallback callback,
                               void* const context) noexcept {
    if (instance == nullptr || callback == nullptr) {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    if (window_ != nullptr) {
        ::SetLastError(ERROR_ALREADY_EXISTS);
        return false;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &MainMessageWindow::WndProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = ClassName();

    const ATOM classAtom = ::RegisterClassExW(&windowClass);
    if (classAtom == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    instance_ = instance;
    callback_ = callback;
    context_ = context;
    ownsClassRegistration_ = classAtom != 0;

    const HWND window = ::CreateWindowExW(WS_EX_TOOLWINDOW, ClassName(), L"OrbitDeck", WS_POPUP, 0,
                                          0, 0, 0, nullptr, nullptr, instance, this);
    if (window == nullptr) {
        const DWORD createError = ::GetLastError();
        if (ownsClassRegistration_) {
            ::UnregisterClassW(ClassName(), instance_);
        }
        instance_ = nullptr;
        callback_ = nullptr;
        context_ = nullptr;
        ownsClassRegistration_ = false;
        ::SetLastError(createError);
        return false;
    }

    window_ = window;
    return true;
}

void MainMessageWindow::Destroy() noexcept {
    if (window_ != nullptr) {
        const HWND window = window_;
        if (!::DestroyWindow(window)) {
            // DestroyWindow is thread-affine; detach before requesting closure.
            ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            ::PostMessageW(window, WM_CLOSE, 0, 0);
        }
        window_ = nullptr;
    }

    if (ownsClassRegistration_) {
        ::UnregisterClassW(ClassName(), instance_);
    }

    instance_ = nullptr;
    callback_ = nullptr;
    context_ = nullptr;
    ownsClassRegistration_ = false;
}

HWND MainMessageWindow::Handle() const noexcept {
    return window_;
}

LRESULT CALLBACK MainMessageWindow::WndProc(const HWND window, const UINT message,
                                            const WPARAM wParam, const LPARAM lParam) noexcept {
    auto* owner = reinterpret_cast<MainMessageWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE && owner == nullptr) {
        if (lParam == 0) {
            return FALSE;
        }
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        owner = static_cast<MainMessageWindow*>(create->lpCreateParams);
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

    if (owner == nullptr) {
        return ::DefWindowProcW(window, message, wParam, lParam);
    }

    const MessageCallback callback = owner->callback_;
    void* const context = owner->context_;

    if (message == WM_NCDESTROY) {
        owner->window_ = nullptr;
        ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }

    return callback != nullptr ? callback(context, window, message, wParam, lParam)
                               : ::DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace orbitdeck
