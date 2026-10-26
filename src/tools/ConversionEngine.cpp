#include "tools/ConversionEngine.h"

#include "platform/UniqueHandle.h"
#include "tools/ConversionProtocol.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace orbitdeck {
namespace {

constexpr ULONGLONG kInspectionTimeoutMs{60'000};
constexpr ULONGLONG kOperationTimeoutMs{60ULL * 60 * 1000};
constexpr ULONGLONG kCancellationGraceMs{5000};
constexpr DWORD kProcessPollIntervalMs{25};
constexpr std::size_t kMaximumCommandCharacters{32767};

class ProcessAttributes {
public:
    ProcessAttributes() = default;
    ~ProcessAttributes() noexcept {
        if (attributes_ != nullptr) {
            ::DeleteProcThreadAttributeList(attributes_);
        }
    }
    ProcessAttributes(const ProcessAttributes&) = delete;
    ProcessAttributes& operator=(const ProcessAttributes&) = delete;

    [[nodiscard]] bool Initialize() {
        SIZE_T bytes{};
        (void)::InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        storage_.resize(bytes);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) {
            return false;
        }
        attributes_ = attributes;
        return true;
    }

    [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST Get() const noexcept {
        return attributes_;
    }

private:
    std::vector<BYTE> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes_{};
};

ConversionResponse RunEngine(const std::vector<std::wstring>& arguments,
                             const std::atomic_bool& cancel, const ConversionProgress& progress) {
    ConversionResponse response;
    if (cancel.load()) {
        response.cancelled = true;
        return response;
    }
    if (arguments.empty()) {
        response.error = L"No conversion engine command was supplied.";
        return response;
    }
    const std::wstring executable = ConversionEnginePath();
    if (::GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
        response.error =
            L"The complete converter package is missing. Run WeaponCycle.exe from "
            L"the portable folder together with its conversion subfolder, not an older single-file "
            L"build.";
        return response;
    }

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    UniqueHandle outputRead;
    UniqueHandle outputWrite;
    UniqueHandle inputRead;
    UniqueHandle inputWrite;
    UniqueHandle processJob;
    UniqueHandle process;
    UniqueHandle thread;
    if (!::CreatePipe(outputRead.Put(), outputWrite.Put(), &security, 0) ||
        !::CreatePipe(inputRead.Put(), inputWrite.Put(), &security, 0) ||
        !::SetHandleInformation(outputRead.Get(), HANDLE_FLAG_INHERIT, 0) ||
        !::SetHandleInformation(inputWrite.Get(), HANDLE_FLAG_INHERIT, 0)) {
        response.error = L"Windows could not create private conversion pipes.";
        return response;
    }
    processJob.Reset(::CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!processJob ||
        !::SetInformationJobObject(processJob.Get(), JobObjectExtendedLimitInformation, &limits,
                                   sizeof(limits))) {
        response.error = L"Windows could not create a cancellable conversion job.";
        return response;
    }
    ProcessAttributes attributes;
    if (!attributes.Initialize()) {
        response.error = L"Windows could not initialize isolated conversion handles.";
        return response;
    }
    HANDLE inherited[]{outputWrite.Get(), inputRead.Get()};
    if (!::UpdateProcThreadAttribute(attributes.Get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                     inherited, sizeof(inherited), nullptr, nullptr)) {
        response.error = L"Windows could not isolate the conversion pipes.";
        return response;
    }
    std::wstring command = QuoteConversionArgument(executable);
    for (const auto& argument : arguments) {
        command += L" " + QuoteConversionArgument(argument);
    }
    if (command.size() >= kMaximumCommandCharacters) {
        response.error = L"The file path is too long for Windows process activation.";
        return response;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = outputWrite.Get();
    startup.StartupInfo.hStdError = outputWrite.Get();
    startup.StartupInfo.hStdInput = inputRead.Get();
    startup.lpAttributeList = attributes.Get();
    PROCESS_INFORMATION created{};
    const std::wstring directory = std::filesystem::path(executable).parent_path().wstring();
    if (!::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                          CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT |
                              BELOW_NORMAL_PRIORITY_CLASS,
                          nullptr, directory.c_str(), &startup.StartupInfo, &created)) {
        response.error = L"The bundled conversion engine could not start (Windows error " +
                         std::to_wstring(::GetLastError()) +
                         L"). Keep its complete conversion folder beside the app.";
        return response;
    }
    process.Reset(created.hProcess);
    thread.Reset(created.hThread);
    if (!::AssignProcessToJobObject(processJob.Get(), process.Get())) {
        (void)::TerminateProcess(process.Get(), 1);
        response.error = L"Windows denied ownership of the conversion process.";
        return response;
    }
    if (::ResumeThread(thread.Get()) == static_cast<DWORD>(-1)) {
        response.error = L"Windows could not resume the conversion process.";
        return response;
    }
    outputWrite.Reset();
    inputRead.Reset();

    ConversionProtocol protocol;
    bool sentCancel{};
    ULONGLONG cancelStarted{};
    const ULONGLONG started = ::GetTickCount64();
    const ULONGLONG timeout =
        arguments.front() == L"inspect" ? kInspectionTimeoutMs : kOperationTimeoutMs;
    for (;;) {
        if (cancel.load() && !sentCancel) {
            DWORD written{};
            constexpr char request[]{"cancel\n"};
            if (!::WriteFile(inputWrite.Get(), request, sizeof(request) - 1, &written, nullptr)) {
                ::OutputDebugStringW(L"Weapon Cycle: cooperative cancellation pipe unavailable.\n");
            }
            sentCancel = true;
            cancelStarted = ::GetTickCount64();
        }
        if ((sentCancel && ::GetTickCount64() - cancelStarted > kCancellationGraceMs) ||
            ::GetTickCount64() - started > timeout) {
            response.cancelled = sentCancel;
            response.error =
                sentCancel ? L"The engine was stopped after cancellation. An identifiable .partial "
                             L"folder may remain; the original is unchanged."
                           : L"The conversion engine exceeded its time limit and was stopped.";
            return response;
        }

        DWORD available{};
        const BOOL peeked =
            ::PeekNamedPipe(outputRead.Get(), nullptr, 0, nullptr, &available, nullptr);
        if (!peeked && ::GetLastError() != ERROR_BROKEN_PIPE) {
            response.error = L"The conversion engine output pipe could not be inspected.";
            return response;
        }
        if (available > 0) {
            char bytes[8192]{};
            DWORD read{};
            if (!::ReadFile(outputRead.Get(), bytes, std::min<DWORD>(available, sizeof(bytes)),
                            &read, nullptr)) {
                response.error = L"The conversion engine output pipe failed.";
                return response;
            }
            if (!protocol.Append(std::string_view(bytes, read), progress)) {
                return protocol.Response();
            }
            continue;
        }

        const DWORD wait = ::WaitForSingleObject(process.Get(), kProcessPollIntervalMs);
        if (wait == WAIT_FAILED) {
            response.error = L"Windows could not wait for the conversion process.";
            return response;
        }
        if (wait == WAIT_OBJECT_0) {
            if (!::PeekNamedPipe(outputRead.Get(), nullptr, 0, nullptr, &available, nullptr) ||
                available == 0) {
                break;
            }
        }
    }

    DWORD exitCode{};
    if (!::GetExitCodeProcess(process.Get(), &exitCode)) {
        response.error = L"Windows could not read the conversion process exit status.";
        return response;
    }
    (void)protocol.Finish(exitCode, progress);
    return protocol.Response();
}

FileConversionResult ReadEngineResult(ConversionResponse response) {
    if (response.error.empty()) {
        if (auto* result = std::get_if<FileConversionResult>(&response.payload)) {
            return std::move(*result);
        }
    }
    FileConversionResult result;
    result.cancelled = response.cancelled;
    result.error = std::move(response.error);
    if (result.error.empty() && !result.cancelled) {
        result.error = L"The conversion engine returned the wrong response type.";
    }
    return result;
}

}  // namespace

std::wstring ConversionEnginePath() {
    std::wstring module(32768, L'\0');
    const DWORD size =
        ::GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (size == 0 || size >= module.size()) {
        return {};
    }
    module.resize(size);
    return (std::filesystem::path(module).parent_path() / L"conversion" /
            L"WeaponCycle.Conversion.exe")
        .wstring();
}

std::wstring QuoteConversionArgument(const std::wstring& value) {
    std::wstring quoted{L"\""};
    std::size_t slashes{};
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        quoted.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        quoted += character;
    }
    quoted.append(slashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

ConversionFormats DiscoverConversionFormats(const std::wstring& source,
                                            const std::atomic_bool& cancel,
                                            const ConversionProgress& progress) {
    ConversionResponse response = RunEngine({L"inspect", source}, cancel, progress);
    if (response.error.empty()) {
        if (auto* formats = std::get_if<ConversionFormats>(&response.payload)) {
            return std::move(*formats);
        }
    }
    ConversionFormats formats;
    formats.error = std::move(response.error);
    if (formats.error.empty()) {
        const auto* result = std::get_if<FileConversionResult>(&response.payload);
        if (response.cancelled || (result != nullptr && result->cancelled)) {
            formats.error = L"Format discovery was cancelled.";
        } else if (result != nullptr && !result->error.empty()) {
            formats.error = result->error;
        } else {
            formats.error = L"The conversion engine returned no formats.";
        }
    }
    return formats;
}

FileConversionResult RunFullConversion(const std::wstring& source, const std::wstring& format,
                                       const std::atomic_bool& cancel,
                                       const ConversionProgress& progress) {
    FileConversionResult result =
        ReadEngineResult(RunEngine({L"convert", source, format}, cancel, progress));
    if (result.succeeded && result.outputPaths.empty()) {
        result.succeeded = false;
        result.error = L"The conversion engine reported success without output files.";
    }
    return result;
}

FileConversionResult InstallConversionEngine(const std::vector<std::wstring>& packages,
                                             const std::atomic_bool& cancel,
                                             const ConversionProgress& progress) {
    FileConversionResult result;
    if (packages.empty()) {
        result.error = L"No required conversion engine was selected.";
        return result;
    }
    for (const auto& package : packages) {
        result = ReadEngineResult(RunEngine({L"install", package}, cancel, progress));
        if (!result.succeeded) {
            return result;
        }
    }
    return result;
}

}  // namespace orbitdeck
