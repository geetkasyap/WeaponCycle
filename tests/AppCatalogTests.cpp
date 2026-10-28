#include "shell/AppCatalog.h"

#include <objbase.h>

int main() {
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::wstring error;
    const auto apps = orbitdeck::DiscoverInstalledApps(error);
    if (apps.empty())
        return 1;
    for (std::size_t index = 0; index < apps.size(); ++index) {
        if (apps[index].name.empty() || apps[index].target.empty() ||
            apps[index].iconSource.empty()) {
            return 2;
        }
        if (index > 0 && ::CompareStringOrdinal(apps[index - 1].name.c_str(),
                                                static_cast<int>(apps[index - 1].name.size()),
                                                apps[index].name.c_str(),
                                                static_cast<int>(apps[index].name.size()),
                                                TRUE) == CSTR_GREATER_THAN) {
            return 3;
        }
    }
    if (SUCCEEDED(com))
        ::CoUninitialize();
    return 0;
}
