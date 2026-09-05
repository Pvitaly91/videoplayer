#include "../../src/VideoPlayer/MainWindow.h"
#include "../../src/VideoPlayer/ProgressStrip.h"
#include "../../src/VideoPlayer/Resource.h"
#include "FakePlayerBackend.h"

#include <commctrl.h>

#include <array>
#include <iostream>
#include <string>
#include <utility>

namespace videoplayer {

// This friend only builds native HWND fixtures and invokes actual production
// methods. The normal application contains no alternative UI implementation.
struct MainWindowTestAccess final {
    struct Checks final {
        int& passed;
        int& failed;
        void Expect(bool condition, const wchar_t* name) {
            if (condition) {
                ++passed;
            } else {
                ++failed;
                std::wcerr << L"FAIL: UI lifecycle: " << name << L'\n';
            }
        }
    };

    struct Fixture final {
        tests::FakePlayerBackend backend;
        MainWindow window;

        bool Create() {
            INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
            InitCommonControlsEx(&controls);
            window.instance_ = GetModuleHandleW(nullptr);
            std::wstring error;
            if (!window.RegisterWindowClasses(error)) {
                return false;
            }
            window.window_ = CreateWindowExW(
                WS_EX_TOOLWINDOW, L"VideoPlayer.MainWindow", L"Hidden lifecycle fixture",
                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                -32000, -32000, 1000, 650, nullptr, nullptr,
                window.instance_, &window);
            if (window.window_ == nullptr ||
                !window.previewPopup_.Create(window.window_, window.instance_, error) ||
                !window.selectionOverlay_.Create(
                    window.instance_, window.window_, window.window_, error)) {
                return false;
            }
            backend.Install(window.player_);
            window.playerInitialized_ = true;
            window.hasMedia_ = true;
            window.RefreshControls();
            return true;
        }
    };

    static std::array<HWND, 10> Controls(const MainWindow& window) {
        return {window.openButton_, window.playButton_, window.stopButton_,
            window.seekSlider_, window.currentTimeLabel_, window.durationLabel_,
            window.muteButton_, window.volumeSlider_, window.zoomButton_,
            window.fullscreenButton_};
    }

    static bool AllParents(const MainWindow& window, HWND parent) {
        for (const HWND control : Controls(window)) {
            if (GetParent(control) != parent) {
                return false;
            }
        }
        return true;
    }

    static bool HasCrop(const MainWindow& window, const VideoCrop& expected) {
        VideoCrop crop{};
        return window.player_.GetVideoCrop(crop) && crop == expected;
    }

    static inline int reparentCalls = 0;
    static inline int failReparentCall = 0;
    static HWND WINAPI FailOneReparent(HWND child, HWND parent) {
        if (++reparentCalls == failReparentCall) {
            SetLastError(ERROR_ACCESS_DENIED);
            return nullptr;
        }
        return ::SetParent(child, parent);
    }

