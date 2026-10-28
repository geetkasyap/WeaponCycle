// Proves the IconCache never-crash / never-null contract that backs the Phase 5
// acceptance "missing icons do not crash". Plain return-code harness (asserts
// compile out under NDEBUG, so the checks must run unconditionally).

#include "overlay/IconCache.h"

#include <objbase.h>
#include <windows.h>

#include <cstdio>

namespace {

[[nodiscard]] UINT PixelCount(IWICBitmapSource* const bitmap) {
    if (bitmap == nullptr) {
        return 0;
    }
    UINT width = 0;
    UINT height = 0;
    if (FAILED(bitmap->GetSize(&width, &height))) {
        return 0;
    }
    return width * height;
}

}  // namespace

int main() {
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    orbitdeck::IconCache cache;
    if (!cache.Initialize()) {
        return 1;
    }

    // Empty source falls back to a generated, non-empty icon.
    IWICBitmapSource* const fallback = cache.Get(L"");
    if (fallback == nullptr || PixelCount(fallback) == 0) {
        return 2;
    }

    // A missing path must not crash and must still yield a fallback.
    IWICBitmapSource* const missing = cache.Get(L"Z:\\no\\such\\icon.png");
    if (missing == nullptr || PixelCount(missing) == 0) {
        return 3;
    }

    // The same source string returns the identical cached instance.
    if (cache.Get(L"") != fallback) {
        return 4;
    }

    // A system executable decodes without crashing; whether it
    // carries an embedded icon or falls back, the result is a usable bitmap.
    wchar_t system[MAX_PATH] = {};
    const UINT length = ::GetSystemDirectoryW(system, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return 5;
    }
    const std::wstring module = std::wstring(system) + L"\\notepad.exe";
    IWICBitmapSource* const executable = cache.Get(module);
    if (executable == nullptr || PixelCount(executable) == 0) {
        return 6;
    }

    if (cache.GetCached(module) != executable)
        return 7;
    if (cache.Get(L"notepad.exe") == nullptr)
        return 13;
    if (!cache.IsFallback(module) && cache.IsFallback(L"notepad.exe"))
        return 14;
    const std::wstring url = L"https://weapon-cycle.invalid/image.png";
    if (cache.Get(url) == nullptr || !cache.IsFallback(url))
        return 15;
    if (cache.GetCached(L"C:\\not-loaded-by-the-draw-path.png") != nullptr)
        return 8;
    const HWND target = ::CreateWindowExW(0, L"STATIC", L"Icon test", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                          nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (target == nullptr)
        return 9;
    cache.ClearEntries();
    if (!cache.Prepare({module}, target, WM_APP + 72))
        return 10;
    const ULONGLONG deadline = ::GetTickCount64() + 10000;
    while (cache.GetCached(module) == nullptr && ::GetTickCount64() < deadline) {
        (void)::MsgWaitForMultipleObjectsEx(0, nullptr, 25, QS_ALLINPUT, 0);
        MSG message{};
        while (::PeekMessageW(&message, target, 0, 0, PM_REMOVE)) {
            ::DispatchMessageW(&message);
        }
        (void)cache.Collect();
    }
    if (cache.GetCached(module) == nullptr)
        return 11;
    cache.ClearEntries();
    (void)cache.Collect();
    if (cache.GetCached(module) != nullptr)
        return 12;
    (void)::DestroyWindow(target);
    cache.Clear();

    if (SUCCEEDED(com)) {
        ::CoUninitialize();
    }

    std::puts("IconCache tests passed");
    return 0;
}
