#include "tools/DeveloperTools.h"

#include <windows.h>

#include <fstream>
#include <limits>

int main() {
    std::wstring output;
    std::wstring error;
    if (!orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::JsonEditor, L"{\"a\":1}", L"",
                                     L"Format", output, error) ||
        output.find(L"\"a\": 1") == std::wstring::npos) {
        return 1;
    }
    std::wstring deepJson;
    for (int index = 0; index < 70; ++index)
        deepJson += L"[";
    deepJson += L"0";
    for (int index = 0; index < 70; ++index)
        deepJson += L"]";
    if (orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::JsonEditor, deepJson, L"", L"Format",
                                    output, error)) {
        return 18;
    }
    if (!orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::Base64Text, L"hello", L"", L"Encode",
                                     output, error) ||
        output != L"aGVsbG8=") {
        return 2;
    }
    if (!orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::UrlCodec, L"a b", L"", L"Encode",
                                     output, error) ||
        output != L"a%20b") {
        return 3;
    }
    if (!orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::NumberBase, L"255", L"", L"To hex",
                                     output, error) ||
        output != L"0xff") {
        return 4;
    }
    if (!orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::TextDiff, L"a\nb", L"a\nc", L"Run",
                                     output, error) ||
        output.find(L"- b") == std::wstring::npos || output.find(L"+ c") == std::wstring::npos) {
        return 5;
    }
    std::wstring tooManyLines;
    for (int index = 0; index < 1201; ++index) {
        tooManyLines += L"x\n";
    }
    if (orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::TextDiff, tooManyLines, L"x", L"Run",
                                    output, error)) {
        return 16;
    }
    if (orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::RegexTester, L"(a+)+$",
                                    L"aaaaaaaaaaaaaaaaaaaaaaaa!", L"Run", output, error)) {
        return 14;
    }
    if (orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::RegexTester, L".*a",
                                    std::wstring(32 * 1024, L'x'), L"Run", output, error)) {
        return 17;
    }
    if (orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::Base64Text, L"A===", L"", L"Decode",
                                    output, error) ||
        orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::Base64Text, L"AB==", L"", L"Decode",
                                    output, error) ||
        orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::UrlCodec, L"%0G", L"", L"Decode",
                                    output, error) ||
        orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::UrlCodec, L"%", L"", L"Decode", output,
                                    error) ||
        orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::UrlCodec, L"%FF", L"", L"Decode",
                                    output, error) ||
        orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::Base64Text, L"YQBi", L"", L"Decode",
                                    output, error) ||
        orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::ColorConverter, L"#12Z", L"", L"Run",
                                    output, error)) {
        return 6;
    }
    if (!orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::UrlCodec, L"a+b", L"", L"Decode",
                                     output, error) ||
        output != L"a+b") {
        return 9;
    }
    if (orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::JwtDecoder, L"e30.e30.sig.extra", L"",
                                    L"Run", output, error)) {
        return 8;
    }
    if (!orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::JwtDecoder,
                                     L"eyJhbGciOiJub25lIn0.e30.", L"", L"Run", output, error) ||
        orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::JwtDecoder, L"e30.e30.%%%", L"", L"Run",
                                    output, error) ||
        orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::JwtDecoder,
                                    L"eyJhbGciOiJub25lIn0.e30.c2ln", L"", L"Run", output, error) ||
        orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::JwtDecoder, L"bm90anNvbg.e30.", L"",
                                    L"Run", output, error)) {
        return 15;
    }
    if (orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::TimestampConverter,
                                    std::to_wstring(std::numeric_limits<long long>::min()), L"",
                                    L"Run", output, error)) {
        return 7;
    }
    if (!orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::TimestampConverter, L"253402300799",
                                     L"", L"Unix seconds", output, error) ||
        output.find(L"9999-12-31") == std::wstring::npos) {
        return 10;
    }
    wchar_t folder[MAX_PATH]{};
    wchar_t file[MAX_PATH]{};
    if (::GetTempPathW(MAX_PATH, folder) == 0 || ::GetTempFileNameW(folder, L"wci", 0, file) == 0) {
        return 11;
    }
    if (!orbitdeck::RunDeveloperTool(orbitdeck::BuiltInTool::Base64Image,
                                     L"data:image/png;base64,iVBORw0KGgo=", file,
                                     L"Decode data URL", output, error)) {
        (void)::DeleteFileW(file);
        return 12;
    }
    std::ifstream decoded(file, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(decoded),
                            std::istreambuf_iterator<char>()};
    (void)::DeleteFileW(file);
    const std::string pngSignature{static_cast<char>(0x89), 'P', 'N', 'G', '\r', '\n',
                                   static_cast<char>(0x1A), '\n'};
    if (bytes != pngSignature)
        return 13;
    return 0;
}
