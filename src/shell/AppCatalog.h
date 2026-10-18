#pragma once

#include "launch/Launcher.h"

#include <atomic>
#include <string>
#include <vector>

namespace orbitdeck {

struct InstalledApp {
    std::wstring name;
    std::wstring target;
    std::wstring iconSource;
    LaunchKind launchKind{LaunchKind::Shell};
};

// Enumerates the Windows AppsFolder only when Preferences asks for it. This
// covers packaged and desktop Start-menu apps without a background indexer.
[[nodiscard]] std::vector<InstalledApp> DiscoverInstalledApps(
    std::wstring& error, const std::atomic_bool* cancel = nullptr) noexcept;

}  // namespace orbitdeck
