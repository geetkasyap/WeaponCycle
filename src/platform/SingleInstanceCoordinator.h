#pragma once

#include <windows.h>
#include <string>

namespace orbitdeck {

class SingleInstanceCoordinator {
public:
    enum class StartResult {
        Primary,
        ExistingSignaled,
        Error,
    };

    SingleInstanceCoordinator() = default;
    ~SingleInstanceCoordinator() noexcept;

    SingleInstanceCoordinator(const SingleInstanceCoordinator&) = delete;
    SingleInstanceCoordinator& operator=(const SingleInstanceCoordinator&) = delete;

    [[nodiscard]] StartResult Start(bool signalExisting = true) noexcept;
    void Stop() noexcept;
    [[nodiscard]] static UINT ActivationMessage() noexcept;
    [[nodiscard]] static bool ForwardFile(const std::wstring& path) noexcept;
    static constexpr ULONG_PTR kConvertFileTag{0x57434346};

private:
    HANDLE mutex_{};
};

}  // namespace orbitdeck