    static void Ownership(Checks& checks) {
        Fixture fixture;
        checks.Expect(fixture.Create(), L"create production HWND fixture");
        auto& window = fixture.window;
        if (window.window_ == nullptr) return;
        checks.Expect(!IsWindowVisible(window.window_), L"owner is never displayed");
        checks.Expect(GetWindow(window.fullscreenControlBar_, GW_OWNER) == window.window_,
            L"fullscreen toolbar belongs to this owner");
        checks.Expect(GetWindow(window.progressStrip_, GW_OWNER) == window.window_,
            L"progress popup belongs to this owner");
        checks.Expect((GetWindowLongPtrW(window.progressStrip_, GWL_STYLE) & WS_CHILD) == 0,
            L"progress is not a child hidden behind LibVLC output");
        const LONG_PTR progressEx = GetWindowLongPtrW(window.progressStrip_, GWL_EXSTYLE);
        checks.Expect((progressEx & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) ==
            (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE), L"progress cannot activate or enter taskbar");
        checks.Expect((progressEx & WS_EX_TOPMOST) == 0, L"progress is not global topmost");
        checks.Expect(SendMessageW(window.progressStrip_, WM_NCHITTEST, 0, 0) == HTTRANSPARENT,
            L"progress preserves bottom mouse activation zone");
        window.EnterFullscreen();
        checks.Expect(AllParents(window, window.fullscreenControlBar_),
            L"all controls reparent into fullscreen host");
        checks.Expect(GetAncestor(window.openButton_, GA_ROOTOWNER) == window.window_,
            L"Open control keyboard root remains its player owner");
        checks.Expect(!IsWindowVisible(window.fullscreenControlBar_) &&
            !IsWindowVisible(window.progressStrip_), L"hidden owner never reveals fullscreen popups");
        HIGHCONTRASTW highContrast{};
        highContrast.cbSize = sizeof(highContrast);
        const bool highContrastOn = SystemParametersInfoW(
            SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0) != FALSE &&
            (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
        const LONG_PTR toolbarEx = GetWindowLongPtrW(window.fullscreenControlBar_, GWL_EXSTYLE);
        BYTE alpha = 0;
        DWORD flags = 0;
        const bool hasAlpha = GetLayeredWindowAttributes(
            window.fullscreenControlBar_, nullptr, &alpha, &flags) != FALSE;
        checks.Expect(highContrastOn ? (toolbarEx & WS_EX_LAYERED) == 0 :
            hasAlpha && (flags & LWA_ALPHA) != 0 && alpha == 160,
            L"fullscreen alpha is 160 with opaque High Contrast fallback");
        checks.Expect((toolbarEx & WS_EX_TRANSPARENT) == 0,
            L"interactive transparent toolbar is not mouse click-through");
        window.ExitFullscreen();
        checks.Expect(AllParents(window, window.controlBar_), L"all controls return to docked host");
        const HWND toolbar = window.fullscreenControlBar_;
        const HWND strip = window.progressStrip_;
        const HWND preview = window.previewPopup_.Window();
        const HWND selection = window.selectionOverlay_.WindowHandle();
        DestroyWindow(window.window_);
        checks.Expect(!IsWindow(toolbar) && !IsWindow(strip) &&
            !IsWindow(preview) && !IsWindow(selection), L"owner teardown destroys every owned popup");
    }

    static void CaptureCancellation(Checks& checks) {
        Fixture fixture;
        checks.Expect(fixture.Create(), L"create capture fixture");
        auto& window = fixture.window;
        if (window.window_ == nullptr) return;
        window.seekDragging_ = true;
        window.volumeDragging_ = true;
        window.mouseControlInteraction_ = true;
        window.seekMouseTracking_ = true;
        SendMessageW(window.seekSlider_, TBM_SETPOS, TRUE, 8500);
        SetCapture(window.seekSlider_);
        SendMessageW(window.seekSlider_, WM_CANCELMODE, 0, 0);
        checks.Expect(fixture.backend.seekCalls == 0, L"WM_CANCELMODE performs no seek");
        checks.Expect(!window.seekDragging_ && !window.volumeDragging_ &&
            !window.mouseControlInteraction_ && !window.seekMouseTracking_,
            L"cancel clears every transient mouse flag");
        checks.Expect(GetCapture() != window.seekSlider_, L"cancel releases owned seek capture");
        checks.Expect(!window.previewPopup_.IsVisible(), L"cancel hides preview");

        window.seekDragging_ = true;
        window.mouseControlInteraction_ = true;
        SetCapture(window.seekSlider_);
        SetCapture(window.videoWindow_); // Real capture-change notification to the subclass.
        checks.Expect(fixture.backend.seekCalls == 0, L"capture loss performs no seek");
        checks.Expect(!window.seekDragging_, L"capture loss clears drag state");
        window.CancelTransientUi();
        window.CancelTransientUi();
        checks.Expect(fixture.backend.seekCalls == 0, L"repeated cancel is idempotent");

        SendMessageW(window.seekSlider_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 10));
        SendMessageW(window.seekSlider_, WM_LBUTTONUP, 0, MAKELPARAM(300, 10));
        checks.Expect(fixture.backend.seekCalls == 1, L"completed mouse drag commits exactly one seek");
        const auto selectedPosition = static_cast<int>(
            SendMessageW(window.seekSlider_, TBM_GETPOS, 0, 0));
        checks.Expect(fixture.backend.positionMs ==
            MainWindow::SliderToTime(selectedPosition, fixture.backend.durationMs),
            L"completed mouse drag seeks to the pointer-selected position");
        checks.Expect(!window.seekDragging_ && GetCapture() != window.seekSlider_,
            L"completed drag releases state and capture");
        const std::array<std::pair<int, int>, 4> keyboardSeeks{{
            {TB_TOP, 0}, {TB_BOTTOM, 10000}, {TB_PAGEUP, 7500}, {TB_PAGEDOWN, 6000}}};
        for (const auto& notification : keyboardSeeks) {
            const int before = fixture.backend.seekCalls;
            SendMessageW(window.seekSlider_, TBM_SETPOS, TRUE, notification.second);
            SendMessageW(window.window_, WM_HSCROLL,
                MAKEWPARAM(notification.first, 0), reinterpret_cast<LPARAM>(window.seekSlider_));
            SendMessageW(window.window_, WM_HSCROLL,
                MAKEWPARAM(TB_ENDTRACK, 0), reinterpret_cast<LPARAM>(window.seekSlider_));
            checks.Expect(fixture.backend.seekCalls == before + 1,
                L"native Home/End/Page seek plus ENDTRACK commits exactly once");
            checks.Expect(fixture.backend.positionMs ==
                notification.second * fixture.backend.durationMs / 10000,
                L"native keyboard seek reaches the selected trackbar time");
        }
    }

