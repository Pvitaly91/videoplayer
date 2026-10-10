#include "targetver.h"
#include "ExportDialog.h"
#include "AppIcon.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include "ExportGeometry.h"
#include "ExportJob.h"
#include "ExportPlanner.h"
#include "ExportProcess.h"
#include "MediaProbe.h"
#include "PathUtils.h"
#include "PrivacyPolicy.h"
#include "RemoveSegment.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace videoplayer {
namespace {

constexpr wchar_t kClassName[] = L"VideoPlayer.ExportDialog";
constexpr wchar_t kActiveDialogProperty[] = L"VideoPlayer.ActiveExportDialog";
constexpr UINT_PTR kOwnerSubclassId = 0x45585054U;
constexpr UINT kProbeDone = WM_APP + 51;
constexpr UINT kExportDone = WM_APP + 52;
constexpr UINT kExportProgress = WM_APP + 53;
constexpr int kFragment = 4101;
constexpr int kImage = 4102;
constexpr int kResolution = 4103;
constexpr int kMethod = 4104;
constexpr int kAudio = 4105;
constexpr int kPath = 4106;
constexpr int kBrowse = 4107;
constexpr int kSave = 4108;
constexpr int kCancel = 4109;
constexpr int kSummary = 4110;
constexpr int kProgress = 4111;
constexpr int kStatus = 4112;
constexpr int kReplaceSource = 4113;
constexpr int kRemovalList = 4114;
constexpr int kRemoveListed = 4115;
constexpr int kClearListed = 4116;

HMENU Id(const int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); }

std::wstring DefaultDestination(const std::wstring& source) {
    const auto separator = source.find_last_of(L"\\/");
    const auto dot = source.find_last_of(L'.');
    const auto stemEnd = dot != std::wstring::npos &&
        (separator == std::wstring::npos || dot > separator) ? dot : source.size();
    std::wstring extension = stemEnd < source.size() ? source.substr(stemEnd) : L".mp4";
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](const wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    if (extension != L".mp4" && extension != L".mkv" && extension != L".webm" &&
        extension != L".mov" && extension != L".m4v") extension = L".mp4";
    return source.substr(0, stemEnd) + L"-export" + extension;
}

std::wstring DefaultRemovalDestination(const std::wstring& source) {
    const std::wstring exported = DefaultDestination(source);
    const auto marker = exported.rfind(L"-export");
    return marker == std::wstring::npos ? exported :
        exported.substr(0, marker) + L"-without-segments" + exported.substr(marker + 7);
}

std::wstring WindowText(const HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring result(static_cast<std::size_t>((std::max)(0, length)) + 1U, L'\0');
    GetWindowTextW(window, result.data(), static_cast<int>(result.size()));
    result.resize(wcslen(result.c_str()));
    return result;
}

void AddComboValues(const HWND combo, const std::vector<const wchar_t*>& values, const int selected) {
    for (const wchar_t* value : values) SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
}

struct ProgressUpdate final { int percent = 0; std::wstring text; };

class Dialog final {
public:
    Dialog(HWND owner, HINSTANCE instance, ExportSnapshot snapshot,
        bool removing = false, CommitSourceRemoval commitSource = {},
        UpdateRemovalIntervals updateIntervals = {})
        : owner_(owner), instance_(instance), snapshot_(std::move(snapshot)),
          removing_(removing), commitSource_(std::move(commitSource)),
          updateIntervals_(std::move(updateIntervals)) {
        if (removing_) {
            if (snapshot_.removalIntervals.empty() && snapshot_.hasSegment) {
                snapshot_.removalIntervals.push_back(
                    {snapshot_.segmentStartMs, snapshot_.segmentEndMs});
            }
            // The editable list is the sole source of cuts in this dialog.
            snapshot_.hasSegment = false;
        }
    }
    ~Dialog() {
        cancellation_.store(true);
        if (worker_.joinable()) worker_.join();
        DetachOwnerSubclass();
    }

