#include "targetver.h"
#include "MainWindow.h"

#include "PlaybackMath.h"
#include "PathUtils.h"
#include "Resource.h"
#include "TimeFormatter.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Shell32.lib")

namespace videoplayer {
namespace {

constexpr wchar_t kWindowClassName[] = L"VideoPlayer.MainWindow";
constexpr wchar_t kVideoClassName[] = L"VideoPlayer.VideoSurface";
constexpr wchar_t kApplicationTitle[] = L"VideoPlayer";
constexpr wchar_t kDropPrompt[] =
    L"Перетягніть відеофайл сюди або натисніть “Відкрити”";
constexpr wchar_t kRuntimeError[] =
    L"Не знайдено компоненти відтворення LibVLC. Повністю розпакуйте програму та не переносіть окремо лише VideoPlayer.exe.";
constexpr wchar_t kMediaError[] =
    L"Не вдалося відтворити файл. Він може бути пошкоджений або містити непідтримуваний кодек.";

constexpr UINT_PTR kUiTimer = 1;
constexpr UINT kTimerIntervalMs = 250;
constexpr int kSeekRange = 10000;
constexpr int kDefaultDpi = 96;
constexpr int kInitialWidth = 1000;
constexpr int kInitialHeight = 650;
constexpr int kMinimumWidth = 640;
constexpr int kMinimumHeight = 400;

constexpr wchar_t kOpenFilter[] =
    L"Відеофайли\0*.mp4;*.avi;*.mkv;*.mov;*.m4v;*.webm;*.wmv;*.mpg;*.mpeg;*.ts;*.m2ts;*.mts;*.flv;*.3gp;*.ogv;*.vob\0"
    L"Усі файли\0*.*\0\0";

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

}  // namespace

MainWindow::~MainWindow() {
    if (window_ != nullptr && IsWindow(window_) != FALSE) {
        DestroyWindow(window_);
    }
    player_.Shutdown();
    if (ownsFont_ && uiFont_ != nullptr) {
        DeleteObject(uiFont_);
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

    DragAcceptFiles(window_, TRUE);
    SetTimer(window_, kUiTimer, kTimerIntervalMs, nullptr);
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
        MessageBoxW(
            window_,
            L"Не вдалося визначити повний шлях до файла.",
            kApplicationTitle,
            MB_OK | MB_ICONERROR);
        return false;
    case MediaPathStatus::NotFound:
        MessageBoxW(
            window_, L"Файл не знайдено.", kApplicationTitle, MB_OK | MB_ICONERROR);
        return false;
    case MediaPathStatus::Directory:
        MessageBoxW(
            window_,
            L"Вибраний шлях є каталогом. Виберіть відеофайл.",
            kApplicationTitle,
            MB_OK | MB_ICONERROR);
        return false;
    case MediaPathStatus::Inaccessible:
        MessageBoxW(
            window_,
            L"Не вдалося отримати доступ до вибраного файла.",
            kApplicationTitle,
            MB_OK | MB_ICONERROR);
        return false;
    }

    mediaErrorShown_ = false;
    std::wstring ignoredError;
    if (!player_.Open(ioPath, ignoredError)) {
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
    SetPromptVisible(false);
    UpdateWindowTitle();
    RefreshControls();
    return true;
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
    if (self == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return self->HandleMessage(message, wParam, lParam);
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
    if (self == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return self->HandleVideoMessage(window, message, wParam, lParam);
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
        LayoutChildren(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_GETMINMAXINFO: {
        auto* const info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = Scale(kMinimumWidth);
        info->ptMinTrackSize.y = Scale(kMinimumHeight);
        return 0;
    }

    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED) {
            switch (LOWORD(wParam)) {
            case IDC_OPEN_BUTTON:
                ShowOpenDialog();
                return 0;
            case IDC_PLAY_BUTTON:
                TogglePlayback();
                return 0;
            case IDC_STOP_BUTTON:
                StopPlayback();
                return 0;
            case IDC_MUTE_BUTTON:
                ToggleMute();
                return 0;
            case IDC_FULLSCREEN_BUTTON:
                ToggleFullscreen();
                return 0;
            default:
                break;
            }
        }
        break;

    case WM_HSCROLL: {
        const HWND source = reinterpret_cast<HWND>(lParam);
        const int notification = LOWORD(wParam);
        if (source == volumeSlider_) {
            const int volume = static_cast<int>(SendMessageW(volumeSlider_, TBM_GETPOS, 0, 0));
            SetVolumeFromSlider(volume);
            return 0;
        }
        if (source == seekSlider_) {
            if (notification == TB_ENDTRACK) {
                seekDragging_ = false;
                CommitSeekFromSlider();
            } else {
                seekDragging_ = true;
                UpdateSeekPreview();
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
        break;

    case WM_DROPFILES:
        HandleDroppedFiles(reinterpret_cast<HDROP>(wParam));
        return 0;

    case kPlayerEventMessage:
        HandlePlayerEvent(
            static_cast<PlayerEvent>(wParam),
            static_cast<std::uint32_t>(lParam));
        return 0;

    case WM_CLOSE:
        DestroyWindow(window_);
        return 0;

    case WM_DESTROY:
        KillTimer(window_, kUiTimer);
        DragAcceptFiles(window_, FALSE);
        RestoreExecutionState();
        player_.Shutdown();
        playerInitialized_ = false;
        PostQuitMessage(0);
        return 0;

    case WM_NCDESTROY:
    {
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
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);

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
            DrawTextW(
                dc,
                kDropPrompt,
                -1,
                &textBounds,
                DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, oldFont);
        }
        EndPaint(window, &paint);
        return 0;
    }

    case WM_LBUTTONDOWN:
        SetFocus(window_);
        return 0;

    case WM_LBUTTONDBLCLK:
        ToggleFullscreen();
        return 0;

    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
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
    return true;
}

bool MainWindow::CreateChildWindows() {
    const DWORD child = WS_CHILD | WS_VISIBLE;
    videoWindow_ = CreateWindowExW(
        0,
        kVideoClassName,
        nullptr,
        child | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0,
        0,
        0,
        0,
        window_,
        ControlId(IDC_VIDEO_SURFACE),
        instance_,
        this);

    openButton_ = CreateWindowExW(
        0, L"BUTTON", L"Відкрити", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, window_, ControlId(IDC_OPEN_BUTTON), instance_, nullptr);
    playButton_ = CreateWindowExW(
        0, L"BUTTON", L"Відтворити", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, window_, ControlId(IDC_PLAY_BUTTON), instance_, nullptr);
    stopButton_ = CreateWindowExW(
        0, L"BUTTON", L"Стоп", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, window_, ControlId(IDC_STOP_BUTTON), instance_, nullptr);
    seekSlider_ = CreateWindowExW(
        0, TRACKBAR_CLASSW, nullptr, child | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
        0, 0, 0, 0, window_, ControlId(IDC_SEEK_SLIDER), instance_, nullptr);
    currentTimeLabel_ = CreateWindowExW(
        0, L"STATIC", L"00:00", child | SS_CENTER | SS_CENTERIMAGE,
        0, 0, 0, 0, window_, ControlId(IDC_CURRENT_TIME), instance_, nullptr);
    durationLabel_ = CreateWindowExW(
        0, L"STATIC", L"00:00", child | SS_CENTER | SS_CENTERIMAGE,
        0, 0, 0, 0, window_, ControlId(IDC_DURATION), instance_, nullptr);
    muteButton_ = CreateWindowExW(
        0, L"BUTTON", L"Без звуку", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, window_, ControlId(IDC_MUTE_BUTTON), instance_, nullptr);
    volumeSlider_ = CreateWindowExW(
        0, TRACKBAR_CLASSW, nullptr, child | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
        0, 0, 0, 0, window_, ControlId(IDC_VOLUME_SLIDER), instance_, nullptr);
    fullscreenButton_ = CreateWindowExW(
        0, L"BUTTON", L"На весь екран", child | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, window_, ControlId(IDC_FULLSCREEN_BUTTON), instance_, nullptr);

    const std::array<HWND, 10> controls{
        videoWindow_, openButton_, playButton_, stopButton_, seekSlider_,
        currentTimeLabel_, durationLabel_, muteButton_, volumeSlider_, fullscreenButton_};
    if (std::any_of(controls.begin(), controls.end(), [](const HWND control) { return control == nullptr; })) {
        return false;
    }

    SendMessageW(seekSlider_, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(seekSlider_, TBM_SETRANGEMAX, FALSE, kSeekRange);
    SendMessageW(seekSlider_, TBM_SETPAGESIZE, 0, 100);
    SendMessageW(seekSlider_, TBM_SETPOS, TRUE, 0);
    EnableWindow(seekSlider_, FALSE);

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

    const std::array<HWND, 9> controls{
        openButton_, playButton_, stopButton_, seekSlider_, currentTimeLabel_,
        durationLabel_, muteButton_, volumeSlider_, fullscreenButton_};
    for (const HWND control : controls) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont_), TRUE);
    }
}

void MainWindow::LayoutChildren(const int width, const int height) const {
    if (videoWindow_ == nullptr) {
        return;
    }

    const int panelHeight = Scale(88);
    const int videoHeight = std::max(0, height - panelHeight);
    MoveWindow(videoWindow_, 0, 0, width, videoHeight, TRUE);

    const int margin = Scale(8);
    const int gap = Scale(6);
    const int rowHeight = Scale(28);
    const int timeWidth = Scale(60);
    const int firstRowY = videoHeight + Scale(7);
    const int sliderX = margin + timeWidth + gap;
    const int sliderWidth = std::max(0, width - (2 * margin) - (2 * timeWidth) - (2 * gap));

    MoveWindow(currentTimeLabel_, margin, firstRowY, timeWidth, rowHeight, TRUE);
    MoveWindow(seekSlider_, sliderX, firstRowY, sliderWidth, rowHeight, TRUE);
    MoveWindow(durationLabel_, width - margin - timeWidth, firstRowY, timeWidth, rowHeight, TRUE);

    const int secondRowY = videoHeight + Scale(48);
    const int openWidth = Scale(80);
    const int playWidth = Scale(96);
    const int stopWidth = Scale(58);
    const int muteWidth = Scale(92);
    const int volumeWidth = Scale(110);
    const int fullscreenWidth = Scale(118);

    int left = margin;
    MoveWindow(openButton_, left, secondRowY, openWidth, rowHeight, TRUE);
    left += openWidth + gap;
    MoveWindow(playButton_, left, secondRowY, playWidth, rowHeight, TRUE);
    left += playWidth + gap;
    MoveWindow(stopButton_, left, secondRowY, stopWidth, rowHeight, TRUE);

    int right = width - margin;
    right -= fullscreenWidth;
    MoveWindow(fullscreenButton_, right, secondRowY, fullscreenWidth, rowHeight, TRUE);
    right -= gap + volumeWidth;
    MoveWindow(volumeSlider_, right, secondRowY, volumeWidth, rowHeight, TRUE);
    right -= gap + muteWidth;
    MoveWindow(muteButton_, right, secondRowY, muteWidth, rowHeight, TRUE);
}

int MainWindow::Scale(const int value) const noexcept {
    return MulDiv(value, dpi_ > 0 ? dpi_ : kDefaultDpi, kDefaultDpi);
}

bool MainWindow::ProcessKeyboardMessage(const MSG& message) {
    if (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN) {
        return false;
    }
    if (window_ == nullptr ||
        (message.hwnd != window_ && GetAncestor(message.hwnd, GA_ROOT) != window_)) {
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
    case 'F':
    case VK_F11:
        if (!repeated) {
            ToggleFullscreen();
        }
        return true;
    case VK_ESCAPE:
        if (fullscreen_) {
            if (!repeated) {
                ExitFullscreen();
            }
            return true;
        }
        return false;
    default:
        return false;
    }
}

void MainWindow::ShowOpenDialog() {
    std::vector<wchar_t> fileName(32768, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = kOpenFilter;
    dialog.nFilterIndex = 1;
    dialog.lpstrFile = fileName.data();
    dialog.nMaxFile = static_cast<DWORD>(fileName.size());
    dialog.lpstrTitle = L"Відкрити відеофайл";
    dialog.Flags =
        OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
        OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog) != FALSE) {
        OpenFile(fileName.data());
    } else if (CommDlgExtendedError() != 0) {
        MessageBoxW(
            window_,
            L"Не вдалося відкрити діалог вибору файла.",
            kApplicationTitle,
            MB_OK | MB_ICONERROR);
    }
}

void MainWindow::HandleDroppedFiles(const HDROP drop) {
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (count > 0) {
        const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
        std::vector<wchar_t> path(static_cast<std::size_t>(length) + 1, L'\0');
        if (DragQueryFileW(drop, 0, path.data(), length + 1) != 0) {
            OpenFile(path.data());
        }
    }
    DragFinish(drop);
}

void MainWindow::TogglePlayback() {
    if (!hasMedia_) {
        return;
    }
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
    player_.Stop();
    RestoreExecutionState();
    SendMessageW(seekSlider_, TBM_SETPOS, TRUE, 0);
    RefreshControls();
}

void MainWindow::SeekBy(const std::int64_t deltaMs) {
    if (!hasMedia_ || !player_.IsSeekable()) {
        return;
    }
    const std::int64_t duration = std::max<std::int64_t>(0, player_.DurationMs());
    if (duration <= 0) {
        return;
    }
    const std::int64_t current = ClampTime(player_.PositionMs(), duration);
    const std::int64_t target = videoplayer::SeekBy(current, deltaMs, duration);
    player_.Seek(target);
    SendMessageW(seekSlider_, TBM_SETPOS, TRUE, TimeToSlider(target, duration));
    SetWindowTextW(currentTimeLabel_, videoplayer::FormatTime(target).c_str());
}

void MainWindow::CommitSeekFromSlider() {
    if (!hasMedia_ || !player_.IsSeekable()) {
        return;
    }
    const std::int64_t duration = player_.DurationMs();
    if (duration <= 0) {
        return;
    }
    const int sliderPosition = static_cast<int>(SendMessageW(seekSlider_, TBM_GETPOS, 0, 0));
    const std::int64_t target = SliderToTime(sliderPosition, duration);
    player_.Seek(target);
    SetWindowTextW(currentTimeLabel_, videoplayer::FormatTime(target).c_str());
}

void MainWindow::UpdateSeekPreview() {
    const std::int64_t duration = player_.DurationMs();
    const int sliderPosition = static_cast<int>(SendMessageW(seekSlider_, TBM_GETPOS, 0, 0));
    SetWindowTextW(
        currentTimeLabel_,
        videoplayer::FormatTime(SliderToTime(sliderPosition, duration)).c_str());
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
    SendMessageW(volumeSlider_, TBM_SETPOS, TRUE, adjusted);
    SetVolumeFromSlider(adjusted);
}

void MainWindow::ToggleMute() {
    if (muted_) {
        const int restored = std::max(1, ClampVolume(lastNonZeroVolume_));
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

    const LONG_PTR fullscreenStyle =
        (savedStyle_ & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) | WS_POPUP;
    const LONG_PTR fullscreenExtendedStyle = savedExtendedStyle_ &
        ~static_cast<LONG_PTR>(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE);
    SetWindowLongPtrW(window_, GWL_STYLE, fullscreenStyle);
    SetWindowLongPtrW(window_, GWL_EXSTYLE, fullscreenExtendedStyle);
    fullscreen_ = true;

    SetWindowPos(
        window_,
        HWND_TOP,
        monitorInfo.rcMonitor.left,
        monitorInfo.rcMonitor.top,
        monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
        monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
        SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    SetWindowTextW(fullscreenButton_, L"Вийти з екрана");
}

void MainWindow::ExitFullscreen() {
    if (!fullscreen_ || window_ == nullptr) {
        return;
    }

    SetWindowLongPtrW(window_, GWL_STYLE, savedStyle_);
    SetWindowLongPtrW(window_, GWL_EXSTYLE, savedExtendedStyle_);
    savedPlacement_.length = sizeof(savedPlacement_);
    SetWindowPlacement(window_, &savedPlacement_);
    SetWindowPos(
        window_,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    fullscreen_ = false;
    SetWindowTextW(fullscreenButton_, L"На весь екран");
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
        break;
    case PlayerEvent::EncounteredError:
        RestoreExecutionState();
        if (player_.State() == PlaybackState::Error) {
            ShowMediaError();
        }
        break;
    case PlayerEvent::Opening:
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

    const PlaybackState state = player_.State();
    const bool playing = state == PlaybackState::Playing;
    UpdateExecutionState(playing);
    SetWindowTextW(playButton_, playing ? L"Пауза" : L"Відтворити");
    EnableWindow(playButton_, hasMedia_ && state != PlaybackState::Opening);
    EnableWindow(stopButton_, hasMedia_ && state != PlaybackState::Stopped);

    const std::int64_t duration = std::max<std::int64_t>(0, player_.DurationMs());
    const std::int64_t position = ClampTime(player_.PositionMs(), duration);
    const bool canSeek = hasMedia_ && duration > 0 && player_.IsSeekable();
    EnableWindow(seekSlider_, canSeek);

    SetWindowTextW(durationLabel_, videoplayer::FormatTime(duration).c_str());
    if (seekDragging_) {
        UpdateSeekPreview();
    } else {
        SendMessageW(seekSlider_, TBM_SETPOS, TRUE, TimeToSlider(position, duration));
        SetWindowTextW(currentTimeLabel_, videoplayer::FormatTime(position).c_str());
    }
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
