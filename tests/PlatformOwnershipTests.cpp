#include "platform/UniqueHandle.h"

#include <cstdio>
#include <type_traits>
#include <utility>

int main() {
    using orbitdeck::UniqueHandle;
    static_assert(!std::is_copy_constructible_v<UniqueHandle>);
    static_assert(!std::is_copy_assignable_v<UniqueHandle>);
    static_assert(std::is_nothrow_move_constructible_v<UniqueHandle>);
    static_assert(std::is_nothrow_move_assignable_v<UniqueHandle>);

    UniqueHandle empty;
    UniqueHandle invalid{INVALID_HANDLE_VALUE};
    if (empty || invalid) {
        return 1;
    }
    UniqueHandle first{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!first) {
        return 2;
    }
    HANDLE original = first.Get();
    UniqueHandle moved{std::move(first)};
    if (first || moved.Get() != original) {
        return 3;
    }
    UniqueHandle second{::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!second) {
        return 4;
    }
    HANDLE displaced = second.Get();
    second = std::move(moved);
    DWORD flags{};
    if (moved || second.Get() != original || ::GetHandleInformation(displaced, &flags)) {
        return 5;
    }
    HANDLE released = second.Release();
    if (second || !::GetHandleInformation(released, &flags)) {
        return 6;
    }
    second.Reset(released);
    second.Reset(second.Get());
    if (!::GetHandleInformation(released, &flags)) {
        return 7;
    }
    ::SetLastError(ERROR_BUSY);
    second.Reset();
    if (::GetLastError() != ERROR_BUSY || ::GetHandleInformation(released, &flags)) {
        return 8;
    }
    UniqueHandle outParameter;
    *outParameter.Put() = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE output = outParameter.Get();
    if (!outParameter) {
        return 9;
    }
    (void)outParameter.Put();
    if (outParameter || ::GetHandleInformation(output, &flags)) {
        return 10;
    }
    std::puts("Move-only Windows handle ownership and last-error preservation passed");
    return 0;
}