    static void MouseFocusRecovery(Checks& checks) {
        Fixture fixture;
        checks.Expect(fixture.Create(), L"create mouse focus recovery fixture");
        auto& window = fixture.window;
        if (window.window_ == nullptr) return;
        // Keep the fixture visible but outside the desktop so Win32 focus and
        // enabled-state behavior are exercised without showing test UI.
        ShowWindow(window.window_, SW_SHOWNA);

        SetFocus(window.seekSlider_);
        window.mouseControlInteraction_ = true;
        window.minimized_ = true;
        window.CancelTransientUi();
        checks.Expect(window.pendingVideoRefocus_ && !window.mouseControlInteraction_,
            L"minimize defers mouse-only toolbar focus recovery");
        window.minimized_ = false;
        window.RestoreUiAfterLifecycle();
        checks.Expect(!window.pendingVideoRefocus_ && GetFocus() == window.videoWindow_,
            L"restore moves interrupted mouse focus off toolbar controls");

        SetFocus(window.volumeSlider_);
        window.mouseControlInteraction_ = true;
        window.volumeDragging_ = true;
        SetCapture(window.volumeSlider_);
        ReleaseCapture();
        window.UpdateControlBarVisibility();
        checks.Expect(!window.volumeDragging_ && !window.mouseControlInteraction_ &&
            GetFocus() == window.videoWindow_,
            L"lost volume capture cannot pin fullscreen toolbar visibility");

        SetFocus(window.openButton_);
        window.mouseControlInteraction_ = true;
        EnableWindow(window.window_, FALSE);
        checks.Expect(!window.mouseControlInteraction_ &&
            (window.pendingVideoRefocus_ || GetFocus() != window.openButton_),
            L"modal disable clears the Open button mouse focus blocker");
        EnableWindow(window.window_, TRUE);
        checks.Expect(!window.pendingVideoRefocus_ && GetFocus() == window.videoWindow_ &&
            IsWindowEnabled(window.openButton_),
            L"modal dismissal restores Open and releases its mouse focus blocker");

        SetFocus(window.openButton_);
        window.CancelTransientUi();
        checks.Expect(!window.pendingVideoRefocus_ && GetFocus() == window.openButton_,
            L"keyboard-focused toolbar control is preserved for accessibility");
        ShowWindow(window.window_, SW_HIDE);
    }

