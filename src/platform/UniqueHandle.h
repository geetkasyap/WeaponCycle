#pragma once

#include <windows.h>

#include <utility>

namespace orbitdeck {

class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~UniqueHandle() noexcept {
        Reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.Release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE Get() const noexcept {
        return handle_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] HANDLE Release() noexcept {
        return std::exchange(handle_, nullptr);
    }

    void Reset(HANDLE replacement = nullptr) noexcept {
        if (handle_ == replacement) {
            return;
        }
        const DWORD previousError = ::GetLastError();
        if (*this && !::CloseHandle(handle_)) {
            ::OutputDebugStringW(L"Weapon Cycle: closing an owned Windows handle failed.\n");
        }
        handle_ = replacement;
        ::SetLastError(previousError);
    }

    [[nodiscard]] HANDLE* Put() noexcept {
        Reset();
        return &handle_;
    }

private:
    HANDLE handle_{};
};

}  // namespace orbitdeck
