#include "shell/FileSelection.h"

#include <objbase.h>
#include <exdisp.h>
#include <servprov.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace orbitdeck {
namespace {
constexpr wchar_t kConversionMenu[]{L"Software\\Classes\\*\\shell\\WeaponCycle.Convert"};

std::wstring ConversionCommand() {
    std::wstring path(32768, L'\0');
    const DWORD length =
        ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    path.resize(length);
    return L"\"" + path + L"\" --convert \"%1\"";
}
}  // namespace

SelectedFile ExplorerSelectedFile(const HWND explorer) noexcept {
    SelectedFile result;
    wchar_t className[64]{};
    (void)::GetClassNameW(explorer, className, 64);
    const bool desktop = wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0;
    if (!desktop && wcscmp(className, L"CabinetWClass") != 0 &&
        wcscmp(className, L"ExploreWClass") != 0)
        return result;
    const HRESULT com =
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) {
        result.error =
            L"Windows could not read Explorer's selection. Choose or drop a file instead.";
        return result;
    }
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IShellWindows> windows;
        HRESULT hr = ::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
                                        IID_PPV_ARGS(windows.GetAddressOf()));
        LONG count{};
        if (SUCCEEDED(hr))
            hr = windows->get_Count(&count);
        for (LONG index = desktop ? -1 : 0; windows != nullptr && index < count; ++index) {
            if (desktop && index >= 0)
                break;
            ComPtr<IDispatch> dispatch;
            if (index == -1) {
                VARIANT location{};
                location.vt = VT_I4;
                location.lVal = CSIDL_DESKTOP;
                VARIANT empty{};
                LONG window{};
                hr = windows->FindWindowSW(&location, &empty, SWC_DESKTOP, &window,
                                           SWFO_NEEDDISPATCH, dispatch.GetAddressOf());
            } else {
                VARIANT item{};
                item.vt = VT_I4;
                item.lVal = index;
                hr = windows->Item(item, dispatch.GetAddressOf());
            }
            if (FAILED(hr) || dispatch == nullptr)
                continue;
            if (!desktop) {
                ComPtr<IWebBrowserApp> browser;
                SHANDLE_PTR handle{};
                if (FAILED(dispatch.As(&browser)) || FAILED(browser->get_HWND(&handle)) ||
                    reinterpret_cast<HWND>(handle) != explorer)
                    continue;
            }
            ComPtr<IServiceProvider> services;
            ComPtr<IShellBrowser> browser;
            ComPtr<IShellView> view;
            ComPtr<IFolderView2> folder;
            ComPtr<IShellItemArray> selected;
            if (FAILED(dispatch.As(&services)) ||
                FAILED(services->QueryService(SID_STopLevelBrowser,
                                              IID_PPV_ARGS(browser.GetAddressOf()))) ||
                FAILED(browser->QueryActiveShellView(view.GetAddressOf())) ||
                FAILED(view.As(&folder)) ||
                FAILED(folder->GetSelection(FALSE, selected.GetAddressOf())))
                continue;
            HWND viewWindow{};
            if (!desktop && SUCCEEDED(view->GetWindow(&viewWindow)) &&
                !::IsWindowVisible(viewWindow))
                continue;
            DWORD selectedCount{};
            if (FAILED(selected->GetCount(&selectedCount)) || selectedCount == 0)
                continue;
            if (selectedCount != 1) {
                result.error =
                    L"Select one file to convert. Multiple selected files are not silently reduced "
                    L"to one.";
                break;
            }
            ComPtr<IShellItem> item;
            PWSTR path{};
            if (SUCCEEDED(selected->GetItemAt(0, item.GetAddressOf())) &&
                SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                const DWORD attributes = ::GetFileAttributesW(path);
                if (attributes != INVALID_FILE_ATTRIBUTES &&
                    (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    result.path = path;
                } else {
                    result.error = L"Choose a file, not a folder or virtual Explorer item.";
                }
                ::CoTaskMemFree(path);
                break;
            }
            if (desktop)
                break;
        }
        if (FAILED(hr))
            result.error =
                L"Explorer could not supply its selection. Choose or drop a file instead.";
    }
    ::CoUninitialize();
    return result;
}

bool ConversionContextMenuEnabled() noexcept {
    std::wstring command(32768, L'\0');
    DWORD bytes = static_cast<DWORD>(command.size() * sizeof(wchar_t));
    const std::wstring key = std::wstring(kConversionMenu) + L"\\command";
    if (::RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr,
                       command.data(), &bytes) != ERROR_SUCCESS)
        return false;
    command.resize(wcsnlen_s(command.c_str(), command.size()));
    return command == ConversionCommand();
}

bool SetConversionContextMenu(const bool enabled, std::wstring& error) noexcept {
    LONG status{};
    if (!enabled) {
        status = ::RegDeleteTreeW(HKEY_CURRENT_USER, kConversionMenu);
        if (status == ERROR_FILE_NOT_FOUND)
            status = ERROR_SUCCESS;
    } else {
        const std::wstring command = ConversionCommand();
        if (command.empty()) {
            error = L"Weapon Cycle could not determine its executable path.";
            return false;
        }
        HKEY key{};
        status = ::RegCreateKeyExW(HKEY_CURRENT_USER, kConversionMenu, 0, nullptr,
                                   REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_CREATE_SUB_KEY,
                                   nullptr, &key, nullptr);
        if (status == ERROR_SUCCESS) {
            constexpr wchar_t label[]{L"Convert with Weapon Cycle"};
            status = ::RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(label),
                                      sizeof(label));
            if (status == ERROR_SUCCESS) {
                constexpr wchar_t single[]{L"Single"};
                status = ::RegSetValueExW(key, L"MultiSelectModel", 0, REG_SZ,
                                          reinterpret_cast<const BYTE*>(single), sizeof(single));
            }
            HKEY action{};
            if (status == ERROR_SUCCESS)
                status = ::RegCreateKeyExW(key, L"command", 0, nullptr, REG_OPTION_NON_VOLATILE,
                                           KEY_SET_VALUE, nullptr, &action, nullptr);
            if (status == ERROR_SUCCESS) {
                status = ::RegSetValueExW(
                    action, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                    static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
            }
            if (action != nullptr)
                (void)::RegCloseKey(action);
            (void)::RegCloseKey(key);
        }
    }
    if (status != ERROR_SUCCESS) {
        error = L"The Explorer menu could not be changed (Windows error " +
                std::to_wstring(status) + L"). No default file associations were changed.";
        return false;
    }
    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

}  // namespace orbitdeck
