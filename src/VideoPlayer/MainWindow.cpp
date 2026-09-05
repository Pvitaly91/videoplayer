#include "targetver.h"
#include "MainWindow.h"

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

constexpr UINT_PTR kUiTimer = 1;
constexpr UINT_PTR kFullscreenToolbarTimer = 2;
constexpr UINT kUiTimerIntervalMs = 250;
constexpr UINT kFullscreenToolbarTimerIntervalMs = 100;
constexpr UINT_PTR kSeekSubclassId = 1;
constexpr int kSeekRange = 10000;
constexpr int kDefaultDpi = 96;
constexpr int kInitialWidth = 1000;
constexpr int kInitialHeight = 650;
constexpr int kMinimumWidth = 760;
constexpr int kMinimumHeight = 400;
constexpr int kControlBarHeight = 88;
constexpr int kProgressStripHeight = 3;
constexpr int kMinimumZoomSelectionLogicalPixels = 28;
constexpr std::size_t kMaximumFilesPerOpen = 8;
constexpr BYTE kFullscreenControlBarAlpha = 160;
constexpr COLORREF kProgressStripColor = RGB(255, 0, 0);

constexpr wchar_t kOpenFilter[] =
    L"Відеофайли\0*.mp4;*.avi;*.mkv;*.mov;*.m4v;*.webm;*.wmv;*.mpg;*.mpeg;*.ts;*.m2ts;*.mts;*.flv;*.3gp;*.ogv;*.vob\0"
    L"Усі файли\0*.*\0\0";

