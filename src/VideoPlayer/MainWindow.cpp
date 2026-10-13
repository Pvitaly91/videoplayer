#include "targetver.h"
#include "MainWindow.h"
#include "AppIcon.h"

#include "ExportDialog.h"
#include "ExportJob.h"
#include "IntervalTimeline.h"

#include "MediaOpen.h"
#include "PlaybackMath.h"
#include "PathUtils.h"
#include "PreviewMath.h"
#include "ProgressStrip.h"
#include "PrivacyPolicy.h"
#include "Resource.h"
#include "TimeFormatter.h"
#include "ToolbarVisibility.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Shell32.lib")

namespace videoplayer {
namespace {

constexpr wchar_t kWindowClassName[] = L"VideoPlayer.MainWindow";
constexpr wchar_t kVideoClassName[] = L"VideoPlayer.VideoSurface";
constexpr wchar_t kControlBarClassName[] = L"VideoPlayer.ControlBar";
constexpr wchar_t kProgressStripClassName[] = L"VideoPlayer.ProgressStrip";
constexpr wchar_t kToolbarRestoreClassName[] = L"VideoPlayer.ToolbarRestore";
constexpr wchar_t kApplicationTitle[] = L"VideoPlayer";
constexpr wchar_t kDropPrompt[] =
    L"Перетягніть відеофайли сюди або натисніть “Відкрити”";
constexpr wchar_t kRuntimeError[] =
    L"Не знайдено компоненти відтворення LibVLC. Повністю розпакуйте програму та не переносіть окремо лише VideoPlayer.exe.";
constexpr wchar_t kMediaError[] =
    L"Не вдалося відтворити файл. Він може бути пошкоджений або містити непідтримуваний кодек.";
constexpr wchar_t kZoomStartText[] = L"Зум області (Z)";
constexpr wchar_t kZoomCancelText[] = L"Скасувати (Z)";
constexpr wchar_t kZoomResetText[] = L"Скинути зум (Z)";
constexpr wchar_t kHideToolbarText[] = L"Сховати панель керування";
constexpr wchar_t kRestoreToolbarText[] = L"Показати панель керування";

constexpr UINT_PTR kUiTimer = 1;
constexpr UINT_PTR kToolbarVisibilityTimer = 2;
constexpr UINT kUiTimerIntervalMs = 100;
constexpr UINT kToolbarVisibilityTimerIntervalMs = 100;
constexpr UINT_PTR kSeekSubclassId = 1;
constexpr int kSeekRange = 10000;
constexpr int kDefaultDpi = 96;
constexpr int kInitialWidth = 1000;
constexpr int kInitialHeight = 650;
constexpr int kMinimumWidth = 760;
constexpr int kMinimumHeight = 400;
constexpr int kControlBarHeight = 154;
constexpr int kProgressStripHeight = 3;
constexpr int kToolbarRestoreWidth = 48;
constexpr int kToolbarRestoreHeight = 34;
constexpr int kToolbarRestoreBottomMargin = 8;
constexpr int kMinimumZoomSelectionLogicalPixels = 28;
constexpr std::size_t kMaximumFilesPerOpen = 8;
constexpr BYTE kFullscreenControlBarAlpha = 160;
constexpr BYTE kToolbarRestoreAlpha = 176;
constexpr COLORREF kProgressStripColor = RGB(255, 0, 0);
constexpr COLORREF kSavedIntervalColor = RGB(255, 153, 0);
constexpr COLORREF kProgressTransparentColor = RGB(1, 0, 1);

constexpr wchar_t kOpenFilter[] =
    L"Відеофайли\0*.mp4;*.avi;*.mkv;*.mov;*.m4v;*.webm;*.wmv;*.mpg;*.mpeg;*.ts;*.m2ts;*.mts;*.flv;*.3gp;*.ogv;*.vob\0"
    L"Усі файли\0*.*\0\0";

bool IsHighContrastEnabled() noexcept {
    HIGHCONTRASTW highContrast{};
    highContrast.cbSize = sizeof(highContrast);
    return SystemParametersInfoW(
               SPI_GETHIGHCONTRAST,
               sizeof(highContrast),
               &highContrast,
               0) != FALSE &&
        (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

HMENU ControlId(const int id) noexcept {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

bool IsClassAlreadyRegistered() noexcept {
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

std::wstring FormatLoopPoint(const std::int64_t milliseconds) {
    if (milliseconds < 0) {
        return L"—";
    }
    return FormatTime(milliseconds) + L"." +
        std::to_wstring(1000 + milliseconds % 1000).substr(1);
}

std::wstring FileNameFromPath(const std::wstring& path) {
    const std::wstring::size_type separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos || separator + 1 >= path.size()) {
        return path;
    }
    return path.substr(separator + 1);
}

bool PointInsideRect(const POINT point, const RECT& rectangle) noexcept {
    return point.x >= rectangle.left && point.x < rectangle.right &&
        point.y >= rectangle.top && point.y < rectangle.bottom;
}

}  // namespace

#if defined(VIDEOPLAYER_TESTING)
MainWindow::MainWindow(PreviewBackendFactory previewBackendFactory)
    : previewEngine_(std::move(previewBackendFactory)) {
}
#endif

MainWindow::~MainWindow() {
    if (window_ != nullptr && IsWindow(window_) != FALSE) {
        DestroyWindow(window_);
    }
    ShutdownPlaybackComponents();
    if (ownsFont_ && uiFont_ != nullptr) {
        DeleteObject(uiFont_);
        uiFont_ = nullptr;
    }
}

bool MainWindow::Create(const HINSTANCE instance, std::wstring& error) {
    instance_ = instance;
    if (!RegisterWindowClasses(error)) {
        return false;
    }

    HDC screenDc = GetDC(nullptr);
    if (screenDc != nullptr) {
        dpi_ = GetDeviceCaps(screenDc, LOGPIXELSX);
        ReleaseDC(nullptr, screenDc);
    }
    if (dpi_ <= 0) {
        dpi_ = kDefaultDpi;
    }

    const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    RECT desiredRect{0, 0, Scale(kInitialWidth), Scale(kInitialHeight)};
    AdjustWindowRectEx(&desiredRect, style, FALSE, 0);

    window_ = CreateWindowExW(
        0,
        kWindowClassName,
        kApplicationTitle,
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        desiredRect.right - desiredRect.left,
        desiredRect.bottom - desiredRect.top,
        nullptr,
        nullptr,
        instance_,
        this);
    if (window_ == nullptr) {
        error = creationError_.empty()
            ? L"Не вдалося створити головне вікно VideoPlayer."
            : creationError_;
        return false;
    }

    if (!player_.Initialize(videoRenderWindow_, window_, error)) {
        error = kRuntimeError;
        DestroyWindow(window_);
        return false;
    }
    playerInitialized_ = true;
    player_.SetVolume(displayedVolume_);
    player_.SetMuted(false);

    if (!previewPopup_.Create(window_, instance_, error) ||
        !selectionOverlay_.Create(instance_, window_, window_, error) ||
        !previewEngine_.Initialize(window_)) {
        if (error.empty()) {
            error = L"Не вдалося ініціалізувати попередній перегляд відео.";
        }
        DestroyWindow(window_);
        return false;
    }
    previewInitialized_ = true;
    previewEngine_.SetPrefetchEnabled(true);

    DragAcceptFiles(window_, TRUE);
    SetTimer(window_, kUiTimer, kUiTimerIntervalMs, nullptr);
    lastInteractionMs_ = GetTickCount64();
    RefreshControls();
    return true;
}

void MainWindow::Show(const int showCommand) const {
    ShowWindow(window_, showCommand);
    UpdateWindow(window_);
}

int MainWindow::RunMessageLoop() {
    MSG message{};
    BOOL result = FALSE;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (ProcessZoomMouseMessage(message) || ProcessKeyboardMessage(message)) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return result == -1 ? 6 : static_cast<int>(message.wParam);
}

bool MainWindow::OpenFile(const std::wstring& path) {
    std::wstring absolutePath;
    std::wstring ioPath;
    switch (PrepareMediaFilePath(path, absolutePath, ioPath)) {
    case MediaPathStatus::Ready:
        break;
    case MediaPathStatus::Empty:
        return false;
    case MediaPathStatus::CannotResolve:
        MessageBoxW(window_, L"Не вдалося визначити повний шлях до файла.", kApplicationTitle, MB_OK | MB_ICONERROR);
        return false;
    case MediaPathStatus::NotFound:
        MessageBoxW(window_, L"Файл не знайдено.", kApplicationTitle, MB_OK | MB_ICONERROR);
        return false;
    case MediaPathStatus::Directory:
        MessageBoxW(window_, L"Вибраний шлях є каталогом. Виберіть відеофайл.", kApplicationTitle, MB_OK | MB_ICONERROR);
        return false;
    case MediaPathStatus::Inaccessible:
        MessageBoxW(window_, L"Не вдалося отримати доступ до вибраного файла.", kApplicationTitle, MB_OK | MB_ICONERROR);
        return false;
    }

    ResetMediaUiState();
    if (previewInitialized_) {
        // Invalidate the old path/cache before the main player begins media
        // replacement. The preview worker observes this generation gate and
        // releases its old media independently of the main LibVLC player.
        previewEngine_.SetMedia({}, player_.Generation());
    }
    mediaErrorShown_ = false;
    std::wstring ignoredError;
    if (!player_.Open(ioPath, ignoredError)) {
        if (previewInitialized_) {
            previewEngine_.SetMedia({}, player_.Generation());
        }
        hasMedia_ = player_.HasMedia();
        if (!hasMedia_) {
            currentPath_.clear();
            SetPromptVisible(true);
            UpdateWindowTitle();
            RestoreExecutionState();
        }
        ShowMediaError();
        RefreshControls();
        return false;
    }

    currentPath_ = absolutePath;
    hasMedia_ = true;
    if (previewInitialized_) {
        previewEngine_.SetMedia(ioPath, player_.Generation());
    }
    SetPromptVisible(false);
    UpdateWindowTitle();
    RegisterInteraction();
    RefreshControls();
    return true;
}

void MainWindow::OpenFiles(const std::vector<std::wstring>& paths) {
    const std::size_t fileCount = static_cast<std::size_t>(std::count_if(
        paths.begin(), paths.end(), [](const std::wstring& path) {
            return !path.empty();
        }));
    if (fileCount > kMaximumFilesPerOpen) {
        MessageBoxW(
            window_,
            L"За один раз можна відкрити не більше 8 відео. Виберіть менше файлів.",
            kApplicationTitle,
            MB_OK | MB_ICONWARNING);
        return;
    }

    std::size_t first = 0;
    while (first < paths.size() && paths[first].empty()) {
        ++first;
    }
    if (first == paths.size()) {
        return;
    }

    OpenFile(paths[first]);

    std::wstring firstLaunchError;
    std::size_t failedLaunches = 0;
    for (std::size_t index = first + 1; index < paths.size(); ++index) {
        if (paths[index].empty()) {
            continue;
        }
        std::wstring launchError;
        if (!LaunchMediaInNewInstance(paths[index], launchError)) {
            ++failedLaunches;
            if (firstLaunchError.empty()) {
                firstLaunchError = std::move(launchError);
            }
        }
    }

    if (failedLaunches != 0) {
        std::wstring message = firstLaunchError.empty()
            ? L"Не вдалося відкрити одне або кілька відео в окремих вікнах."
            : firstLaunchError;
        if (failedLaunches > 1) {
            message.append(L" Не відкрито файлів: ");
            message.append(std::to_wstring(failedLaunches));
            message.push_back(L'.');
        }
        MessageBoxW(window_, message.c_str(), kApplicationTitle, MB_OK | MB_ICONERROR);
    }
}

LRESULT CALLBACK MainWindow::StaticWindowProc(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* const create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    }
    return self == nullptr
        ? DefWindowProcW(window, message, wParam, lParam)
        : self->HandleMessage(message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::StaticVideoProc(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* const create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self == nullptr
        ? DefWindowProcW(window, message, wParam, lParam)
        : self->HandleVideoMessage(window, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::StaticControlBarProc(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* const create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self == nullptr
        ? DefWindowProcW(window, message, wParam, lParam)
        : self->HandleControlBarMessage(window, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::StaticToolbarRestoreProc(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* const create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self == nullptr
        ? DefWindowProcW(window, message, wParam, lParam)
        : self->HandleToolbarRestoreMessage(window, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::StaticSeekSubclass(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam,
    const UINT_PTR subclassId,
    const DWORD_PTR referenceData) {
    UNREFERENCED_PARAMETER(subclassId);
    MainWindow* const self = reinterpret_cast<MainWindow*>(referenceData);
    return self == nullptr
        ? DefSubclassProc(window, message, wParam, lParam)
        : self->HandleSeekSubclass(window, message, wParam, lParam);
}

LRESULT CALLBACK MainWindow::StaticProgressStripProc(
    const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam) {
    MainWindow* self = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self == nullptr
        ? DefWindowProcW(window, message, wParam, lParam)
        : self->HandleProgressStripMessage(window, message, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        if (!CreateChildWindows()) {
            creationError_ = L"Не вдалося створити елементи керування VideoPlayer.";
            return -1;
        }
        return 0;

    case WM_SIZE:
        EndZoomPan();
        minimized_ = wParam == SIZE_MINIMIZED;
        if (minimized_) {
            CancelTransientUi();
            return 0;
        }
        if (zoomState_ == ZoomState::Selecting) {
            CancelTransientUi();
        }
        RestoreUiAfterLifecycle();
        return 0;

    case WM_MOVE:
        EndZoomPan();
        if (zoomState_ == ZoomState::Selecting) {
            CancelTransientUi();
        }
        if ((fullscreen_ || toolbarManuallyHidden_) && IsIconic(window_) == FALSE) {
            RECT client{};
            if (GetClientRect(window_, &client) != FALSE) {
                LayoutChildren(
                    static_cast<int>(client.right - client.left),
                    static_cast<int>(client.bottom - client.top));
            }
        }
        break;

    case WM_ENABLE:
        if (fullscreenControlBar_ != nullptr) {
            EnableWindow(fullscreenControlBar_, wParam != FALSE);
        }
        if (toolbarRestorePopup_ != nullptr) {
            EnableWindow(toolbarRestorePopup_, wParam != FALSE);
        }
        if (wParam == FALSE) {
            CancelTransientUi();
        } else {
            RestoreUiAfterLifecycle();
        }
        break;

    case WM_CANCELMODE:
        CancelTransientUi();
        break;

    case WM_ACTIVATEAPP:
        applicationActive_ = wParam != FALSE;
        if (!applicationActive_) {
            CancelTransientUi();
        } else {
            RestoreUiAfterLifecycle();
        }
        break;

    case WM_SHOWWINDOW:
        if (wParam == FALSE) {
            CancelTransientUi();
        } else {
            RestoreUiAfterLifecycle();
        }
        break;

    case WM_WINDOWPOSCHANGED: {
        const auto* position = reinterpret_cast<const WINDOWPOS*>(lParam);
        const LRESULT result = DefWindowProcW(window_, message, wParam, lParam);
        if (position != nullptr && (position->flags & SWP_SHOWWINDOW) != 0) {
            RestoreUiAfterLifecycle();
        }
        return result;
    }

    case WM_GETMINMAXINFO: {
        auto* const info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = Scale(kMinimumWidth);
        info->ptMinTrackSize.y = Scale(kMinimumHeight);
        return 0;
    }

    case WM_COMMAND:
        if (shutdownComplete_ || IsWindowEnabled(window_) == FALSE) {
            return 0;
        }
        if (LOWORD(wParam) == IDC_SAVED_INTERVAL_LIST && HIWORD(wParam) == CBN_SELCHANGE) {
            RegisterInteraction();
            InvalidateIntervalTimeline();
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED) {
            RegisterInteraction();
            switch (LOWORD(wParam)) {
            case IDC_OPEN_BUTTON:
                ShowOpenDialog();
                FinishMouseControlInteraction();
                return 0;
            case IDC_PLAY_BUTTON:
                TogglePlayback();
                FinishMouseControlInteraction();
                return 0;
            case IDC_STOP_BUTTON:
                StopPlayback();
                FinishMouseControlInteraction();
                return 0;
            case IDC_EXPORT_BUTTON:
                ShowExportVideoDialog();
                FinishMouseControlInteraction();
                return 0;
            case IDC_REMOVE_SEGMENT_BUTTON:
                ShowRemoveSegmentVideoDialog();
                FinishMouseControlInteraction();
                return 0;
            case IDC_ADD_REMOVAL_BUTTON:
                AddRemovalInterval();
                FinishMouseControlInteraction();
                return 0;
            case IDC_REMOVE_SAVED_INTERVAL:
                RemoveSavedInterval();
                FinishMouseControlInteraction();
                return 0;
            case IDC_MUTE_BUTTON:
                ToggleMute();
                FinishMouseControlInteraction();
                return 0;
            case IDC_ZOOM_BUTTON:
                ToggleAreaZoom();
                FinishMouseControlInteraction();
                return 0;
            case IDC_FULLSCREEN_BUTTON:
                ToggleFullscreen();
                FinishMouseControlInteraction();
                return 0;
            case IDC_HIDE_TOOLBAR_BUTTON:
                SetToolbarManuallyHidden(true);
                FinishMouseControlInteraction();
                return 0;
            case IDC_LOOP_VIDEO_BUTTON:
                ToggleWholeVideoLoop();
                FinishMouseControlInteraction();
                return 0;
            case IDC_LOOP_START_BUTTON:
                SetLoopPoint(true);
                FinishMouseControlInteraction();
                return 0;
            case IDC_LOOP_END_BUTTON:
                SetLoopPoint(false);
                FinishMouseControlInteraction();
                return 0;
            case IDC_LOOP_SEGMENT_BUTTON:
                ToggleSegmentLoop();
                FinishMouseControlInteraction();
                return 0;
            case IDC_LOOP_CLEAR_BUTTON:
                ClearLoopSegment();
                FinishMouseControlInteraction();
                return 0;
            default:
                break;
            }
        }
        break;

    case WM_HSCROLL: {
        if (cancellingTransient_ || minimized_ || !applicationActive_ ||
            IsWindowEnabled(window_) == FALSE) {
            return 0;
        }
        const HWND source = reinterpret_cast<HWND>(lParam);
        const int notification = LOWORD(wParam);
        RegisterInteraction();
        if (source == volumeSlider_) {
            volumeDragging_ = notification != TB_ENDTRACK && GetCapture() == volumeSlider_;
            const int volume = static_cast<int>(SendMessageW(volumeSlider_, TBM_GETPOS, 0, 0));
            SetVolumeFromSlider(volume);
            if (notification == TB_ENDTRACK) {
                volumeDragging_ = false;
                FinishMouseControlInteraction();
            }
            return 0;
        }
        if (source == seekSlider_) {
            if (notification == TB_ENDTRACK) {
                const bool commit = seekDragging_;
                seekDragging_ = false;
                if (commit) {
                    CommitSeekFromSlider();
                }
                HideSeekPreview();
                FinishMouseControlInteraction();
            } else {
                seekDragging_ = GetCapture() == seekSlider_;
                UpdateSeekLabel();
                const bool discreteSeek = notification == TB_LINEUP ||
                    notification == TB_LINEDOWN || notification == TB_PAGEUP ||
                    notification == TB_PAGEDOWN || notification == TB_TOP ||
                    notification == TB_BOTTOM;
                if (discreteSeek && !seekDragging_) {
                    // Keyboard changes have no mouse capture. Commit here,
                    // not on ENDTRACK, which also follows cancelled drags.
                    CommitSeekFromSlider();
                    HideSeekPreview();
                }
            }
            return 0;
        }
        break;
    }

    case WM_TIMER:
        if (wParam == kUiTimer) {
            RefreshControls();
            return 0;
        }
        if (wParam == kToolbarVisibilityTimer) {
            UpdateControlBarVisibility();
            return 0;
        }
        break;

    case WM_MOUSEMOVE:
        RegisterInteraction(true);
        return 0;

    case WM_MOUSEWHEEL:
        if (HandleZoomWheel(GET_WHEEL_DELTA_WPARAM(wParam),
                {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)})) {
            return 0;
        }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) {
            RestoreUiAfterLifecycle();
            RegisterInteraction();
        } else {
            const HWND next = reinterpret_cast<HWND>(lParam);
            if (next == nullptr || GetAncestor(next, GA_ROOTOWNER) != window_) {
                CancelTransientUi();
            }
        }
        break;

    case WM_SETTINGCHANGE:
        ApplyFullscreenControlBarOpacity();
        break;

    case WM_DPICHANGED: {
        dpi_ = static_cast<int>(HIWORD(wParam));
        CancelTransientUi();
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested != nullptr && !fullscreen_) {
            SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
        }
        RestoreUiAfterLifecycle();
        return 0;
    }

    case WM_DROPFILES:
        HandleDroppedFiles(reinterpret_cast<HDROP>(wParam));
        return 0;

    case kPlayerEventMessage:
        HandlePlayerEvent(
            static_cast<PlayerEvent>(wParam),
            static_cast<std::uint32_t>(lParam));
        return 0;

    case kPreviewFrameReadyMessage:
        HandlePreviewResult();
        return 0;

    case kSelectionOverlayMessage:
        HandleSelectionOverlay(
            static_cast<SelectionOverlayEvent>(wParam),
            reinterpret_cast<const SelectionOverlayResult*>(lParam));
        return 0;

    case WM_CLOSE:
        DestroyWindow(window_);
        return 0;

    case WM_DESTROY:
        CancelTransientUi();
        KillTimer(window_, kUiTimer);
        KillTimer(window_, kToolbarVisibilityTimer);
        DragAcceptFiles(window_, FALSE);
        RestoreExecutionState();
        ShutdownPlaybackComponents();
        PostQuitMessage(0);
        return 0;

    case WM_NCDESTROY: {
        const HWND destroyedWindow = window_;
        SetWindowLongPtrW(destroyedWindow, GWLP_USERDATA, 0);
        const LRESULT result = DefWindowProcW(destroyedWindow, message, wParam, lParam);
        window_ = nullptr;
        return result;
    }

    default:
        break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

LRESULT MainWindow::HandleVideoMessage(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND: {
        RECT bounds{};
        GetClientRect(window, &bounds);
        FillRect(reinterpret_cast<HDC>(wParam), &bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT bounds{};
        GetClientRect(window, &bounds);
        FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        if (promptVisible_) {
            const HFONT oldFont = static_cast<HFONT>(SelectObject(dc, uiFont_));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(225, 225, 225));
            RECT textBounds = bounds;
            const int inset = Scale(24);
            InflateRect(&textBounds, -inset, -inset);
            DrawTextW(dc, kDropPrompt, -1, &textBounds,
                DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, oldFont);
        }
        EndPaint(window, &paint);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (ZoomGestureActive()) {
            if ((wParam & MK_LBUTTON) == 0) {
                EndZoomPan();
            } else {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (ClientToScreen(window, &point) != FALSE) {
                    if (videoResizeDragging_) {
                        UpdateVideoResize(point);
                    } else {
                        UpdateZoomPan(point);
                    }
                }
            }
        }
        RegisterInteraction(true);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (ClientToScreen(window, &point) != FALSE &&
            (BeginVideoResize(point) || BeginZoomPan(point))) {
            return 0;
        }
        RegisterInteraction();
        SetFocus(window_);
        return 0;
    }

    case WM_LBUTTONUP:
        if (ZoomGestureActive()) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (ClientToScreen(window, &point) != FALSE) {
                if (videoResizeDragging_) {
                    UpdateVideoResize(point);
                } else {
                    UpdateZoomPan(point);
                }
            }
            EndZoomPan();
            RegisterInteraction();
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (ZoomGestureActive() && reinterpret_cast<HWND>(lParam) != videoWindow_) {
            EndZoomPan();
        }
        return 0;

    case WM_CANCELMODE:
        CancelTransientUi();
        return 0;

    case WM_MOUSEWHEEL:
        if (HandleZoomWheel(GET_WHEEL_DELTA_WPARAM(wParam),
                {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)})) {
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);

    case WM_SETCURSOR: {
        POINT point{};
        if (LOWORD(lParam) == HTCLIENT && GetCursorPos(&point) != FALSE) {
            SetVideoPointerCursor(point);
            return TRUE;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    case WM_LBUTTONDBLCLK: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (ClientToScreen(window, &point) != FALSE &&
            (ZoomGestureActive() || BeginVideoResize(point))) {
            return 0;
        }
        RegisterInteraction();
        ToggleFullscreen();
        return 0;
    }

    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

LRESULT MainWindow::HandleControlBarMessage(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    switch (message) {
    case WM_NOTIFY: {
        const auto* draw = reinterpret_cast<const NMCUSTOMDRAW*>(lParam);
        if (draw == nullptr || draw->hdr.hwndFrom != seekSlider_ ||
            draw->hdr.code != NM_CUSTOMDRAW) break;
        if (draw->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
        if (draw->dwDrawStage == CDDS_POSTPAINT) {
            DrawTimelineMarkers(draw->hdc, SeekTimelineRect(), true);
            return CDRF_DODEFAULT;
        }
        if (draw->dwItemSpec == TBCD_CHANNEL) {
            if (draw->dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYPOSTPAINT;
            if (draw->dwDrawStage == CDDS_ITEMPOSTPAINT) {
                RECT channel{}, thumb{};
                SendMessageW(seekSlider_, TBM_GETCHANNELRECT, 0,
                    reinterpret_cast<LPARAM>(&channel));
                SendMessageW(seekSlider_, TBM_GETTHUMBRECT, 0,
                    reinterpret_cast<LPARAM>(&thumb));
                RECT timeline = TrackbarPointerRange(channel, thumb);
                if (timeline.bottom - timeline.top > 2) {
                    ++timeline.top;
                    --timeline.bottom;
                }
                DrawSavedIntervals(draw->hdc, timeline);
                DrawCurrentInterval(draw->hdc, timeline);
            }
        }
        return CDRF_DODEFAULT;
    }

    case WM_COMMAND:
    case WM_HSCROLL:
        RegisterInteraction();
        return SendMessageW(window_, message, wParam, lParam);

    case WM_MOUSEMOVE:
        RegisterInteraction(true);
        return 0;

    case WM_MOUSEACTIVATE:
        if (window == fullscreenControlBar_) {
            POINT cursor{};
            const HWND hitWindow = GetCursorPos(&cursor) != FALSE
                ? WindowFromPoint(cursor)
                : nullptr;
            const std::array<HWND, 19> interactiveControls{
                openButton_, playButton_, stopButton_, exportButton_,
                removeSegmentButton_, addRemovalButton_, savedIntervalList_, removeSavedIntervalButton_, seekSlider_,
                muteButton_, volumeSlider_, zoomButton_, fullscreenButton_,
                hideToolbarButton_, loopVideoButton_, loopStartButton_,
                loopEndButton_, loopSegmentButton_, loopClearButton_};
            const bool overInteractiveControl = std::any_of(
                interactiveControls.begin(),
                interactiveControls.end(),
                [hitWindow](const HWND control) {
                    return control != nullptr && hitWindow != nullptr &&
                        (hitWindow == control || IsChild(control, hitWindow) != FALSE);
                });
            if (!overInteractiveControl) {
                if (videoWindow_ != nullptr) {
                    SetFocus(videoWindow_);
                }
                return MA_NOACTIVATE;
            }
        }
        break;

    case WM_LBUTTONDOWN:
        if (window == fullscreenControlBar_) {
            RegisterInteraction();
            if (videoWindow_ != nullptr) {
                SetFocus(videoWindow_);
            }
            return 0;
        }
        break;

    case WM_CLOSE:
        SendMessageW(window_, WM_CLOSE, 0, 0);
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0U) == SC_CLOSE) {
            SendMessageW(window_, WM_CLOSE, 0, 0);
            return 0;
        }
        break;

    case WM_PARENTNOTIFY:
        if (LOWORD(wParam) == WM_LBUTTONDOWN) {
            // For mouse notifications HIWORD(wParam) is undefined; only
            // WM_CREATE/WM_DESTROY carry a child control ID there. Every
            // descendant mouse-down here belongs to a toolbar control.
            mouseControlInteraction_ = true;
        }
        break;

    case WM_ERASEBKGND: {
        RECT bounds{};
        GetClientRect(window, &bounds);
        FillRect(
            reinterpret_cast<HDC>(wParam),
            &bounds,
            reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
        return 1;
    }

    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;

    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT MainWindow::HandleToolbarRestoreMessage(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_RESTORE_TOOLBAR_BUTTON &&
            HIWORD(wParam) == BN_CLICKED &&
            reinterpret_cast<HWND>(lParam) == toolbarRestoreButton_) {
            SetToolbarManuallyHidden(false);
            return 0;
        }
        break;

    case WM_MOUSEACTIVATE:
        // Keep playback keyboard focus, but do not eat the restoring click.
        return MA_NOACTIVATE;

    case WM_DRAWITEM: {
        const auto* const item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item == nullptr || item->CtlID != IDC_RESTORE_TOOLBAR_BUTTON) {
            break;
        }

        const bool highContrast = IsHighContrastEnabled();
        const bool pressed = (item->itemState & ODS_SELECTED) != 0;
        const COLORREF background = highContrast
            ? GetSysColor(pressed ? COLOR_HIGHLIGHT : COLOR_BTNFACE)
            : (pressed ? RGB(62, 62, 62) : RGB(28, 28, 28));
        const COLORREF foreground = highContrast
            ? GetSysColor(pressed ? COLOR_HIGHLIGHTTEXT : COLOR_BTNTEXT)
            : RGB(255, 255, 255);
        HBRUSH backgroundBrush = CreateSolidBrush(background);
        HBRUSH borderBrush = CreateSolidBrush(foreground);
        if (backgroundBrush != nullptr) {
            FillRect(item->hDC, &item->rcItem, backgroundBrush);
            DeleteObject(backgroundBrush);
        }
        if (borderBrush != nullptr) {
            FrameRect(item->hDC, &item->rcItem, borderBrush);
            DeleteObject(borderBrush);
        }

        const int offset = pressed ? Scale(1) : 0;
        const int centerX = (item->rcItem.left + item->rcItem.right) / 2 + offset;
        const int centerY = (item->rcItem.top + item->rcItem.bottom) / 2 + offset;
        const int halfWidth = (std::max)(Scale(7), 2);
        const int rise = (std::max)(Scale(5), 2);
        HPEN chevronPen = CreatePen(PS_SOLID, (std::max)(1, Scale(2)), foreground);
        if (chevronPen != nullptr) {
            const HGDIOBJ previousPen = SelectObject(item->hDC, chevronPen);
            MoveToEx(item->hDC, centerX - halfWidth, centerY + rise / 2, nullptr);
            LineTo(item->hDC, centerX, centerY - rise);
            LineTo(item->hDC, centerX + halfWidth, centerY + rise / 2);
            SelectObject(item->hDC, previousPen);
            DeleteObject(chevronPen);
        }
        return TRUE;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;

    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;

    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void MainWindow::DrawSavedIntervals(const HDC dc, const RECT& timeline) const noexcept {
    if (dc == nullptr || !hasMedia_ || removalIntervals_.empty() ||
        playbackSnapshot_.durationMs <= 0) return;
    const int savedDc = SaveDC(dc);
    if (savedDc == 0) return;
    SetDCBrushColor(dc, IsHighContrastEnabled()
        ? GetSysColor(COLOR_HIGHLIGHT) : kSavedIntervalColor);
    for (const auto& interval : removalIntervals_) {
        const RECT rectangle = IntervalTimelineRect(
            interval, playbackSnapshot_.durationMs, timeline);
        if (rectangle.right > rectangle.left && rectangle.bottom > rectangle.top) {
            FillRect(dc, &rectangle, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        }
    }
    RestoreDC(dc, savedDc);
}

void MainWindow::DrawCurrentInterval(const HDC dc, const RECT& timeline) const noexcept {
    if (dc == nullptr || !hasMedia_ || !playbackLoop_.HasSegment(playbackSnapshot_.durationMs)) return;
    const RECT bounds = IntervalTimelineRect({playbackLoop_.StartMs(), playbackLoop_.EndMs()},
        playbackSnapshot_.durationMs, timeline);
    const int saved = SaveDC(dc);
    if (saved == 0) return;
    SetDCBrushColor(dc, IsHighContrastEnabled() ? GetSysColor(COLOR_HIGHLIGHT) : RGB(0, 160, 230));
    FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    RestoreDC(dc, saved);
}

RECT MainWindow::SeekTimelineRect() const noexcept {
    RECT channel{}, thumb{};
    if (seekSlider_ == nullptr) return {};
    SendMessageW(seekSlider_, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
    SendMessageW(seekSlider_, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    return TrackbarPointerRange(channel, thumb);
}

void MainWindow::DrawTimelineMarkers(const HDC dc, const RECT& timeline, const bool labels) const noexcept {
    if (dc == nullptr || !hasMedia_ || playbackSnapshot_.durationMs <= 0 || timeline.right <= timeline.left) return;
    const int saved = SaveDC(dc);
    if (saved == 0) return;
    HFONT font = labels ? CreateFontW(-Scale(10), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH, L"Segoe UI") : nullptr;
    if (font != nullptr) SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    const auto draw = [&](const std::int64_t time, const bool start, const COLORREF color, const bool caption) {
        if (time < 0 || time > playbackSnapshot_.durationMs) return;
        const LONG x = IntervalTimelineX(time, playbackSnapshot_.durationMs, timeline);
        const int half = labels ? (std::max)(1, Scale(1)) : 1;
        RECT line{(std::max)(timeline.left, x - half), labels ? (std::max)(0L, timeline.top - Scale(3)) : timeline.top,
            (std::min)(timeline.right, x + half + 1), labels ? timeline.bottom + Scale(5) : timeline.bottom};
        SetDCBrushColor(dc, color);
        FillRect(dc, &line, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        if (labels) {
            RECT cap{(std::max)(timeline.left, x - Scale(4)), (std::max)(0L, timeline.top - Scale(4)),
                (std::min)(timeline.right, x + Scale(4) + 1), timeline.top};
            FillRect(dc, &cap, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            if (caption) {
                SetTextColor(dc, IsHighContrastEnabled() ? GetSysColor(COLOR_BTNTEXT) : color);
                RECT text{x - Scale(10), (std::max)(0L, timeline.top - Scale(16)),
                    x + Scale(10), (std::max)(Scale(11), static_cast<int>(timeline.top - Scale(4)))};
                DrawTextW(dc, start ? L"A" : L"B", 1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
    };
    const auto selected = SendMessageW(savedIntervalList_, CB_GETCURSEL, 0, 0);
    const auto orange = IsHighContrastEnabled() ? GetSysColor(COLOR_HIGHLIGHT) : kSavedIntervalColor;
    for (std::size_t index = 0; index < removalIntervals_.size(); ++index) {
        const auto& interval = removalIntervals_[index];
        draw(interval.startMs, true, orange, selected == static_cast<LRESULT>(index));
        draw(interval.endMs, false, orange, selected == static_cast<LRESULT>(index));
    }
    draw(playbackLoop_.StartMs(), true, IsHighContrastEnabled() ? GetSysColor(COLOR_HIGHLIGHT) : RGB(0, 160, 230), true);
    draw(playbackLoop_.EndMs(), false, IsHighContrastEnabled() ? GetSysColor(COLOR_HIGHLIGHT) : RGB(0, 190, 100), true);
    RestoreDC(dc, saved);
    if (font != nullptr) DeleteObject(font);
}

int MainWindow::SavedIntervalAtPointer(const int mouseX) const noexcept {
    const auto time = PreviewTimeFromChannelX(mouseX, SeekTimelineRect(), playbackSnapshot_.durationMs);
    for (std::size_t i = 0; i < removalIntervals_.size(); ++i) {
        if (time >= removalIntervals_[i].startMs && time < removalIntervals_[i].endMs) return static_cast<int>(i);
    }
    return -1;
}

bool MainWindow::BeginMarkerDrag(const int mouseX, const int mouseY) {
    if (markerDragging_ || !hasMedia_ || !playerInitialized_ || HasActiveExportDialog(window_)) return false;
    playbackSnapshot_ = player_.Snapshot();
    const auto duration = playbackSnapshot_.durationMs;
    if (!playbackSnapshot_.seekable || duration <= 0 || playbackSnapshot_.state == PlaybackState::Opening ||
        playbackSnapshot_.state == PlaybackState::Error) return false;
    const RECT timeline = SeekTimelineRect();
    if (timeline.right <= timeline.left) return false;
    // Marker flags live above the channel; the channel/thumb keep normal seek.
    if (mouseY >= 0 && mouseY >= timeline.top) return false;
    int index = -2;
    bool start = true;
    long long distance = Scale(8) + 1;
    const auto consider = [&](const VideoInterval interval, const int candidateIndex) {
        for (const bool candidateStart : {true, false}) {
            const auto time = candidateStart ? interval.startMs : interval.endMs;
            if (time < 0 || time > duration) continue;
            const auto delta = std::llabs(static_cast<long long>(mouseX) - IntervalTimelineX(time, duration, timeline));
            if (delta < distance) { distance = delta; index = candidateIndex; start = candidateStart; }
        }
    };
    consider({playbackLoop_.StartMs(), playbackLoop_.EndMs()}, -1);
    const auto selected = SendMessageW(savedIntervalList_, CB_GETCURSEL, 0, 0);
    if (selected >= 0 && static_cast<std::size_t>(selected) < removalIntervals_.size()) {
        consider(removalIntervals_[selected], static_cast<int>(selected));
    }
    for (std::size_t i = 0; i < removalIntervals_.size(); ++i) consider(removalIntervals_[i], static_cast<int>(i));
    if (index == -2 || distance > Scale(8)) return false;
    const auto generation = player_.Generation();
    SetFocus(seekSlider_);
    SetCapture(seekSlider_);
    if (GetCapture() != seekSlider_) return true;
    if (!hasMedia_ || generation != player_.Generation()) { ReleaseCapture(); return true; }
    markerSavedIndex_ = index;
    markerStart_ = start;
    markerGeneration_ = generation;
    markerOriginalLoop_ = playbackLoop_;
    markerOriginal_ = index < 0 ? VideoInterval{playbackLoop_.StartMs(), playbackLoop_.EndMs()} : removalIntervals_[index];
    if (index >= 0) SendMessageW(savedIntervalList_, CB_SETCURSEL, index, 0);
    markerDragging_ = true;
    seekDragging_ = false;
    mouseControlInteraction_ = true;
    playbackLoop_.CancelPending();
    HideSeekPreview();
    RegisterInteraction();
    InvalidateIntervalTimeline();
    return true;
}

void MainWindow::UpdateMarkerDrag(const int mouseX) {
    if (!markerDragging_) return;
    if (markerSavedIndex_ >= 0 && static_cast<std::size_t>(markerSavedIndex_) >= removalIntervals_.size()) {
        EndMarkerDrag(false);
        if (GetCapture() == seekSlider_) ReleaseCapture();
        return;
    }
    if (markerGeneration_ != player_.Generation() || playbackSnapshot_.state == PlaybackState::Error) {
        EndMarkerDrag(false);
        if (GetCapture() == seekSlider_) ReleaseCapture();
        return;
    }
    const auto duration = playbackSnapshot_.durationMs;
    const auto desired = PreviewTimeFromChannelX(mouseX, SeekTimelineRect(), duration);
    VideoInterval candidate = markerSavedIndex_ < 0
        ? VideoInterval{playbackLoop_.StartMs(), playbackLoop_.EndMs()}
        : removalIntervals_[markerSavedIndex_];
    if (markerStart_) candidate.startMs = desired;
    else candidate.endMs = desired;
    if (candidate.startMs < 0 || candidate.startMs >= duration || candidate.endMs > duration ||
        (candidate.endMs >= 0 && candidate.endMs - candidate.startMs < 500)) return;
    for (std::size_t i = 0; i < removalIntervals_.size(); ++i) {
        if (static_cast<int>(i) == markerSavedIndex_) continue;
        if (candidate.endMs < 0 ? IsInsideSavedInterval(candidate.startMs, {removalIntervals_[i]})
                               : OverlapsSavedInterval(candidate, {removalIntervals_[i]})) return;
    }
    if (markerSavedIndex_ >= 0) {
        if (playbackLoop_.HasSegment(duration) && OverlapsSavedInterval(candidate,
            {{playbackLoop_.StartMs(), playbackLoop_.EndMs()}})) return;
        const auto currentA = playbackLoop_.StartMs();
        if (currentA >= 0 && candidate.startMs < currentA && currentA < candidate.endMs) return;
        removalIntervals_[markerSavedIndex_] = candidate;
        RefreshSavedIntervalList();
    } else if (!playbackLoop_.MovePoint(markerStart_, desired, duration)) return;
    RegisterInteraction(true);
    UpdateLoopControls();
    InvalidateIntervalTimeline();
}

void MainWindow::EndMarkerDrag(const bool cancel) noexcept {
    if (!markerDragging_) return;
    markerDragging_ = false;
    if (cancel && hasMedia_ && markerGeneration_ == player_.Generation() &&
        playbackSnapshot_.state != PlaybackState::Error) {
        if (markerSavedIndex_ < 0) { playbackLoop_ = markerOriginalLoop_; playbackLoop_.CancelPending(); }
        else if (static_cast<std::size_t>(markerSavedIndex_) < removalIntervals_.size()) removalIntervals_[markerSavedIndex_] = markerOriginal_;
    }
    markerSavedIndex_ = -1;
    InvalidateIntervalTimeline();
}

void MainWindow::RefreshSavedIntervalList() {
    const bool same = listedRemovalIntervals_.size() == removalIntervals_.size() &&
        std::equal(listedRemovalIntervals_.begin(), listedRemovalIntervals_.end(), removalIntervals_.begin(),
            [](const VideoInterval& a, const VideoInterval& b) { return a.startMs == b.startMs && a.endMs == b.endMs; });
    if (same || savedIntervalList_ == nullptr) return;
    const auto selected = SendMessageW(savedIntervalList_, CB_GETCURSEL, 0, 0);
    SendMessageW(savedIntervalList_, CB_RESETCONTENT, 0, 0);
    for (std::size_t i = 0; i < removalIntervals_.size(); ++i) {
        const auto& interval = removalIntervals_[i];
        const std::wstring text = std::to_wstring(i + 1) + L". " + FormatLoopPoint(interval.startMs) +
            L"–" + FormatLoopPoint(interval.endMs);
        SendMessageW(savedIntervalList_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    if (!removalIntervals_.empty()) SendMessageW(savedIntervalList_, CB_SETCURSEL,
        (std::max)(LRESULT{0}, (std::min)(selected, static_cast<LRESULT>(removalIntervals_.size() - 1))), 0);
    listedRemovalIntervals_ = removalIntervals_;
}

void MainWindow::RemoveSavedInterval(int index) {
    if (!hasMedia_ || !playerInitialized_ || shutdownComplete_ || markerDragging_ || HasActiveExportDialog(window_)) return;
    if (index < 0) index = static_cast<int>(SendMessageW(savedIntervalList_, CB_GETCURSEL, 0, 0));
    if (index < 0 || static_cast<std::size_t>(index) >= removalIntervals_.size()) return;
    removalIntervals_.erase(removalIntervals_.begin() + index);
    RefreshSavedIntervalList();
    InvalidateIntervalTimeline();
    RegisterInteraction();
    RefreshControls();
}

void MainWindow::DrawCompactProgress(const HDC dc, const RECT& bounds) const noexcept {
    if (dc == nullptr) return;
    const int savedDc = SaveDC(dc);
    if (savedDc == 0) return;
    SetDCBrushColor(dc, kProgressTransparentColor);
    FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    RECT completed = bounds;
    completed.right = (std::min)(bounds.right,
        bounds.left + static_cast<LONG>(progressCompletedWidth_));
    if (completed.right > completed.left) {
        SetDCBrushColor(dc, kProgressStripColor);
        FillRect(dc, &completed, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    DrawSavedIntervals(dc, bounds);
    DrawCurrentInterval(dc, bounds);
    DrawTimelineMarkers(dc, bounds, false);
    RestoreDC(dc, savedDc);
}

LRESULT MainWindow::HandleProgressStripMessage(
    const HWND window, const UINT message, const WPARAM wParam, const LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT bounds{};
        GetClientRect(window, &bounds);
        DrawCompactProgress(dc, bounds);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT bounds{};
        GetClientRect(window, &bounds);
        DrawCompactProgress(reinterpret_cast<HDC>(wParam), bounds);
        return 0;
    }
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void MainWindow::InvalidateIntervalTimeline() noexcept {
    if (seekSlider_ != nullptr) InvalidateRect(seekSlider_, nullptr, FALSE);
    if (progressStrip_ != nullptr) InvalidateRect(progressStrip_, nullptr, FALSE);
    UpdateProgressStrip();
}

LRESULT MainWindow::HandleSeekSubclass(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    switch (message) {
    case WM_MOUSEMOVE: {
        if (markerDragging_) {
            UpdateMarkerDrag(GET_X_LPARAM(lParam));
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return 0;
        }
        if (!seekMouseTracking_) {
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = window;
            seekMouseTracking_ = TrackMouseEvent(&tracking) != FALSE;
        }
        RegisterInteraction(true);
        if (seekDragging_ && GetCapture() == window) {
            SetSeekSliderFromPointer(GET_X_LPARAM(lParam));
        }
        HandleSeekPointer(GET_X_LPARAM(lParam));
        if (seekDragging_) {
            return 0;
        }
        break;
    }

    case WM_MOUSELEAVE:
        seekMouseTracking_ = false;
        if (!seekDragging_) {
            HideSeekPreview();
        }
        break;

    case WM_LBUTTONDOWN:
        if (BeginMarkerDrag(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) return 0;
        if (const int index = SavedIntervalAtPointer(GET_X_LPARAM(lParam)); index >= 0) {
            SendMessageW(savedIntervalList_, CB_SETCURSEL, index, 0);
            InvalidateIntervalTimeline();
        }
        if (hasMedia_ && player_.IsSeekable() && player_.DurationMs() > 0) {
            playbackLoop_.CancelPending();
            seekDragging_ = true;
            mouseControlInteraction_ = true;
            RegisterInteraction();
            SetFocus(window);
            SetCapture(window);
            if (GetCapture() != window) {
                seekDragging_ = false;
                FinishMouseControlInteraction();
                HideSeekPreview();
                return 0;
            }
            SetSeekSliderFromPointer(GET_X_LPARAM(lParam));
            HandleSeekPointer(GET_X_LPARAM(lParam));
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (markerDragging_) {
            UpdateMarkerDrag(GET_X_LPARAM(lParam));
            EndMarkerDrag(false);
            if (GetCapture() == window) ReleaseCapture();
            RefreshControls();
            FinishMouseControlInteraction();
            return 0;
        }
        if (seekDragging_) {
            RegisterInteraction();
            SetSeekSliderFromPointer(GET_X_LPARAM(lParam));
            seekDragging_ = false;
            if (GetCapture() == window) {
                ReleaseCapture();
            }
            CommitSeekFromSlider();
            HideSeekPreview();
            FinishMouseControlInteraction();
            return 0;
        }
        break;

    case WM_CANCELMODE:
        if (seekDragging_ || markerDragging_) {
            CancelTransientUi();
            RefreshControls();
            return 0;
        }
        break;

    case WM_CAPTURECHANGED:
        if ((seekDragging_ || markerDragging_) && reinterpret_cast<HWND>(lParam) != window) {
            CancelTransientUi();
            RefreshControls();
            return 0;
        }
        break;

    case WM_NCDESTROY:
        EndMarkerDrag(false);
        seekMouseTracking_ = false;
        seekDragging_ = false;
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        break;

    case WM_KEYDOWN:
        if (markerDragging_ && wParam == VK_ESCAPE) {
            CancelTransientUi();
            RefreshControls();
            return 0;
        }
        break;

    case WM_RBUTTONUP: {
        if (markerDragging_ || HasActiveExportDialog(window_) || !hasMedia_) return 0;
        const int index = SavedIntervalAtPointer(GET_X_LPARAM(lParam));
        if (index < 0) break;
        SendMessageW(savedIntervalList_, CB_SETCURSEL, index, 0);
        InvalidateIntervalTimeline();
        const auto interval = removalIntervals_[index];
        const auto generation = player_.Generation();
        mouseControlInteraction_ = true;
        SetFocus(window);
        RegisterInteraction();
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr) return 0;
        AppendMenuW(menu, MF_STRING, 1, L"Прибрати цей відрізок зі списку");
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ClientToScreen(window, &point);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            point.x, point.y, 0, window_, nullptr);
        DestroyMenu(menu);
        if (command == 1 && generation == player_.Generation() &&
            static_cast<std::size_t>(index) < removalIntervals_.size() &&
            removalIntervals_[index].startMs == interval.startMs && removalIntervals_[index].endMs == interval.endMs) {
            RemoveSavedInterval(index);
        }
        FinishMouseControlInteraction();
        return 0;
    }

    default:
        break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

bool MainWindow::RegisterWindowClasses(std::wstring& error) const {
    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.style = CS_HREDRAW | CS_VREDRAW;
    mainClass.lpfnWndProc = StaticWindowProc;
    mainClass.hInstance = instance_;
    mainClass.hIcon = LoadPlayerIcon(instance_,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    mainClass.lpszClassName = kWindowClassName;
    mainClass.hIconSm = LoadPlayerIcon(instance_,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    if (RegisterClassExW(&mainClass) == 0 && !IsClassAlreadyRegistered()) {
        error = L"Не вдалося зареєструвати головне вікно VideoPlayer.";
        return false;
    }

    WNDCLASSEXW videoClass{};
    videoClass.cbSize = sizeof(videoClass);
    videoClass.style = CS_DBLCLKS;
    videoClass.lpfnWndProc = StaticVideoProc;
    videoClass.hInstance = instance_;
    videoClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    videoClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    videoClass.lpszClassName = kVideoClassName;
    if (RegisterClassExW(&videoClass) == 0 && !IsClassAlreadyRegistered()) {
        error = L"Не вдалося зареєструвати область відео.";
        return false;
    }

    WNDCLASSEXW controlBarClass{};
    controlBarClass.cbSize = sizeof(controlBarClass);
    controlBarClass.style = CS_HREDRAW | CS_VREDRAW;
    controlBarClass.lpfnWndProc = StaticControlBarProc;
    controlBarClass.hInstance = instance_;
    controlBarClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    controlBarClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    controlBarClass.lpszClassName = kControlBarClassName;
    if (RegisterClassExW(&controlBarClass) == 0 && !IsClassAlreadyRegistered()) {
        error = L"Не вдалося зареєструвати панель керування.";
        return false;
    }

    WNDCLASSEXW progressStripClass{};
    progressStripClass.cbSize = sizeof(progressStripClass);
    progressStripClass.style = CS_HREDRAW;
    progressStripClass.lpfnWndProc = StaticProgressStripProc;
    progressStripClass.hInstance = instance_;
    progressStripClass.lpszClassName = kProgressStripClassName;
    if (RegisterClassExW(&progressStripClass) == 0 && !IsClassAlreadyRegistered()) {
        error = L"Не вдалося зареєструвати індикатор прогресу.";
        return false;
    }

    WNDCLASSEXW toolbarRestoreClass{};
    toolbarRestoreClass.cbSize = sizeof(toolbarRestoreClass);
    toolbarRestoreClass.style = CS_HREDRAW | CS_VREDRAW;
    toolbarRestoreClass.lpfnWndProc = StaticToolbarRestoreProc;
    toolbarRestoreClass.hInstance = instance_;
    toolbarRestoreClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
    toolbarRestoreClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    toolbarRestoreClass.lpszClassName = kToolbarRestoreClassName;
    if (RegisterClassExW(&toolbarRestoreClass) == 0 && !IsClassAlreadyRegistered()) {
        error = L"Не вдалося зареєструвати кнопку відновлення панелі.";
        return false;
    }
    return true;
}

bool MainWindow::CreateChildWindows() {
    const DWORD child = WS_CHILD | WS_VISIBLE;
    videoWindow_ = CreateWindowExW(
        0,
        kVideoClassName,
        nullptr,
        child | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, 0, 0,
        window_,
        ControlId(IDC_VIDEO_SURFACE),
        instance_,
        this);
    videoRenderWindow_ = videoWindow_ == nullptr ? nullptr : CreateWindowExW(
        0, kVideoClassName, nullptr,
        child | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, 0, 0, videoWindow_,
        ControlId(IDC_VIDEO_RENDER_SURFACE), instance_, this);
    controlBar_ = CreateWindowExW(
        0,
        kControlBarClassName,
        nullptr,
        child | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, 0, 0,
        window_,
        ControlId(IDC_CONTROL_BAR),
        instance_,
        this);
    fullscreenControlBar_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kControlBarClassName,
        nullptr,
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 0, 0,
        window_,
        nullptr,
        instance_,
        this);
    progressStrip_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        kProgressStripClassName,
        nullptr,
        WS_POPUP,
        0, 0, 0, 0,
        window_,
        nullptr,
        instance_,
        this);
    toolbarRestorePopup_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kToolbarRestoreClassName,
        nullptr,
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 0, 0,
        window_,
        nullptr,
        instance_,
        this);

    openButton_ = CreateWindowExW(
        0, L"BUTTON", L"Відкрити", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, controlBar_, ControlId(IDC_OPEN_BUTTON), instance_, nullptr);
    playButton_ = CreateWindowExW(
        0, L"BUTTON", L"Відтворити", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, controlBar_, ControlId(IDC_PLAY_BUTTON), instance_, nullptr);
    stopButton_ = CreateWindowExW(
        0, L"BUTTON", L"Стоп", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, controlBar_, ControlId(IDC_STOP_BUTTON), instance_, nullptr);
    exportButton_ = CreateWindowExW(
        0, L"BUTTON", L"Зберегти відео…", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, controlBar_, ControlId(IDC_EXPORT_BUTTON), instance_, nullptr);
    removeSegmentButton_ = CreateWindowExW(
        0, L"BUTTON", L"Видалити відрізки…", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, controlBar_,
        ControlId(IDC_REMOVE_SEGMENT_BUTTON), instance_, nullptr);
    addRemovalButton_ = CreateWindowExW(
        0, L"BUTTON", L"Додати A–B до списку", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, controlBar_, ControlId(IDC_ADD_REMOVAL_BUTTON), instance_, nullptr);
    removalCountLabel_ = CreateWindowExW(
        0, L"STATIC", L"У списку на видалення: 0",
        child | SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS,
        0, 0, 0, 0, controlBar_, ControlId(IDC_REMOVAL_COUNT_LABEL), instance_, nullptr);
    savedIntervalList_ = CreateWindowExW(
        0, WC_COMBOBOXW, nullptr, child | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 0, 0, controlBar_, ControlId(IDC_SAVED_INTERVAL_LIST), instance_, nullptr);
    removeSavedIntervalButton_ = CreateWindowExW(
        0, L"BUTTON", L"Прибрати зі списку", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, controlBar_, ControlId(IDC_REMOVE_SAVED_INTERVAL), instance_, nullptr);
    seekSlider_ = CreateWindowExW(
        0, TRACKBAR_CLASSW, nullptr,
        child | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_ENABLESELRANGE,
        0, 0, 0, 0, controlBar_, ControlId(IDC_SEEK_SLIDER), instance_, nullptr);
    currentTimeLabel_ = CreateWindowExW(
        0, L"STATIC", L"00:00", child | SS_CENTER | SS_CENTERIMAGE,
        0, 0, 0, 0, controlBar_, ControlId(IDC_CURRENT_TIME), instance_, nullptr);
    durationLabel_ = CreateWindowExW(
        0, L"STATIC", L"00:00", child | SS_CENTER | SS_CENTERIMAGE,
        0, 0, 0, 0, controlBar_, ControlId(IDC_DURATION), instance_, nullptr);
    muteButton_ = CreateWindowExW(
        0, L"BUTTON", L"Без звуку", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, controlBar_, ControlId(IDC_MUTE_BUTTON), instance_, nullptr);
    volumeSlider_ = CreateWindowExW(
        0, TRACKBAR_CLASSW, nullptr, child | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
        0, 0, 0, 0, controlBar_, ControlId(IDC_VOLUME_SLIDER), instance_, nullptr);
    zoomButton_ = CreateWindowExW(
        0, L"BUTTON", kZoomStartText, child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, controlBar_, ControlId(IDC_ZOOM_BUTTON), instance_, nullptr);
    fullscreenButton_ = CreateWindowExW(
        0, L"BUTTON", L"На весь екран", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, controlBar_, ControlId(IDC_FULLSCREEN_BUTTON), instance_, nullptr);
    hideToolbarButton_ = CreateWindowExW(
        0, L"BUTTON", kHideToolbarText, child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, controlBar_, ControlId(IDC_HIDE_TOOLBAR_BUTTON), instance_, nullptr);
    loopVideoButton_ = CreateWindowExW(
        0, L"BUTTON", L"Повтор відео", child | WS_TABSTOP | BS_CHECKBOX | BS_PUSHLIKE,
        0, 0, 0, 0, controlBar_, ControlId(IDC_LOOP_VIDEO_BUTTON), instance_, nullptr);
    loopStartButton_ = CreateWindowExW(
        0, L"BUTTON", L"Початок A", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, controlBar_, ControlId(IDC_LOOP_START_BUTTON), instance_, nullptr);
    loopEndButton_ = CreateWindowExW(
        0, L"BUTTON", L"Кінець B", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, controlBar_, ControlId(IDC_LOOP_END_BUTTON), instance_, nullptr);
    loopSegmentButton_ = CreateWindowExW(
        0, L"BUTTON", L"Повтор A–B", child | WS_TABSTOP | BS_CHECKBOX | BS_PUSHLIKE,
        0, 0, 0, 0, controlBar_, ControlId(IDC_LOOP_SEGMENT_BUTTON), instance_, nullptr);
    loopClearButton_ = CreateWindowExW(
        0, L"BUTTON", L"Скинути A–B", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, controlBar_, ControlId(IDC_LOOP_CLEAR_BUTTON), instance_, nullptr);
    loopRangeLabel_ = CreateWindowExW(
        0, L"STATIC", L"A: —   B: —", child | SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS,
        0, 0, 0, 0, controlBar_, ControlId(IDC_LOOP_RANGE_LABEL), instance_, nullptr);
    toolbarRestoreButton_ = CreateWindowExW(
        0, L"BUTTON", kRestoreToolbarText, child | BS_OWNERDRAW,
        0, 0, 0, 0, toolbarRestorePopup_,
        ControlId(IDC_RESTORE_TOOLBAR_BUTTON), instance_, nullptr);

    const std::array<HWND, 30> controls{
        videoWindow_, videoRenderWindow_, controlBar_, fullscreenControlBar_, progressStrip_,
        toolbarRestorePopup_, toolbarRestoreButton_,
        openButton_, playButton_, stopButton_, exportButton_, removeSegmentButton_,
        addRemovalButton_, removalCountLabel_, savedIntervalList_, removeSavedIntervalButton_,
        seekSlider_, currentTimeLabel_,
        durationLabel_, muteButton_, volumeSlider_, zoomButton_, fullscreenButton_,
        hideToolbarButton_, loopVideoButton_, loopStartButton_, loopEndButton_,
        loopSegmentButton_, loopClearButton_, loopRangeLabel_};
    if (std::any_of(
            controls.begin(), controls.end(),
            [](const HWND control) { return control == nullptr; })) {
        return false;
    }

    ApplyFullscreenControlBarOpacity();

    // A layered owned popup composites above LibVLC's native video output.
    // WS_EX_TRANSPARENT makes this noninteractive strip pass input through
    // even when the video output belongs to LibVLC's rendering thread.
    if (SetLayeredWindowAttributes(progressStrip_, kProgressTransparentColor,
            255, LWA_ALPHA | LWA_COLORKEY) == FALSE) {
        return false;
    }

    SendMessageW(seekSlider_, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(seekSlider_, TBM_SETRANGEMAX, FALSE, kSeekRange);
    SendMessageW(seekSlider_, TBM_SETPAGESIZE, 0, 100);
    SendMessageW(seekSlider_, TBM_SETPOS, TRUE, 0);
    EnableWindow(seekSlider_, FALSE);
    if (SetWindowSubclass(
            seekSlider_, StaticSeekSubclass, kSeekSubclassId,
            reinterpret_cast<DWORD_PTR>(this)) == FALSE) {
        return false;
    }

    SendMessageW(volumeSlider_, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(volumeSlider_, TBM_SETRANGEMAX, FALSE, 100);
    SendMessageW(volumeSlider_, TBM_SETPAGESIZE, 0, 5);
    SendMessageW(volumeSlider_, TBM_SETPOS, TRUE, displayedVolume_);

    ApplySystemFont();
    RECT client{};
    GetClientRect(window_, &client);
    LayoutChildren(client.right, client.bottom);
    return true;
}

void MainWindow::ApplySystemFont() {
    HDC dc = GetDC(window_);
    if (dc != nullptr) {
        dpi_ = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(window_, dc);
    }
    if (dpi_ <= 0) {
        dpi_ = kDefaultDpi;
    }

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) != FALSE) {
        uiFont_ = CreateFontIndirectW(&metrics.lfMessageFont);
        ownsFont_ = uiFont_ != nullptr;
    }
    if (uiFont_ == nullptr) {
        uiFont_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }

    const std::array<HWND, 24> controls{
        openButton_, playButton_, stopButton_, exportButton_, removeSegmentButton_,
        addRemovalButton_, removalCountLabel_, savedIntervalList_, removeSavedIntervalButton_,
        seekSlider_, currentTimeLabel_,
        durationLabel_, muteButton_, volumeSlider_, zoomButton_, fullscreenButton_,
        hideToolbarButton_, toolbarRestoreButton_, loopVideoButton_, loopStartButton_,
        loopEndButton_, loopSegmentButton_, loopClearButton_, loopRangeLabel_};
    for (const HWND control : controls) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
    }
}

void MainWindow::LayoutChildren(const int width, const int height) {
    const HWND activeControlBar = ActiveControlBar();
    if (videoWindow_ == nullptr || activeControlBar == nullptr) {
        return;
    }

    const int panelHeight = Scale(kControlBarHeight);
    const int panelTop = (std::max)(0, height - panelHeight);
    const int videoHeight = (fullscreen_ || toolbarManuallyHidden_) ? height : panelTop;
    MoveWindow(videoWindow_, 0, 0, width, (std::max)(0, videoHeight), TRUE);
    UpdateVideoDisplayLayout();

    POINT controlBarOrigin{0, panelTop};
    if (fullscreen_) {
        ClientToScreen(window_, &controlBarOrigin);
    }
    SetWindowPos(
        activeControlBar, HWND_TOP,
        controlBarOrigin.x, controlBarOrigin.y, width, panelHeight,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    const int margin = Scale(8);
    const int gap = Scale(6);
    const int rowHeight = Scale(28);
    const int timeWidth = Scale(60);
    const int hideToolbarWidth = Scale(176);
    const int firstRowY = Scale(7);
    const int sliderX = margin + timeWidth + gap;
    const int hideToolbarX = (std::max)(margin, width - margin - hideToolbarWidth);
    const int durationX = (std::max)(
        sliderX, hideToolbarX - gap - timeWidth);
    const int sliderWidth = (std::max)(
        0, durationX - gap - sliderX);

    MoveWindow(currentTimeLabel_, margin, firstRowY, timeWidth, rowHeight, TRUE);
    MoveWindow(seekSlider_, sliderX, firstRowY, sliderWidth, rowHeight, TRUE);
    MoveWindow(durationLabel_, durationX, firstRowY, timeWidth, rowHeight, TRUE);
    MoveWindow(
        hideToolbarButton_, hideToolbarX, firstRowY,
        hideToolbarWidth, rowHeight, TRUE);

    const int secondRowY = Scale(48);
    const int openWidth = Scale(76);
    const int playWidth = Scale(92);
    const int stopWidth = Scale(54);
    const int muteWidth = Scale(86);
    const int volumeWidth = Scale(92);
    const int zoomWidth = Scale(132);
    const int fullscreenWidth = Scale(116);

    int left = margin;
    MoveWindow(openButton_, left, secondRowY, openWidth, rowHeight, TRUE);
    left += openWidth + gap;
    MoveWindow(playButton_, left, secondRowY, playWidth, rowHeight, TRUE);
    left += playWidth + gap;
    MoveWindow(stopButton_, left, secondRowY, stopWidth, rowHeight, TRUE);
    left += stopWidth + gap;
    MoveWindow(exportButton_, left, secondRowY, Scale(132), rowHeight, TRUE);

    int right = width - margin;
    right -= fullscreenWidth;
    MoveWindow(fullscreenButton_, right, secondRowY, fullscreenWidth, rowHeight, TRUE);
    right -= gap + zoomWidth;
    MoveWindow(zoomButton_, right, secondRowY, zoomWidth, rowHeight, TRUE);
    right -= gap + volumeWidth;
    MoveWindow(volumeSlider_, right, secondRowY, volumeWidth, rowHeight, TRUE);
    right -= gap + muteWidth;
    MoveWindow(muteButton_, right, secondRowY, muteWidth, rowHeight, TRUE);

    const int loopRowY = Scale(84);
    left = margin;
    const std::array<std::pair<HWND, int>, 5> loopButtons{{
        {loopVideoButton_, 112}, {loopStartButton_, 92}, {loopEndButton_, 88},
        {loopSegmentButton_, 104}, {loopClearButton_, 100}}};
    for (const auto& button : loopButtons) {
        const int buttonWidth = Scale(button.second);
        MoveWindow(button.first, left, loopRowY, buttonWidth, rowHeight, TRUE);
        left += buttonWidth + gap;
    }
    MoveWindow(loopRangeLabel_, left, loopRowY,
        (std::max)(0, width - margin - left), rowHeight, TRUE);
    const int removalRowY = Scale(118);
    const int addRemovalWidth = Scale(160);
    const int removeWidth = Scale(156);
    MoveWindow(addRemovalButton_, margin, removalRowY, addRemovalWidth, rowHeight, TRUE);
    left = margin + addRemovalWidth + gap;
    MoveWindow(removeSegmentButton_, left, removalRowY, removeWidth, rowHeight, TRUE);
    left += removeWidth + gap;
    const int listWidth = (std::min)(Scale(180), (std::max)(Scale(100),
        width - left - Scale(148) - 2 * gap - margin));
    MoveWindow(savedIntervalList_, left, removalRowY, listWidth, Scale(220), TRUE);
    left += listWidth + gap;
    const int removeSavedWidth = Scale(148);
    MoveWindow(removeSavedIntervalButton_, left, removalRowY, removeSavedWidth, rowHeight, TRUE);
    left += removeSavedWidth + gap;
    MoveWindow(removalCountLabel_, left, removalRowY,
        (std::max)(0, width - margin - left), rowHeight, TRUE);

    if (fullscreen_ && controlBarVisible_ && CanShowOwnedPopups() &&
        zoomState_ != ZoomState::Selecting && !ZoomGestureActive()) {
        SetWindowPos(
            activeControlBar, HWND_TOP,
            controlBarOrigin.x, controlBarOrigin.y, width, panelHeight,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
    UpdateProgressStrip();
    const bool restoreVisible = toolbarRestorePopup_ != nullptr &&
        (GetWindowLongPtrW(toolbarRestorePopup_, GWL_STYLE) & WS_VISIBLE) != 0;
    if (restoreVisible) {
        SetToolbarRestoreVisible(true);
    }
}

int MainWindow::Scale(const int value) const noexcept {
    return MulDiv(value, dpi_ > 0 ? dpi_ : kDefaultDpi, kDefaultDpi);
}

HWND MainWindow::ActiveControlBar() const noexcept {
    if (fullscreen_ && fullscreenControlBar_ != nullptr) {
        return fullscreenControlBar_;
    }
    return controlBar_;
}

bool MainWindow::ReparentControlBarControls(const HWND parent) noexcept {
    if (parent == nullptr || IsWindow(parent) == FALSE) {
        return false;
    }

    const std::array<HWND, 23> controls{
        openButton_, playButton_, stopButton_, exportButton_, removeSegmentButton_,
        addRemovalButton_, removalCountLabel_, savedIntervalList_, removeSavedIntervalButton_,
        seekSlider_, currentTimeLabel_,
        durationLabel_, muteButton_, volumeSlider_, zoomButton_, fullscreenButton_,
        hideToolbarButton_, loopVideoButton_, loopStartButton_, loopEndButton_,
        loopSegmentButton_, loopClearButton_, loopRangeLabel_};
    std::array<HWND, 23> previousParents{};
    for (std::size_t index = 0; index < controls.size(); ++index) {
        if (controls[index] == nullptr || IsWindow(controls[index]) == FALSE) {
            return false;
        }
        previousParents[index] = GetParent(controls[index]);
        if (previousParents[index] == nullptr) {
            return false;
        }
    }
    std::size_t reparented = 0;
    for (; reparented < controls.size(); ++reparented) {
        if (previousParents[reparented] != parent &&
            setParent_(controls[reparented], parent) == nullptr) {
            break;
        }
    }
    if (reparented != controls.size()) {
        bool restored = true;
        for (std::size_t index = 0; index < reparented; ++index) {
            if (GetParent(controls[index]) != previousParents[index]) {
                ::SetParent(controls[index], previousParents[index]);
            }
            restored = restored && GetParent(controls[index]) == previousParents[index];
        }
        if (!restored) {
            // The first control's old parent is the last coherent toolbar
            // host. Make one best-effort reconciliation so a failed rollback
            // does not intentionally leave controls split between parents.
            const HWND recoveryParent = previousParents.front();
            for (const HWND control : controls) {
                if (GetParent(control) != recoveryParent) {
                    ::SetParent(control, recoveryParent);
                }
            }
        }
        return false;
    }

    RedrawWindow(
        parent,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    return true;
}

void MainWindow::ApplyFullscreenControlBarOpacity() noexcept {
    const bool highContrast = IsHighContrastEnabled();
    const auto apply = [highContrast](const HWND overlay, const BYTE alpha) {
        if (overlay == nullptr || IsWindow(overlay) == FALSE) {
            return;
        }
        LONG_PTR extendedStyle = GetWindowLongPtrW(overlay, GWL_EXSTYLE);
        if (highContrast) {
            if ((extendedStyle & WS_EX_LAYERED) != 0) {
                SetWindowLongPtrW(
                    overlay,
                    GWL_EXSTYLE,
                    extendedStyle & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
            }
        } else {
            if ((extendedStyle & WS_EX_LAYERED) == 0) {
                extendedStyle |= WS_EX_LAYERED;
                SetWindowLongPtrW(overlay, GWL_EXSTYLE, extendedStyle);
            }
            if (SetLayeredWindowAttributes(overlay, 0, alpha, LWA_ALPHA) == FALSE) {
                extendedStyle = GetWindowLongPtrW(overlay, GWL_EXSTYLE);
                SetWindowLongPtrW(
                    overlay,
                    GWL_EXSTYLE,
                    extendedStyle & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
            }
        }

        RedrawWindow(
            overlay,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    };

    apply(fullscreenControlBar_, kFullscreenControlBarAlpha);
    apply(toolbarRestorePopup_, kToolbarRestoreAlpha);
}

void MainWindow::UpdateProgressStrip() noexcept {
    if (progressStrip_ == nullptr || window_ == nullptr) {
        return;
    }

    RECT client{};
    if (!CanShowOwnedPopups() ||
        GetClientRect(videoWindow_, &client) == FALSE) {
        ShowWindow(progressStrip_, SW_HIDE);
        return;
    }

    const HWND activeControlBar = ActiveControlBar();
    const bool toolbarStyleVisible = activeControlBar != nullptr &&
        (GetWindowLongPtrW(activeControlBar, GWL_STYLE) & WS_VISIBLE) != 0;

    ProgressStripInput input{};
    input.compactMode = fullscreen_ || toolbarManuallyHidden_;
    input.toolbarVisible = toolbarStyleVisible;
    input.hasMedia = hasMedia_;
    input.availableWidth = (std::max)(
        0, static_cast<int>(client.right - client.left));
    input.positionMs = playbackSnapshot_.positionMs;
    input.durationMs = playbackSnapshot_.durationMs;
    const ProgressStripDecision decision = EvaluateProgressStrip(input);
    const bool hasSavedIntervals = (!removalIntervals_.empty() || playbackLoop_.StartMs() >= 0) && input.hasMedia &&
        input.durationMs > 0 && input.availableWidth > 0 &&
        input.compactMode && !input.toolbarVisible;
    const int previousCompletedWidth = progressCompletedWidth_;
    progressCompletedWidth_ = decision.completedWidth;
    if (!decision.visible && !hasSavedIntervals) {
        ShowWindow(progressStrip_, SW_HIDE);
        return;
    }

    const int height = (std::max)(
        0, static_cast<int>(client.bottom - client.top));
    const int stripHeight = (std::min)(height, (std::max)(1, Scale(kProgressStripHeight)));
    POINT origin{0, height - stripHeight};
    if (ClientToScreen(videoWindow_, &origin) == FALSE) {
        ShowWindow(progressStrip_, SW_HIDE);
        return;
    }
    SetWindowPos(
        progressStrip_,
        HWND_TOP,
        origin.x,
        origin.y,
        hasSavedIntervals ? input.availableWidth : decision.completedWidth,
        stripHeight,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    if (previousCompletedWidth != progressCompletedWidth_) {
        InvalidateRect(progressStrip_, nullptr, FALSE);
    }
}

void MainWindow::SetToolbarRestoreVisible(const bool visible) noexcept {
    if (toolbarRestorePopup_ == nullptr || toolbarRestoreButton_ == nullptr ||
        videoWindow_ == nullptr) {
        return;
    }

    const bool show = visible && toolbarManuallyHidden_ &&
        zoomState_ != ZoomState::Selecting && CanShowOwnedPopups();
    if (!show) {
        if ((GetWindowLongPtrW(toolbarRestorePopup_, GWL_STYLE) & WS_VISIBLE) != 0) {
            ShowWindow(toolbarRestorePopup_, SW_HIDE);
        }
        return;
    }

    RECT videoClient{};
    if (GetClientRect(videoWindow_, &videoClient) == FALSE) {
        ShowWindow(toolbarRestorePopup_, SW_HIDE);
        return;
    }
    const int videoWidth = (std::max)(
        0, static_cast<int>(videoClient.right - videoClient.left));
    const int videoHeight = (std::max)(
        0, static_cast<int>(videoClient.bottom - videoClient.top));
    const int popupWidth = (std::max)(1, Scale(kToolbarRestoreWidth));
    const int popupHeight = (std::max)(1, Scale(kToolbarRestoreHeight));
    const int bottomInset = (std::max)(1, Scale(kProgressStripHeight)) +
        (std::max)(0, Scale(kToolbarRestoreBottomMargin));
    if (videoWidth < popupWidth || videoHeight < popupHeight + bottomInset) {
        ShowWindow(toolbarRestorePopup_, SW_HIDE);
        return;
    }

    POINT origin{
        (videoWidth - popupWidth) / 2,
        videoHeight - bottomInset - popupHeight};
    if (ClientToScreen(videoWindow_, &origin) == FALSE) {
        ShowWindow(toolbarRestorePopup_, SW_HIDE);
        return;
    }
    RECT buttonBounds{};
    if (GetClientRect(toolbarRestoreButton_, &buttonBounds) == FALSE ||
        buttonBounds.right - buttonBounds.left != popupWidth ||
        buttonBounds.bottom - buttonBounds.top != popupHeight) {
        MoveWindow(toolbarRestoreButton_, 0, 0, popupWidth, popupHeight, TRUE);
    }
    EnableWindow(toolbarRestorePopup_, IsWindowEnabled(window_));
    RECT currentBounds{};
    const bool currentlyVisible =
        (GetWindowLongPtrW(toolbarRestorePopup_, GWL_STYLE) & WS_VISIBLE) != 0;
    if (currentlyVisible &&
        GetWindowRect(toolbarRestorePopup_, &currentBounds) != FALSE &&
        currentBounds.left == origin.x && currentBounds.top == origin.y &&
        currentBounds.right - currentBounds.left == popupWidth &&
        currentBounds.bottom - currentBounds.top == popupHeight) {
        return;
    }
    SetWindowPos(
        toolbarRestorePopup_,
        HWND_TOP,
        origin.x,
        origin.y,
        popupWidth,
        popupHeight,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER |
            (currentlyVisible ? 0U : SWP_SHOWWINDOW));
}

void MainWindow::UpdateToolbarVisibilityTimer() noexcept {
    if (window_ == nullptr) {
        return;
    }
    if (shutdownComplete_ || (!fullscreen_ && !toolbarManuallyHidden_)) {
        KillTimer(window_, kToolbarVisibilityTimer);
        return;
    }
    SetTimer(
        window_, kToolbarVisibilityTimer,
        kToolbarVisibilityTimerIntervalMs, nullptr);
}

bool MainWindow::CanShowOwnedPopups() const noexcept {
    return window_ != nullptr && !shutdownComplete_ && !minimized_ &&
        applicationActive_ && !cancellingTransient_ &&
        IsWindowVisible(window_) != FALSE && IsIconic(window_) == FALSE &&
        IsWindowEnabled(window_) != FALSE;
}

void MainWindow::CancelTransientUi() noexcept {
    if (cancellingTransient_) {
        return;
    }
    cancellingTransient_ = true;
    EndMarkerDrag(true);
    EndZoomPan();
    zoomWheelRemainder_ = 0;
    pendingVideoRefocus_ = pendingVideoRefocus_ || mouseControlInteraction_ ||
        seekDragging_ || markerDragging_ || volumeDragging_;
    seekDragging_ = false;
    volumeDragging_ = false;
    mouseControlInteraction_ = false;
    if (seekMouseTracking_ && seekSlider_ != nullptr) {
        TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_CANCEL | TME_LEAVE,
            seekSlider_, 0};
        TrackMouseEvent(&tracking);
    }
    seekMouseTracking_ = false;
    HideSeekPreview();
    selectionOverlay_.Cancel();
    if (zoomState_ == ZoomState::Selecting) {
        zoomState_ = ZoomState::None;
        zoomSelectionCrop_ = {};
        selectionVideo_ = {};
        selectionViewportScreen_ = {};
        SetWindowTextW(zoomButton_, kZoomStartText);
    }
    const HWND capture = GetCapture();
    if (capture != nullptr && window_ != nullptr &&
        (capture == window_ || IsChild(window_, capture) != FALSE ||
         GetAncestor(capture, GA_ROOTOWNER) == window_)) {
        // Clear flags before releasing: WM_CAPTURECHANGED can run reentrantly.
        ReleaseCapture();
    }
    if (fullscreenControlBar_ != nullptr) {
        ShowWindow(fullscreenControlBar_, SW_HIDE);
    }
    if (progressStrip_ != nullptr) {
        ShowWindow(progressStrip_, SW_HIDE);
    }
    if (toolbarRestorePopup_ != nullptr) {
        ShowWindow(toolbarRestorePopup_, SW_HIDE);
    }
    hasLastCursor_ = false;
    cancellingTransient_ = false;
    RestoreVideoFocusAfterMouseInteraction();
}

void MainWindow::RestoreUiAfterLifecycle() {
    if (restoringUi_ || cancellingTransient_ || shutdownComplete_ || window_ == nullptr ||
        videoWindow_ == nullptr || controlBar_ == nullptr || fullscreenControlBar_ == nullptr ||
        toolbarRestorePopup_ == nullptr) {
        return;
    }
    if (IsIconic(window_) != FALSE || minimized_) {
        CancelTransientUi();
        return;
    }
    restoringUi_ = true;
    const HWND activeBar = ActiveControlBar();
    // Reconcile parenting only at lifecycle boundaries, not on a UI timer.
    if (!ReparentControlBarControls(activeBar)) {
        restoringUi_ = false;
        return;
    }
    EnableWindow(fullscreenControlBar_, IsWindowEnabled(window_));
    EnableWindow(toolbarRestorePopup_, IsWindowEnabled(window_));
    ShowWindow(fullscreen_ ? controlBar_ : fullscreenControlBar_, SW_HIDE);
    RECT client{};
    if (GetClientRect(window_, &client) != FALSE) {
        LayoutChildren(static_cast<int>(client.right), static_cast<int>(client.bottom));
    }
    RestoreVideoFocusAfterMouseInteraction();
    if (playerInitialized_) {
        SetTimer(window_, kUiTimer, kUiTimerIntervalMs, nullptr);
        RefreshControls();
    }
    UpdateToolbarVisibilityTimer();
    SetControlBarVisible(
        !toolbarManuallyHidden_ && (controlBarVisible_ || !fullscreen_));
    restoringUi_ = false;
}

bool MainWindow::ProcessKeyboardMessage(const MSG& message) {
    if (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN) {
        return false;
    }
    if (window_ == nullptr || shutdownComplete_ || IsWindowEnabled(window_) == FALSE ||
        (message.hwnd != window_ && GetAncestor(message.hwnd, GA_ROOTOWNER) != window_ &&
         GetAncestor(message.hwnd, GA_ROOT) != window_)) {
        return false;
    }
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
        return false;
    }
    const bool controlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool repeated = (message.lParam & (static_cast<LPARAM>(1) << 30)) != 0;
    return HandleHotKey(static_cast<UINT>(message.wParam), controlDown, repeated);
}

bool MainWindow::ProcessZoomMouseMessage(const MSG& message) {
    if (message.message != WM_MOUSEWHEEL || window_ == nullptr ||
        (message.hwnd != window_ &&
         GetAncestor(message.hwnd, GA_ROOTOWNER) != window_)) {
        return false;
    }
    // Windows 7 sends wheel input to the focused HWND. A focused seek/volume
    // trackbar must not consume a wheel gesture made over the zoomed video.
    return HandleZoomWheel(GET_WHEEL_DELTA_WPARAM(message.wParam),
        {GET_X_LPARAM(message.lParam), GET_Y_LPARAM(message.lParam)});
}

bool MainWindow::HandleHotKey(
    const UINT key,
    const bool controlDown,
    const bool repeated) {
    if (markerDragging_ && key == VK_ESCAPE) {
        CancelTransientUi();
        RefreshControls();
        return true;
    }
    if (!controlDown && zoomState_ == ZoomState::Applied &&
        (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN)) {
        PanZoomByKeyboard(key);
        return true;
    }
    if (controlDown && key == 'O') {
        if (!repeated) {
            ShowOpenDialog();
        }
        return true;
    }
    if (controlDown && key == 'H') {
        if (!repeated) {
            ToggleManualToolbarVisibility();
        }
        return true;
    }
    if (controlDown && key == 'E') {
        if (!repeated) {
            ShowExportVideoDialog();
        }
        return true;
    }
    switch (key) {
    case VK_SPACE:
        if (!repeated) {
            TogglePlayback();
        }
        return true;
    case VK_LEFT:
        SeekBy(controlDown ? -30000 : -5000);
        return true;
    case VK_RIGHT:
        SeekBy(controlDown ? 30000 : 5000);
        return true;
    case VK_UP:
        AdjustVolume(5);
        return true;
    case VK_DOWN:
        AdjustVolume(-5);
        return true;
    case 'M':
        if (!repeated) {
            ToggleMute();
        }
        return true;
    case 'Z':
        if (!controlDown && !repeated) {
            ToggleAreaZoom();
        }
        return !controlDown;
    case 'L':
        if (!controlDown && !repeated) {
            ToggleWholeVideoLoop();
        }
        return !controlDown;
    case VK_OEM_4:
    case VK_OEM_6:
        if (!controlDown && !repeated) {
            SetLoopPoint(key == VK_OEM_4);
        }
        return !controlDown;
    case 'F':
    case VK_F11:
        if (!repeated) {
            ToggleFullscreen();
        }
        return true;
    case VK_ESCAPE:
        if (!repeated && (zoomState_ != ZoomState::None || fullscreen_)) {
            HandleZoomEscape();
            return true;
        }
        return zoomState_ != ZoomState::None || fullscreen_;
    default:
        return false;
    }
}

void MainWindow::ShowOpenDialog() {
    if (window_ == nullptr || shutdownComplete_ || IsWindowEnabled(window_) == FALSE) {
        return;
    }
    CancelTransientUi();
    std::vector<wchar_t> fileName(65536, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = kOpenFilter;
    dialog.nFilterIndex = 1;
    dialog.lpstrFile = fileName.data();
    dialog.nMaxFile = static_cast<DWORD>(fileName.size());
    dialog.lpstrTitle = L"Відкрити відеофайли";
    dialog.Flags = kPrivateOpenDialogFlags;
    if (GetOpenFileNameW(&dialog) != FALSE) {
        OpenFiles(ParseOpenDialogPaths(fileName.data(), fileName.size()));
    } else if (CommDlgExtendedError() != 0) {
        MessageBoxW(
            window_, L"Не вдалося відкрити діалог вибору файла.",
            kApplicationTitle, MB_OK | MB_ICONERROR);
    }
    RestoreUiAfterLifecycle();
}

void MainWindow::ShowExportVideoDialog() {
    if (window_ == nullptr || shutdownComplete_ || !hasMedia_ ||
        IsWindowEnabled(window_) == FALSE || HasActiveExportDialog(window_)) {
        return;
    }
    CancelTransientUi();
    playbackSnapshot_ = player_.Snapshot();
    ExportSnapshot snapshot{};
    snapshot.sourcePath = currentPath_;
    snapshot.sourceIdentity = CaptureExportFileIdentity(snapshot.sourcePath);
    snapshot.generation = player_.Generation();
    if (!snapshot.sourceIdentity.valid) {
        MessageBoxW(window_, L"Не вдалося зафіксувати identity файла-джерела.",
            kApplicationTitle, MB_OK | MB_ICONERROR);
        RestoreUiAfterLifecycle();
        return;
    }
    snapshot.durationMs = playbackSnapshot_.durationMs;
    snapshot.hasSegment = playbackLoop_.HasSegment(snapshot.durationMs);
    snapshot.segmentStartMs = playbackLoop_.StartMs();
    snapshot.segmentEndMs = playbackLoop_.EndMs();
    snapshot.zoomApplied = zoomState_ == ZoomState::Applied;
    if (!CurrentVideoViewport(snapshot.playerVideo, snapshot.viewportScreen)) {
        MessageBoxW(window_, L"Не вдалося отримати геометрію відео для експорту.",
            kApplicationTitle, MB_OK | MB_ICONERROR);
        RestoreUiAfterLifecycle();
        return;
    }
    snapshot.playerCrop = {0, 0, snapshot.playerVideo.width, snapshot.playerVideo.height};
    snapshot.presentationScreen = ComputeVideoContentRect(
        snapshot.playerVideo, snapshot.viewportScreen);
    if (snapshot.zoomApplied) {
        RECT render{};
        if (!player_.GetVideoCrop(snapshot.playerCrop) ||
            GetWindowRect(videoRenderWindow_, &render) == FALSE) {
            MessageBoxW(window_, L"Не вдалося зафіксувати поточну область зуму.",
                kApplicationTitle, MB_OK | MB_ICONERROR);
            RestoreUiAfterLifecycle();
            return;
        }
        const GeometryRect renderScreen{render.left, render.top, render.right, render.bottom};
        snapshot.renderScreen = renderScreen;
        snapshot.presentationScreen = ComputeVideoContentRect(
            {snapshot.playerCrop.width, snapshot.playerCrop.height}, renderScreen);
    }
    ShowExportDialog(window_, instance_, std::move(snapshot));
    RestoreUiAfterLifecycle();
}

void MainWindow::AddRemovalInterval() {
    if (!hasMedia_ || !playerInitialized_ || shutdownComplete_ ||
        HasActiveExportDialog(window_) || markerDragging_) return;
    playbackSnapshot_ = player_.Snapshot();
    if (!playbackLoop_.HasSegment(playbackSnapshot_.durationMs) ||
        playbackSnapshot_.state == PlaybackState::Opening ||
        playbackSnapshot_.state == PlaybackState::Error) return;
    const VideoInterval interval{playbackLoop_.StartMs(), playbackLoop_.EndMs()};
    if (OverlapsSavedInterval(interval, removalIntervals_)) return;
    const auto duplicate = std::find_if(removalIntervals_.begin(), removalIntervals_.end(),
        [&](const VideoInterval& existing) {
            return existing.startMs == interval.startMs && existing.endMs == interval.endMs;
        });
    if (duplicate == removalIntervals_.end()) {
        removalIntervals_.push_back(interval);
        std::sort(removalIntervals_.begin(), removalIntervals_.end(),
            [](const VideoInterval& a, const VideoInterval& b) {
                return a.startMs < b.startMs ||
                    (a.startMs == b.startMs && a.endMs < b.endMs);
            });
    }
    playbackLoop_.ClearSegment();
    InvalidateIntervalTimeline();
    RegisterInteraction();
    RefreshControls();
}

void MainWindow::ShowRemoveSegmentVideoDialog() {
    if (window_ == nullptr || shutdownComplete_ || !hasMedia_ ||
        IsWindowEnabled(window_) == FALSE || HasActiveExportDialog(window_)) return;
    playbackSnapshot_ = player_.Snapshot();
    ExportSnapshot snapshot{};
    snapshot.sourcePath = currentPath_;
    snapshot.sourceIdentity = CaptureExportFileIdentity(currentPath_);
    snapshot.generation = player_.Generation();
    snapshot.durationMs = playbackSnapshot_.durationMs;
    snapshot.hasSegment = playbackLoop_.HasSegment(snapshot.durationMs);
    snapshot.segmentStartMs = playbackLoop_.StartMs();
    snapshot.segmentEndMs = playbackLoop_.EndMs();
    if (snapshot.hasSegment && OverlapsSavedInterval(
            {snapshot.segmentStartMs, snapshot.segmentEndMs}, removalIntervals_)) return;
    snapshot.removalIntervals = removalIntervals_;
    if (snapshot.hasSegment) {
        const VideoInterval current{snapshot.segmentStartMs, snapshot.segmentEndMs};
        const auto duplicate = std::find_if(snapshot.removalIntervals.begin(),
            snapshot.removalIntervals.end(), [&](const VideoInterval& interval) {
                return interval.startMs == current.startMs && interval.endMs == current.endMs;
            });
        if (duplicate == snapshot.removalIntervals.end()) {
            snapshot.removalIntervals.push_back(current);
        }
    }
    if (snapshot.removalIntervals.empty() || !snapshot.sourceIdentity.valid) {
        MessageBoxW(window_,
            L"Спочатку позначте A і B або додайте відрізки до списку.",
            kApplicationTitle, MB_OK | MB_ICONWARNING);
        return;
    }
    // Include a current single A-B selection in the same editable queue.
    removalIntervals_ = snapshot.removalIntervals;
    std::sort(removalIntervals_.begin(), removalIntervals_.end(),
        [](const VideoInterval& a, const VideoInterval& b) {
            return a.startMs < b.startMs ||
                (a.startMs == b.startMs && a.endMs < b.endMs);
        });
    snapshot.removalIntervals = removalIntervals_;
    playbackLoop_.ClearSegment();
    InvalidateIntervalTimeline();
    RefreshControls();
    CancelTransientUi();
    ShowRemoveSegmentDialog(window_, instance_, snapshot,
        [this](const ExportSnapshot& source, const std::wstring& partial,
            std::wstring& error) {
            return CommitSourceRemoval(source, partial, error);
        },
        [this, snapshot](const std::vector<VideoInterval>& intervals) {
            const ExportFileIdentity identity =
                CaptureExportFileIdentity(snapshot.sourcePath);
            if (_wcsicmp(currentPath_.c_str(), snapshot.sourcePath.c_str()) == 0 &&
                identity.valid && identity.volume == snapshot.sourceIdentity.volume &&
                identity.fileIndex == snapshot.sourceIdentity.fileIndex &&
                identity.size == snapshot.sourceIdentity.size &&
                identity.modifiedTime == snapshot.sourceIdentity.modifiedTime) {
                removalIntervals_ = intervals;
                InvalidateIntervalTimeline();
                RefreshControls();
            }
        });
    RestoreUiAfterLifecycle();
}

bool MainWindow::CommitSourceRemoval(const ExportSnapshot& snapshot,
    const std::wstring& partialPath, std::wstring& error) {
    const ExportFileIdentity currentIdentity =
        CaptureExportFileIdentity(snapshot.sourcePath);
    if (!currentIdentity.valid ||
        currentIdentity.volume != snapshot.sourceIdentity.volume ||
        currentIdentity.fileIndex != snapshot.sourceIdentity.fileIndex ||
        currentIdentity.size != snapshot.sourceIdentity.size ||
        currentIdentity.modifiedTime != snapshot.sourceIdentity.modifiedTime ||
        _wcsicmp(currentPath_.c_str(), snapshot.sourcePath.c_str()) != 0) {
        error = L"Відкритий файл змінився; оригінал не перезаписано.";
        return false;
    }
    const std::vector<VideoInterval> queuedIntervals = removalIntervals_;
    if (previewInitialized_) {
        previewEngine_.Shutdown();
        previewInitialized_ = false;
    }
    if (playerInitialized_) {
        player_.Shutdown();
        playerInitialized_ = false;
    }
    const ExportFileIdentity readyIdentity =
        CaptureExportFileIdentity(snapshot.sourcePath);
    const bool sourceStillSame = readyIdentity.valid &&
        readyIdentity.volume == snapshot.sourceIdentity.volume &&
        readyIdentity.fileIndex == snapshot.sourceIdentity.fileIndex &&
        readyIdentity.size == snapshot.sourceIdentity.size &&
        readyIdentity.modifiedTime == snapshot.sourceIdentity.modifiedTime;
    const bool replaced = sourceStillSame &&
        MoveFileExW(PathForWin32Io(partialPath).c_str(),
            PathForWin32Io(snapshot.sourcePath).c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    const DWORD replaceError = replaced ? ERROR_SUCCESS : GetLastError();
    std::wstring initializationError;
    if (player_.Initialize(videoRenderWindow_, window_, initializationError)) {
        playerInitialized_ = true;
        player_.SetVolume(displayedVolume_);
        player_.SetMuted(muted_);
        if (previewEngine_.Initialize(window_)) {
            previewInitialized_ = true;
            previewEngine_.SetPrefetchEnabled(true);
        } else {
            initializationError = L"Не вдалося повторно запустити preview кадрів.";
        }
        if (!OpenFile(snapshot.sourcePath) && initializationError.empty()) {
            initializationError = L"Не вдалося повторно відкрити відео.";
        }
    } else {
        hasMedia_ = false;
        currentPath_.clear();
        RestoreExecutionState();
        SetPromptVisible(true);
        UpdateWindowTitle();
    }
    if (!replaced) {
        if (sourceStillSame &&
            _wcsicmp(currentPath_.c_str(), snapshot.sourcePath.c_str()) == 0) {
            removalIntervals_ = queuedIntervals;
            InvalidateIntervalTimeline();
            RefreshControls();
        }
        error = sourceStillSame
            ? L"Не вдалося атомарно перезаписати оригінал (Win32 " +
                std::to_wstring(replaceError) + L")."
            : L"Оригінал змінився перед перезаписом; його збережено.";
        if (!initializationError.empty()) error += L" " + initializationError;
        return false;
    }
    if (!initializationError.empty()) {
        MessageBoxW(window_,
            (L"Файл перезаписано, але плеєр не вдалося повторно відкрити: " +
                initializationError).c_str(), kApplicationTitle,
            MB_OK | MB_ICONWARNING);
    }
    return true;
}

void MainWindow::HandleDroppedFiles(const HDROP drop) {
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::wstring> paths;
    paths.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        std::vector<wchar_t> path(static_cast<std::size_t>(length) + 1, L'\0');
        if (length != 0 &&
            DragQueryFileW(drop, index, path.data(), length + 1) != 0) {
            paths.emplace_back(path.data());
        }
    }
    DragFinish(drop);
    OpenFiles(paths);
}

void MainWindow::TogglePlayback() {
    if (!hasMedia_) {
        return;
    }
    RegisterInteraction();
    playbackLoop_.CancelPending();
    if (player_.State() == PlaybackState::Playing) {
        loopPlaybackRequested_ = false;
        player_.Pause();
        RestoreExecutionState();
    } else {
        loopPlaybackRequested_ = true;
        std::wstring ignoredError;
        if (!player_.Play(ignoredError)) {
            ShowMediaError();
        }
    }
    RefreshControls();
}

void MainWindow::ToggleWholeVideoLoop() {
    if (!hasMedia_ || !playerInitialized_ || markerDragging_ || player_.State() == PlaybackState::Error) {
        return;
    }
    RegisterInteraction();
    playbackLoop_.ToggleWholeVideo();
    if (player_.State() == PlaybackState::Ended) {
        loopPlaybackRequested_ = true;
    }
    RefreshControls();
}

void MainWindow::SetLoopPoint(const bool start) {
    if (!hasMedia_ || !playerInitialized_ || markerDragging_) {
        return;
    }
    playbackSnapshot_ = player_.Snapshot();
    const auto& snapshot = playbackSnapshot_;
    if (!snapshot.seekable || snapshot.durationMs <= 0 ||
        snapshot.state == PlaybackState::Opening || snapshot.state == PlaybackState::Error) {
        return;
    }
    const std::int64_t position = ClampTime(snapshot.positionMs, snapshot.durationMs);
    if (start && position >= snapshot.durationMs) {
        return;
    }
    if (IsInsideSavedInterval(position, removalIntervals_) ||
        (!start && OverlapsSavedInterval(
            {playbackLoop_.StartMs(), position}, removalIntervals_))) return;
    const bool changed = start
        ? playbackLoop_.SetStart(position, snapshot.durationMs)
        : playbackLoop_.SetEnd(position, snapshot.durationMs);
    if (changed) {
        if (snapshot.state == PlaybackState::Ended) {
            loopPlaybackRequested_ = true;
        }
        RegisterInteraction();
        RefreshControls();
    }
}

void MainWindow::ToggleSegmentLoop() {
    if (!hasMedia_ || !playerInitialized_ || markerDragging_) {
        return;
    }
    playbackSnapshot_ = player_.Snapshot();
    if (playbackLoop_.Mode() != LoopMode::Segment && !playbackSnapshot_.seekable) {
        return;
    }
    if (playbackLoop_.ToggleSegment(playbackSnapshot_.durationMs)) {
        if (playbackSnapshot_.state == PlaybackState::Ended) {
            loopPlaybackRequested_ = true;
        }
        RegisterInteraction();
        RefreshControls();
    }
}

void MainWindow::ClearLoopSegment() {
    if (markerDragging_) CancelTransientUi();
    playbackLoop_.ClearSegment();
    RegisterInteraction();
    RefreshControls();
}

void MainWindow::ApplyPlaybackLoop() {
    if (applyingPlaybackLoop_ || !hasMedia_ || shutdownComplete_) {
        return;
    }
    if (playbackSnapshot_.state == PlaybackState::Playing) {
        loopPlaybackRequested_ = true;
    } else if (playbackSnapshot_.state == PlaybackState::Paused ||
        playbackSnapshot_.state == PlaybackState::Error) {
        loopPlaybackRequested_ = false;
    }
    // Manual Stop/Pause must win over a late EndReached notification. Update
    // still observes generation/errors, but cannot restart user-stopped media.
    const LoopAction action = playbackLoop_.Update(
        playbackSnapshot_, GetTickCount64(), seekDragging_ || markerDragging_ || !loopPlaybackRequested_);
    applyingPlaybackLoop_ = true;
    if (action.kind == LoopActionKind::Seek) {
        player_.Seek(action.positionMs);
        playbackSnapshot_ = player_.CachedSnapshot();
    } else if (action.kind == LoopActionKind::Play) {
        std::wstring error;
        if (!player_.Restart(error)) {
            playbackLoop_.Reset();
            loopPlaybackRequested_ = false;
            ShowMediaError();
        }
        playbackSnapshot_ = player_.CachedSnapshot();
    }
    applyingPlaybackLoop_ = false;
}

void MainWindow::UpdateLoopControls() {
    const auto& snapshot = playbackSnapshot_;
    const auto start = playbackLoop_.StartMs();
    const auto end = playbackLoop_.EndMs();
    const bool usable = hasMedia_ && snapshot.state != PlaybackState::Error;
    const bool canMark = usable && snapshot.seekable && snapshot.durationMs > 0 &&
        snapshot.state != PlaybackState::Opening;
    const auto position = ClampTime(snapshot.positionMs, snapshot.durationMs);
    const bool pointBlocked = IsInsideSavedInterval(position, removalIntervals_);
    const bool endBlocked = OverlapsSavedInterval({start, position}, removalIntervals_);
    EnableWindow(loopVideoButton_, usable);
    EnableWindow(loopStartButton_, canMark && position < snapshot.durationMs && !pointBlocked);
    EnableWindow(loopEndButton_, canMark && start >= 0 &&
        position >= start && position - start >= 500 && !pointBlocked && !endBlocked);
    EnableWindow(loopSegmentButton_, playbackLoop_.Mode() == LoopMode::Segment ||
        (canMark && playbackLoop_.HasSegment(snapshot.durationMs)));
    EnableWindow(loopClearButton_, start >= 0 || end >= 0);
    for (const auto& button : std::array<std::pair<HWND, bool>, 2>{{
            {loopVideoButton_, playbackLoop_.Mode() == LoopMode::WholeVideo},
            {loopSegmentButton_, playbackLoop_.Mode() == LoopMode::Segment}}}) {
        const WPARAM check = button.second ? BST_CHECKED : BST_UNCHECKED;
        if (SendMessageW(button.first, BM_GETCHECK, 0, 0) != static_cast<LRESULT>(check)) {
            SendMessageW(button.first, BM_SETCHECK, check, 0);
        }
    }
    std::wstring label =
        L"A: " + FormatLoopPoint(start) + L"   B: " + FormatLoopPoint(end);
    if (canMark && (pointBlocked || endBlocked)) {
        label += L"   Перетин зі збереженим відрізком";
    }
    SetWindowTextW(loopRangeLabel_, label.c_str());
    if (timelineDurationMs_ != snapshot.durationMs || timelineStartMs_ != start || timelineEndMs_ != end) {
        timelineDurationMs_ = snapshot.durationMs;
        timelineStartMs_ = start;
        timelineEndMs_ = end;
        InvalidateIntervalTimeline();
    }
    if (playbackLoop_.HasSegment(snapshot.durationMs)) {
        const auto selectionStart = TimeToSlider(start, snapshot.durationMs);
        const auto selectionEnd = TimeToSlider(end, snapshot.durationMs);
        if (SendMessageW(seekSlider_, TBM_GETSELSTART, 0, 0) != selectionStart ||
            SendMessageW(seekSlider_, TBM_GETSELEND, 0, 0) != selectionEnd) {
            SendMessageW(seekSlider_, TBM_SETSELSTART, FALSE, selectionStart);
            SendMessageW(seekSlider_, TBM_SETSELEND, TRUE, selectionEnd);
        }
    } else if (SendMessageW(seekSlider_, TBM_GETSELSTART, 0, 0) != -1 ||
        SendMessageW(seekSlider_, TBM_GETSELEND, 0, 0) != -1) {
        SendMessageW(seekSlider_, TBM_CLEARSEL, TRUE, 0);
    }
}

void MainWindow::StopPlayback() {
    if (!hasMedia_) {
        return;
    }
    RegisterInteraction();
    playbackLoop_.CancelPending();
    loopPlaybackRequested_ = false;
    player_.Stop();
    RestoreExecutionState();
    SendMessageW(seekSlider_, TBM_SETPOS, TRUE, 0);
    RefreshControls();
}

void MainWindow::SeekBy(const std::int64_t deltaMs) {
    if (!hasMedia_ || !player_.IsSeekable()) {
        return;
    }
    const std::int64_t duration = (std::max<std::int64_t>)(0, player_.DurationMs());
    if (duration <= 0) {
        return;
    }
    RegisterInteraction();
    const std::int64_t current = ClampTime(player_.PositionMs(), duration);
    const std::int64_t target = videoplayer::SeekBy(current, deltaMs, duration);
    playbackLoop_.CancelPending();
    player_.Seek(target);
    playbackSnapshot_ = player_.CachedSnapshot();
    RenderPlaybackSnapshot();
}

void MainWindow::CommitSeekFromSlider() {
    if (!hasMedia_ || !player_.IsSeekable()) {
        return;
    }
    const std::int64_t duration = player_.DurationMs();
    if (duration <= 0) {
        return;
    }
    const int sliderPosition = static_cast<int>(
        SendMessageW(seekSlider_, TBM_GETPOS, 0, 0));
    const std::int64_t target = SliderToTime(sliderPosition, duration);
    playbackLoop_.CancelPending();
    player_.Seek(target);
    playbackSnapshot_ = player_.CachedSnapshot();
    RenderPlaybackSnapshot();
}

void MainWindow::UpdateSeekLabel() {
    const std::int64_t duration = playbackSnapshot_.durationMs;
    const int sliderPosition = static_cast<int>(
        SendMessageW(seekSlider_, TBM_GETPOS, 0, 0));
    SetWindowTextW(
        currentTimeLabel_,
        videoplayer::FormatTime(SliderToTime(sliderPosition, duration)).c_str());
}

bool MainWindow::SetSeekSliderFromPointer(const int mouseX) {
    if (!hasMedia_ || !player_.IsSeekable() || player_.DurationMs() <= 0) {
        return false;
    }

    RECT channel{};
    RECT thumb{};
    SendMessageW(
        seekSlider_,
        TBM_GETCHANNELRECT,
        0,
        reinterpret_cast<LPARAM>(&channel));
    SendMessageW(
        seekSlider_,
        TBM_GETTHUMBRECT,
        0,
        reinterpret_cast<LPARAM>(&thumb));
    const RECT pointerRange = TrackbarPointerRange(channel, thumb);
    if (pointerRange.right <= pointerRange.left) {
        return false;
    }

    const int position = TrackbarPositionFromPointerX(
        mouseX, pointerRange, 0, kSeekRange);
    SendMessageW(seekSlider_, TBM_SETPOS, TRUE, position);
    UpdateSeekLabel();
    return true;
}

void MainWindow::HandleSeekPointer(const int mouseX) {
    if (!CanShowOwnedPopups() || !hasMedia_ || !player_.IsSeekable() || !previewInitialized_) {
        HideSeekPreview();
        return;
    }
    const std::int64_t duration = player_.DurationMs();
    if (duration <= 0) {
        HideSeekPreview();
        return;
    }

    RECT channel{};
    RECT thumb{};
    SendMessageW(seekSlider_, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
    SendMessageW(seekSlider_, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    const RECT pointerRange = TrackbarPointerRange(channel, thumb);
    if (pointerRange.right <= pointerRange.left) {
        HideSeekPreview();
        return;
    }
    previewHoverTimeMs_ = PreviewTimeFromChannelX(mouseX, pointerRange, duration);
    POINT anchor{mouseX, pointerRange.top};
    ClientToScreen(seekSlider_, &anchor);
    previewAnchorScreen_ = anchor;
    const std::uint32_t generation = player_.Generation();
    const std::int64_t quantized = QuantizePreviewTimestamp(
        previewHoverTimeMs_, duration);
    const bool sameRequest = CanReusePreviewRequest(generation, quantized);
    if (sameRequest) {
        if (previewDisplayedRequestId_ == previewRequestId_ && previewDisplayedFrame_) {
            previewPopup_.ShowFrameAt(
                previewAnchorScreen_, dpi_, previewHoverTimeMs_, previewDisplayedFrame_);
        } else {
            ShowPendingSeekPreview(duration);
        }
        return;
    }

    previewRequestGeneration_ = generation;
    previewRequestTimestampMs_ = quantized;
    previewDisplayedRequestId_ = 0;
    // Retain a same-file image as an explicitly approximate fallback until
    // the new bucket arrives. It must never satisfy the new request ID.
    previewRequestId_ = previewEngine_.RequestFrame(previewHoverTimeMs_, duration);
    // A RAM-cache hit is published synchronously. Consume it before exposing
    // any placeholder instead of waiting for the posted ready notification.
    if (!TryShowPreviewResult(false)) {
        ShowPendingSeekPreview(duration);
    }
}

void MainWindow::ShowPendingSeekPreview(const std::int64_t durationMs) {
    const auto generation = player_.Generation();
    if (previewRequestId_ == 0 || previewRequestGeneration_ != generation ||
        !CanShowOwnedPopups()) {
        previewDisplayedFrame_.reset();
        previewPopup_.Hide();
        return;
    }
    if (previewDisplayedFrame_ && (!previewDisplayedFrame_->IsValid() ||
        previewDisplayedFrame_->mediaGeneration != generation)) {
        previewDisplayedFrame_.reset();
    }
    const auto nearby = previewEngine_.FindNearestCachedFrame(
        previewHoverTimeMs_, (std::max)(std::int64_t{0}, durationMs));
    if (nearby && nearby->mediaGeneration == generation) {
        previewDisplayedFrame_ = nearby;
    }
    if (previewDisplayedFrame_) {
        previewPopup_.ShowFrameAt(previewAnchorScreen_, dpi_, previewHoverTimeMs_,
            previewDisplayedFrame_, previewEngine_.IsRequestPending(previewRequestId_));
    } else {
        previewPopup_.ShowPlaceholderAt(previewAnchorScreen_, dpi_, previewHoverTimeMs_,
            previewEngine_.IsRequestPending(previewRequestId_));
    }
}

bool MainWindow::CanReusePreviewRequest(
    const std::uint32_t generation,
    const std::int64_t quantizedTimestamp) const noexcept {
    return previewRequestId_ != 0 &&
        previewRequestGeneration_ == generation &&
        previewRequestTimestampMs_ == quantizedTimestamp &&
        ((previewDisplayedRequestId_ == previewRequestId_ && previewDisplayedFrame_ &&
          previewDisplayedFrame_->IsValid() &&
          previewDisplayedFrame_->mediaGeneration == generation &&
          previewDisplayedFrame_->timestampMs == quantizedTimestamp) ||
         previewEngine_.IsRequestPending(previewRequestId_));
}

void MainWindow::HideSeekPreview(const bool cancelRequest) noexcept {
    if (cancelRequest && previewInitialized_) {
        previewEngine_.CancelRequests();
    }
    previewRequestId_ = 0;
    previewDisplayedRequestId_ = 0;
    previewRequestGeneration_ = 0;
    previewRequestTimestampMs_ = -1;
    previewDisplayedFrame_.reset();
    previewPopup_.Hide();
}

void MainWindow::HandlePreviewResult() {
    if (!TryShowPreviewResult(true) && previewPopup_.IsVisible() &&
        previewRequestId_ != 0 && previewDisplayedRequestId_ != previewRequestId_) {
        // A bounded failure ends the updating badge without discarding the
        // useful approximate image or reopening a dismissed popup.
        ShowPendingSeekPreview(player_.DurationMs());
    }
}

bool MainWindow::TryShowPreviewResult(const bool requireVisible) {
    if (!previewInitialized_) {
        return false;
    }
    const auto result = previewEngine_.TakeLatestResult();
    return result.has_value() && PresentPreviewResult(*result, requireVisible);
}

bool MainWindow::PresentPreviewResult(
    const PreviewResult& result, const bool requireVisible) {
    if (!previewInitialized_ || previewRequestId_ == 0 ||
        result.requestId != previewRequestId_ ||
        result.requestId <= previewDisplayedRequestId_ ||
        result.mediaGeneration != previewRequestGeneration_ ||
        result.mediaGeneration != player_.Generation() ||
        result.timestampMs != previewRequestTimestampMs_ ||
        !CanShowOwnedPopups() || (requireVisible && !previewPopup_.IsVisible())) {
        return false;
    }
    if (result.frame && result.frame->IsValid() &&
        result.frame->mediaGeneration == result.mediaGeneration &&
        result.frame->timestampMs == result.timestampMs) {
        previewDisplayedRequestId_ = result.requestId;
        previewDisplayedFrame_ = result.frame;
        previewPopup_.ShowFrameAt(
            previewAnchorScreen_, dpi_, previewHoverTimeMs_, previewDisplayedFrame_);
        return true;
    }
    return false;
}

void MainWindow::SetVolumeFromSlider(const int volume) {
    displayedVolume_ = ClampVolume(volume);
    if (displayedVolume_ > 0) {
        lastNonZeroVolume_ = displayedVolume_;
        muted_ = false;
        player_.SetVolume(displayedVolume_);
        player_.SetMuted(false);
    } else {
        muted_ = true;
        player_.SetVolume(0);
        player_.SetMuted(true);
    }
    SetWindowTextW(muteButton_, muted_ ? L"Звук" : L"Без звуку");
}

void MainWindow::AdjustVolume(const int delta) {
    const int base = muted_ ? 0 : displayedVolume_;
    const int adjusted = ClampVolume(base + delta);
    RegisterInteraction();
    SendMessageW(volumeSlider_, TBM_SETPOS, TRUE, adjusted);
    SetVolumeFromSlider(adjusted);
}

void MainWindow::ToggleMute() {
    RegisterInteraction();
    if (muted_) {
        const int restored = (std::max)(1, ClampVolume(lastNonZeroVolume_));
        displayedVolume_ = restored;
        muted_ = false;
        player_.SetVolume(restored);
        player_.SetMuted(false);
        SendMessageW(volumeSlider_, TBM_SETPOS, TRUE, restored);
    } else {
        if (displayedVolume_ > 0) {
            lastNonZeroVolume_ = displayedVolume_;
        }
        muted_ = true;
        player_.SetMuted(true);
        SendMessageW(volumeSlider_, TBM_SETPOS, TRUE, 0);
    }
    SetWindowTextW(muteButton_, muted_ ? L"Звук" : L"Без звуку");
}

void MainWindow::ToggleFullscreen() {
    if (fullscreen_) {
        ExitFullscreen();
    } else {
        EnterFullscreen();
    }
}

void MainWindow::EnterFullscreen() {
    if (fullscreen_ || window_ == nullptr) {
        return;
    }
    CancelTransientUi();

    savedPlacement_.length = sizeof(savedPlacement_);
    if (GetWindowPlacement(window_, &savedPlacement_) == FALSE) {
        return;
    }
    savedStyle_ = GetWindowLongPtrW(window_, GWL_STYLE);
    savedExtendedStyle_ = GetWindowLongPtrW(window_, GWL_EXSTYLE);

    const HMONITOR monitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(monitor, &monitorInfo) == FALSE) {
        return;
    }

    const HWND focusedBeforeTransition = GetFocus();
    const bool controlHadFocusBeforeTransition =
        focusedBeforeTransition == controlBar_ ||
        (focusedBeforeTransition != nullptr &&
         IsChild(controlBar_, focusedBeforeTransition) != FALSE);
    if (!ReparentControlBarControls(fullscreenControlBar_)) {
        if (!toolbarManuallyHidden_) {
            ShowWindow(controlBar_, SW_SHOWNA);
        }
        return;
    }
    ShowWindow(controlBar_, SW_HIDE);
    ApplyFullscreenControlBarOpacity();

    const LONG_PTR fullscreenStyle =
        (savedStyle_ & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) | WS_POPUP;
    const LONG_PTR fullscreenExtendedStyle = savedExtendedStyle_ &
        ~static_cast<LONG_PTR>(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
    SetWindowLongPtrW(window_, GWL_STYLE, fullscreenStyle);
    SetWindowLongPtrW(window_, GWL_EXSTYLE, fullscreenExtendedStyle);
    fullscreen_ = true;
    toolbarWasFullscreen_ = false;
    controlBarVisible_ = !toolbarManuallyHidden_;
    lastInteractionMs_ = GetTickCount64();
    UpdateToolbarVisibilityTimer();

    SetWindowPos(
        window_, HWND_TOP,
        monitorInfo.rcMonitor.left,
        monitorInfo.rcMonitor.top,
        monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
        monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
        SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    RECT fullscreenClient{};
    GetClientRect(window_, &fullscreenClient);
    LayoutChildren(
        static_cast<int>(fullscreenClient.right - fullscreenClient.left),
        static_cast<int>(fullscreenClient.bottom - fullscreenClient.top));
    SetWindowTextW(fullscreenButton_, L"Вийти з екрана");
    SetControlBarVisible(!toolbarManuallyHidden_);
    // Entering via F/F11 can inherit button focus from windowed mode, where
    // mouse interaction has already ended. Do not let that stale focus pin
    // the toolbar open; keyboard focus acquired after entry still blocks hide.
    const HWND activeControlBar = ActiveControlBar();
    const HWND focused = GetFocus();
    if (controlHadFocusBeforeTransition || focused == activeControlBar ||
        (focused != nullptr && IsChild(activeControlBar, focused) != FALSE)) {
        SetFocus(videoWindow_);
    }
    UpdateControlBarVisibility();
    FinishMouseControlInteraction();
}

void MainWindow::ExitFullscreen() {
    if (!fullscreen_ || window_ == nullptr) {
        return;
    }

    CancelTransientUi();
    ShowWindow(fullscreenControlBar_, SW_HIDE);
    if (!ReparentControlBarControls(controlBar_)) {
        if (controlBarVisible_ && CanShowOwnedPopups()) {
            ShowWindow(fullscreenControlBar_, SW_SHOWNA);
        }
        return;
    }
    fullscreen_ = false;
    toolbarWasFullscreen_ = false;

    SetWindowLongPtrW(window_, GWL_STYLE, savedStyle_);
    SetWindowLongPtrW(window_, GWL_EXSTYLE, savedExtendedStyle_);
    savedPlacement_.length = sizeof(savedPlacement_);
    SetWindowPlacement(window_, &savedPlacement_);
    SetWindowPos(
        window_, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
        SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    SetWindowTextW(fullscreenButton_, L"На весь екран");
    RECT client{};
    GetClientRect(window_, &client);
    LayoutChildren(client.right, client.bottom);
    UpdateToolbarVisibilityTimer();
    SetControlBarVisible(!toolbarManuallyHidden_);
    UpdateControlBarVisibility();
}

void MainWindow::ToggleManualToolbarVisibility() {
    SetToolbarManuallyHidden(!toolbarManuallyHidden_);
}

void MainWindow::SetToolbarManuallyHidden(const bool hidden) {
    if (toolbarManuallyHidden_ == hidden || window_ == nullptr) {
        UpdateControlBarVisibility();
        return;
    }

    CancelTransientUi();
    toolbarManuallyHidden_ = hidden;
    controlBarVisible_ = !hidden;
    lastInteractionMs_ = GetTickCount64();
    SetControlBarVisible(!hidden);

    RECT client{};
    if (GetClientRect(window_, &client) != FALSE) {
        LayoutChildren(
            static_cast<int>(client.right - client.left),
            static_cast<int>(client.bottom - client.top));
    }
    UpdateToolbarVisibilityTimer();
    if (videoWindow_ != nullptr && CanShowOwnedPopups()) {
        SetFocus(videoWindow_);
    }
    UpdateControlBarVisibility(true);
}

void MainWindow::SetControlBarVisible(const bool visible) {
    const HWND activeControlBar = ActiveControlBar();
    if (activeControlBar == nullptr) {
        return;
    }
    const bool effectiveVisible = visible && !toolbarManuallyHidden_;
    if (fullscreen_ && (zoomState_ == ZoomState::Selecting || ZoomGestureActive()) &&
        effectiveVisible) {
        return;
    }
    controlBarVisible_ = effectiveVisible;
    const bool show = effectiveVisible && (!fullscreen_ || CanShowOwnedPopups());
    const bool styleVisible =
        (GetWindowLongPtrW(activeControlBar, GWL_STYLE) & WS_VISIBLE) != 0;
    if (styleVisible == show) {
        UpdateProgressStrip();
        return;
    }
    ShowWindow(activeControlBar, show ? SW_SHOWNA : SW_HIDE);
    if (show) {
        SetWindowPos(
            activeControlBar, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    UpdateProgressStrip();
}

void MainWindow::RegisterInteraction(const bool pointerMoved) {
    lastInteractionMs_ = GetTickCount64();
    if (ZoomGestureActive()) {
        return;
    }
    if (toolbarManuallyHidden_) {
        UpdateControlBarVisibility(pointerMoved);
        return;
    }
    if (!fullscreen_ || controlBarVisible_ || !pointerMoved) {
        SetControlBarVisible(true);
        return;
    }
    UpdateControlBarVisibility(true);
}

void MainWindow::UpdateControlBarVisibility(const bool pointerMoved) {
    POINT cursorScreen{};
    const bool haveCursor = GetCursorPos(&cursorScreen) != FALSE;
    UpdateControlBarVisibilityFromCursor(pointerMoved, haveCursor, cursorScreen);
}

void MainWindow::UpdateControlBarVisibilityFromCursor(
    const bool pointerMoved,
    const bool haveCursor,
    const POINT cursorScreen) {
    const HWND activeControlBar = ActiveControlBar();
    if (activeControlBar == nullptr || window_ == nullptr) {
        return;
    }
    if (volumeDragging_ && GetCapture() != volumeSlider_) {
        volumeDragging_ = false;
    }
    if (mouseControlInteraction_ && !seekDragging_ && !markerDragging_ && !volumeDragging_ &&
        GetCapture() == nullptr) {
        // A trackbar can lose capture without sending TB_ENDTRACK. Clear the
        // mouse-only focus blocker; a keyboard-focused control is untouched.
        FinishMouseControlInteraction();
    }
    if (!CanShowOwnedPopups()) {
        if (fullscreen_) {
            ShowWindow(fullscreenControlBar_, SW_HIDE);
        }
        ShowWindow(progressStrip_, SW_HIDE);
        ShowWindow(toolbarRestorePopup_, SW_HIDE);
        return;
    }

    if (ZoomGestureActive()) {
        if (fullscreen_) {
            SetControlBarVisible(false);
        }
        SetToolbarRestoreVisible(false);
        UpdateProgressStrip();
        return;
    }

    const bool toolbarActuallyVisible =
        (GetWindowLongPtrW(activeControlBar, GWL_STYLE) & WS_VISIBLE) != 0;
    controlBarVisible_ = toolbarActuallyVisible;

    const std::uint64_t now = GetTickCount64();
    bool moved = pointerMoved;
    if (haveCursor) {
        moved = moved || !hasLastCursor_ ||
            cursorScreen.x != lastCursorScreen_.x ||
            cursorScreen.y != lastCursorScreen_.y;
        lastCursorScreen_ = cursorScreen;
        hasLastCursor_ = true;
    }

    RECT client{};
    GetClientRect(videoWindow_, &client);
    POINT cursorClient = cursorScreen;
    const bool converted = haveCursor && ScreenToClient(videoWindow_, &cursorClient) != FALSE;
    const bool cursorInside = converted && PointInsideRect(cursorClient, client);

    RECT toolbarBounds{};
    const bool pointerOverToolbar = toolbarActuallyVisible && haveCursor &&
        GetWindowRect(activeControlBar, &toolbarBounds) != FALSE &&
        PointInsideRect(cursorScreen, toolbarBounds);
    const bool restoreActuallyVisible = toolbarRestorePopup_ != nullptr &&
        (GetWindowLongPtrW(toolbarRestorePopup_, GWL_STYLE) & WS_VISIBLE) != 0;
    RECT restoreBounds{};
    const bool pointerOverRestore = restoreActuallyVisible && haveCursor &&
        GetWindowRect(toolbarRestorePopup_, &restoreBounds) != FALSE &&
        PointInsideRect(cursorScreen, restoreBounds);
    const HWND focused = GetFocus();
    const bool controlHasFocus = focused == activeControlBar ||
        (focused != nullptr && IsChild(activeControlBar, focused) != FALSE);

    ToolbarVisibilityInput input{};
    input.wasFullscreen = toolbarWasFullscreen_;
    input.fullscreen = fullscreen_;
    input.manuallyHidden = toolbarManuallyHidden_;
    input.playing = playbackSnapshot_.state == PlaybackState::Playing;
    input.currentlyVisible = toolbarActuallyVisible;
    input.restoreCurrentlyVisible = restoreActuallyVisible;
    input.ownedPopupsAllowed = true;
    input.cursorInsideClient = cursorInside;
    input.cursorMoved = moved;
    input.pointerOverToolbar = pointerOverToolbar;
    input.pointerOverRestore = pointerOverRestore;
    input.seekDragging = seekDragging_ || markerDragging_;
    input.volumeDragging = volumeDragging_;
    input.previewVisible = previewPopup_.IsVisible();
    input.zoomSelecting = zoomState_ == ZoomState::Selecting;
    input.controlHasFocus = controlHasFocus;
    input.cursorY = cursorClient.y;
    input.clientHeight = client.bottom - client.top;
    input.dpi = static_cast<unsigned>(dpi_ > 0 ? dpi_ : kDefaultDpi);
    input.nowMs = now;
    input.lastInteractionMs = lastInteractionMs_;

    const ToolbarVisibilityDecision decision = EvaluateToolbarVisibility(input);
    if (decision.action == ToolbarVisibilityAction::Show) {
        SetControlBarVisible(true);
    } else if (decision.action == ToolbarVisibilityAction::Hide) {
        SetControlBarVisible(false);
    }
    if (moved) {
        lastInteractionMs_ = now;
    }
    toolbarWasFullscreen_ = fullscreen_;
    UpdateProgressStrip();
    if (decision.restoreAction == ToolbarVisibilityAction::Show) {
        SetToolbarRestoreVisible(true);
    } else if (decision.restoreAction == ToolbarVisibilityAction::Hide) {
        SetToolbarRestoreVisible(false);
    }
}

void MainWindow::FinishMouseControlInteraction() noexcept {
    if (!mouseControlInteraction_) {
        return;
    }
    mouseControlInteraction_ = false;
    pendingVideoRefocus_ = true;
    RestoreVideoFocusAfterMouseInteraction();
}

void MainWindow::RestoreVideoFocusAfterMouseInteraction() noexcept {
    if (!pendingVideoRefocus_ || !CanShowOwnedPopups() || videoWindow_ == nullptr ||
        IsWindow(videoWindow_) == FALSE) {
        return;
    }
    SetFocus(videoWindow_);
    if (GetFocus() == videoWindow_) {
        pendingVideoRefocus_ = false;
    }
}

void MainWindow::ToggleAreaZoom() {
    if (zoomState_ == ZoomState::Selecting || zoomState_ == ZoomState::Applied) {
        ResetAreaZoom();
    } else {
        BeginAreaZoom();
    }
}

void MainWindow::BeginAreaZoom() {
    HideSeekPreview();

    VideoDimensions video{};
    GeometryRect viewportScreen{};
    if (!CanShowOwnedPopups() || !hasMedia_ || !CurrentVideoViewport(video, viewportScreen)) {
        return;
    }
    const GeometryRect content = ComputeVideoContentRect(video, viewportScreen);
    if (content.IsEmpty()) {
        return;
    }

    selectionVideo_ = video;
    selectionViewportScreen_ = viewportScreen;
    const int minimum = ScaleLogicalPixels(
        kMinimumZoomSelectionLogicalPixels,
        static_cast<unsigned>(dpi_ > 0 ? dpi_ : kDefaultDpi));
    if (selectionOverlay_.Begin(content, minimum)) {
        zoomState_ = ZoomState::Selecting;
        zoomSelectionCrop_ = {};
        SetWindowTextW(zoomButton_, kZoomCancelText);
        if (fullscreen_) {
            SetControlBarVisible(false);
            SetFocus(videoWindow_);
        }
    }
}

void MainWindow::ResetAreaZoom() {
    EndZoomPan();
    zoomWheelRemainder_ = 0;
    if (zoomState_ == ZoomState::Selecting) {
        selectionOverlay_.Cancel();
    }
    if (playerInitialized_) {
        player_.ResetVideoCrop();
    }
    zoomState_ = ZoomState::None;
    zoomSelectionCrop_ = {};
    selectionVideo_ = {};
    selectionViewportScreen_ = {};
    ResetVideoDisplay();
    if (zoomButton_ != nullptr) {
        SetWindowTextW(zoomButton_, kZoomStartText);
    }
    RegisterInteraction();
}

void MainWindow::HandleZoomEscape() {
    const ZoomEscapeDecision decision = EvaluateZoomEscape(zoomState_, fullscreen_);
    switch (decision.action) {
    case ZoomEscapeAction::CancelSelection:
        selectionOverlay_.Cancel();
        zoomState_ = decision.nextState;
        zoomSelectionCrop_ = {};
        selectionVideo_ = {};
        selectionViewportScreen_ = {};
        SetWindowTextW(zoomButton_, kZoomStartText);
        FinishMouseControlInteraction();
        RegisterInteraction();
        break;
    case ZoomEscapeAction::ResetCrop:
        ResetAreaZoom();
        break;
    case ZoomEscapeAction::ExitFullscreen:
        ExitFullscreen();
        break;
    case ZoomEscapeAction::None:
    default:
        break;
    }
}

void MainWindow::HandleSelectionOverlay(
    const SelectionOverlayEvent event,
    const SelectionOverlayResult* const result) {
    if (zoomState_ != ZoomState::Selecting) {
        return;
    }
    if (event != SelectionOverlayEvent::Completed || result == nullptr) {
        zoomState_ = ZoomState::None;
        zoomSelectionCrop_ = {};
        selectionVideo_ = {};
        selectionViewportScreen_ = {};
        SetWindowTextW(zoomButton_, kZoomStartText);
        FinishMouseControlInteraction();
        RegisterInteraction();
        return;
    }

    const int minimum = ScaleLogicalPixels(
        kMinimumZoomSelectionLogicalPixels,
        static_cast<unsigned>(dpi_ > 0 ? dpi_ : kDefaultDpi));
    const VideoCropMapping mapping = MapSelectionToVideoCrop(
        selectionVideo_, selectionViewportScreen_, result->screenRect, minimum);
    if (!mapping.valid || !ApplyAreaZoom(mapping.crop)) {
        zoomState_ = ZoomState::None;
        zoomSelectionCrop_ = {};
        selectionVideo_ = {};
        selectionViewportScreen_ = {};
        SetWindowTextW(zoomButton_, kZoomStartText);
        FinishMouseControlInteraction();
        RegisterInteraction();
        return;
    }

    selectionVideo_ = {};
    selectionViewportScreen_ = {};
    FinishMouseControlInteraction();
    RegisterInteraction();
}

bool MainWindow::ApplyAreaZoom(const VideoCrop& crop) {
    if (!player_.ApplyVideoCrop(crop)) {
        return false;
    }
    zoomState_ = ZoomState::Applied;
    zoomSelectionCrop_ = crop;
    zoomWheelRemainder_ = 0;
    ResetVideoDisplay();
    SetWindowTextW(zoomButton_, kZoomResetText);
    return true;
}

bool MainWindow::ApplyZoomNavigation(const VideoCrop& crop) {
    if (zoomState_ != ZoomState::Applied || !zoomSelectionCrop_.IsValid()) {
        return false;
    }
    if (!player_.ApplyVideoCrop(crop)) {
        return false;
    }
    UpdateVideoDisplayLayout();
    return true;
}

bool MainWindow::CurrentVideoViewport(
    VideoDimensions& video,
    GeometryRect& viewportScreen) const noexcept {
    video = {};
    viewportScreen = {};
    if (!playerInitialized_ || videoWindow_ == nullptr ||
        !player_.GetVideoSize(video)) {
        return false;
    }
    RECT bounds{};
    if (GetWindowRect(videoWindow_, &bounds) == FALSE ||
        bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        return false;
    }
    viewportScreen = {bounds.left, bounds.top, bounds.right, bounds.bottom};
    return true;
}

bool MainWindow::CurrentZoomViewport(
    VideoDimensions& video,
    VideoCrop& crop,
    GeometryRect& contentScreen) const noexcept {
    GeometryRect viewport{};
    if (zoomState_ != ZoomState::Applied || !hasMedia_ ||
        !CanShowOwnedPopups() || !CurrentVideoViewport(video, viewport) ||
        !player_.GetVideoCrop(crop)) {
        return false;
    }
    RECT render{};
    if (videoDisplayActive_ && GetWindowRect(videoRenderWindow_, &render) != FALSE) {
        viewport = {render.left, render.top, render.right, render.bottom};
    }
    contentScreen = ComputeVideoContentRect({crop.width, crop.height}, viewport);
    return !contentScreen.IsEmpty();
}

bool MainWindow::IsZoomPointerTarget(
    const POINT point,
    const GeometryRect& contentScreen) const noexcept {
    if (point.x < contentScreen.left || point.x >= contentScreen.right ||
        point.y < contentScreen.top || point.y >= contentScreen.bottom) {
        return false;
    }
    RECT viewport{};
    if (GetWindowRect(videoWindow_, &viewport) == FALSE ||
        !PointInsideRect(point, viewport)) {
        return false;
    }
    // Wheel messages may be delivered to the focused video while the cursor
    // is over a sibling/owned toolbar. Those controls keep their own input.
    for (const HWND overlay : {ActiveControlBar(), toolbarRestorePopup_,
            previewPopup_.Window()}) {
        RECT bounds{};
        if (overlay != nullptr && IsWindowVisible(overlay) != FALSE &&
            GetWindowRect(overlay, &bounds) != FALSE && PointInsideRect(point, bounds)) {
            return false;
        }
    }
    return true;
}

bool MainWindow::PanZoomByKeyboard(const UINT key) {
    VideoDimensions video{};
    VideoCrop crop{};
    GeometryRect content{};
    if (!CurrentZoomViewport(video, crop, content)) {
        return false;
    }
    EndZoomPan();
    GeometryRect viewport{};
    CurrentVideoViewport(video, viewport);
    const double visibleX = videoDisplayActive_
        ? (std::min)(1.0, static_cast<double>(viewport.Width()) / content.Width()) : 1.0;
    const double visibleY = videoDisplayActive_
        ? (std::min)(1.0, static_cast<double>(viewport.Height()) / content.Height()) : 1.0;
    const double stepX = (std::max)(2.0, crop.width * visibleX * 0.05);
    const double stepY = (std::max)(2.0, crop.height * visibleY * 0.05);
    const double dx = key == VK_LEFT ? -stepX : key == VK_RIGHT ? stepX : 0.0;
    const double dy = key == VK_UP ? -stepY : key == VK_DOWN ? stepY : 0.0;
    if (videoDisplayActive_) {
        const bool result = ApplyDisplayPan(
            video, crop, videoDisplayState_, viewport, content, dx, dy);
        RegisterInteraction();
        return result;
    }
    const VideoCrop next = PanVideoCrop(video, crop, dx, dy);
    if (!(next == crop) && !ApplyZoomNavigation(next)) {
        return false;
    }
    RegisterInteraction();
    return true;
}

bool MainWindow::HandleZoomWheel(const int wheelDelta, const POINT screenPoint) {
    VideoDimensions video{};
    VideoCrop crop{};
    GeometryRect content{};
    if (!CurrentZoomViewport(video, crop, content) ||
        !IsZoomPointerTarget(screenPoint, content)) {
        zoomWheelRemainder_ = 0;
        return false;
    }
    // Do not change the coordinate transform beneath an in-progress drag.
    if (ZoomGestureActive()) {
        return true;
    }
    const int accumulated = zoomWheelRemainder_ +
        std::clamp(wheelDelta, -120 * 32, 120 * 32);
    const int steps = accumulated / WHEEL_DELTA;
    zoomWheelRemainder_ = accumulated % WHEEL_DELTA;
    if (steps == 0) {
        return true;
    }
    const double anchorX =
        static_cast<double>(screenPoint.x - content.left) / content.Width();
    const double anchorY =
        static_cast<double>(screenPoint.y - content.top) / content.Height();
    const VideoCrop next = ZoomVideoCrop(
        video, crop, std::pow(1.12, -steps), anchorX, anchorY,
        {zoomSelectionCrop_.width, zoomSelectionCrop_.height});
    if (!(next == crop)) {
        ApplyZoomNavigation(next);
    }
    RegisterInteraction();
    return true;
}

bool MainWindow::BeginZoomPan(const POINT screenPoint) {
    VideoDimensions video{};
    VideoCrop crop{};
    GeometryRect content{};
    if (!CurrentZoomViewport(video, crop, content) ||
        !IsZoomPointerTarget(screenPoint, content)) {
        return false;
    }
    EndZoomPan();
    HideSeekPreview();
    SetFocus(videoWindow_);
    SetCapture(videoWindow_);
    if (GetCapture() != videoWindow_) {
        return false;
    }
    zoomPanAnchorScreen_ = screenPoint;
    zoomPanStartCrop_ = crop;
    zoomPanVideo_ = video;
    zoomPanContentScreen_ = content;
    CurrentVideoViewport(video, zoomPanViewportScreen_);
    zoomPanDisplayState_ = videoDisplayState_;
    zoomPanDragging_ = true;
    zoomWheelRemainder_ = 0;
    lastInteractionMs_ = GetTickCount64();
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    UpdateControlBarVisibility();
    return true;
}

void MainWindow::UpdateZoomPan(const POINT screenPoint) {
    if (!zoomPanDragging_) {
        return;
    }
    if (GetCapture() != videoWindow_ || !CanShowOwnedPopups() ||
        zoomState_ != ZoomState::Applied) {
        EndZoomPan();
        return;
    }
    // Map from the crop as it was at mouse-down. Never sum rounded deltas:
    // returning the cursor to its start must restore exactly the start crop.
    const double dx = (static_cast<double>(zoomPanAnchorScreen_.x) - screenPoint.x) *
        zoomPanStartCrop_.width / zoomPanContentScreen_.Width();
    const double dy = (static_cast<double>(zoomPanAnchorScreen_.y) - screenPoint.y) *
        zoomPanStartCrop_.height / zoomPanContentScreen_.Height();
    if (videoDisplayActive_) {
        if (!ApplyDisplayPan(zoomPanVideo_, zoomPanStartCrop_, zoomPanDisplayState_,
                zoomPanViewportScreen_, zoomPanContentScreen_, dx, dy)) {
            EndZoomPan();
        }
        return;
    }
    const VideoCrop next = PanVideoCrop(zoomPanVideo_, zoomPanStartCrop_, dx, dy);
    VideoCrop current{};
    if (!player_.GetVideoCrop(current) ||
        (!(next == current) && !ApplyZoomNavigation(next))) {
        EndZoomPan();
    }
}

void MainWindow::EndZoomPan() noexcept {
    const bool wasDragging = ZoomGestureActive();
    zoomPanDragging_ = false;
    videoResizeDragging_ = false;
    videoResizeEdge_ = VideoEdge::None;
    zoomPanStartCrop_ = {};
    zoomPanVideo_ = {};
    zoomPanContentScreen_ = {};
    zoomPanViewportScreen_ = {};
    if (wasDragging && GetCapture() == videoWindow_) {
        ReleaseCapture();
    }
    if (wasDragging) {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
}

bool MainWindow::ZoomGestureActive() const noexcept {
    return zoomPanDragging_ || videoResizeDragging_;
}

void MainWindow::UpdateVideoDisplayLayout() noexcept {
    if (videoWindow_ == nullptr || videoRenderWindow_ == nullptr) {
        return;
    }
    RECT client{};
    if (GetClientRect(videoWindow_, &client) == FALSE) {
        return;
    }
    GeometryRect bounds{0, 0, client.right, client.bottom};
    if (videoDisplayActive_ && zoomState_ == ZoomState::Applied && !bounds.IsEmpty()) {
        VideoCrop crop{};
        if (!player_.GetVideoCrop(crop)) {
            return;
        }
        const VideoDimensions size{crop.width, crop.height};
        videoDisplayState_ = NormalizeVideoDisplayState(size, bounds, videoDisplayState_);
        bounds = ComputeVideoDisplayRect(size, bounds, videoDisplayState_);
    }
    RECT old{};
    GetWindowRect(videoRenderWindow_, &old);
    MapWindowPoints(nullptr, videoWindow_, reinterpret_cast<POINT*>(&old), 2);
    if (old.left != bounds.left || old.top != bounds.top ||
        old.right != bounds.right || old.bottom != bounds.bottom) {
        SetWindowPos(videoRenderWindow_, nullptr, bounds.left, bounds.top,
            bounds.Width(), bounds.Height(), SWP_NOACTIVATE | SWP_NOZORDER);
        // Exposed letterbox regions belong to the fixed black parent.
        InvalidateRect(videoWindow_, nullptr, TRUE);
    }
}

void MainWindow::ResetVideoDisplay() noexcept {
    videoDisplayActive_ = false;
    videoDisplayState_ = {};
    UpdateVideoDisplayLayout();
}

VideoEdge MainWindow::VideoResizeEdgeAt(const POINT screenPoint) const noexcept {
    VideoDimensions video{};
    GeometryRect viewport{};
    if (!hasMedia_ || zoomState_ == ZoomState::Selecting ||
        !CanShowOwnedPopups() || !CurrentVideoViewport(video, viewport) ||
        !IsZoomPointerTarget(screenPoint, viewport)) {
        return VideoEdge::None;
    }
    GeometryRect content = ComputeVideoContentRect(video, viewport);
    if (zoomState_ == ZoomState::Applied) {
        VideoCrop crop{};
        if (!CurrentZoomViewport(video, crop, content)) {
            return VideoEdge::None;
        }
    }
    return HitTestVideoEdge(content, viewport,
        {screenPoint.x, screenPoint.y}, Scale(8));
}

void MainWindow::SetVideoPointerCursor(const POINT screenPoint) const noexcept {
    const VideoEdge edge = videoResizeDragging_
        ? videoResizeEdge_ : VideoResizeEdgeAt(screenPoint);
    LPCWSTR cursor = IDC_ARROW;
    switch (edge) {
    case VideoEdge::Left:
    case VideoEdge::Right:
        cursor = IDC_SIZEWE;
        break;
    case VideoEdge::Top:
    case VideoEdge::Bottom:
        cursor = IDC_SIZENS;
        break;
    case VideoEdge::TopLeft:
    case VideoEdge::BottomRight:
        cursor = IDC_SIZENWSE;
        break;
    case VideoEdge::TopRight:
    case VideoEdge::BottomLeft:
        cursor = IDC_SIZENESW;
        break;
    case VideoEdge::None:
        if (zoomPanDragging_) {
            cursor = IDC_SIZEALL;
        }
        break;
    }
    SetCursor(LoadCursorW(nullptr, cursor));
}

bool MainWindow::BeginVideoResize(const POINT screenPoint) {
    const VideoEdge edge = VideoResizeEdgeAt(screenPoint);
    VideoDimensions video{};
    GeometryRect viewport{};
    if (edge == VideoEdge::None || !CurrentVideoViewport(video, viewport)) {
        return false;
    }
    EndZoomPan();
    HideSeekPreview();
    SetFocus(videoWindow_);
    SetCapture(videoWindow_);
    if (GetCapture() != videoWindow_) {
        return false;
    }
    if (zoomState_ == ZoomState::None &&
        !ApplyAreaZoom({0, 0, video.width, video.height})) {
        ReleaseCapture();
        return false;
    }
    VideoCrop crop{};
    GeometryRect content{};
    if (!CurrentZoomViewport(video, crop, content)) {
        ReleaseCapture();
        return false;
    }
    videoDisplayActive_ = true;
    UpdateVideoDisplayLayout();
    videoResizeStartState_ = videoDisplayState_;
    videoResizeStartCrop_ = crop;
    videoResizeAspectReference_ = zoomSelectionCrop_;
    videoResizeVideo_ = video;
    videoResizeViewport_ = viewport;
    videoResizeContent_ = content;
    videoResizeAnchor_ = {screenPoint.x, screenPoint.y};
    videoResizeEdge_ = edge;
    videoResizeDragging_ = true;
    zoomWheelRemainder_ = 0;
    lastInteractionMs_ = GetTickCount64();
    SetVideoPointerCursor(screenPoint);
    UpdateControlBarVisibility();
    return true;
}

void MainWindow::UpdateVideoResize(const POINT screenPoint) {
    if (!videoResizeDragging_) {
        return;
    }
    if (GetCapture() != videoWindow_ || !CanShowOwnedPopups() ||
        zoomState_ != ZoomState::Applied) {
        EndZoomPan();
        return;
    }
    const VideoAreaResizeResult resized = ResizeVideoArea(
        videoResizeVideo_, videoResizeStartCrop_, videoResizeStartState_,
        videoResizeViewport_, videoResizeContent_, videoResizeEdge_, videoResizeAnchor_,
        {screenPoint.x, screenPoint.y});
    VideoCrop current{};
    if (!resized.valid || !player_.GetVideoCrop(current) ||
        (!(resized.crop == current) && !player_.ApplyVideoCrop(resized.crop))) {
        EndZoomPan();
        return;
    }
    // Resize edits the area itself. Wheel/pan must subsequently retain its
    // new proportions instead of snapping back to the first Z-selection.
    zoomSelectionCrop_ = resized.crop == videoResizeStartCrop_
        ? videoResizeAspectReference_ : resized.crop;
    videoDisplayState_ = resized.display;
    UpdateVideoDisplayLayout();
}

bool MainWindow::ApplyDisplayPan(
    const VideoDimensions video, const VideoCrop crop, VideoDisplayState state,
    const GeometryRect viewport, const GeometryRect content,
    const double deltaX, const double deltaY) {
    if (!crop.IsValid() || viewport.IsEmpty() || content.IsEmpty()) {
        return false;
    }
    VideoCrop next = crop;
    if (deltaX != 0.0 || deltaY != 0.0) {
        // The viewport can show only part of an oversized render host. Move
        // its visible source center, not just the crop origin: a full-source
        // crop has no spare origin range, but its clipped image still pans.
        const double visibleWidth = crop.width *
            (std::min)(1.0, static_cast<double>(viewport.Width()) / content.Width());
        const double visibleHeight = crop.height *
            (std::min)(1.0, static_cast<double>(viewport.Height()) / content.Height());
        const double centerX = std::clamp(
            crop.x + state.centerX * crop.width + deltaX,
            visibleWidth * 0.5, video.width - visibleWidth * 0.5);
        const double centerY = std::clamp(
            crop.y + state.centerY * crop.height + deltaY,
            visibleHeight * 0.5, video.height - visibleHeight * 0.5);
        next = PanVideoCrop(video, crop,
            centerX - (crop.x + crop.width * 0.5),
            centerY - (crop.y + crop.height * 0.5));
        state.centerX = (centerX - next.x) / next.width;
        state.centerY = (centerY - next.y) / next.height;
    }
    VideoCrop current{};
    if (!player_.GetVideoCrop(current) ||
        (!(next == current) && !player_.ApplyVideoCrop(next))) {
        return false;
    }
    videoDisplayState_ = NormalizeVideoDisplayState(
        {next.width, next.height}, viewport, state);
    UpdateVideoDisplayLayout();
    return true;
}

void MainWindow::HandlePlayerEvent(
    const PlayerEvent event,
    const std::uint32_t generation) {
    if (generation != player_.Generation()) {
        return;
    }

    switch (event) {
    case PlayerEvent::Playing:
        UpdateExecutionState(true);
        break;
    case PlayerEvent::Paused:
        RestoreExecutionState();
        SetControlBarVisible(true);
        break;
    case PlayerEvent::Stopped:
        // Restart emits a service Stopped event. A real manual Stop clears
        // loopPlaybackRequested_ first; a loop transition must not flash the bar.
        if (!loopPlaybackRequested_ || playbackLoop_.Mode() == LoopMode::Off) {
            RestoreExecutionState();
            SetControlBarVisible(true);
        }
        break;
    case PlayerEvent::EndReached:
        if (playbackLoop_.Mode() != LoopMode::Off && loopPlaybackRequested_) {
            playbackSnapshot_ = player_.Snapshot();
            // LibVLC may publish input-death Stopped before the queued
            // EndReached reaches this UI thread. Preserve the end event, but
            // never turn a late event into another restart of Playing/Opening.
            if (playbackSnapshot_.state == PlaybackState::Stopped) {
                playbackSnapshot_.state = PlaybackState::Ended;
            }
            ApplyPlaybackLoop();
            RenderPlaybackSnapshot();
            return;
        }
        RestoreExecutionState();
        SetControlBarVisible(true);
        break;
    case PlayerEvent::EncounteredError:
        RestoreExecutionState();
        ResetAreaZoom();
        HideSeekPreview();
        if (player_.State() == PlaybackState::Error) {
            ShowMediaError();
        }
        break;
    case PlayerEvent::Opening:
        SetControlBarVisible(true);
        break;
    case PlayerEvent::LengthChanged:
    case PlayerEvent::SeekableChanged:
        break;
    default:
        break;
    }
    RefreshControls();
}

void MainWindow::RefreshControls() {
    if (!playerInitialized_) {
        return;
    }

    playbackSnapshot_ = player_.Snapshot();
    ApplyPlaybackLoop();
    RenderPlaybackSnapshot();
}

void MainWindow::RenderPlaybackSnapshot() {
    const PlaybackState state = playbackSnapshot_.state;
    if (state == PlaybackState::Error) {
        if (markerDragging_) CancelTransientUi();
        playbackLoop_.Reset();
        if (!removalIntervals_.empty()) {
            removalIntervals_.clear();
            InvalidateIntervalTimeline();
        }
        loopPlaybackRequested_ = false;
    }
    // Polling can observe an error even if its posted event was lost during a
    // lifecycle transition. Keep the UI's zoom state aligned with the engine.
    if ((state == PlaybackState::Error && zoomState_ != ZoomState::None) ||
        (zoomState_ == ZoomState::Applied && !player_.IsVideoCropped())) {
        CancelTransientUi();
        zoomState_ = ZoomState::None;
        zoomSelectionCrop_ = {};
        ResetVideoDisplay();
        SetWindowTextW(zoomButton_, kZoomStartText);
    }
    const bool playing = state == PlaybackState::Playing;
    UpdateExecutionState(playing);
    SetWindowTextW(playButton_, playing ? L"Пауза" : L"Відтворити");
    EnableWindow(playButton_, hasMedia_ && state != PlaybackState::Opening);
    EnableWindow(stopButton_, hasMedia_ && state != PlaybackState::Stopped);

    const std::int64_t duration =
        (std::max<std::int64_t>)(0, playbackSnapshot_.durationMs);
    const std::int64_t position = ClampTime(playbackSnapshot_.positionMs, duration);
    const bool canSeek = hasMedia_ && duration > 0 && playbackSnapshot_.seekable;
    if (canSeek && previewInitialized_ && state != PlaybackState::Error &&
        state != PlaybackState::Opening) {
        previewEngine_.WarmMedia(duration);
    }
    EnableWindow(seekSlider_, canSeek);
    UpdateLoopControls();

    SetWindowTextW(durationLabel_, videoplayer::FormatTime(duration).c_str());
    if (seekDragging_) {
        UpdateSeekLabel();
    } else {
        SendMessageW(seekSlider_, TBM_SETPOS, TRUE, TimeToSlider(position, duration));
        SetWindowTextW(currentTimeLabel_, videoplayer::FormatTime(position).c_str());
    }

    VideoDimensions dimensions{};
    const bool canZoom = hasMedia_ &&
        state != PlaybackState::Opening && state != PlaybackState::Error &&
        (zoomState_ != ZoomState::None || player_.GetVideoSize(dimensions));
    EnableWindow(zoomButton_, canZoom);
    EnableWindow(exportButton_, hasMedia_ && state != PlaybackState::Opening &&
        state != PlaybackState::Error && duration > 0);
    const bool canRemove = hasMedia_ &&
        state != PlaybackState::Opening && state != PlaybackState::Error &&
        duration > 0 && !HasActiveExportDialog(window_);
    RefreshSavedIntervalList();
    EnableWindow(savedIntervalList_, canRemove && !markerDragging_ && !removalIntervals_.empty());
    EnableWindow(removeSavedIntervalButton_, canRemove && !markerDragging_ && !removalIntervals_.empty());
    const bool hasCurrentSegment = playbackLoop_.HasSegment(duration);
    const bool currentConflict = hasCurrentSegment && OverlapsSavedInterval(
        {playbackLoop_.StartMs(), playbackLoop_.EndMs()}, removalIntervals_);
    EnableWindow(addRemovalButton_, canRemove && hasCurrentSegment && !currentConflict);
    EnableWindow(removeSegmentButton_, canRemove && !currentConflict &&
        (!removalIntervals_.empty() || playbackLoop_.HasSegment(duration)));
    const std::wstring removalCount =
        L"У списку на видалення: " + std::to_wstring(removalIntervals_.size());
    SetWindowTextW(removalCountLabel_, removalCount.c_str());
    UpdateControlBarVisibility();
}

void MainWindow::UpdateExecutionState(const bool playing) {
    if (playing && !executionStateActive_) {
        const EXECUTION_STATE result = SetThreadExecutionState(
            ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
        executionStateActive_ = result != 0;
    } else if (!playing) {
        RestoreExecutionState();
    }
}

void MainWindow::RestoreExecutionState() {
    if (executionStateActive_) {
        SetThreadExecutionState(ES_CONTINUOUS);
        executionStateActive_ = false;
    }
}

void MainWindow::ShowMediaError() {
    ResetAreaZoom();
    HideSeekPreview();
    if (mediaErrorShown_) {
        return;
    }
    mediaErrorShown_ = true;
    MessageBoxW(window_, kMediaError, kApplicationTitle, MB_OK | MB_ICONERROR);
}

void MainWindow::SetPromptVisible(const bool visible) {
    if (promptVisible_ == visible) {
        return;
    }
    promptVisible_ = visible;
    InvalidateRect(videoWindow_, nullptr, TRUE);
    InvalidateRect(videoRenderWindow_, nullptr, TRUE);
}

void MainWindow::UpdateWindowTitle() {
    const std::wstring fileName = FileNameFromPath(currentPath_);
    const std::wstring title = fileName.empty()
        ? std::wstring(kApplicationTitle)
        : std::wstring(kApplicationTitle) + L" — " + fileName;
    SetWindowTextW(window_, title.c_str());
}

void MainWindow::ResetMediaUiState() noexcept {
    CancelTransientUi();
    playbackSnapshot_ = {};
    playbackLoop_.Reset();
    removalIntervals_.clear();
    InvalidateIntervalTimeline();
    SetWindowTextW(removalCountLabel_, L"У списку на видалення: 0");
    loopPlaybackRequested_ = false;
    UpdateLoopControls();
    if (playerInitialized_) {
        player_.ResetVideoCrop();
    }
    zoomState_ = ZoomState::None;
    zoomSelectionCrop_ = {};
    selectionVideo_ = {};
    selectionViewportScreen_ = {};
    ResetVideoDisplay();
    if (zoomButton_ != nullptr) {
        SetWindowTextW(zoomButton_, kZoomStartText);
    }
}

void MainWindow::ShutdownPlaybackComponents() noexcept {
    if (shutdownComplete_) {
        return;
    }
    CancelTransientUi();
    shutdownComplete_ = true;

    if (seekSlider_ != nullptr && IsWindow(seekSlider_) != FALSE) {
        seekDragging_ = false;
        if (GetCapture() == seekSlider_) {
            ReleaseCapture();
        }
        RemoveWindowSubclass(seekSlider_, StaticSeekSubclass, kSeekSubclassId);
    }
    selectionOverlay_.Cancel();
    selectionOverlay_.Destroy();
    if (previewInitialized_) {
        previewEngine_.Shutdown();
        previewInitialized_ = false;
    }
    previewPopup_.Destroy();
    if (playerInitialized_) {
        player_.ResetVideoCrop();
        player_.Shutdown();
        playerInitialized_ = false;
    } else {
        player_.Shutdown();
    }
}

int MainWindow::TimeToSlider(
    const std::int64_t positionMs,
    const std::int64_t durationMs) noexcept {
    if (durationMs <= 0 || positionMs <= 0) {
        return 0;
    }
    if (positionMs >= durationMs) {
        return kSeekRange;
    }
    const long double fraction = static_cast<long double>(positionMs) /
        static_cast<long double>(durationMs);
    return std::clamp(static_cast<int>(fraction * kSeekRange), 0, kSeekRange);
}

std::int64_t MainWindow::SliderToTime(
    const int sliderPosition,
    const std::int64_t durationMs) noexcept {
    if (durationMs <= 0) {
        return 0;
    }
    const std::int64_t position = std::clamp(sliderPosition, 0, kSeekRange);
    const std::int64_t whole = durationMs / kSeekRange;
    const std::int64_t remainder = durationMs % kSeekRange;
    return (whole * position) + ((remainder * position) / kSeekRange);
}

}  // namespace videoplayer