    static void MinimizeAndModal(Checks& checks) {
        Fixture fixture;
        checks.Expect(fixture.Create(), L"create lifecycle fixture");
        auto& window = fixture.window;
        if (window.window_ == nullptr) return;
        const VideoCrop crop{80, 40, 320, 180};
        checks.Expect(window.ApplyAreaZoom(crop), L"apply crop before minimize");
        window.EnterFullscreen();
        window.seekDragging_ = true;
        window.volumeDragging_ = true;
        window.mouseControlInteraction_ = true;
        window.seekMouseTracking_ = true;
        SetCapture(window.seekSlider_);
        SendMessageW(window.window_, WM_SIZE, SIZE_MINIMIZED, 0);
        checks.Expect(window.minimized_, L"SIZE_MINIMIZED records suspended owner");
        checks.Expect(!window.seekDragging_ && !window.volumeDragging_ &&
            !window.seekMouseTracking_ && !window.mouseControlInteraction_,
            L"minimize clears drag and hover state");
        checks.Expect(fixture.backend.seekCalls == 0, L"minimize cannot commit pending drag");
        checks.Expect(HasCrop(window, crop), L"minimize retains source crop");
        checks.Expect(!IsWindowVisible(window.fullscreenControlBar_) &&
            !IsWindowVisible(window.progressStrip_) && !window.previewPopup_.IsVisible(),
            L"minimize hides transient popups");
        SendMessageW(window.window_, WM_TIMER, 2, 0);
        checks.Expect(!IsWindowVisible(window.fullscreenControlBar_),
            L"fullscreen timer cannot resurrect minimized toolbar");
        SendMessageW(window.window_, WM_SIZE, SIZE_RESTORED, MAKELPARAM(1000, 650));
        checks.Expect(!window.minimized_ && HasCrop(window, crop), L"restore preserves applied crop");
        checks.Expect(AllParents(window, window.fullscreenControlBar_) &&
            IsWindowEnabled(window.openButton_), L"restore retains usable controls in correct host");
        checks.Expect(!window.previewPopup_.IsVisible(), L"restore does not restore old hover preview");

        EnableWindow(window.window_, FALSE);
        checks.Expect(!IsWindowEnabled(window.fullscreenControlBar_), L"modal disable propagates to popup host");
        window.RefreshControls();
        SendMessageW(window.window_, WM_TIMER, 2, 0);
        checks.Expect(!IsWindowEnabled(window.window_) &&
            !IsWindowEnabled(window.fullscreenControlBar_), L"timers preserve modal disabled state");
        EnableWindow(window.window_, TRUE);
        checks.Expect(IsWindowEnabled(window.fullscreenControlBar_) &&
            IsWindowEnabled(window.openButton_), L"modal dismissal restores toolbar and Open");
        checks.Expect(HasCrop(window, crop), L"modal transition preserves crop");

        window.zoomState_ = ZoomState::Selecting;
        window.selectionVideo_ = {640, 360};
        window.selectionViewportScreen_ = {-32000, -32000, -31360, -31640};
        SendMessageW(window.window_, WM_SIZE, SIZE_MINIMIZED, 0);
        checks.Expect(window.zoomState_ != ZoomState::Selecting &&
            !window.selectionOverlay_.IsActive(), L"minimize cancels selection");
        window.ExitFullscreen();
    }

    static void ReparentRollback(Checks& checks) {
        Fixture fixture;
        checks.Expect(fixture.Create(), L"create reparent failure fixture");
        auto& window = fixture.window;
        if (window.window_ == nullptr) return;
        window.setParent_ = FailOneReparent;
        reparentCalls = 0;
        failReparentCall = 4;
        window.EnterFullscreen();
        checks.Expect(!window.fullscreen_ && AllParents(window, window.controlBar_),
            L"partial entry reparent failure rolls back every moved control");
        checks.Expect((GetWindowLongPtrW(window.controlBar_, GWL_STYLE) & WS_VISIBLE) != 0 &&
            IsWindowEnabled(window.openButton_), L"entry failure leaves Open in visible docked host");
        window.setParent_ = ::SetParent;
        window.EnterFullscreen();
        window.setParent_ = FailOneReparent;
        reparentCalls = 0;
        failReparentCall = 4;
        window.ExitFullscreen();
        checks.Expect(window.fullscreen_ && AllParents(window, window.fullscreenControlBar_),
            L"partial exit reparent failure rolls controls back into fullscreen host");
        window.setParent_ = ::SetParent;
        window.ExitFullscreen();
        checks.Expect(!window.fullscreen_ && AllParents(window, window.controlBar_),
            L"next transition recovers after one reparent failure");
    }