LRESULT CALLBACK ProgressStripWindowProc(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    UNREFERENCED_PARAMETER(lParam);
    switch (message) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_ERASEBKGND: {
        RECT bounds{};
        GetClientRect(window, &bounds);
        const HDC dc = reinterpret_cast<HDC>(wParam);
        SetDCBrushColor(dc, kProgressStripColor);
        FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT bounds{};
        GetClientRect(window, &bounds);
        SetDCBrushColor(dc, kProgressStripColor);
        FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        EndPaint(window, &paint);
        return 0;
    }

    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

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

    if (!player_.Initialize(videoWindow_, window_, error)) {
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
        if (ProcessKeyboardMessage(message)) {
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
        if (zoomState_ == ZoomState::Selecting) {
            CancelTransientUi();
        }
        if (fullscreen_ && IsIconic(window_) == FALSE) {
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
        if (wParam == kFullscreenToolbarTimer) {
            UpdateControlBarVisibility();
            return 0;
        }
        break;

    case WM_MOUSEMOVE:
        RegisterInteraction(true);
        return 0;

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
        KillTimer(window_, kFullscreenToolbarTimer);
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

    case WM_MOUSEMOVE:
        RegisterInteraction(true);
        return 0;

    case WM_LBUTTONDOWN:
        RegisterInteraction();
        SetFocus(window_);
        return 0;

    case WM_LBUTTONDBLCLK:
        RegisterInteraction();
        ToggleFullscreen();
        return 0;

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
            const std::array<HWND, 8> interactiveControls{
                openButton_, playButton_, stopButton_, seekSlider_,
                muteButton_, volumeSlider_, zoomButton_, fullscreenButton_};
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

LRESULT MainWindow::HandleSeekSubclass(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    switch (message) {
    case WM_MOUSEMOVE: {
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
        if (hasMedia_ && player_.IsSeekable() && player_.DurationMs() > 0) {
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
        if (seekDragging_) {
            CancelTransientUi();
            RefreshControls();
            return 0;
        }
        break;

    case WM_CAPTURECHANGED:
        if (seekDragging_ && reinterpret_cast<HWND>(lParam) != window) {
            CancelTransientUi();
            RefreshControls();
            return 0;
        }
        break;

    case WM_NCDESTROY:
        seekMouseTracking_ = false;
        seekDragging_ = false;
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        break;

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
    mainClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    mainClass.lpszClassName = kWindowClassName;
    mainClass.hIconSm = mainClass.hIcon;
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
    progressStripClass.lpfnWndProc = ProgressStripWindowProc;
    progressStripClass.hInstance = instance_;
    progressStripClass.lpszClassName = kProgressStripClassName;
    if (RegisterClassExW(&progressStripClass) == 0 && !IsClassAlreadyRegistered()) {
        error = L"Не вдалося зареєструвати індикатор прогресу.";
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
        nullptr);

    openButton_ = CreateWindowExW(
        0, L"BUTTON", L"Відкрити", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, controlBar_, ControlId(IDC_OPEN_BUTTON), instance_, nullptr);
    playButton_ = CreateWindowExW(
        0, L"BUTTON", L"Відтворити", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, controlBar_, ControlId(IDC_PLAY_BUTTON), instance_, nullptr);
    stopButton_ = CreateWindowExW(
        0, L"BUTTON", L"Стоп", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, controlBar_, ControlId(IDC_STOP_BUTTON), instance_, nullptr);
    seekSlider_ = CreateWindowExW(
        0, TRACKBAR_CLASSW, nullptr, child | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
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

    const std::array<HWND, 14> controls{
        videoWindow_, controlBar_, fullscreenControlBar_, progressStrip_,
        openButton_, playButton_, stopButton_, seekSlider_, currentTimeLabel_,
        durationLabel_, muteButton_, volumeSlider_, zoomButton_, fullscreenButton_};
    if (std::any_of(
            controls.begin(), controls.end(),
            [](const HWND control) { return control == nullptr; })) {
        return false;
    }

    ApplyFullscreenControlBarOpacity();

    // A layered owned popup composites above LibVLC's native video output.
    // WS_EX_TRANSPARENT makes this noninteractive strip pass input through
    // even when the video output belongs to LibVLC's rendering thread.
    if (SetLayeredWindowAttributes(progressStrip_, 0, 255, LWA_ALPHA) == FALSE) {
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

    const std::array<HWND, 10> controls{
        openButton_, playButton_, stopButton_, seekSlider_, currentTimeLabel_,
        durationLabel_, muteButton_, volumeSlider_, zoomButton_, fullscreenButton_};
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
    const int videoHeight = fullscreen_ ? height : panelTop;
    MoveWindow(videoWindow_, 0, 0, width, (std::max)(0, videoHeight), TRUE);

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
    const int firstRowY = Scale(7);
    const int sliderX = margin + timeWidth + gap;
    const int sliderWidth = (std::max)(
        0, width - (2 * margin) - (2 * timeWidth) - (2 * gap));

    MoveWindow(currentTimeLabel_, margin, firstRowY, timeWidth, rowHeight, TRUE);
    MoveWindow(seekSlider_, sliderX, firstRowY, sliderWidth, rowHeight, TRUE);
    MoveWindow(durationLabel_, width - margin - timeWidth, firstRowY, timeWidth, rowHeight, TRUE);

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

    int right = width - margin;
    right -= fullscreenWidth;
    MoveWindow(fullscreenButton_, right, secondRowY, fullscreenWidth, rowHeight, TRUE);
    right -= gap + zoomWidth;
    MoveWindow(zoomButton_, right, secondRowY, zoomWidth, rowHeight, TRUE);
    right -= gap + volumeWidth;
    MoveWindow(volumeSlider_, right, secondRowY, volumeWidth, rowHeight, TRUE);
    right -= gap + muteWidth;
    MoveWindow(muteButton_, right, secondRowY, muteWidth, rowHeight, TRUE);

    if (fullscreen_ && controlBarVisible_ && CanShowOwnedPopups() &&
        zoomState_ != ZoomState::Selecting) {
        SetWindowPos(
            activeControlBar, HWND_TOP,
            controlBarOrigin.x, controlBarOrigin.y, width, panelHeight,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
    UpdateProgressStrip();
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

    const std::array<HWND, 10> controls{
        openButton_, playButton_, stopButton_, seekSlider_, currentTimeLabel_,
        durationLabel_, muteButton_, volumeSlider_, zoomButton_, fullscreenButton_};
    std::array<HWND, 10> previousParents{};
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
    if (fullscreenControlBar_ == nullptr) {
        return;
    }

    LONG_PTR extendedStyle = GetWindowLongPtrW(
        fullscreenControlBar_, GWL_EXSTYLE);
    if (IsHighContrastEnabled()) {
        if ((extendedStyle & WS_EX_LAYERED) != 0) {
            SetWindowLongPtrW(
                fullscreenControlBar_,
                GWL_EXSTYLE,
                extendedStyle & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
        }
    } else {
        if ((extendedStyle & WS_EX_LAYERED) == 0) {
            extendedStyle |= WS_EX_LAYERED;
            SetWindowLongPtrW(
                fullscreenControlBar_, GWL_EXSTYLE, extendedStyle);
        }
        if (SetLayeredWindowAttributes(
                fullscreenControlBar_,
                0,
                kFullscreenControlBarAlpha,
                LWA_ALPHA) == FALSE) {
            extendedStyle = GetWindowLongPtrW(
                fullscreenControlBar_, GWL_EXSTYLE);
            SetWindowLongPtrW(
                fullscreenControlBar_,
                GWL_EXSTYLE,
                extendedStyle & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
        }
    }

    RedrawWindow(
        fullscreenControlBar_,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
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
    input.fullscreen = fullscreen_;
    input.toolbarVisible = toolbarStyleVisible;
    input.hasMedia = hasMedia_;
    input.availableWidth = (std::max)(
        0, static_cast<int>(client.right - client.left));
    input.positionMs = playbackSnapshot_.positionMs;
    input.durationMs = playbackSnapshot_.durationMs;
    const ProgressStripDecision decision = EvaluateProgressStrip(input);
    if (!decision.visible || decision.completedWidth <= 0) {
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
        decision.completedWidth,
        stripHeight,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
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
    pendingVideoRefocus_ = pendingVideoRefocus_ || mouseControlInteraction_ ||
        seekDragging_ || volumeDragging_;
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
    hasLastCursor_ = false;
    cancellingTransient_ = false;
    RestoreVideoFocusAfterMouseInteraction();
}

void MainWindow::RestoreUiAfterLifecycle() {
    if (restoringUi_ || cancellingTransient_ || shutdownComplete_ || window_ == nullptr ||
        videoWindow_ == nullptr || controlBar_ == nullptr || fullscreenControlBar_ == nullptr) {
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
    ShowWindow(fullscreen_ ? controlBar_ : fullscreenControlBar_, SW_HIDE);
    RECT client{};
    if (GetClientRect(window_, &client) != FALSE) {
        LayoutChildren(static_cast<int>(client.right), static_cast<int>(client.bottom));
    }
    RestoreVideoFocusAfterMouseInteraction();
    if (playerInitialized_) {
        SetTimer(window_, kUiTimer, kUiTimerIntervalMs, nullptr);
        if (fullscreen_) {
            SetTimer(window_, kFullscreenToolbarTimer,
                kFullscreenToolbarTimerIntervalMs, nullptr);
        }
        RefreshControls();
    }
    SetControlBarVisible(controlBarVisible_ || !fullscreen_);
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

bool MainWindow::HandleHotKey(
    const UINT key,
    const bool controlDown,
    const bool repeated) {
    if (controlDown && key == 'O') {
        if (!repeated) {
            ShowOpenDialog();
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
    if (player_.State() == PlaybackState::Playing) {
        player_.Pause();
        RestoreExecutionState();
    } else {
        std::wstring ignoredError;
        if (!player_.Play(ignoredError)) {
            ShowMediaError();
        }
    }
    RefreshControls();
}

void MainWindow::StopPlayback() {
    if (!hasMedia_) {
        return;
    }
    RegisterInteraction();
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
        if (previewDisplayedFrame_) {
            previewPopup_.ShowFrameAt(
                previewAnchorScreen_, dpi_, previewHoverTimeMs_, previewDisplayedFrame_);
        } else {
            previewPopup_.ShowPlaceholderAt(
                previewAnchorScreen_, dpi_, previewHoverTimeMs_);
        }
        return;
    }

    previewRequestGeneration_ = generation;
    previewRequestTimestampMs_ = quantized;
    previewDisplayedRequestId_ = 0;
    previewDisplayedFrame_.reset();
    previewPopup_.ShowPlaceholderAt(
        previewAnchorScreen_, dpi_, previewHoverTimeMs_);
    previewRequestId_ = previewEngine_.RequestFrame(previewHoverTimeMs_, duration);
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
    if (!previewInitialized_) {
        return;
    }
    const auto result = previewEngine_.TakeLatestResult();
    if (!result.has_value() || previewRequestId_ == 0 ||
        result->requestId != previewRequestId_ ||
        result->requestId <= previewDisplayedRequestId_ ||
        result->mediaGeneration != previewRequestGeneration_ ||
        result->mediaGeneration != player_.Generation() ||
        !CanShowOwnedPopups() || !previewPopup_.IsVisible()) {
        return;
    }
    if (result->frame && result->frame->IsValid()) {
        previewDisplayedRequestId_ = result->requestId;
        previewDisplayedFrame_ = result->frame;
        previewPopup_.ShowFrameAt(
            previewAnchorScreen_, dpi_, previewHoverTimeMs_, previewDisplayedFrame_);
    }
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
        ShowWindow(controlBar_, SW_SHOWNA);
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
    controlBarVisible_ = true;
    lastInteractionMs_ = GetTickCount64();
    SetTimer(
        window_, kFullscreenToolbarTimer,
        kFullscreenToolbarTimerIntervalMs, nullptr);

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
    SetControlBarVisible(true);
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
    KillTimer(window_, kFullscreenToolbarTimer);
    fullscreen_ = false;
    toolbarWasFullscreen_ = false;
    SetControlBarVisible(true);

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
}

void MainWindow::SetControlBarVisible(const bool visible) {
    const HWND activeControlBar = ActiveControlBar();
    if (activeControlBar == nullptr) {
        return;
    }
    if (!fullscreen_ && !visible) {
        return;
    }
    if (fullscreen_ && zoomState_ == ZoomState::Selecting && visible) {
        return;
    }
    controlBarVisible_ = visible;
    const bool show = visible && (!fullscreen_ || CanShowOwnedPopups());
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
    if (!fullscreen_ || controlBarVisible_ || !pointerMoved) {
        SetControlBarVisible(true);
        return;
    }
    UpdateControlBarVisibility(true);
}

void MainWindow::UpdateControlBarVisibility(const bool pointerMoved) {
    const HWND activeControlBar = ActiveControlBar();
    if (activeControlBar == nullptr || window_ == nullptr) {
        return;
    }
    if (volumeDragging_ && GetCapture() != volumeSlider_) {
        volumeDragging_ = false;
    }
    if (mouseControlInteraction_ && !seekDragging_ && !volumeDragging_ &&
        GetCapture() == nullptr) {
        // A trackbar can lose capture without sending TB_ENDTRACK. Clear the
        // mouse-only focus blocker; a keyboard-focused control is untouched.
        FinishMouseControlInteraction();
    }
    if (!fullscreen_) {
        toolbarWasFullscreen_ = false;
        SetControlBarVisible(true);
        return;
    }

    if (!CanShowOwnedPopups()) {
        ShowWindow(fullscreenControlBar_, SW_HIDE);
        ShowWindow(progressStrip_, SW_HIDE);
        return;
    }

    const bool toolbarActuallyVisible =
        (GetWindowLongPtrW(activeControlBar, GWL_STYLE) & WS_VISIBLE) != 0;
    controlBarVisible_ = toolbarActuallyVisible;

    const std::uint64_t now = GetTickCount64();
    POINT cursorScreen{};
    const bool haveCursor = GetCursorPos(&cursorScreen) != FALSE;
    bool moved = pointerMoved;
    if (haveCursor) {
        moved = moved || !hasLastCursor_ ||
            cursorScreen.x != lastCursorScreen_.x ||
            cursorScreen.y != lastCursorScreen_.y;
        lastCursorScreen_ = cursorScreen;
        hasLastCursor_ = true;
    }

    RECT client{};
    GetClientRect(window_, &client);
    POINT cursorClient = cursorScreen;
    const bool converted = haveCursor && ScreenToClient(window_, &cursorClient) != FALSE;
    const bool cursorInside = converted && PointInsideRect(cursorClient, client);

    RECT toolbarBounds{};
    const bool pointerOverToolbar = toolbarActuallyVisible && haveCursor &&
        GetWindowRect(activeControlBar, &toolbarBounds) != FALSE &&
        PointInsideRect(cursorScreen, toolbarBounds);
    const HWND focused = GetFocus();
    const bool controlHasFocus = focused == activeControlBar ||
        (focused != nullptr && IsChild(activeControlBar, focused) != FALSE);

    ToolbarVisibilityInput input{};
    input.wasFullscreen = toolbarWasFullscreen_;
    input.fullscreen = fullscreen_;
    input.playing = playbackSnapshot_.state == PlaybackState::Playing;
    input.currentlyVisible = toolbarActuallyVisible;
    input.cursorInsideClient = cursorInside;
    input.cursorMoved = moved;
    input.pointerOverToolbar = pointerOverToolbar;
    input.seekDragging = seekDragging_;
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
    toolbarWasFullscreen_ = true;
    UpdateProgressStrip();
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
        SetWindowTextW(zoomButton_, kZoomCancelText);
        if (fullscreen_) {
            SetControlBarVisible(false);
            SetFocus(videoWindow_);
        }
    }
}

void MainWindow::ResetAreaZoom() {
    if (zoomState_ == ZoomState::Selecting) {
        selectionOverlay_.Cancel();
    }
    if (playerInitialized_) {
        player_.ResetVideoCrop();
    }
    zoomState_ = ZoomState::None;
    selectionVideo_ = {};
    selectionViewportScreen_ = {};
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
    SetWindowTextW(zoomButton_, kZoomResetText);
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
    case PlayerEvent::Stopped:
    case PlayerEvent::EndReached:
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
    RenderPlaybackSnapshot();
}

void MainWindow::RenderPlaybackSnapshot() {
    const PlaybackState state = playbackSnapshot_.state;
    // Polling can observe an error even if its posted event was lost during a
    // lifecycle transition. Keep the UI's zoom state aligned with the engine.
    if ((state == PlaybackState::Error && zoomState_ != ZoomState::None) ||
        (zoomState_ == ZoomState::Applied && !player_.IsVideoCropped())) {
        CancelTransientUi();
        zoomState_ = ZoomState::None;
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
    EnableWindow(seekSlider_, canSeek);

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
    if (playerInitialized_) {
        player_.ResetVideoCrop();
    }
    zoomState_ = ZoomState::None;
    selectionVideo_ = {};
    selectionViewportScreen_ = {};
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
