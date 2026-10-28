#include "tools/ConversionEngine.h"
#include "shell/FileSelection.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {
int CheckPackagedEngine() {
    GUID id{};
    if (FAILED(::CoCreateGuid(&id))) {
        return 30;
    }
    wchar_t name[40]{};
    if (::StringFromGUID2(id, name, 40) == 0) {
        return 31;
    }
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / (L"WeaponCycle-Bridge-" + std::wstring(name));
    if (!std::filesystem::create_directory(folder)) {
        return 32;
    }
    struct Fixture {
        std::filesystem::path folder;
        ~Fixture() {
            std::error_code error;
            std::filesystem::remove_all(folder, error);
            if (error) {
                std::fprintf(stderr, "The native bridge fixture could not be removed.\n");
            }
        }
    } fixture{folder};
    const auto source = folder / L"native transport.txt";
    {
        std::ofstream file(source);
        file << "Native transport ownership and typed protocol test.";
    }
    std::atomic_bool cancel{};
    const auto formats = orbitdeck::DiscoverConversionFormats(source.wstring(), cancel);
    const bool hasWord =
        std::any_of(formats.targets.begin(), formats.targets.end(),
                    [](const auto& target) { return target.id == L"docx" && target.available; });
    if (!formats.error.empty() || !hasWord) {
        std::fwprintf(stderr, L"Native discovery failed: %ls\n", formats.error.c_str());
        return 33;
    }
    const auto result = orbitdeck::RunFullConversion(source.wstring(), L"docx", cancel, {});
    if (!result.succeeded || result.outputPaths.size() != 1 ||
        !std::filesystem::exists(result.outputPath) ||
        std::filesystem::file_size(result.outputPath) == 0) {
        std::fwprintf(stderr, L"Native conversion failed: %ls\n", result.error.c_str());
        return 34;
    }
    std::ifstream original(source);
    const std::string content{std::istreambuf_iterator<char>(original), {}};
    if (content != "Native transport ownership and typed protocol test.") {
        return 35;
    }
    std::puts("Packaged native pipe/process/protocol bridge passed");
    return 0;
}

int CheckContextMenu() {
    GUID id{};
    if (FAILED(::CoCreateGuid(&id)))
        return 10;
    wchar_t name[40]{};
    if (::StringFromGUID2(id, name, 40) == 0)
        return 11;
    const std::wstring path = L"Software\\OrbitDeck.MenuTest." + std::wstring(name);
    HKEY key{};
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                          KEY_ALL_ACCESS, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return 12;
    const bool redirected = ::RegOverridePredefKey(HKEY_CURRENT_USER, key) == ERROR_SUCCESS;
    int result = 13;
    if (redirected) {
        std::wstring error;
        if (orbitdeck::ConversionContextMenuEnabled())
            result = 14;
        else if (!orbitdeck::SetConversionContextMenu(true, error))
            result = 15;
        else if (!orbitdeck::ConversionContextMenuEnabled())
            result = 16;
        else if (!orbitdeck::SetConversionContextMenu(false, error))
            result = 17;
        else
            result = orbitdeck::ConversionContextMenuEnabled() ? 18 : 0;
        if (result != 0)
            std::fwprintf(stderr, L"Context menu check %d: %ls\n", result, error.c_str());
        if (::RegOverridePredefKey(HKEY_CURRENT_USER, nullptr) != ERROR_SUCCESS)
            result = 19;
    }
    (void)::RegCloseKey(key);
    (void)::RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    return result;
}
}  // namespace

int wmain(const int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--integration") {
        return CheckPackagedEngine();
    }
    for (const std::wstring value : {L"", L"plain.png", L"C:\\space folder\\quote \" test.png",
                                     L"C:\\trailing\\", L"\u65E5\u672C\u8A9E.png"}) {
        const std::wstring command = L"test.exe " + orbitdeck::QuoteConversionArgument(value);
        int count{};
        wchar_t** arguments = ::CommandLineToArgvW(command.c_str(), &count);
        if (arguments == nullptr)
            return 1;
        const bool same = count == 2 && arguments[1] == value;
        ::LocalFree(arguments);
        if (!same)
            return 2;
    }
    std::atomic_bool cancel{true};
    const auto stopped = orbitdeck::RunFullConversion(L"missing.png", L"png", cancel, {});
    if (!stopped.cancelled || stopped.succeeded)
        return 3;
    if (!orbitdeck::ExplorerSelectedFile(nullptr).path.empty())
        return 4;
    const auto noPackage = orbitdeck::InstallConversionEngine({}, cancel, {});
    if (noPackage.succeeded || noPackage.error.empty())
        return 5;
    const int menu = CheckContextMenu();
    if (menu != 0)
        return menu;
    std::puts("Conversion engine argument, cancellation and selection guards passed");
    return 0;
}