    static void ZoomAndEscape(Checks& checks) {
        Fixture fixture;
        checks.Expect(fixture.Create(), L"create windowed zoom fixture");
        auto& window = fixture.window;
        if (window.window_ == nullptr) return;
        RECT before{};
        GetWindowRect(window.window_, &before);
        const auto generation = window.player_.Generation();
        window.zoomState_ = ZoomState::Selecting;
        window.selectionVideo_ = {640, 360};
        window.selectionViewportScreen_ = {0, 0, 640, 360};
        SelectionOverlayResult selected{};
        selected.screenRect = {160, 90, 480, 270};
        window.HandleSelectionOverlay(SelectionOverlayEvent::Completed, &selected);
        const VideoCrop crop{160, 90, 320, 180};
        RECT after{};
        GetWindowRect(window.window_, &after);
        checks.Expect(window.zoomState_ == ZoomState::Applied && HasCrop(window, crop),
            L"selection stores the mapped source crop");
        checks.Expect(!window.fullscreen_ && EqualRect(&before, &after),
            L"windowed selection changes neither fullscreen nor window placement");
        checks.Expect(window.player_.Generation() == generation, L"crop does not replace media generation");
        RECT viewport{};
        RECT docked{};
        GetWindowRect(window.videoWindow_, &viewport);
        GetWindowRect(window.controlBar_, &docked);
        checks.Expect(viewport.bottom <= docked.top, L"windowed video viewport excludes toolbar");
        window.EnterFullscreen();
        checks.Expect(HasCrop(window, crop), L"enter fullscreen retains source crop");
        window.ExitFullscreen();
        checks.Expect(HasCrop(window, crop), L"exit fullscreen retains source crop");
        SendMessageW(window.window_, WM_SIZE, SIZE_MAXIMIZED, MAKELPARAM(1280, 800));
        checks.Expect(HasCrop(window, crop), L"resize never rounds or resets source crop");
        window.HandleZoomEscape();
        checks.Expect(window.zoomState_ == ZoomState::None && !window.player_.IsVideoCropped() &&
            !window.fullscreen_, L"Escape from applied windowed zoom resets only crop");
        window.EnterFullscreen();
        window.ApplyAreaZoom(crop);
        window.HandleZoomEscape();
        checks.Expect(window.fullscreen_ && window.zoomState_ == ZoomState::None,
            L"first Escape resets fullscreen crop without exiting fullscreen");
        window.HandleZoomEscape();
        checks.Expect(!window.fullscreen_, L"second Escape exits fullscreen");
        window.zoomState_ = ZoomState::Selecting;
        window.HandleZoomEscape();
        checks.Expect(window.zoomState_ == ZoomState::None && !window.fullscreen_,
            L"selection Escape cancels without changing window mode");
        window.HandleZoomEscape();
        checks.Expect(IsWindow(window.window_), L"Escape with no windowed zoom does not close player");
        window.ApplyAreaZoom(crop);
        window.ResetMediaUiState();
        checks.Expect(!window.player_.IsVideoCropped() && window.zoomState_ == ZoomState::None,
            L"new-media UI reset discards crop");
    }

