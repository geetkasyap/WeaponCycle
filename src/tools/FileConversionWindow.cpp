#include "tools/FileConversionWindow.h"

#include "tools/FileConversion.h"
#include "resource.h"
#include "shell/FileSelection.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>

namespace orbitdeck {
namespace {

constexpr wchar_t kClassName[]{L"OrbitDeck.FileConversion.v1"};
enum ControlId : int {
    kChoose = 5001,
    kPath,
    kFormat,
    kConvert,
    kCancel,
    kStatus,
    kReveal,
    kInstallEngine,
    kDescription,
    kFormatLabel,
    kPrivacy,
    kProgress
};

HRESULT ChooseFile(const HWND owner, const wchar_t* title, const COMDLG_FILTERSPEC* filters,
                   const UINT count, std::wstring& path) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(dialog.GetAddressOf()));
    if (SUCCEEDED(hr)) {
        hr = dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
    }
    if (SUCCEEDED(hr))
        hr = dialog->SetTitle(title);
    if (SUCCEEDED(hr))
        hr = dialog->SetFileTypes(count, filters);
    if (SUCCEEDED(hr))
        hr = dialog->Show(owner);
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (SUCCEEDED(hr))
        hr = dialog->GetResult(item.GetAddressOf());
    PWSTR selected{};
    if (SUCCEEDED(hr))
        hr = item->GetDisplayName(SIGDN_FILESYSPATH, &selected);
    if (SUCCEEDED(hr))
        path = selected;
    ::CoTaskMemFree(selected);
    return hr;
}

}  // namespace

struct FileConversionWindow::Job {
    std::atomic_bool cancel{};
    std::atomic_bool done{};
    std::mutex mutex;
    HWND target{};
    FileConversionResult result;
    ConversionFormats formats;
    Operation operation{};
    std::wstring selectedSource;
    std::wstring message;
    int percent{};
};

FileConversionWindow::~FileConversionWindow() noexcept {
    Destroy();
}

bool FileConversionWindow::Create(const HINSTANCE instance, const HWND owner) noexcept {
    if (window_ != nullptr || instance == nullptr || owner == nullptr)
        return false;
    instance_ = instance;
    background_ = ::CreateSolidBrush(RGB(18, 23, 29));
    field_ = ::CreateSolidBrush(RGB(30, 36, 44));
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &FileConversionWindow::WndProc;
    wc.hInstance = instance;
    wc.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(IDI_ORBITDECK));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = background_;
    wc.lpszClassName = kClassName;
    const ATOM atom = ::RegisterClassExW(&wc);
    if (atom == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Destroy();
        return false;
    }
    ownsClass_ = atom != 0;
    window_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT | WS_EX_ACCEPTFILES, kClassName,
        L"Weapon Cycle - Convert a file", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 660, 540, owner, nullptr, instance, this);
    if (window_ == nullptr) {
        Destroy();
        return false;
    }
    const BOOL dark = TRUE;
    (void)::DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    Layout();
    return true;
}

