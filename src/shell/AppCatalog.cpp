#include "shell/AppCatalog.h"

#include <windows.h>

#include <propsys.h>
#include <propkey.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <set>

namespace orbitdeck {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring DisplayName(IShellItem* const item, const SIGDN form) {
    if (item == nullptr)
        return {};
    PWSTR raw = nullptr;
    const HRESULT result = item->GetDisplayName(form, &raw);
    std::wstring value;
    if (SUCCEEDED(result) && raw != nullptr)
        value.assign(raw);
    if (raw != nullptr)
        ::CoTaskMemFree(raw);
    return value;
}

std::wstring PropertyString(IShellItem2* const item, const PROPERTYKEY& key) {
    if (item == nullptr)
        return {};
    PWSTR raw = nullptr;
    const HRESULT result = item->GetString(key, &raw);
    std::wstring value;
    if (SUCCEEDED(result) && raw != nullptr)
        value.assign(raw);
    if (raw != nullptr)
        ::CoTaskMemFree(raw);
    return value;
}

struct CaseInsensitiveLess {
    bool operator()(const std::wstring& left, const std::wstring& right) const noexcept {
        return ::CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
                                      static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
    }
};

}  // namespace

std::vector<InstalledApp> DiscoverInstalledApps(std::wstring& error,
                                                const std::atomic_bool* const cancel) noexcept {
    error.clear();
    std::vector<InstalledApp> apps;

    ComPtr<IShellItem> folder;
    HRESULT result = ::SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr,
                                            IID_PPV_ARGS(folder.GetAddressOf()));
    if (FAILED(result)) {
        error = L"Windows AppsFolder is unavailable.";
        return apps;
    }

    ComPtr<IEnumShellItems> enumerator;
    result =
        folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(enumerator.GetAddressOf()));
    if (FAILED(result)) {
        error = L"Windows could not enumerate installed apps.";
        return apps;
    }

    std::set<std::wstring, CaseInsensitiveLess> seenTargets;
    for (;;) {
        if (cancel != nullptr && cancel->load())
            break;
        ComPtr<IShellItem> shellItem;
        ULONG fetched = 0;
        result = enumerator->Next(1, shellItem.GetAddressOf(), &fetched);
        if (result == S_FALSE || fetched == 0)
            break;
        if (FAILED(result)) {
            error = L"Windows stopped enumerating installed apps.";
            break;
        }

        const std::wstring name = DisplayName(shellItem.Get(), SIGDN_NORMALDISPLAY);
        const std::wstring parsingName = DisplayName(shellItem.Get(), SIGDN_DESKTOPABSOLUTEPARSING);
        if (name.empty() || parsingName.empty())
            continue;

        ComPtr<IShellItem2> item2;
        (void)shellItem.As(&item2);
        const std::wstring aumid = PropertyString(item2.Get(), PKEY_AppUserModel_ID);

        InstalledApp app;
        app.name = name;
        if (!aumid.empty()) {
            app.target = aumid;
            app.iconSource = L"shell:AppsFolder\\" + aumid;
            app.launchKind = LaunchKind::Aumid;
        } else {
            app.target = parsingName;
            app.iconSource = parsingName;
            app.launchKind = LaunchKind::Shell;
        }
        if (!seenTargets.insert(app.target).second)
            continue;
        apps.push_back(std::move(app));
    }

    std::sort(apps.begin(), apps.end(), [](const InstalledApp& left, const InstalledApp& right) {
        return ::CompareStringOrdinal(left.name.c_str(), static_cast<int>(left.name.size()),
                                      right.name.c_str(), static_cast<int>(right.name.size()),
                                      TRUE) == CSTR_LESS_THAN;
    });
    return apps;
}

}  // namespace orbitdeck