    static void RepeatedIndependentControllers(Checks& checks) {
        Fixture first;
        Fixture second;
        Fixture third;
        checks.Expect(first.Create() && second.Create() && third.Create(),
            L"create three independent production controllers");
        if (!first.window.window_ || !second.window.window_ || !third.window.window_) return;
        const VideoCrop cropA{0, 0, 320, 180};
        const VideoCrop cropB{160, 90, 320, 180};
        const VideoCrop cropC{320, 180, 320, 180};
        first.window.ApplyAreaZoom(cropA);
        second.window.ApplyAreaZoom(cropB);
        third.window.ApplyAreaZoom(cropC);
        const HWND firstToolbar = first.window.fullscreenControlBar_;
        const HWND firstStrip = first.window.progressStrip_;
        const HWND firstOverlay = first.window.selectionOverlay_.WindowHandle();
        bool cyclesValid = true;
        for (int cycle = 0; cycle < 30; ++cycle) {
            auto& window = first.window;
            window.EnterFullscreen();
            SendMessageW(window.window_, WM_SIZE, SIZE_MINIMIZED, 0);
            SendMessageW(window.window_, WM_SIZE, SIZE_RESTORED, MAKELPARAM(1000, 650));
            EnableWindow(window.window_, FALSE);
            EnableWindow(window.window_, TRUE);
            window.ExitFullscreen();
            first.backend.positionMs = 2000 + cycle * 250;
            SendMessageW(window.window_, WM_TIMER, 1, 0);
            cyclesValid = cyclesValid && HasCrop(window, cropA) &&
                HasCrop(second.window, cropB) && HasCrop(third.window, cropC) &&
                AllParents(window, window.controlBar_) && IsWindowEnabled(window.openButton_) &&
                firstToolbar == window.fullscreenControlBar_ && firstStrip == window.progressStrip_ &&
                firstOverlay == window.selectionOverlay_.WindowHandle() &&
                window.playbackSnapshot_.positionMs == first.backend.positionMs;
        }
        checks.Expect(cyclesValid, L"30 hidden lifecycle cycles preserve controls, snapshots, crop and HWNDs");
        checks.Expect(!second.window.fullscreen_ && !third.window.fullscreen_,
            L"one controller fullscreen/minimize never changes other controllers");
        first.window.ResetAreaZoom();
        checks.Expect(HasCrop(second.window, cropB) && HasCrop(third.window, cropC),
            L"one controller reset never changes other crops");
        checks.Expect(GetWindow(second.window.progressStrip_, GW_OWNER) == second.window.window_ &&
            GetWindow(third.window.progressStrip_, GW_OWNER) == third.window.window_,
            L"independent progress popups retain distinct owners");
    }

    static void ReentrantCropGetter(Checks& checks) {
        Fixture fixture;
        checks.Expect(fixture.Create(), L"create reentrant getter fixture");
        auto& window = fixture.window;
        if (window.window_ == nullptr) return;
        const auto before = window.player_.Snapshot();
        bool callbackRan = false;
        bool coherent = false;
        fixture.backend.onCrop = [&] {
            const auto during = window.player_.Snapshot();
            VideoCrop previousCrop{};
            window.player_.GetVideoCrop(previousCrop);
            callbackRan = true;
            coherent = during.generation == before.generation &&
                during.positionMs == before.positionMs && during.durationMs == before.durationMs;
        };
        const bool applied = window.ApplyAreaZoom({80, 40, 320, 180});
        fixture.backend.onCrop = {};
        checks.Expect(applied && callbackRan && coherent,
            L"reentrant backend crop getter returns coherent snapshot without deadlock");
    }