void FileConversionWindow::Show(const POINT anchor, const std::wstring& source,
                                const HWND explorer) noexcept {
    if (window_ == nullptr)
        return;
    if (job_ != nullptr && job_->done)
        Complete();
    Layout();
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    const HMONITOR monitor = ::MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    if (::GetMonitorInfoW(monitor, &info)) {
        RECT bounds{};
        (void)::GetWindowRect(window_, &bounds);
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        (void)::SetWindowPos(
            window_, HWND_TOP,
            std::clamp<int>(anchor.x + 16, info.rcWork.left,
                            std::max<LONG>(info.rcWork.left, info.rcWork.right - width)),
            std::clamp<int>(anchor.y + 16, info.rcWork.top,
                            std::max<LONG>(info.rcWork.top, info.rcWork.bottom - height)),
            0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }
    (void)::ShowWindow(window_, ::IsIconic(window_) ? SW_RESTORE : SW_SHOW);
    if (job_ != nullptr && ::SetTimer(window_, kJobTimerId, 125, nullptr) == 0) {
        (void)::SetWindowTextW(
            status_, L"Conversion is running; progress notifications could not be scheduled.");
    }
    (void)::SetForegroundWindow(window_);
    (void)::SetFocus(::GetDlgItem(window_, kChoose));
    if (job_ == nullptr) {
        if (!source.empty())
            SetSource(source);
        else if (explorer != nullptr)
            StartJob(Operation::Selection, explorer);
    } else if (!source.empty()) {
        (void)::SetWindowTextW(
            status_,
            L"A conversion is already active. Finish or cancel it before opening another file.");
    }
}

bool FileConversionWindow::IsVisible() const noexcept {
    return window_ != nullptr && ::IsWindowVisible(window_);
}

void FileConversionWindow::Destroy() noexcept {
    if (window_ != nullptr)
        (void)::KillTimer(window_, kJobTimerId);
    if (job_ != nullptr) {
        job_->cancel = true;
        std::lock_guard lock(job_->mutex);
        job_->target = nullptr;
    }
    job_.reset();
    if (window_ != nullptr) {
        (void)::DestroyWindow(window_);
        window_ = nullptr;
    }
    if (ownsClass_)
        (void)::UnregisterClassW(kClassName, instance_);
    ownsClass_ = false;
    if (font_ != nullptr)
        (void)::DeleteObject(font_);
    if (titleFont_ != nullptr)
        (void)::DeleteObject(titleFont_);
    if (background_ != nullptr)
        (void)::DeleteObject(background_);
    if (field_ != nullptr)
        (void)::DeleteObject(field_);
    font_ = nullptr;
    titleFont_ = nullptr;
    background_ = nullptr;
    field_ = nullptr;
}

bool FileConversionWindow::CreateControls() noexcept {
    bool complete = true;
    const auto control = [&](const wchar_t* type, const wchar_t* text, const DWORD style,
                             const int id) {
        const HWND created = ::CreateWindowExW(
            0, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (created == nullptr)
            complete = false;
        return created;
    };
    heading_ = control(L"STATIC", L"A new format. The same file.", SS_LEFT, 0);
    (void)control(L"STATIC",
                  L"Choose or drop a file. In Explorer: select one file, then press Ctrl+Alt+C.",
                  SS_LEFT, kDescription);
    path_ = control(L"EDIT", L"No file selected",
                    WS_BORDER | ES_READONLY | ES_AUTOHSCROLL | WS_TABSTOP, kPath);
    (void)control(L"BUTTON", L"Choose file...", BS_PUSHBUTTON | WS_TABSTOP, kChoose);
    (void)control(L"STATIC", L"Convert to", SS_LEFT, kFormatLabel);
    format_ = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kFormat);
    (void)::SendMessageW(format_, CB_SETCUEBANNER, 0,
                         reinterpret_cast<LPARAM>(L"Select a file to see its formats"));
    (void)control(L"BUTTON", L"Convert", BS_DEFPUSHBUTTON | WS_TABSTOP, kConvert);
    (void)control(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, kCancel);
    status_ = control(L"STATIC", L"1. Choose a file   2. Pick its output format   3. Convert",
                      SS_LEFT | SS_NOPREFIX, kStatus);
    (void)control(L"BUTTON", L"Show result", BS_PUSHBUTTON | WS_TABSTOP, kReveal);
    (void)control(L"BUTTON", L"Add required engine...", BS_PUSHBUTTON | WS_TABSTOP, kInstallEngine);
    progress_ = control(PROGRESS_CLASSW, L"", PBS_SMOOTH, kProgress);
    (void)::SendMessageW(progress_, PBM_SETRANGE32, 0, 100);
    (void)control(
        L"STATIC",
        L"Images, PDF, Word, spreadsheets, media, archives and more. Only valid routes "
        L"are offered. Original files are kept. Document layout can change; scanned PDFs need OCR.",
        SS_LEFT, kPrivacy);
    SetBusy(false);
    return complete && heading_ != nullptr && path_ != nullptr && format_ != nullptr &&
           status_ != nullptr;
}

void FileConversionWindow::Layout() noexcept {
    if (window_ == nullptr)
        return;
    UINT dpi = ::GetDpiForWindow(window_);
    if (dpi == 0)
        dpi = 96;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(::MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &info)) {
        dpi = std::min<UINT>(
            dpi, static_cast<UINT>(std::max(
                     48, std::min(::MulDiv(info.rcWork.right - info.rcWork.left - 24, 96, 660),
                                  ::MulDiv(info.rcWork.bottom - info.rcWork.top - 24, 96, 540)))));
    }
    if (font_ != nullptr)
        (void)::DeleteObject(font_);
    if (titleFont_ != nullptr)
        (void)::DeleteObject(titleFont_);
    font_ = ::CreateFontW(-::MulDiv(14, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    titleFont_ = ::CreateFontW(-::MulDiv(24, dpi, 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (HWND child = ::GetWindow(window_, GW_CHILD); child != nullptr;
         child = ::GetWindow(child, GW_HWNDNEXT)) {
        (void)::SendMessageW(child, WM_SETFONT,
                             reinterpret_cast<WPARAM>(child == heading_ ? titleFont_ : font_),
                             TRUE);
        (void)::SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
    }
    const auto place = [&](HWND child, int x, int y, int w, int h) {
        (void)::SetWindowPos(child, nullptr, ::MulDiv(x, dpi, 96), ::MulDiv(y, dpi, 96),
                             ::MulDiv(w, dpi, 96), ::MulDiv(h, dpi, 96),
                             SWP_NOZORDER | SWP_NOACTIVATE);
    };
    const auto at = [&](int id, int x, int y, int w, int h) {
        place(::GetDlgItem(window_, id), x, y, w, h);
    };
    place(heading_, 26, 22, 600, 36);
    at(kDescription, 26, 70, 600, 40);
    at(kPath, 26, 120, 440, 34);
    at(kChoose, 480, 120, 140, 34);
    at(kFormatLabel, 26, 178, 600, 22);
    at(kFormat, 26, 206, 440, 180);
    at(kConvert, 480, 204, 140, 36);
    at(kCancel, 480, 254, 140, 32);
    at(kStatus, 26, 262, 440, 108);
    at(kProgress, 26, 376, 594, 8);
    at(kReveal, 26, 398, 144, 34);
    at(kInstallEngine, 184, 398, 282, 34);
    at(kPrivacy, 26, 450, 600, 48);
    (void)::SetWindowPos(window_, nullptr, 0, 0, ::MulDiv(660, dpi, 96), ::MulDiv(540, dpi, 96),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void FileConversionWindow::ChooseSource() noexcept {
    const COMDLG_FILTERSPEC filters[]{
        {L"All convertible files", L"*.*"},
        {L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff;*.gif;*.webp;*.heic;*.heif;*.avif"},
        {L"PDF and Word documents", L"*.pdf;*.docx;*.doc;*.odt;*.rtf;*.txt;*.md"},
        {L"Media", L"*.mp4;*.mkv;*.mov;*.webm;*.mp3;*.wav;*.flac;*.m4a"}};
    std::wstring path;
    const HRESULT hr = ChooseFile(window_, L"Choose a file to convert", filters, 4, path);
    if (SUCCEEDED(hr))
        SetSource(std::move(path));
    else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        (void)::SetWindowTextW(status_, L"The file picker could not be opened.");
    }
}

void FileConversionWindow::SetSource(std::wstring path) noexcept {
    if (job_ != nullptr)
        return;
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        (void)::SetWindowTextW(status_, L"Choose one accessible file, not a folder.");
        return;
    }
    sourcePath_ = std::move(path);
    outputPath_.clear();
    desiredFormat_.clear();
    targets_.clear();
    (void)::SetWindowTextW(path_, sourcePath_.c_str());
    StartJob(Operation::Inspect);
}

void FileConversionWindow::SetBusy(const bool busy) noexcept {
    for (int id : {kChoose, kFormat})
        (void)::EnableWindow(::GetDlgItem(window_, id), !busy);
    (void)::EnableWindow(::GetDlgItem(window_, kConvert), !busy && !targets_.empty());
    (void)::EnableWindow(::GetDlgItem(window_, kCancel), busy);
    (void)::EnableWindow(::GetDlgItem(window_, kReveal), !busy && !outputPath_.empty());
    (void)::EnableWindow(::GetDlgItem(window_, kInstallEngine), FALSE);
    if (!busy)
        UpdateTarget();
}

void FileConversionWindow::Convert() noexcept {
    if (job_ != nullptr || sourcePath_.empty())
        return;
    const LRESULT selected = ::SendMessageW(format_, CB_GETCURSEL, 0, 0);
    if (selected < 0 || static_cast<std::size_t>(selected) >= targets_.size()) {
        (void)::SetWindowTextW(status_, L"Choose an output format first.");
        return;
    }
    if (!targets_[static_cast<std::size_t>(selected)].available) {
        UpdateTarget();
        return;
    }
    desiredFormat_ = targets_[static_cast<std::size_t>(selected)].id;
    StartJob(Operation::Convert);
}

void FileConversionWindow::StartJob(const Operation operation, const HWND explorer) noexcept {
    if (job_ != nullptr)
        return;
    auto job = std::make_shared<Job>();
    job->operation = operation;
    job->target = window_;
    job_ = job;
    if (operation != Operation::Selection)
        outputPath_.clear();
    SetBusy(true);
    (void)::SetWindowTextW(
        status_, operation == Operation::Selection ? L"Reading the selected Explorer file..."
                 : operation == Operation::Inspect ? L"Finding formats for this file..."
                 : operation == Operation::Install
                     ? L"Downloading the selected engine (your file is not uploaded)..."
                     : L"Converting locally... You can keep using the wheel.");
    (void)::SendMessageW(progress_, PBM_SETPOS, 0, 0);
    shownProgress_ = 0;
    shownProgressMessage_.clear();
    if (::SetTimer(window_, kJobTimerId, 125, nullptr) == 0) {
        (void)::SetWindowTextW(
            status_,
            L"Converting locally. Windows could not schedule backup completion notifications.");
    }
    try {
        const auto selected = ::SendMessageW(format_, CB_GETCURSEL, 0, 0);
        const std::vector<std::wstring> packages =
            selected >= 0 && static_cast<std::size_t>(selected) < targets_.size()
                ? targets_[static_cast<std::size_t>(selected)].packages
                : std::vector<std::wstring>{};
        std::thread([job, source = sourcePath_, format = desiredFormat_, packages, explorer,
                     fallback = fallbackImaging_] {
            (void)::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            const auto progress = [job](const int percent, const std::wstring& message) {
                std::lock_guard lock(job->mutex);
                job->percent = percent;
                job->message = message;
            };
            FileConversionResult result;
            ConversionFormats formats;
            std::wstring selection;
            try {
                if (job->operation == Operation::Selection) {
                    const auto selectedFile = ExplorerSelectedFile(explorer);
                    selection = selectedFile.path;
                    result.error = selectedFile.error;
                } else if (job->operation == Operation::Inspect) {
                    formats = DiscoverConversionFormats(source, job->cancel, progress);
                } else if (job->operation == Operation::Install) {
                    result = InstallConversionEngine(packages, job->cancel, progress);
                } else if (fallback) {
                    const ImageFormat imageFormat = format == L"jpg"    ? ImageFormat::Jpeg
                                                    : format == L"bmp"  ? ImageFormat::Bmp
                                                    : format == L"tiff" ? ImageFormat::Tiff
                                                                        : ImageFormat::Png;
                    result = ConvertImageFile(source, imageFormat, job->cancel);
                } else {
                    result = RunFullConversion(source, format, job->cancel, progress);
                }
            } catch (const std::bad_alloc&) {
                result.error = L"Not enough memory to complete the conversion request.";
                formats.error = result.error;
            } catch (const std::filesystem::filesystem_error& error) {
                result.error = L"The conversion file path could not be accessed (Windows error " +
                               std::to_wstring(error.code().value()) + L").";
                formats.error = result.error;
            }
            std::lock_guard lock(job->mutex);
            job->result = std::move(result);
            job->formats = std::move(formats);
            job->selectedSource = std::move(selection);
            job->done = true;
            if (job->target != nullptr) {
                if (!::PostMessageW(job->target, kCompletedMessage, 0, 0)) {
                    ::OutputDebugStringW(
                        L"Weapon Cycle: conversion completion message was not queued.\n");
                }
            }
        }).detach();
    } catch (const std::system_error&) {
        job_.reset();
        (void)::KillTimer(window_, kJobTimerId);
        SetBusy(false);
        (void)::SetWindowTextW(status_, L"The conversion worker could not start. Try again.");
    }
}

void FileConversionWindow::Complete() noexcept {
    if (job_ == nullptr || !job_->done)
        return;
    (void)::KillTimer(window_, kJobTimerId);
    FileConversionResult result;
    ConversionFormats formats;
    const Operation operation = job_->operation;
    std::wstring selection;
    const bool cancelled = job_->cancel;
    {
        std::lock_guard lock(job_->mutex);
        result = std::move(job_->result);
        formats = std::move(job_->formats);
        selection = std::move(job_->selectedSource);
    }
    job_.reset();
    if (operation == Operation::Selection) {
        SetBusy(false);
        if (!selection.empty() && !cancelled)
            SetSource(std::move(selection));
        else
            (void)::SetWindowTextW(status_,
                                   result.error.empty()
                                       ? L"Choose or drop a file here. Selecting/double-clicking a "
                                         L"file alone does not convert or replace it."
                                       : result.error.c_str());
        return;
    }
    if (operation == Operation::Inspect) {
        if (cancelled) {
            targets_.clear();
            (void)::SendMessageW(format_, CB_RESETCONTENT, 0, 0);
            SetBusy(false);
            (void)::SetWindowTextW(status_,
                                   L"Format discovery cancelled. Choose a file to try again.");
            return;
        }
        fallbackImaging_ =
            ::GetFileAttributesW(ConversionEnginePath().c_str()) == INVALID_FILE_ATTRIBUTES;
        targets_ = std::move(formats.targets);
        if (fallbackImaging_) {
            targets_ = {{L"png", L"PNG (basic Windows imaging)", {}, 0, true},
                        {L"jpg", L"JPEG (basic Windows imaging)", {}, 0, true},
                        {L"bmp", L"BMP (basic Windows imaging)", {}, 0, true},
                        {L"tiff", L"TIFF (basic Windows imaging)", {}, 0, true}};
        }
        (void)::SendMessageW(format_, CB_RESETCONTENT, 0, 0);
        int selected{};
        for (std::size_t index = 0; index < targets_.size(); ++index) {
            const auto& target = targets_[index];
            const std::wstring label =
                target.label + (target.available ? L""
                                                 : L" - add " + (target.packages.empty()
                                                                     ? L"engine"
                                                                     : target.packages.front()));
            (void)::SendMessageW(format_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (target.id == desiredFormat_ ||
                (desiredFormat_.empty() && target.id == (formats.category == L"Image" ? L"png"
                                                         : formats.category == L"Pdf" ? L"docx"
                                                                                      : L"pdf"))) {
                selected = static_cast<int>(index);
            }
        }
        (void)::SendMessageW(format_, CB_SETCURSEL, selected, 0);
        SetBusy(false);
        if (!formats.error.empty()) {
            (void)::SetWindowTextW(status_, formats.error.c_str());
        } else if (targets_.empty()) {
            (void)::SetWindowTextW(status_,
                                   L"No conversion route is available for this file type.");
        }
        return;
    }
    if (operation == Operation::Install && result.succeeded) {
        StartJob(Operation::Inspect);
        return;
    }
    outputPath_ = result.outputPath;
    const std::wstring status =
        result.succeeded
            ? L"Converted. Original kept.\n" +
                  std::filesystem::path(outputPath_).filename().wstring() +
                  (result.outputPaths.size() > 1
                       ? L" (and " + std::to_wstring(result.outputPaths.size() - 1) + L" more)"
                       : L"") +
                  (result.notice.empty() ? L"" : L"\n" + result.notice)
        : result.cancelled ? L"Cancelled. Your original is unchanged."
                           : L"Could not convert: " + result.error;
    SetBusy(false);
    (void)::SendMessageW(progress_, PBM_SETPOS, result.succeeded ? 100 : 0, 0);
    (void)::SetWindowTextW(status_, status.c_str());
}

void FileConversionWindow::Cancel() noexcept {
    if (job_ == nullptr)
        return;
    job_->cancel = true;
    (void)::SetWindowTextW(status_, L"Cancelling and removing the unfinished copy...");
    (void)::EnableWindow(::GetDlgItem(window_, kCancel), FALSE);
}

void FileConversionWindow::InstallRequired() noexcept {
    if (job_ != nullptr)
        return;
    const LRESULT index = ::SendMessageW(format_, CB_GETCURSEL, 0, 0);
    if (index < 0 || static_cast<std::size_t>(index) >= targets_.size())
        return;
    const auto& target = targets_[static_cast<std::size_t>(index)];
    if (target.available || target.packages.empty())
        return;
    const std::wstring prompt =
        L"Download the required open-source conversion engine (" +
        std::to_wstring((target.downloadBytes + 999999) / 1000000) +
        L" MB)?\n\nThis adds formats inside Weapon Cycle. Only engine files are downloaded; "
        L"your selected file is never uploaded. Third-party licenses are in the portable "
        L"conversion folder.";
    if (::MessageBoxW(window_, prompt.c_str(), L"Add conversion engine",
                      MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    desiredFormat_ = target.id;
    StartJob(Operation::Install);
}

void FileConversionWindow::UpdateTarget() noexcept {
    const LRESULT index = ::SendMessageW(format_, CB_GETCURSEL, 0, 0);
    if (job_ != nullptr || index < 0 || static_cast<std::size_t>(index) >= targets_.size()) {
        (void)::EnableWindow(::GetDlgItem(window_, kConvert), FALSE);
        return;
    }
    const auto& target = targets_[static_cast<std::size_t>(index)];
    (void)::EnableWindow(::GetDlgItem(window_, kConvert), target.available);
    (void)::EnableWindow(::GetDlgItem(window_, kInstallEngine),
                         !target.available && !target.packages.empty());
    std::wstring message =
        target.available
            ? (fallbackImaging_ ? L"Basic imaging only: keep the bundled conversion folder beside "
                                  L"WeaponCycle.exe."
                                : L"Ready. Result files will appear in a new (converted) folder "
                                  L"beside the original.")
            : L"This route needs " +
                  (target.packages.empty() ? L"another engine" : target.packages.front()) +
                  L". Use Add required engine (" +
                  std::to_wstring((target.downloadBytes + 999999) / 1000000) + L" MB).";
    const std::wstring extension = std::filesystem::path(sourcePath_).extension().wstring();
    if (_wcsicmp(extension.c_str(), L".pdf") == 0 && target.id == L"docx") {
        message +=
            L"\nPDF to Word reconstructs content; complex layouts may differ. Scanned pages are "
            L"not automatically OCR'd.";
    }
    (void)::SetWindowTextW(status_, message.c_str());
}

void FileConversionWindow::ShowProgress() noexcept {
    if (job_ == nullptr)
        return;
    int percent{};
    std::wstring message;
    {
        std::lock_guard lock(job_->mutex);
        percent = job_->percent;
        if (!job_->cancel)
            message = job_->message;
    }
    if (shownProgress_ != percent) {
        (void)::SendMessageW(progress_, PBM_SETPOS, percent, 0);
        shownProgress_ = percent;
    }
    if (!message.empty() && message != shownProgressMessage_) {
        (void)::SetWindowTextW(status_, message.c_str());
        shownProgressMessage_ = std::move(message);
    }
}

void FileConversionWindow::DropFiles(const HDROP drop) noexcept {
    const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (count != 1 || job_ != nullptr) {
        (void)::SetWindowTextW(
            status_, job_ != nullptr
                         ? L"Finish or cancel this conversion before dropping another file."
                         : L"Drop one file at a time.");
        ::DragFinish(drop);
        return;
    }
    const UINT length = ::DragQueryFileW(drop, 0, nullptr, 0);
    std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
    if (::DragQueryFileW(drop, 0, path.data(), length + 1) != 0) {
        path.resize(length);
        SetSource(std::move(path));
    } else {
        (void)::SetWindowTextW(status_, L"The dropped file path could not be read.");
    }
    ::DragFinish(drop);
}

LRESULT CALLBACK FileConversionWindow::WndProc(const HWND window, const UINT message,
                                               const WPARAM wParam, const LPARAM lParam) noexcept {
    auto* self =
        reinterpret_cast<FileConversionWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<FileConversionWindow*>(create->lpCreateParams);
        if (self == nullptr)
            return FALSE;
        self->window_ = window;
        (void)::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self == nullptr)
        return ::DefWindowProcW(window, message, wParam, lParam);
    const LRESULT result = self->HandleMessage(message, wParam, lParam);
    if (message == WM_NCDESTROY) {
        self->window_ = nullptr;
        (void)::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return result;
}

LRESULT FileConversionWindow::HandleMessage(const UINT message, const WPARAM wParam,
                                            const LPARAM lParam) noexcept {
    switch (message) {
        case WM_CREATE:
            return CreateControls() ? 0 : -1;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            (void)::SetTextColor(reinterpret_cast<HDC>(wParam), RGB(230, 237, 241));
            (void)::SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            return reinterpret_cast<LRESULT>(background_);
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            (void)::SetTextColor(reinterpret_cast<HDC>(wParam), RGB(239, 244, 247));
            (void)::SetBkColor(reinterpret_cast<HDC>(wParam), RGB(30, 36, 44));
            return reinterpret_cast<LRESULT>(field_);
        case WM_COMMAND:
            if (LOWORD(wParam) == kFormat && HIWORD(wParam) == CBN_SELCHANGE) {
                UpdateTarget();
                return 0;
            }
            switch (LOWORD(wParam)) {
                case kChoose:
                    ChooseSource();
                    break;
                case kConvert:
                case IDOK:
                    Convert();
                    break;
                case kCancel:
                    Cancel();
                    break;
                case kInstallEngine:
                    InstallRequired();
                    break;
                case kReveal:
                    if (!outputPath_.empty()) {
                        PIDLIST_ABSOLUTE item{};
                        const HRESULT parsed =
                            ::SHParseDisplayName(outputPath_.c_str(), nullptr, &item, 0, nullptr);
                        const HRESULT opened =
                            SUCCEEDED(parsed) ? ::SHOpenFolderAndSelectItems(item, 0, nullptr, 0)
                                              : parsed;
                        ::CoTaskMemFree(item);
                        if (FAILED(opened)) {
                            (void)::SetWindowTextW(status_,
                                                   L"The result folder could not be opened.");
                        }
                    }
                    break;
                case IDCANCEL:
                    Cancel();
                    (void)::KillTimer(window_, kJobTimerId);
                    (void)::ShowWindow(window_, SW_HIDE);
                    break;
            }
            return 0;
        case WM_DROPFILES:
            DropFiles(reinterpret_cast<HDROP>(wParam));
            return 0;
        case kCompletedMessage:
            Complete();
            return 0;
        case WM_TIMER:
            if (wParam == kJobTimerId) {
                ShowProgress();
                Complete();
            }
            return 0;
        case WM_CLOSE:
            Cancel();
            (void)::KillTimer(window_, kJobTimerId);
            (void)::ShowWindow(window_, SW_HIDE);
            return 0;
        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
            Layout();
            return 0;
    }
    return ::DefWindowProcW(window_, message, wParam, lParam);
}

}  // namespace orbitdeck