    void Run() {
        if (!Register()) return;
        window_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName,
            removing_ ? L"Видалити відрізки відео" : L"Зберегти відео",
            WS_POPUP | WS_CAPTION | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, 760, 650, owner_, nullptr, instance_, this);
        if (window_ == nullptr) return;
        SetPropW(owner_, kActiveDialogProperty, reinterpret_cast<HANDLE>(window_));
        EnableWindow(owner_, FALSE);
        RECT bounds{};
        GetWindowRect(window_, &bounds);
        const int x = (GetSystemMetrics(SM_CXSCREEN) - (bounds.right - bounds.left)) / 2;
        const int y = (GetSystemMetrics(SM_CYSCREEN) - (bounds.bottom - bounds.top)) / 2;
        SetWindowPos(window_, HWND_TOP, x, y, 0, 0, SWP_NOSIZE);
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        BeginProbe();
        MSG message{};
        int messageResult = 1;
        while (window_ != nullptr &&
            (messageResult = static_cast<int>(GetMessageW(&message, nullptr, 0, 0))) > 0) {
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (window_ != nullptr) {
            cancellation_.store(true);
            if (worker_.joinable()) worker_.join();
            DestroyWindow(window_);
        }
        if (owner_ != nullptr && IsWindow(owner_) != FALSE) {
            EnableWindow(owner_, TRUE);
            if (!closeOwnerWhenDone_) SetForegroundWindow(owner_);
        }
        if (messageResult == 0) PostQuitMessage(static_cast<int>(message.wParam));
    }

private:
    static LRESULT CALLBACK OwnerWindowProc(HWND window, UINT message,
        WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
        Dialog* self = reinterpret_cast<Dialog*>(data);
        if (self != nullptr && message == WM_CLOSE && self->exporting_) {
            if (MessageBoxW(self->window_,
                    L"Експорт ще триває. Скасувати його і закрити плеєр?",
                    L"Закриття плеєра", MB_YESNO | MB_ICONWARNING) == IDYES) {
                self->closeOwnerWhenDone_ = true;
                self->cancellation_.store(true);
                EnableWindow(window, FALSE);
                EnableWindow(self->cancel_, FALSE);
                SetWindowTextW(self->status_, L"Скасування експорту перед закриттям…");
            }
            return 0;
        }
        if (self != nullptr && message == WM_NCDESTROY) {
            self->ownerSubclassed_ = false;
            self->owner_ = nullptr;
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        Dialog* self = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Dialog*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window_ = window;
        }
        return self == nullptr ? DefWindowProcW(window, message, wParam, lParam)
            : self->Handle(message, wParam, lParam);
    }