    static void SnapshotProgress(Checks& checks) {
        Fixture normal;
        Fixture cropped;
        checks.Expect(normal.Create() && cropped.Create(), L"create progress snapshot fixtures");
        if (!normal.window.window_ || !cropped.window.window_) return;
        cropped.window.ApplyAreaZoom({80, 40, 320, 180});
        const std::array<std::int64_t, 4> positions{1000, 6000, 6000, 15000};
        bool equalProgress = true;
        bool equalSlider = true;
        for (std::size_t index = 0; index < positions.size(); ++index) {
            normal.backend.positionMs = cropped.backend.positionMs = positions[index];
            normal.backend.backendState = cropped.backend.backendState = index == 2 ? 4 : 3;
            SendMessageW(normal.window.window_, WM_TIMER, 1, 0);
            SendMessageW(cropped.window.window_, WM_TIMER, 1, 0);
            const auto strip = [](const MainWindow& window) {
                ProgressStripInput input{};
                input.fullscreen = true;
                input.toolbarVisible = false;
                input.hasMedia = window.hasMedia_;
                input.availableWidth = 960;
                input.positionMs = window.playbackSnapshot_.positionMs;
                input.durationMs = window.playbackSnapshot_.durationMs;
                return EvaluateProgressStrip(input);
            };
            const auto first = strip(normal.window);
            const auto second = strip(cropped.window);
            equalProgress = equalProgress && first.visible && second.visible &&
                first.completedWidth == second.completedWidth &&
                first.completedWidth == positions[index] * 960 / normal.backend.durationMs;
            equalSlider = equalSlider &&
                SendMessageW(normal.window.seekSlider_, TBM_GETPOS, 0, 0) ==
                SendMessageW(cropped.window.seekSlider_, TBM_GETPOS, 0, 0);
        }
        checks.Expect(equalProgress && normal.window.zoomState_ == ZoomState::None &&
            cropped.window.zoomState_ == ZoomState::Applied,
            L"timer snapshots produce equal mini-progress with zoom None and Applied, including pause");
        checks.Expect(equalSlider, L"full slider consumes the same time snapshots as mini-progress");
        normal.window.StopPlayback();
        checks.Expect(normal.window.playbackSnapshot_.positionMs == 0,
            L"Stop immediately refreshes progress snapshot to zero");
        cropped.backend.backendState = 6;
        cropped.window.RefreshControls();
        checks.Expect(cropped.window.playbackSnapshot_.positionMs == cropped.backend.durationMs,
            L"Ended snapshot reaches the media duration");
    }

    static void PreviewRequestReuse(Checks& checks) {
        MainWindow window;
        window.previewRequestGeneration_ = 7;
        window.previewRequestTimestampMs_ = 3000;
        checks.Expect(!window.CanReusePreviewRequest(7, 3000),
            L"rejected zero preview request can retry the same timestamp");
        window.previewRequestId_ = 21;
        window.previewEngine_.CancelRequests();
        checks.Expect(!window.CanReusePreviewRequest(7, 3000),
            L"cancelled or no-longer-active preview request can retry the same timestamp");
        window.previewEngine_.Shutdown();
        checks.Expect(!window.CanReusePreviewRequest(7, 3000),
            L"stopped preview worker does not leave a permanent same-request placeholder");
        auto frame = std::make_shared<PreviewFrame>();
        frame->mediaGeneration = 7;
        frame->timestampMs = 3000;
        frame->width = 2;
        frame->height = 2;
        frame->pitch = 8;
        frame->pixels.assign(16, 0);
        window.previewDisplayedFrame_ = frame;
        window.previewDisplayedRequestId_ = 21;
        checks.Expect(window.CanReusePreviewRequest(7, 3000),
            L"exact valid displayed preview can be reused without decoder work");
        checks.Expect(!window.CanReusePreviewRequest(8, 3000),
            L"same timestamp from another media generation cannot reuse preview");
        checks.Expect(!window.CanReusePreviewRequest(7, 3750),
            L"different quantized timestamp cannot reuse preview");
        window.previewDisplayedRequestId_ = 20;
        checks.Expect(!window.CanReusePreviewRequest(7, 3000),
            L"old displayed request does not satisfy a new request");
        window.previewDisplayedRequestId_ = 21;
        frame->mediaGeneration = 6;
        checks.Expect(!window.CanReusePreviewRequest(7, 3000),
            L"old-file frame identity cannot satisfy a current preview request");
    }

    static void Run(int& passed, int& failed) {
        Checks checks{passed, failed};
        Ownership(checks);
        CaptureCancellation(checks);
        MouseFocusRecovery(checks);
        MinimizeAndModal(checks);
        ReparentRollback(checks);
        ZoomAndEscape(checks);
        RepeatedIndependentControllers(checks);
        ReentrantCropGetter(checks);
        SnapshotProgress(checks);
        PreviewRequestReuse(checks);
        // Destroying each real MainWindow posts WM_QUIT. These hidden fixtures
        // do not own the test process's message loop, so consume only quit.
        MSG quit{};
        while (PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
    }
};

} // namespace videoplayer

void RunUiLifecycleTests(int& passed, int& failed) {
    videoplayer::MainWindowTestAccess::Run(passed, failed);
}