    bool Register() const {
        WNDCLASSEXW type{sizeof(type)};
        type.lpfnWndProc = WindowProc;
        type.hInstance = instance_;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hIcon = LoadPlayerIcon(instance_,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
        type.hIconSm = LoadPlayerIcon(instance_,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        type.lpszClassName = kClassName;
        return RegisterClassExW(&type) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    HWND Child(const wchar_t* type, const wchar_t* text, DWORD style,
        int x, int y, int width, int height, int id) {
        HWND result = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
            x, y, width, height, window_, Id(id), instance_, nullptr);
        SendMessageW(result, WM_SETFONT,
            reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return result;
    }

    void CreateControls() {
        if (removing_) {
            Child(L"STATIC", L"Видалення вибраних відрізків без перекодування",
                SS_LEFT, 18, 18, 700, 24, -1);
            Child(L"STATIC", L"Відрізки на видалення (час у вихідному відео)",
                SS_LEFT, 18, 50, 700, 22, -1);
            removalList_ = Child(L"LISTBOX", L"",
                WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                18, 74, 502, 128, kRemovalList);
            removeListed_ = Child(L"BUTTON", L"Прибрати вибраний",
                WS_TABSTOP | BS_PUSHBUTTON, 532, 74, 188, 30, kRemoveListed);
            clearListed_ = Child(L"BUTTON", L"Очистити список",
                WS_TABSTOP | BS_PUSHBUTTON, 532, 112, 188, 30, kClearListed);
            replaceSource_ = Child(L"BUTTON", L"Перезаписати відкритий файл",
                WS_TABSTOP | BS_AUTOCHECKBOX, 18, 214, 400, 25, kReplaceSource);
            Child(L"STATIC", L"Новий файл", SS_LEFT, 18, 257, 155, 22, -1);
            savedDestination_ = DefaultRemovalDestination(snapshot_.sourcePath);
            path_ = Child(L"EDIT", savedDestination_.c_str(),
                WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 180, 252, 450, 26, kPath);
            browse_ = Child(L"BUTTON", L"Огляд…", WS_TABSTOP | BS_PUSHBUTTON,
                638, 251, 82, 28, kBrowse);
            Child(L"STATIC", L"Фактичні межі після округлення до ключових кадрів",
                SS_LEFT, 18, 292, 700, 22, -1);
            summary_ = Child(L"EDIT", L"Аналіз відео та ключових кадрів…",
                WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                18, 318, 702, 190, kSummary);
            status_ = Child(L"STATIC", L"Аналіз відео…",
                SS_LEFT | SS_ENDELLIPSIS, 18, 520, 702, 22, kStatus);
            progress_ = Child(PROGRESS_CLASSW, L"", PBS_SMOOTH,
                18, 546, 702, 18, kProgress);
            save_ = Child(L"BUTTON", L"Видалити всі", WS_TABSTOP | BS_DEFPUSHBUTTON,
                490, 578, 120, 32, kSave);
            cancel_ = Child(L"BUTTON", L"Скасувати", WS_TABSTOP | BS_PUSHBUTTON,
                620, 578, 100, 32, kCancel);
            SendMessageW(progress_, PBM_SETRANGE32, 0, 100);
            RefreshRemovalList();
            EnableWindow(save_, FALSE);
            return;
        }
        Child(L"STATIC", L"Фрагмент", SS_LEFT, 18, 18, 155, 22, -1);
        fragment_ = Child(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
            180, 14, 540, 200, kFragment);
        Child(L"STATIC", L"Зображення", SS_LEFT, 18, 58, 155, 22, -1);
        image_ = Child(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
            180, 54, 540, 200, kImage);
        Child(L"STATIC", L"Роздільність", SS_LEFT, 18, 98, 155, 22, -1);
        resolution_ = Child(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
            180, 94, 540, 200, kResolution);
        Child(L"STATIC", L"Спосіб", SS_LEFT, 18, 138, 155, 22, -1);
        method_ = Child(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
            180, 134, 540, 200, kMethod);
        Child(L"STATIC", L"Аудіо", SS_LEFT, 18, 178, 155, 22, -1);
        audio_ = Child(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
            180, 174, 540, 200, kAudio);
        Child(L"STATIC", L"Файл результату", SS_LEFT, 18, 218, 155, 22, -1);
        path_ = Child(L"EDIT", DefaultDestination(snapshot_.sourcePath).c_str(),
            WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 180, 214, 450, 26, kPath);
        browse_ = Child(L"BUTTON", L"Огляд…", WS_TABSTOP | BS_PUSHBUTTON, 638, 213, 82, 28, kBrowse);
        Child(L"STATIC", L"Параметри джерела та план", SS_LEFT, 18, 256, 300, 22, -1);
        summary_ = Child(L"EDIT", L"Аналіз відео…", WS_BORDER | ES_MULTILINE |
            ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, 18, 280, 702, 228, kSummary);
        status_ = Child(L"STATIC", L"Запуск bundled ffprobe…", SS_LEFT | SS_ENDELLIPSIS,
            18, 520, 702, 22, kStatus);
        progress_ = Child(PROGRESS_CLASSW, L"", PBS_SMOOTH, 18, 546, 702, 18, kProgress);
        save_ = Child(L"BUTTON", L"Зберегти", WS_TABSTOP | BS_DEFPUSHBUTTON,
            510, 578, 100, 32, kSave);
        cancel_ = Child(L"BUTTON", L"Скасувати", WS_TABSTOP | BS_PUSHBUTTON,
            620, 578, 100, 32, kCancel);
        AddComboValues(fragment_, {L"Усе відео", L"Вибраний фрагмент A–B"}, snapshot_.hasSegment ? 1 : 0);
        AddComboValues(image_, {L"Повний кадр", L"Поточна видима область зуму"}, snapshot_.zoomApplied ? 1 : 0);
        AddComboValues(resolution_,
            {L"Полотно джерела (можливі чорні поля)",
             L"Фізично обрізаний кадр (native crop)"},
            snapshot_.zoomApplied ? 1 : 0);
        AddComboValues(method_, {L"Точно (frame-accurate)", L"Швидко, без перекодування"}, 0);
        AddComboValues(audio_, {L"Зберегти аудіо", L"Без аудіо"}, 0);
        SendMessageW(progress_, PBM_SETRANGE32, 0, 100);
        EnableWindow(resolution_, snapshot_.zoomApplied ? TRUE : FALSE);
        EnableWindow(save_, FALSE);
    }

    ExportOptions Options() const {
        ExportOptions options{};
        options.fragment = SendMessageW(fragment_, CB_GETCURSEL, 0, 0) == 1
            ? ExportFragment::Segment : ExportFragment::WholeVideo;
        options.image = SendMessageW(image_, CB_GETCURSEL, 0, 0) == 1
            ? ExportImage::VisibleZoom : ExportImage::FullFrame;
        options.resolution = SendMessageW(resolution_, CB_GETCURSEL, 0, 0) == 1
            ? ExportResolution::NativeCrop : ExportResolution::SourceCanvas;
        options.method = SendMessageW(method_, CB_GETCURSEL, 0, 0) == 1
            ? ExportMethod::FastStreamCopy : ExportMethod::Exact;
        options.audio = SendMessageW(audio_, CB_GETCURSEL, 0, 0) == 1
            ? ExportAudio::None : ExportAudio::Original;
        options.destinationPath = WindowText(path_);
        return options;
    }

    void BeginProbe() {
        cancellation_.store(false);
        const std::wstring ffprobe = ExportToolsDirectory() + L"\\ffprobe.exe";
        worker_ = std::thread([this, ffprobe] {
            probe_ = ProbeMediaFile(ffprobe, snapshot_.sourcePath, &cancellation_);
            if (removing_ && probe_.valid) {
                std::wstring error;
                keyframes_ = ProbeVideoKeyframes(ffprobe, snapshot_.sourcePath,
                    probe_, &cancellation_, error);
                if (!error.empty()) keyframeError_ = error;
            }
            PostMessageW(window_, kProbeDone, 0, 0);
        });
    }

    ExportSourceRegion VisibleRegion() const {
        if (!snapshot_.zoomApplied) return {};
        const MediaStreamInfo* video = probe_.MainVideo();
        if (video == nullptr) return {};
        GeometryRect presentation = snapshot_.presentationScreen;
        if (!snapshot_.renderScreen.IsEmpty()) {
            presentation = ComputeExportPresentationRect(snapshot_.playerCrop,
                snapshot_.renderScreen, video->sampleAspectRatio, video->rotationDegrees);
        }
        return ComputeExportSourceRegionTransformed(
            {video->width, video->height}, snapshot_.playerCrop,
            presentation, snapshot_.viewportScreen, video->rotationDegrees);
    }

    void UpdateSummary() {
        if (probing_ || exporting_ || !probe_.valid) return;
        if (removing_) {
            const std::wstring destination = SendMessageW(
                replaceSource_, BM_GETCHECK, 0, 0) == BST_CHECKED
                ? snapshot_.sourcePath : WindowText(path_);
            const RemovalPlan plan = BuildRemovalPlan(
                snapshot_, probe_, keyframes_, destination);
            const std::wstring text = plan.valid
                ? plan.summary + L"\r\n\r\n" + DescribeMediaProbe(probe_)
                : L"Видалення недоступне: " +
                    (keyframeError_.empty() ? plan.error : keyframeError_);
            SetWindowTextW(summary_, text.c_str());
            EnableWindow(save_, plan.valid);
            return;
        }
        ExportPlan plan = BuildExportPlan(snapshot_, probe_, Options(), VisibleRegion());
        std::wstring text = DescribeMediaProbe(probe_) + L"\r\n\r\n";
        text += plan.valid ? plan.summary : L"Поточний вибір: " + plan.error;
        SetWindowTextW(summary_, text.c_str());
    }

    void RefreshRemovalList() {
        SendMessageW(removalList_, LB_RESETCONTENT, 0, 0);
        for (std::size_t index = 0; index < snapshot_.removalIntervals.size(); ++index) {
            const auto& interval = snapshot_.removalIntervals[index];
            const std::wstring label = std::to_wstring(index + 1) + L". " +
                FormatExportSeconds(interval.startMs) + L"–" +
                FormatExportSeconds(interval.endMs) + L" с";
            SendMessageW(removalList_, LB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(label.c_str()));
        }
        if (!snapshot_.removalIntervals.empty()) {
            SendMessageW(removalList_, LB_SETCURSEL, 0, 0);
        }
        EnableWindow(removeListed_, !snapshot_.removalIntervals.empty() && !exporting_);
        EnableWindow(clearListed_, !snapshot_.removalIntervals.empty() && !exporting_);
    }

    void RemoveListedInterval(const bool clearAll) {
        if (exporting_) return;
        if (clearAll) {
            snapshot_.removalIntervals.clear();
        } else {
            const LRESULT index = SendMessageW(removalList_, LB_GETCURSEL, 0, 0);
            if (index == LB_ERR ||
                static_cast<std::size_t>(index) >= snapshot_.removalIntervals.size()) return;
            snapshot_.removalIntervals.erase(snapshot_.removalIntervals.begin() + index);
        }
        if (updateIntervals_) updateIntervals_(snapshot_.removalIntervals);
        RefreshRemovalList();
        EnableWindow(save_, FALSE);
        UpdateSummary();
    }

    void Browse() {
        std::vector<wchar_t> name(32768, L'\0');
        const std::wstring current = WindowText(path_);
        wcsncpy_s(name.data(), name.size(), current.c_str(), _TRUNCATE);
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"MP4 відео (*.mp4)\0*.mp4\0Matroska (*.mkv)\0*.mkv\0WebM (*.webm)\0*.webm\0\0";
        dialog.lpstrFile = name.data();
        dialog.nMaxFile = static_cast<DWORD>(name.size());
        dialog.lpstrDefExt = L"mp4";
        dialog.Flags = kPrivateSaveDialogFlags;
        if (GetSaveFileNameW(&dialog) != FALSE) {
            SetWindowTextW(path_, name.data());
            UpdateSummary();
        }
    }

    void BeginExport() {
        if (removing_) {
            BeginRemoval();
            return;
        }
        ExportOptions options = Options();
        std::wstring absoluteDestination;
        if (options.destinationPath.empty() ||
            !MakeAbsolutePath(options.destinationPath, absoluteDestination)) {
            MessageBoxW(window_, L"Не вдалося визначити абсолютний шлях результату.",
                L"Експорт", MB_OK | MB_ICONERROR);
            return;
        }
        options.destinationPath = absoluteDestination;
        SetWindowTextW(path_, options.destinationPath.c_str());
        ExportPlan plan = BuildExportPlan(snapshot_, probe_, options, VisibleRegion());
        if (plan.requiresCodecFallbackConfirmation) {
            if (MessageBoxW(window_,
                    (plan.error + L"\r\n\r\nПродовжити з H.264/AAC у MP4?").c_str(),
                    L"Підтвердження кодека", MB_YESNO | MB_ICONWARNING) != IDYES) return;
            options.confirmedCodecFallback = true;
            plan = BuildExportPlan(snapshot_, probe_, options, VisibleRegion());
        }
        if (!plan.valid) {
            MessageBoxW(window_, plan.error.c_str(), L"Експорт недоступний", MB_OK | MB_ICONWARNING);
            return;
        }
        if (MessageBoxW(window_, (plan.summary + L"\r\n\r\nПочати експорт?").c_str(),
                L"Підтвердження експорту", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return;
        const DWORD attributes = GetFileAttributesW(PathForWin32Io(options.destinationPath).c_str());
        bool overwrite = false;
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                MessageBoxW(window_, L"Шлях результату вказує на каталог.", L"Експорт", MB_OK | MB_ICONERROR);
                return;
            }
            if (MessageBoxW(window_, L"Файл уже існує. Перезаписати його після успішної перевірки?",
                    L"Підтвердження перезапису", MB_YESNO | MB_ICONWARNING) != IDYES) return;
            overwrite = true;
        }
        if (worker_.joinable()) worker_.join();
        exporting_ = true;
        cancellation_.store(false);
        EnableConfiguration(FALSE);
        SetWindowTextW(status_, L"Експорт відео…");
        if (SetWindowSubclass(owner_, OwnerWindowProc, kOwnerSubclassId,
                reinterpret_cast<DWORD_PTR>(this)) != FALSE) {
            ownerSubclassed_ = true;
            EnableWindow(owner_, TRUE);
        }
        const std::wstring tools = ExportToolsDirectory();
        worker_ = std::thread([this, options, plan, overwrite, tools] {
            jobResult_ = RunExportJob(tools + L"\\ffmpeg.exe", tools + L"\\ffprobe.exe",
                snapshot_, options, plan, overwrite, cancellation_,
                [this](const int percent, const std::wstring& text) {
                    auto update = std::make_unique<ProgressUpdate>();
                    update->percent = percent;
                    update->text = text;
                    PostMessageW(window_, kExportProgress, 0,
                        reinterpret_cast<LPARAM>(update.release()));
                });
            PostMessageW(window_, kExportDone, 0, 0);
        });
    }

    void BeginRemoval() {
        if (probing_ || exporting_) return;
        const bool replaceSource =
            SendMessageW(replaceSource_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        std::wstring destination;
        if (replaceSource) {
            destination = snapshot_.sourcePath;
        } else if (!MakeAbsolutePath(WindowText(path_), destination)) {
            MessageBoxW(window_, L"Некоректний шлях нового файла.",
                L"Видалення відрізків", MB_OK | MB_ICONERROR);
            return;
        }
        const RemovalPlan plan =
            BuildRemovalPlan(snapshot_, probe_, keyframes_, destination);
        if (!plan.valid) {
            MessageBoxW(window_, plan.error.c_str(), L"Видалення відрізків",
                MB_OK | MB_ICONWARNING);
            return;
        }
        const ExportFileIdentity destinationIdentity =
            CaptureExportFileIdentity(destination);
        if (!replaceSource && destinationIdentity.valid &&
            destinationIdentity.volume == snapshot_.sourceIdentity.volume &&
            destinationIdentity.fileIndex == snapshot_.sourceIdentity.fileIndex) {
            MessageBoxW(window_, L"Новий файл не може бути джерелом або його hard link.",
                L"Видалення відрізків", MB_OK | MB_ICONERROR);
            return;
        }
        const DWORD attributes = GetFileAttributesW(PathForWin32Io(destination).c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            MessageBoxW(window_, L"Шлях результату є каталогом.",
                L"Видалення відрізків", MB_OK | MB_ICONERROR);
            return;
        }
        const bool overwrite = replaceSource || attributes != INVALID_FILE_ATTRIBUTES;
        std::wstring confirmation = L"Фактичних інтервалів на видалення: " +
            std::to_wstring(plan.actualIntervals.size()) + L".\r\n";
        if (plan.actualIntervals.size() <= 8) {
            for (const auto& interval : plan.actualIntervals) {
                confirmation += FormatExportSeconds(interval.startMs) + L"–" +
                    FormatExportSeconds(interval.endMs) + L" с\r\n";
            }
        } else {
            confirmation += L"Усі фактичні межі показано в плані діалогу.\r\n";
        }
        confirmation += L"Тривалість результату: приблизно " +
            FormatExportSeconds(plan.expectedDurationMs) + L" с.\r\n"
            L"Відео й аудіо зберігаються без перекодування.\r\n\r\n";
        confirmation += replaceSource
            ? L"Після повної перевірки буде перезаписано ВІДКРИТИЙ ОРИГІНАЛ. Продовжити?"
            : overwrite
                ? L"Існуючий файл буде перезаписано після перевірки. Продовжити?"
                : L"Зберегти результат у новий файл?";
        if (MessageBoxW(window_, confirmation.c_str(), L"Підтвердження видалення відрізків",
                MB_YESNO | MB_ICONWARNING) != IDYES) return;
        if (worker_.joinable()) worker_.join();
        exporting_ = true;
        cancellation_.store(false);
        EnableConfiguration(FALSE);
        SetWindowTextW(status_, L"Копіювання фрагментів…");
        if (SetWindowSubclass(owner_, OwnerWindowProc, kOwnerSubclassId,
                reinterpret_cast<DWORD_PTR>(this)) != FALSE) {
            ownerSubclassed_ = true;
            EnableWindow(owner_, TRUE);
        }
        const std::wstring tools = ExportToolsDirectory();
        const ExportSnapshot jobSnapshot = snapshot_;
        worker_ = std::thread([this, plan, destination, overwrite, replaceSource, tools, jobSnapshot] {
            jobResult_ = RunRemovalJob(tools + L"\\ffmpeg.exe",
                tools + L"\\ffprobe.exe", jobSnapshot, probe_, plan,
                destination, overwrite, replaceSource, cancellation_,
                [this](int percent, const std::wstring& message) {
                    auto update = std::make_unique<ProgressUpdate>();
                    update->percent = percent;
                    update->text = message;
                    PostMessageW(window_, kExportProgress, 0,
                        reinterpret_cast<LPARAM>(update.release()));
                });
            PostMessageW(window_, kExportDone, 0, 0);
        });
    }

    void EnableConfiguration(const BOOL enabled) {
        if (removing_) {
            EnableWindow(removalList_, enabled);
            EnableWindow(removeListed_, enabled && !snapshot_.removalIntervals.empty());
            EnableWindow(clearListed_, enabled && !snapshot_.removalIntervals.empty());
            EnableWindow(replaceSource_, enabled);
            const bool replaceSource = SendMessageW(
                replaceSource_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            EnableWindow(path_, enabled && !replaceSource);
            EnableWindow(browse_, enabled && !replaceSource);
            EnableWindow(save_, enabled && !probing_ && probe_.valid &&
                !snapshot_.removalIntervals.empty());
            return;
        }
        for (const HWND control : {fragment_, image_, resolution_, method_, audio_, path_, browse_, save_}) {
            EnableWindow(control, enabled);
        }
        if (enabled != FALSE && SendMessageW(image_, CB_GETCURSEL, 0, 0) != 1) {
            EnableWindow(resolution_, FALSE);
        }
    }

    void DetachOwnerSubclass() {
        if (ownerSubclassed_ && owner_ != nullptr && IsWindow(owner_) != FALSE) {
            RemoveWindowSubclass(owner_, OwnerWindowProc, kOwnerSubclassId);
        }
        ownerSubclassed_ = false;
    }

    void Close() {
        if (exporting_ || probing_) {
            cancellation_.store(true);
            SetWindowTextW(status_, L"Скасування…");
            EnableWindow(cancel_, FALSE);
            return;
        }
        DestroyWindow(window_);
    }

    LRESULT Handle(const UINT message, const WPARAM wParam, const LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            probing_ = true;
            CreateControls();
            return 0;
        case WM_COMMAND:
            if (removing_ && LOWORD(wParam) == kReplaceSource &&
                HIWORD(wParam) == BN_CLICKED) {
                if (exporting_) return 0;
                const bool replaceSource = SendMessageW(
                    replaceSource_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                if (replaceSource) {
                    savedDestination_ = WindowText(path_);
                    SetWindowTextW(path_, snapshot_.sourcePath.c_str());
                } else {
                    SetWindowTextW(path_, savedDestination_.c_str());
                }
                EnableConfiguration(TRUE);
                UpdateSummary();
                return 0;
            }
            if (removing_ && LOWORD(wParam) == kPath && HIWORD(wParam) == EN_CHANGE) {
                UpdateSummary();
            }
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                if (LOWORD(wParam) == kImage) {
                    const bool visibleZoom = SendMessageW(image_, CB_GETCURSEL, 0, 0) == 1;
                    if (visibleZoom) SendMessageW(resolution_, CB_SETCURSEL, 1, 0);
                    EnableWindow(resolution_, visibleZoom && !exporting_ ? TRUE : FALSE);
                }
                UpdateSummary();
            }
            switch (LOWORD(wParam)) {
            case kRemoveListed: RemoveListedInterval(false); return 0;
            case kClearListed: RemoveListedInterval(true); return 0;
            case kBrowse: Browse(); return 0;
            case kSave: BeginExport(); return 0;
            case kCancel: Close(); return 0;
            default: break;
            }
            break;
        case kProbeDone:
            if (worker_.joinable()) worker_.join();
            probing_ = false;
            if (!probe_.valid || (removing_ && keyframes_.empty())) {
                SetWindowTextW(status_, L"Аналіз не виконано.");
                SetWindowTextW(summary_,
                    (removing_ && !keyframeError_.empty() ? keyframeError_ : probe_.error).c_str());
            } else {
                SetWindowTextW(status_, removing_
                    ? L"Готово до видалення відрізків." : L"Готово до експорту.");
                EnableWindow(save_, TRUE);
                UpdateSummary();
            }
            if (cancellation_.load()) DestroyWindow(window_);
            return 0;
        case kExportProgress: {
            std::unique_ptr<ProgressUpdate> update(reinterpret_cast<ProgressUpdate*>(lParam));
            if (update) {
                SendMessageW(progress_, PBM_SETPOS, update->percent, 0);
                SetWindowTextW(status_, update->text.c_str());
            }
            return 0;
        }
        case kExportDone:
            if (worker_.joinable()) worker_.join();
            exporting_ = false;
            if (closeOwnerWhenDone_) {
                if (removing_ && !jobResult_.partialPath.empty()) {
                    DeleteFileW(PathForWin32Io(jobResult_.partialPath).c_str());
                }
                const HWND owner = owner_;
                DetachOwnerSubclass();
                DestroyWindow(window_);
                if (owner != nullptr && IsWindow(owner) != FALSE) {
                    EnableWindow(owner, TRUE);
                    PostMessageW(owner, WM_CLOSE, 0, 0);
                }
                return 0;
            }
            if (removing_ && cancellation_.load() && jobResult_.success &&
                !jobResult_.partialPath.empty()) {
                DeleteFileW(PathForWin32Io(jobResult_.partialPath).c_str());
                jobResult_.success = false;
                jobResult_.cancelled = true;
            }
            if (removing_ && jobResult_.success && !jobResult_.partialPath.empty()) {
                std::wstring error;
                if (owner_ == nullptr || IsWindow(owner_) == FALSE ||
                    !commitSource_ ||
                    !commitSource_(snapshot_, jobResult_.partialPath, error)) {
                    jobResult_.success = false;
                    if (error.empty()) {
                        error = L"Вікно плеєра вже закрито; оригінал не перезаписано.";
                    }
                    jobResult_.diagnostics = error +
                        L"\r\nПеревірений результат збережено тут:\r\n" +
                        jobResult_.partialPath;
                } else {
                    jobResult_.partialPath.clear();
                }
            }
            if (owner_ != nullptr && IsWindow(owner_) != FALSE) EnableWindow(owner_, FALSE);
            if (jobResult_.success) {
                MessageBoxW(window_, (L"Відео успішно збережено:\r\n" + jobResult_.finalPath).c_str(),
                    removing_ ? L"Відрізки видалено" : L"Експорт завершено",
                    MB_OK | MB_ICONINFORMATION);
                DestroyWindow(window_);
            } else if (jobResult_.cancelled) {
                SetWindowTextW(status_, L"Експорт скасовано; partial-файл видалено.");
                EnableWindow(cancel_, TRUE);
                EnableConfiguration(TRUE);
            } else {
                SetWindowTextW(status_, L"Помилка експорту.");
                MessageBoxW(window_, jobResult_.diagnostics.c_str(), L"Помилка експорту", MB_OK | MB_ICONERROR);
                EnableWindow(cancel_, TRUE);
                EnableConfiguration(TRUE);
            }
            return 0;
        case WM_CLOSE:
            Close();
            return 0;
        case WM_DESTROY:
            if (owner_ != nullptr && IsWindow(owner_) != FALSE &&
                GetPropW(owner_, kActiveDialogProperty) == reinterpret_cast<HANDLE>(window_)) {
                RemovePropW(owner_, kActiveDialogProperty);
            }
            DetachOwnerSubclass();
            window_ = nullptr;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND fragment_ = nullptr;
    HWND image_ = nullptr;
    HWND resolution_ = nullptr;
    HWND method_ = nullptr;
    HWND audio_ = nullptr;
    HWND path_ = nullptr;
    HWND browse_ = nullptr;
    HWND save_ = nullptr;
    HWND cancel_ = nullptr;
    HWND summary_ = nullptr;
    HWND progress_ = nullptr;
    HWND status_ = nullptr;
    HWND replaceSource_ = nullptr;
    HWND removalList_ = nullptr;
    HWND removeListed_ = nullptr;
    HWND clearListed_ = nullptr;
    std::wstring savedDestination_;
    ExportSnapshot snapshot_;
    MediaProbeResult probe_;
    ExportJobResult jobResult_;
    std::vector<std::int64_t> keyframes_;
    std::wstring keyframeError_;
    bool removing_ = false;
    CommitSourceRemoval commitSource_;
    UpdateRemovalIntervals updateIntervals_;
    ExportSourceRegion visibleRegion_{};
    std::atomic_bool cancellation_{false};
    std::thread worker_;
    bool probing_ = false;
    bool exporting_ = false;
    bool ownerSubclassed_ = false;
    bool closeOwnerWhenDone_ = false;
};

}  // namespace

bool HasActiveExportDialog(const HWND owner) noexcept {
    return owner != nullptr && GetPropW(owner, kActiveDialogProperty) != nullptr;
}

void ShowExportDialog(HWND owner, HINSTANCE instance, ExportSnapshot snapshot) {
    Dialog(owner, instance, std::move(snapshot)).Run();
}

void ShowRemoveSegmentDialog(HWND owner, HINSTANCE instance,
    ExportSnapshot snapshot, CommitSourceRemoval commitSource,
    UpdateRemovalIntervals updateIntervals) {
    Dialog(owner, instance, std::move(snapshot), true, std::move(commitSource),
        std::move(updateIntervals)).Run();
}

}  // namespace videoplayer
