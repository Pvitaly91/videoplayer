#include "../../src/VideoPlayer/MainWindow.h"
#include "../../src/VideoPlayer/ExportDialog.h"
#include "../../src/VideoPlayer/PreviewMath.h"
#include "../../src/VideoPlayer/ProgressStrip.h"
#include "../../src/VideoPlayer/Resource.h"
#include "../../src/VideoPlayer/IntervalTimeline.h"
#include "FakePlayerBackend.h"

#include <commctrl.h>

#include <array>
#include <atomic>
#include <iostream>
#include <memory>
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

        Fixture() = default;
        explicit Fixture(PreviewBackendFactory previewBackendFactory)
            : window(std::move(previewBackendFactory)) {
        }

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

    struct ControlledPreview final {
        HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE returned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::atomic<PreviewDecodeStatus> outcome{PreviewDecodeStatus::Ready};
        std::atomic<unsigned> decodeCalls{0};
        std::atomic<bool> cancellationObserved{false};

        ~ControlledPreview() {
            if (entered) CloseHandle(entered);
            if (release) CloseHandle(release);
            if (returned) CloseHandle(returned);
        }
        void Arm(const PreviewDecodeStatus next) {
            ResetEvent(entered);
            ResetEvent(release);
            ResetEvent(returned);
            outcome.store(next);
            cancellationObserved.store(false);
        }
    };

    class ControlledPreviewBackend final : public PreviewBackend {
    public:
        explicit ControlledPreviewBackend(std::shared_ptr<ControlledPreview> state)
            : state_(std::move(state)) {
        }
        PreviewDecodeStatus Decode(const PreviewDecodeRequest& request,
            const PreviewCancellation& cancellation, PreviewFramePtr& result) override {
            ++state_->decodeCalls;
            SetEvent(state_->entered);
            const bool released = WaitForSingleObject(state_->release, 3000) == WAIT_OBJECT_0;
            const bool cancelled = cancellation.IsCancelled(request);
            state_->cancellationObserved.store(cancelled);
            const auto status = cancelled ? PreviewDecodeStatus::Cancelled :
                released ? state_->outcome.load() : PreviewDecodeStatus::RuntimeFailure;
            if (status == PreviewDecodeStatus::Ready) {
                auto frame = std::make_shared<PreviewFrame>();
                frame->mediaGeneration = request.mediaGeneration;
                frame->timestampMs = request.timestampMs;
                frame->width = frame->height = 2;
                frame->pitch = 8;
                frame->pixels.assign(16, static_cast<std::uint8_t>(request.timestampMs / 5000));
                result = std::move(frame);
            }
            SetEvent(state_->returned);
            return status;
        }
        void ReleaseMedia() noexcept override {}
    private:
        std::shared_ptr<ControlledPreview> state_;
    };

    template<typename Predicate>
    static bool PumpPreviewUntil(MainWindow& window, Predicate complete) {
        const ULONGLONG started = GetTickCount64();
        for (;;) {
            MSG message{};
            while (PeekMessageW(&message, window.window_, kPreviewFrameReadyMessage,
                kPreviewFrameReadyMessage, PM_REMOVE)) {
                DispatchMessageW(&message);
            }
            if (complete()) return true;
            const ULONGLONG elapsed = GetTickCount64() - started;
            if (elapsed >= 3000) return false;
            // Only the production engine's posted notifications advance UI
            // presentation. No sleeps, decoder polling, or unrelated UI input.
            MsgWaitForMultipleObjects(0, nullptr, FALSE,
                static_cast<DWORD>(3000 - elapsed), QS_POSTMESSAGE);
        }
    }

    static std::array<HWND, 23> Controls(const MainWindow& window) {
        return {window.openButton_, window.playButton_, window.stopButton_, window.exportButton_,
            window.removeSegmentButton_, window.addRemovalButton_, window.removalCountLabel_,
            window.savedIntervalList_, window.removeSavedIntervalButton_,
            window.seekSlider_, window.currentTimeLabel_, window.durationLabel_,
            window.muteButton_, window.volumeSlider_, window.zoomButton_,
            window.fullscreenButton_, window.hideToolbarButton_,
            window.loopVideoButton_, window.loopStartButton_, window.loopEndButton_,
            window.loopSegmentButton_, window.loopClearButton_, window.loopRangeLabel_};
    }

    static std::wstring ControlText(const HWND control) {
        wchar_t text[128]{};
        GetWindowTextW(control, text, static_cast<int>(std::size(text)));
        return text;
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

    static bool IconDimensions(const HICON icon, const int width, const int height) {
        ICONINFO info{};
        if (!icon || !GetIconInfo(icon, &info)) return false;
        BITMAP color{};
        const bool matches = info.fIcon && info.hbmColor &&
            GetObjectW(info.hbmColor, sizeof(color), &color) == static_cast<int>(sizeof(color)) &&
            color.bmWidth == width && color.bmHeight == height;
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
        return matches;
    }

    static void ApplicationIcon(Checks& checks) {
        Fixture fixture;
        checks.Expect(fixture.Create(), L"create production icon HWND fixture");
        const auto& window = fixture.window;
        if (!window.window_) return;
        const HICON large = reinterpret_cast<HICON>(GetClassLongPtrW(window.window_, GCLP_HICON));
        const HICON small = reinterpret_cast<HICON>(GetClassLongPtrW(window.window_, GCLP_HICONSM));
        const HICON expectedLarge = static_cast<HICON>(LoadImageW(window.instance_,
            MAKEINTRESOURCEW(IDI_VIDEOPLAYER), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR | LR_SHARED));
        const HICON expectedSmall = static_cast<HICON>(LoadImageW(window.instance_,
            MAKEINTRESOURCEW(IDI_VIDEOPLAYER), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED));
        checks.Expect(large && large == expectedLarge,
            L"main window class uses the embedded shared large application icon");
        checks.Expect(small && small == expectedSmall,
            L"main window class uses the embedded shared small application icon");
        checks.Expect(large != LoadIconW(nullptr, IDI_APPLICATION) &&
            small != LoadIconW(nullptr, IDI_APPLICATION),
            L"main window does not use the generic Windows application icon");
        checks.Expect(IconDimensions(large, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON)),
            L"main window large icon follows the system icon dimensions");
        checks.Expect(IconDimensions(small, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON)),
            L"main window small icon follows the system small-icon dimensions");
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
        checks.Expect(GetWindow(window.toolbarRestorePopup_, GW_OWNER) == window.window_ &&
            GetParent(window.toolbarRestoreButton_) == window.toolbarRestorePopup_ &&
            GetAncestor(window.toolbarRestoreButton_, GA_ROOTOWNER) == window.window_,
            L"toolbar restore popup and button belong only to this player owner");
        const LONG_PTR restoreStyle = GetWindowLongPtrW(
            window.toolbarRestorePopup_, GWL_STYLE);
        const LONG_PTR restoreButtonStyle = GetWindowLongPtrW(
            window.toolbarRestoreButton_, GWL_STYLE);
        checks.Expect((restoreStyle & WS_POPUP) != 0 &&
            (restoreStyle & WS_CHILD) == 0 &&
            (restoreButtonStyle & WS_CHILD) != 0 &&
            (restoreButtonStyle & BS_TYPEMASK) == BS_OWNERDRAW &&
            GetDlgCtrlID(window.toolbarRestoreButton_) == IDC_RESTORE_TOOLBAR_BUTTON,
            L"restore popup stays above native video while its owner-drawn button remains a child");
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
        checks.Expect(GetDlgCtrlID(window.exportButton_) == IDC_EXPORT_BUTTON &&
            ControlText(window.exportButton_) == L"Зберегти відео…" &&
            window.HandleHotKey('E', true, true),
            L"export button and repeat-safe Ctrl+E are wired to this player owner");
        checks.Expect(!IsWindowVisible(window.fullscreenControlBar_) &&
            !IsWindowVisible(window.progressStrip_), L"hidden owner never reveals fullscreen popups");
        HIGHCONTRASTW highContrast{};
        highContrast.cbSize = sizeof(highContrast);
        const bool highContrastOn = SystemParametersInfoW(
            SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0) != FALSE &&
            (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
        const LONG_PTR restoreEx = GetWindowLongPtrW(
            window.toolbarRestorePopup_, GWL_EXSTYLE);
        BYTE restoreAlpha = 0;
        DWORD restoreFlags = 0;
        const bool restoreHasAlpha = GetLayeredWindowAttributes(
            window.toolbarRestorePopup_, nullptr, &restoreAlpha, &restoreFlags) != FALSE;
        checks.Expect((restoreEx & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) ==
            (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) &&
            (restoreEx & (WS_EX_TOPMOST | WS_EX_TRANSPARENT)) == 0,
            L"interactive restore popup is nonactivating, owner-scoped and not click-through");
        checks.Expect(highContrastOn ? (restoreEx & WS_EX_LAYERED) == 0 :
            (restoreEx & WS_EX_LAYERED) != 0 && restoreHasAlpha &&
                (restoreFlags & LWA_ALPHA) != 0 && restoreAlpha == 176,
            L"restore affordance alpha is 176 with opaque High Contrast fallback");
        checks.Expect(SendMessageW(
            window.toolbarRestorePopup_, WM_MOUSEACTIVATE,
            reinterpret_cast<WPARAM>(window.window_),
            MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)) == MA_NOACTIVATE,
            L"restore affordance click cannot activate its popup");
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
        const HWND restorePopup = window.toolbarRestorePopup_;
        const HWND restoreButton = window.toolbarRestoreButton_;
        const HWND preview = window.previewPopup_.Window();
        const HWND selection = window.selectionOverlay_.WindowHandle();
        DestroyWindow(window.window_);
        checks.Expect(!IsWindow(toolbar) && !IsWindow(strip) &&
            !IsWindow(restorePopup) && !IsWindow(restoreButton) &&
            !IsWindow(preview) && !IsWindow(selection), L"owner teardown destroys every owned popup");
    }

    static void ManualToolbarHiding(Checks& checks) {
        Fixture fixture;
        Fixture independent;
        checks.Expect(fixture.Create() && independent.Create(),
            L"create manual-toolbar and independent fixtures");
        auto& window = fixture.window;
        if (window.window_ == nullptr || independent.window.window_ == nullptr) return;

        // Owned popups require a WS_VISIBLE owner. Keep the real production
        // HWND far outside the desktop and never activate it.
        ShowWindow(window.window_, SW_SHOWNA);
        RECT offscreenOwner{};
        GetWindowRect(window.window_, &offscreenOwner);
        checks.Expect(IsWindowVisible(window.window_) &&
            offscreenOwner.right < 0 && offscreenOwner.bottom < 0 &&
            GetForegroundWindow() != window.window_,
            L"manual-toolbar fixture is visible offscreen without activation");

        const VideoCrop crop{80, 40, 320, 180};
        checks.Expect(window.ApplyAreaZoom(crop),
            L"apply crop before manually hiding toolbar");
        fixture.backend.positionMs = 6500;
        window.RefreshControls();
        const std::uint32_t generation = window.player_.Generation();
        const std::int64_t position = window.playbackSnapshot_.positionMs;
        const LRESULT sliderPosition = SendMessageW(
            window.seekSlider_, TBM_GETPOS, 0, 0);

        RECT ownerClient{};
        RECT dockedVideoClient{};
        GetClientRect(window.window_, &ownerClient);
        GetClientRect(window.videoWindow_, &dockedVideoClient);
        checks.Expect(dockedVideoClient.bottom < ownerClient.bottom,
            L"visible windowed toolbar initially reserves its docked height");

        SetFocus(window.hideToolbarButton_);
        SendMessageW(window.hideToolbarButton_, BM_CLICK, 0, 0);
        RECT hiddenVideoClient{};
        GetClientRect(window.videoWindow_, &hiddenVideoClient);
        checks.Expect(window.toolbarManuallyHidden_ && !window.controlBarVisible_ &&
            (GetWindowLongPtrW(window.controlBar_, GWL_STYLE) & WS_VISIBLE) == 0,
            L"actual Hide button enters persistent manual-hide mode");
        checks.Expect(hiddenVideoClient.right == ownerClient.right &&
            hiddenVideoClient.bottom == ownerClient.bottom,
            L"windowed manual hide expands video through the full client area");
        checks.Expect(IsWindowVisible(window.progressStrip_),
            L"windowed manual hide shows compact progress above LibVLC output");
        checks.Expect(GetFocus() == window.videoWindow_,
            L"hiding toolbar moves focus off its now-hidden child button");

        RECT videoClient{};
        GetClientRect(window.videoWindow_, &videoClient);
        POINT awayFromBottom{videoClient.right / 2, 0};
        ClientToScreen(window.videoWindow_, &awayFromBottom);
        window.UpdateControlBarVisibilityFromCursor(
            false, true, awayFromBottom);
        checks.Expect(!IsWindowVisible(window.toolbarRestorePopup_),
            L"restore affordance stays hidden outside the lower video zone");
        POINT nearBottom{videoClient.right / 2, videoClient.bottom - 1};
        ClientToScreen(window.videoWindow_, &nearBottom);
        window.UpdateControlBarVisibilityFromCursor(false, true, nearBottom);
        RECT videoBounds{};
        RECT stripBounds{};
        RECT restoreBounds{};
        RECT restoreButtonBounds{};
        GetWindowRect(window.videoWindow_, &videoBounds);
        GetWindowRect(window.progressStrip_, &stripBounds);
        GetWindowRect(window.toolbarRestorePopup_, &restoreBounds);
        GetWindowRect(window.toolbarRestoreButton_, &restoreButtonBounds);
        const int expectedCompletedWidth = static_cast<int>(
            (position * (videoBounds.right - videoBounds.left)) /
            fixture.backend.durationMs);
        const int videoCenter = videoBounds.left +
            (videoBounds.right - videoBounds.left) / 2;
        const int restoreCenter = restoreBounds.left +
            (restoreBounds.right - restoreBounds.left) / 2;
        checks.Expect(IsWindowVisible(window.toolbarRestorePopup_) &&
            IsWindowVisible(window.toolbarRestoreButton_),
            L"production cursor path reveals popup and button in the lower video zone");
        checks.Expect(restoreCenter >= videoCenter - 1 &&
            restoreCenter <= videoCenter + 1 &&
            restoreBounds.bottom < stripBounds.top &&
            stripBounds.bottom == videoBounds.bottom &&
            stripBounds.bottom - stripBounds.top == window.Scale(3) &&
            stripBounds.right - stripBounds.left == expectedCompletedWidth,
            L"restore affordance and completed-width strip use exact video geometry");
        checks.Expect(EqualRect(&restoreBounds, &restoreButtonBounds) != FALSE,
            L"restore button fills its owned popup hit target");

        SendMessageW(window.toolbarRestoreButton_, BM_CLICK, 0, 0);
        RECT restoredVideoBounds{};
        RECT restoredBarBounds{};
        GetWindowRect(window.videoWindow_, &restoredVideoBounds);
        GetWindowRect(window.controlBar_, &restoredBarBounds);
        checks.Expect(!window.toolbarManuallyHidden_ && window.controlBarVisible_ &&
            (GetWindowLongPtrW(window.controlBar_, GWL_STYLE) & WS_VISIBLE) != 0,
            L"actual restore icon click restores the docked toolbar");
        checks.Expect(restoredVideoBounds.bottom <= restoredBarBounds.top &&
            restoredVideoBounds.bottom < videoBounds.bottom,
            L"toolbar restore shrinks the windowed video viewport again");
        checks.Expect(!IsWindowVisible(window.progressStrip_) &&
            !IsWindowVisible(window.toolbarRestorePopup_),
            L"toolbar restore immediately hides compact strip and affordance");
        checks.Expect(window.player_.Generation() == generation &&
            window.playbackSnapshot_.positionMs == position &&
            fixture.backend.positionMs == position &&
            fixture.backend.seekCalls == 0 &&
            SendMessageW(window.seekSlider_, TBM_GETPOS, 0, 0) == sliderPosition &&
            HasCrop(window, crop),
            L"toolbar hide and restore retain seek, snapshot, crop and media generation");
        checks.Expect(GetFocus() == window.videoWindow_,
            L"nonactivating restore retains video keyboard focus");

        SendMessageW(window.hideToolbarButton_, BM_CLICK, 0, 0);
        fixture.backend.backendState = 4; // libvlc_Paused
        window.HandlePlayerEvent(PlayerEvent::Paused, generation);
        window.RegisterInteraction(true);
        checks.Expect(window.toolbarManuallyHidden_ && !window.controlBarVisible_ &&
            (GetWindowLongPtrW(window.controlBar_, GWL_STYLE) & WS_VISIBLE) == 0,
            L"Pause event and ordinary interaction cannot override manual hide");

        SendMessageW(window.window_, WM_SIZE, SIZE_MINIMIZED, 0);
        checks.Expect(window.minimized_ && window.toolbarManuallyHidden_ &&
            !IsWindowVisible(window.progressStrip_) &&
            !IsWindowVisible(window.toolbarRestorePopup_),
            L"minimize hides compact overlays but preserves manual-hide intent");
        SendMessageW(window.window_, WM_SIZE, SIZE_RESTORED,
            MAKELPARAM(ownerClient.right, ownerClient.bottom));
        checks.Expect(!window.minimized_ && window.toolbarManuallyHidden_ &&
            !window.controlBarVisible_ && IsWindowVisible(window.progressStrip_) &&
            HasCrop(window, crop) &&
            window.player_.Generation() == generation,
            L"restore reinstates compact progress and retains manual hide, zoom and generation");

        // Keep the owner hidden while the production F11 transition moves it
        // to the monitor; this exercises the actual reparent/placement path
        // without displaying test UI.
        ShowWindow(window.window_, SW_HIDE);
        window.EnterFullscreen();
        checks.Expect(window.fullscreen_ && window.toolbarManuallyHidden_ &&
            !window.controlBarVisible_ &&
            AllParents(window, window.fullscreenControlBar_) && HasCrop(window, crop),
            L"fullscreen entry preserves manual hide and crop with coherent parents");
        window.ExitFullscreen();
        checks.Expect(!window.fullscreen_ && window.toolbarManuallyHidden_ &&
            !window.controlBarVisible_ && AllParents(window, window.controlBar_) &&
            HasCrop(window, crop) && window.player_.Generation() == generation,
            L"F11 roundtrip returns hidden controls without losing zoom or generation");
        ShowWindow(window.window_, SW_SHOWNA);
        checks.Expect(window.toolbarManuallyHidden_ &&
            (GetWindowLongPtrW(window.controlBar_, GWL_STYLE) & WS_VISIBLE) == 0,
            L"visible offscreen owner still honors manual hide after F11 roundtrip");

        checks.Expect(!independent.window.toolbarManuallyHidden_ &&
            independent.window.toolbarRestorePopup_ != window.toolbarRestorePopup_,
            L"manual toolbar state and restore HWND do not bleed into another fixture");
        checks.Expect(independent.window.HandleHotKey('H', true, false) &&
            independent.window.toolbarManuallyHidden_,
            L"Ctrl+H enters manual-hide mode");
        checks.Expect(independent.window.HandleHotKey('H', true, true) &&
            independent.window.toolbarManuallyHidden_ &&
            independent.window.HandleHotKey('H', true, false) &&
            !independent.window.toolbarManuallyHidden_,
            L"repeated Ctrl+H is ignored and the next press restores the toolbar");
        checks.Expect(window.toolbarManuallyHidden_ &&
            !independent.window.toolbarManuallyHidden_,
            L"independent toolbar toggle cannot restore the first fixture");
        ShowWindow(window.window_, SW_HIDE);
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
        window.setParent_ = FailOneReparent;
        reparentCalls = 0;
        failReparentCall = static_cast<int>(Controls(window).size());
        window.EnterFullscreen();
        checks.Expect(!window.fullscreen_ && AllParents(window, window.controlBar_),
            L"late entry failure also rolls back multi-cut controls and count label");
        window.setParent_ = ::SetParent;
        window.EnterFullscreen();
        window.setParent_ = FailOneReparent;
        reparentCalls = 0;
        failReparentCall = static_cast<int>(Controls(window).size());
        window.ExitFullscreen();
        checks.Expect(window.fullscreen_ && AllParents(window, window.fullscreenControlBar_),
            L"late exit failure keeps multi-cut controls and count label in fullscreen host");
        window.setParent_ = ::SetParent;
        window.ExitFullscreen();
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

    static void PlaybackLoopControls(Checks& checks) {
        Fixture fixture;
        Fixture independent;
        checks.Expect(fixture.Create() && independent.Create(),
            L"create playback-loop control fixtures");
        auto& window = fixture.window;
        auto& other = independent.window;
        if (window.window_ == nullptr || other.window_ == nullptr) return;

        const auto click = [](const HWND button) {
            SendMessageW(button, BM_CLICK, 0, 0);
        };
        const auto checked = [](const HWND button) {
            return SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED;
        };
        const LONG_PTR wholeStyle = GetWindowLongPtrW(window.loopVideoButton_, GWL_STYLE);
        const LONG_PTR segmentStyle = GetWindowLongPtrW(window.loopSegmentButton_, GWL_STYLE);
        const LONG_PTR seekStyle = GetWindowLongPtrW(window.seekSlider_, GWL_STYLE);
        checks.Expect((wholeStyle & BS_TYPEMASK) == BS_CHECKBOX &&
            (segmentStyle & BS_TYPEMASK) == BS_CHECKBOX &&
            (wholeStyle & BS_PUSHLIKE) != 0 && (segmentStyle & BS_PUSHLIKE) != 0 &&
            (seekStyle & TBS_ENABLESELRANGE) != 0 &&
            IsWindowEnabled(window.loopVideoButton_) != FALSE &&
            IsWindowEnabled(window.loopStartButton_) != FALSE &&
            IsWindowEnabled(window.loopEndButton_) == FALSE &&
            IsWindowEnabled(window.loopSegmentButton_) == FALSE &&
            IsWindowEnabled(window.loopClearButton_) == FALSE,
            L"loop controls expose push-like toggles, range trackbar and valid initial actions");

        fixture.backend.positionMs = 2000;
        window.RefreshControls();
        click(window.loopStartButton_);
        fixture.backend.positionMs = 2499;
        window.RefreshControls();
        const bool invalidBDisabled = IsWindowEnabled(window.loopEndButton_) == FALSE;
        click(window.loopEndButton_);
        checks.Expect(window.playbackLoop_.StartMs() == 2000 &&
            window.playbackLoop_.EndMs() == -1 &&
            window.playbackLoop_.Mode() == LoopMode::Off && invalidBDisabled,
            L"actual A stores 2000 ms and disabled B rejects a sub-500 ms segment");

        fixture.backend.positionMs = 6000;
        window.RefreshControls();
        const bool validBEnabled = IsWindowEnabled(window.loopEndButton_) != FALSE;
        const int seeksBeforeB = fixture.backend.seekCalls;
        click(window.loopEndButton_);
        std::array<wchar_t, 80> rangeLabel{};
        GetWindowTextW(
            window.loopRangeLabel_, rangeLabel.data(), static_cast<int>(rangeLabel.size()));
        checks.Expect(window.playbackLoop_.StartMs() == 2000 &&
            window.playbackLoop_.EndMs() == 6000 &&
            window.playbackLoop_.Mode() == LoopMode::Segment &&
            validBEnabled &&
            fixture.backend.seekCalls == seeksBeforeB + 1 &&
            fixture.backend.positionMs == 2000,
            L"actual B button completes A-B and immediately returns an active boundary to A");
        checks.Expect(IsWindowEnabled(window.loopSegmentButton_) != FALSE &&
            checked(window.loopSegmentButton_) && !checked(window.loopVideoButton_) &&
            SendMessageW(window.seekSlider_, TBM_GETSELSTART, 0, 0) ==
                MainWindow::TimeToSlider(2000, fixture.backend.durationMs) &&
            SendMessageW(window.seekSlider_, TBM_GETSELEND, 0, 0) ==
                MainWindow::TimeToSlider(6000, fixture.backend.durationMs) &&
            std::wstring(rangeLabel.data()) == L"A: 00:02.000   B: 00:06.000",
            L"completed segment updates checks, selected range and accessible time label");
        checks.Expect(other.playbackLoop_.Mode() == LoopMode::Off &&
            other.playbackLoop_.StartMs() == -1 && other.playbackLoop_.EndMs() == -1 &&
            !checked(other.loopVideoButton_) && !checked(other.loopSegmentButton_),
            L"loop configuration and button state stay independent per player instance");

        window.RefreshControls();
        const int seeksBeforeBoundary = fixture.backend.seekCalls;
        fixture.backend.positionMs = 5999;
        window.RefreshControls();
        const bool stayedBeforeBoundary = fixture.backend.seekCalls == seeksBeforeBoundary &&
            fixture.backend.positionMs == 5999;
        fixture.backend.positionMs = 6000;
        window.RefreshControls();
        const int seeksAfterBoundary = fixture.backend.seekCalls;
        const bool crossedToA = seeksAfterBoundary == seeksBeforeBoundary + 1 &&
            fixture.backend.positionMs == 2000;
        window.RefreshControls();
        checks.Expect(stayedBeforeBoundary && crossedToA &&
            fixture.backend.seekCalls == seeksAfterBoundary,
            L"exclusive B boundary seeks once to A and its acknowledgement does not repeat");

        click(window.playButton_);
        fixture.backend.positionMs = 6000;
        const int pausedSeeks = fixture.backend.seekCalls;
        window.RefreshControls();
        const bool pauseSuppressed = fixture.backend.backendState == 4 &&
            fixture.backend.seekCalls == pausedSeeks &&
            window.playbackLoop_.Mode() == LoopMode::Segment;
        click(window.playButton_);
        checks.Expect(pauseSuppressed && fixture.backend.backendState == 3 &&
            fixture.backend.seekCalls == pausedSeeks + 1 &&
            fixture.backend.positionMs == 2000,
            L"actual Pause suppresses A-B while resume applies the retained boundary once");
        window.RefreshControls();
        fixture.backend.positionMs = 6000;
        window.seekDragging_ = true;
        const int dragSeeks = fixture.backend.seekCalls;
        window.RefreshControls();
        const bool dragSuppressed = fixture.backend.seekCalls == dragSeeks;
        window.seekDragging_ = false;
        window.RefreshControls();
        checks.Expect(dragSuppressed && fixture.backend.seekCalls == dragSeeks + 1 &&
            fixture.backend.positionMs == 2000,
            L"seek dragging suppresses A-B until the drag ends, then seeks once");
        window.RefreshControls();

        const VideoCrop crop{80, 40, 320, 180};
        const std::uint32_t generation = window.player_.Generation();
        const bool zoomApplied = window.ApplyAreaZoom(crop);
        window.SetToolbarManuallyHidden(true);
        window.EnterFullscreen();
        const bool preservedInFullscreen = window.playbackLoop_.Mode() == LoopMode::Segment &&
            window.playbackLoop_.StartMs() == 2000 &&
            window.playbackLoop_.EndMs() == 6000 &&
            window.toolbarManuallyHidden_ && window.fullscreen_ &&
            AllParents(window, window.fullscreenControlBar_) && HasCrop(window, crop);
        window.ExitFullscreen();
        window.SetToolbarManuallyHidden(false);
        checks.Expect(zoomApplied && preservedInFullscreen &&
            window.playbackLoop_.Mode() == LoopMode::Segment &&
            window.player_.Generation() == generation &&
            AllParents(window, window.controlBar_) && checked(window.loopSegmentButton_),
            L"zoom, fullscreen and compact roundtrip preserve A-B and restore its controls");

        click(window.loopVideoButton_);
        const bool wholeWasExclusive = window.playbackLoop_.Mode() == LoopMode::WholeVideo &&
            checked(window.loopVideoButton_) && !checked(window.loopSegmentButton_);
        click(window.loopSegmentButton_);
        checks.Expect(wholeWasExclusive && window.playbackLoop_.Mode() == LoopMode::Segment &&
            !checked(window.loopVideoButton_) && checked(window.loopSegmentButton_),
            L"whole-video and segment push-like toggles remain mutually exclusive");
        fixture.backend.positionMs = 4000;
        window.RefreshControls();
        click(window.loopStartButton_);
        const bool newAResetSegment = window.playbackLoop_.StartMs() == 4000 &&
            window.playbackLoop_.EndMs() == -1 &&
            window.playbackLoop_.Mode() == LoopMode::Off &&
            SendMessageW(window.seekSlider_, TBM_GETSELSTART, 0, 0) == -1 &&
            SendMessageW(window.seekSlider_, TBM_GETSELEND, 0, 0) == -1;
        click(window.loopClearButton_);
        checks.Expect(newAResetSegment && window.playbackLoop_.StartMs() == -1 &&
            window.playbackLoop_.EndMs() == -1 &&
            IsWindowEnabled(window.loopClearButton_) == FALSE,
            L"new A clears B/mode/range and actual Clear removes the remaining marker");

        window.hasMedia_ = false;
        window.ResetMediaUiState();
        click(window.loopVideoButton_);
        click(window.loopStartButton_);
        checks.Expect(window.playbackLoop_.Mode() == LoopMode::Off &&
            window.playbackLoop_.StartMs() == -1 &&
            IsWindowEnabled(window.loopVideoButton_) == FALSE &&
            IsWindowEnabled(window.loopStartButton_) == FALSE,
            L"no-media UI disables loop actions and cannot retain configuration");

        independent.backend.positionMs = 2000;
        other.RefreshControls();
        click(other.loopStartButton_);
        independent.backend.positionMs = 6000;
        other.RefreshControls();
        click(other.loopEndButton_);
        other.RefreshControls();
        const std::uint32_t oldGeneration = other.player_.Generation();
        independent.backend.Install(other.player_);
        independent.backend.backendState = 3;
        independent.backend.positionMs = 1000;
        other.RefreshControls();
        checks.Expect(other.player_.Generation() != oldGeneration &&
            other.playbackLoop_.Mode() == LoopMode::Off &&
            other.playbackLoop_.StartMs() == -1 && other.playbackLoop_.EndMs() == -1 &&
            SendMessageW(other.seekSlider_, TBM_GETSELSTART, 0, 0) == -1 &&
            SendMessageW(other.seekSlider_, TBM_GETSELEND, 0, 0) == -1,
            L"new media generation clears loop mode, endpoints and selected range");
    }

    static void RemovalIntervalControls(Checks& checks) {
        Fixture fixture;
        Fixture independent;
        checks.Expect(fixture.Create() && independent.Create(),
            L"create multi-cut and independent control fixtures");
        auto& window = fixture.window;
        auto& other = independent.window;
        if (window.window_ == nullptr || other.window_ == nullptr) return;

        const auto click = [](const HWND button) {
            SendMessageW(button, BM_CLICK, 0, 0);
        };
        const auto select = [&](const std::int64_t start, const std::int64_t end) {
            fixture.backend.positionMs = start;
            window.RefreshControls();
            click(window.loopStartButton_);
            fixture.backend.positionMs = end;
            window.RefreshControls();
            click(window.loopEndButton_);
            window.RefreshControls();
        };
        const auto intervalIs = [&](const std::size_t index,
                                   const std::int64_t start,
                                   const std::int64_t end) {
            return index < window.removalIntervals_.size() &&
                window.removalIntervals_[index].startMs == start &&
                window.removalIntervals_[index].endMs == end;
        };
        const std::wstring emptyCount = ControlText(window.removalCountLabel_);
        checks.Expect(GetDlgCtrlID(window.addRemovalButton_) == IDC_ADD_REMOVAL_BUTTON &&
            GetDlgCtrlID(window.removalCountLabel_) == IDC_REMOVAL_COUNT_LABEL &&
            (GetWindowLongPtrW(window.addRemovalButton_, GWL_STYLE) & WS_TABSTOP) != 0 &&
            !emptyCount.empty() && window.removalIntervals_.empty() &&
            IsWindowEnabled(window.addRemovalButton_) == FALSE &&
            IsWindowEnabled(window.removeSegmentButton_) == FALSE,
            L"multi-cut controls have distinct IDs and require a valid initial selection");

        select(8000, 10000);
        checks.Expect(IsWindowEnabled(window.addRemovalButton_) != FALSE &&
            IsWindowEnabled(window.removeSegmentButton_) != FALSE,
            L"completed A-B enables adding and one-shot removal");
        const auto generation = window.player_.Generation();
        const int seeksBeforeAdd = fixture.backend.seekCalls;
        click(window.addRemovalButton_);
        checks.Expect(window.removalIntervals_.size() == 1 && intervalIs(0, 8000, 10000) &&
            window.playbackLoop_.StartMs() == -1 && window.playbackLoop_.EndMs() == -1 &&
            window.playbackLoop_.Mode() == LoopMode::Off &&
            IsWindowEnabled(window.addRemovalButton_) == FALSE &&
            IsWindowEnabled(window.removeSegmentButton_) != FALSE &&
            SendMessageW(window.seekSlider_, TBM_GETSELSTART, 0, 0) == -1 &&
            SendMessageW(window.seekSlider_, TBM_GETSELEND, 0, 0) == -1 &&
            fixture.backend.seekCalls == seeksBeforeAdd &&
            window.player_.Generation() == generation,
            L"actual Add queues A-B and clears looping/range without seek or media replacement");
        checks.Expect(ControlText(window.removalCountLabel_) != emptyCount &&
            ControlText(window.removalCountLabel_).find(L"1") != std::wstring::npos,
            L"queued interval updates the accessible count label");

        fixture.backend.positionMs = 15000;
        const int unrestrictedSeeks = fixture.backend.seekCalls;
        window.RefreshControls();
        checks.Expect(fixture.backend.positionMs == 15000 &&
            fixture.backend.seekCalls == unrestrictedSeeks,
            L"queued cut does not constrain playback while selecting the next interval");
        select(2000, 4000);
        click(window.addRemovalButton_);
        checks.Expect(window.removalIntervals_.size() == 2 && intervalIs(0, 2000, 4000) &&
            intervalIs(1, 8000, 10000) &&
            ControlText(window.removalCountLabel_).find(L"2") != std::wstring::npos,
            L"out-of-order selections are retained in timeline order with updated count");
        select(8000, 10000);
        window.AddRemovalInterval();
        checks.Expect(window.removalIntervals_.size() == 2 && intervalIs(0, 2000, 4000) &&
            intervalIs(1, 8000, 10000) && window.playbackLoop_.StartMs() == 8000 &&
            window.playbackLoop_.EndMs() == -1 &&
            IsWindowEnabled(window.addRemovalButton_) == FALSE,
            L"saved-boundary A is allowed but duplicate B is rejected without changing queued cuts");

        select(12000, 14000);
        click(window.loopClearButton_);
        checks.Expect(window.removalIntervals_.size() == 2 && intervalIs(0, 2000, 4000) &&
            intervalIs(1, 8000, 10000) && window.playbackLoop_.StartMs() == -1 &&
            window.playbackLoop_.EndMs() == -1 &&
            IsWindowEnabled(window.removeSegmentButton_) != FALSE,
            L"Clear A-B cancels only the current markers and preserves queued deletion cuts");
        window.AddRemovalInterval();
        checks.Expect(window.removalIntervals_.size() == 2 &&
            other.removalIntervals_.empty() &&
            ControlText(other.removalCountLabel_) == emptyCount &&
            IsWindowEnabled(other.removeSegmentButton_) == FALSE,
            L"no-selection Add is harmless and queued cuts stay local to their player instance");

        select(12000, 14000);
        const bool activeDialogSet = SetPropW(window.window_,
            L"VideoPlayer.ActiveExportDialog", reinterpret_cast<HANDLE>(window.window_)) != FALSE;
        window.RefreshControls();
        window.AddRemovalInterval();
        const bool editingBlocked = activeDialogSet &&
            window.removalIntervals_.size() == 2 && intervalIs(0, 2000, 4000) &&
            intervalIs(1, 8000, 10000) && window.playbackLoop_.StartMs() == 12000 &&
            window.playbackLoop_.EndMs() == 14000 &&
            IsWindowEnabled(window.addRemovalButton_) == FALSE &&
            IsWindowEnabled(window.removeSegmentButton_) == FALSE;
        RemovePropW(window.window_, L"VideoPlayer.ActiveExportDialog");
        window.RefreshControls();
        checks.Expect(editingBlocked && IsWindowEnabled(window.addRemovalButton_) != FALSE &&
            IsWindowEnabled(window.removeSegmentButton_) != FALSE,
            L"active export dialog blocks queued-cut edits and restores actions after dismissal");
        click(window.loopClearButton_);

        window.EnterFullscreen();
        const bool fullscreenQueue = window.fullscreen_ &&
            AllParents(window, window.fullscreenControlBar_) &&
            window.removalIntervals_.size() == 2 && intervalIs(0, 2000, 4000) &&
            intervalIs(1, 8000, 10000);
        window.ExitFullscreen();
        checks.Expect(fullscreenQueue && !window.fullscreen_ &&
            AllParents(window, window.controlBar_) && window.removalIntervals_.size() == 2 &&
            IsWindowEnabled(window.removeSegmentButton_) != FALSE,
            L"fullscreen roundtrip reparents all cut controls while preserving queued intervals");

        window.hasMedia_ = false;
        window.ResetMediaUiState();
        window.RefreshControls();
        window.AddRemovalInterval();
        checks.Expect(window.removalIntervals_.empty() &&
            ControlText(window.removalCountLabel_) == emptyCount &&
            IsWindowEnabled(window.addRemovalButton_) == FALSE &&
            IsWindowEnabled(window.removeSegmentButton_) == FALSE,
            L"new-media reset clears queued cuts/count and no-media Add cannot recreate them");
    }

    static void RemovalOverlapGuards(Checks& checks) {
        Fixture fixture;
        Fixture independent;
        fixture.backend.backendState = independent.backend.backendState = 4; // paused
        checks.Expect(fixture.Create() && independent.Create(),
            L"create saved-interval overlap fixtures");
        auto& window = fixture.window;
        auto& other = independent.window;
        if (window.window_ == nullptr || other.window_ == nullptr) return;
        window.removalIntervals_ = {{8000, 10000}};
        window.RefreshControls();
        const auto click = [](const HWND button) {
            SendMessageW(button, BM_CLICK, 0, 0);
        };
        const auto select = [&](const std::int64_t start, const std::int64_t end) {
            fixture.backend.positionMs = start;
            window.RefreshControls();
            click(window.loopStartButton_);
            fixture.backend.positionMs = end;
            window.RefreshControls();
            click(window.loopEndButton_);
            window.RefreshControls();
        };
        select(2000, 4000);
        const int seeksBeforeRejections = fixture.backend.seekCalls;
        const auto originalSelectionPreserved = [&] {
            return window.playbackLoop_.StartMs() == 2000 &&
                window.playbackLoop_.EndMs() == 4000 &&
                window.playbackLoop_.Mode() == LoopMode::Segment &&
                fixture.backend.seekCalls == seeksBeforeRejections &&
                SendMessageW(window.seekSlider_, TBM_GETSELSTART, 0, 0) ==
                    MainWindow::TimeToSlider(2000, fixture.backend.durationMs) &&
                SendMessageW(window.seekSlider_, TBM_GETSELEND, 0, 0) ==
                    MainWindow::TimeToSlider(4000, fixture.backend.durationMs);
        };
        fixture.backend.positionMs = 9000;
        window.RefreshControls();
        const bool aDisabled = IsWindowEnabled(window.loopStartButton_) == FALSE;
        click(window.loopStartButton_);
        const bool aButtonPreserved = originalSelectionPreserved();
        const bool aKeyHandled = window.HandleHotKey(VK_OEM_4, false, false);
        const bool aKeyPreserved = originalSelectionPreserved();
        window.SetLoopPoint(true);
        checks.Expect(aDisabled && aButtonPreserved && aKeyHandled && aKeyPreserved &&
            originalSelectionPreserved(),
            L"saved-interior A is blocked by HWND button, keyboard and direct setter without mutation");
        const bool bDisabled = IsWindowEnabled(window.loopEndButton_) == FALSE;
        click(window.loopEndButton_);
        const bool bButtonPreserved = originalSelectionPreserved();
        const bool bKeyHandled = window.HandleHotKey(VK_OEM_6, false, false);
        const bool bKeyPreserved = originalSelectionPreserved();
        window.SetLoopPoint(false);
        checks.Expect(bDisabled && bButtonPreserved && bKeyHandled && bKeyPreserved &&
            originalSelectionPreserved(),
            L"saved-interior B preserves the existing A-B, loop mode and selected slider range");

        select(6000, 8000);
        const bool leftAdjacent = window.playbackLoop_.StartMs() == 6000 &&
            window.playbackLoop_.EndMs() == 8000 &&
            IsWindowEnabled(window.addRemovalButton_) != FALSE;
        click(window.addRemovalButton_);
        select(10000, 12000);
        const bool rightAdjacent = window.playbackLoop_.StartMs() == 10000 &&
            window.playbackLoop_.EndMs() == 12000 &&
            IsWindowEnabled(window.addRemovalButton_) != FALSE;
        click(window.addRemovalButton_);
        checks.Expect(leftAdjacent && rightAdjacent && window.removalIntervals_.size() == 3 &&
            window.removalIntervals_[0].startMs == 6000 &&
            window.removalIntervals_[0].endMs == 8000 &&
            window.removalIntervals_[1].startMs == 8000 &&
            window.removalIntervals_[1].endMs == 10000 &&
            window.removalIntervals_[2].startMs == 10000 &&
            window.removalIntervals_[2].endMs == 12000,
            L"touching endpoints permit adjacent A-B selections and preserve all three queued cuts");

        window.removalIntervals_ = {{8000, 10000}};
        fixture.backend.positionMs = 6000;
        window.RefreshControls();
        click(window.loopStartButton_);
        fixture.backend.positionMs = 12000;
        window.RefreshControls();
        const bool crossingBDisabled = IsWindowEnabled(window.loopEndButton_) == FALSE;
        click(window.loopEndButton_);
        const bool crossingKeyHandled = window.HandleHotKey(VK_OEM_6, false, false);
        window.SetLoopPoint(false);
        checks.Expect(crossingBDisabled && crossingKeyHandled &&
            window.playbackLoop_.StartMs() == 6000 && window.playbackLoop_.EndMs() == -1 &&
            window.playbackLoop_.Mode() == LoopMode::Off,
            L"B outside saved cuts still rejects a candidate spanning a saved interval");

        const std::array<VideoInterval, 5> conflictingCandidates{{
            {6000, 9000}, {9000, 12000}, {6000, 12000},
            {8500, 9500}, {8000, 10000}}};
        bool defensiveGuards = true;
        for (const auto& candidate : conflictingCandidates) {
            window.playbackLoop_.SetStart(candidate.startMs, fixture.backend.durationMs);
            window.playbackLoop_.SetEnd(candidate.endMs, fixture.backend.durationMs);
            window.RefreshControls();
            defensiveGuards = defensiveGuards &&
                IsWindowEnabled(window.addRemovalButton_) == FALSE &&
                IsWindowEnabled(window.removeSegmentButton_) == FALSE;
            window.AddRemovalInterval();
            window.ShowRemoveSegmentVideoDialog();
            defensiveGuards = defensiveGuards &&
                window.playbackLoop_.StartMs() == candidate.startMs &&
                window.playbackLoop_.EndMs() == candidate.endMs &&
                window.playbackLoop_.Mode() == LoopMode::Segment &&
                window.removalIntervals_.size() == 1 &&
                window.removalIntervals_[0].startMs == 8000 &&
                window.removalIntervals_[0].endMs == 10000 &&
                !HasActiveExportDialog(window.window_);
        }
        checks.Expect(defensiveGuards,
            L"stale partial, containing, contained and duplicate A-B cannot Add or open one-shot removal");
        click(window.loopClearButton_);
        checks.Expect(window.playbackLoop_.StartMs() == -1 &&
            window.playbackLoop_.EndMs() == -1 && window.removalIntervals_.size() == 1 &&
            IsWindowEnabled(window.removeSegmentButton_) != FALSE,
            L"clearing rejected current A-B restores removal of the valid saved queue");

        independent.backend.positionMs = 9000;
        other.RefreshControls();
        const bool otherAEnabled = IsWindowEnabled(other.loopStartButton_) != FALSE;
        const bool otherAHandled = other.HandleHotKey(VK_OEM_4, false, false);
        independent.backend.positionMs = 11000;
        other.RefreshControls();
        const bool otherBEnabled = IsWindowEnabled(other.loopEndButton_) != FALSE;
        const bool otherBHandled = other.HandleHotKey(VK_OEM_6, false, false);
        checks.Expect(otherAEnabled && otherBEnabled && otherAHandled && otherBHandled &&
            other.playbackLoop_.StartMs() == 9000 && other.playbackLoop_.EndMs() == 11000 &&
            other.removalIntervals_.empty() && window.removalIntervals_.size() == 1,
            L"saved cuts in one player do not block keyboard selection in another instance");
    }

    static void TimelineEditing(Checks& checks) {
        Fixture fixture;
        fixture.backend.backendState = 4;
        checks.Expect(fixture.Create(), L"create editable A-B timeline fixture");
        auto& window = fixture.window;
        if (window.window_ == nullptr) return;
        fixture.backend.positionMs = 4000;
        window.SetLoopPoint(true);
        checks.Expect(window.playbackLoop_.StartMs() == 4000 && window.playbackLoop_.EndMs() == -1 &&
            window.timelineStartMs_ == 4000 && window.removalIntervals_.empty(),
            L"standalone A immediately invalidates timeline before B or saving");
        fixture.backend.positionMs = 7000;
        window.SetLoopPoint(false);
        window.ToggleSegmentLoop();
        checks.Expect(window.playbackLoop_.HasSegment(20000) && window.timelineEndMs_ == 7000 &&
            window.removalIntervals_.empty(), L"unsaved A-B exists on timeline without queue import");
        const auto timeline = window.SeekTimelineRect();
        const auto x = [&](const std::int64_t time) { return IntervalTimelineX(time, 20000, timeline); };
        const auto point = [&](const LONG px) { return MAKELPARAM(static_cast<WORD>(px), static_cast<WORD>((std::max)(0L, timeline.top - 1))); };
        const int originalSeeks = fixture.backend.seekCalls;
        SendMessageW(window.seekSlider_, WM_LBUTTONDOWN, MK_LBUTTON, point(x(4000)));
        checks.Expect(window.markerDragging_ && !window.seekDragging_ && GetCapture() == window.seekSlider_,
            L"actual marker flag starts its own captured drag instead of seeking");
        SendMessageW(window.seekSlider_, WM_MOUSEMOVE, MK_LBUTTON, point(x(5000)));
        const auto movedA = PreviewTimeFromChannelX(x(5000), timeline, 20000);
        checks.Expect(window.playbackLoop_.StartMs() == movedA && window.playbackLoop_.EndMs() == 7000 &&
            fixture.backend.seekCalls == originalSeeks, L"dragging A preserves B and does not seek playback");
        SendMessageW(window.seekSlider_, WM_LBUTTONUP, 0, point(x(5000)));
        checks.Expect(!window.markerDragging_ && GetCapture() != window.seekSlider_ &&
            window.playbackLoop_.Mode() == LoopMode::Off, L"committed marker drag preserves disabled loop mode");
        SendMessageW(window.seekSlider_, WM_LBUTTONDOWN, MK_LBUTTON, point(x(movedA)));
        SendMessageW(window.seekSlider_, WM_MOUSEMOVE, MK_LBUTTON, point(x(7000)));
        checks.Expect(window.playbackLoop_.StartMs() == movedA && window.playbackLoop_.EndMs() == 7000,
            L"A cannot cross B or collapse the half-second minimum interval");
        SendMessageW(window.seekSlider_, WM_MOUSEMOVE, MK_LBUTTON, point(x(6000)));
        SendMessageW(window.seekSlider_, WM_CANCELMODE, 0, 0);
        checks.Expect(!window.markerDragging_ && window.playbackLoop_.StartMs() == movedA &&
            window.playbackLoop_.EndMs() == 7000, L"cancelled marker capture restores the original current range");

        window.removalIntervals_ = {{8000, 10000}, {14000, 16000}};
        window.RefreshControls();
        checks.Expect(SendMessageW(window.savedIntervalList_, CB_GETCOUNT, 0, 0) == 2 &&
            IsWindowEnabled(window.removeSavedIntervalButton_), L"saved ranges appear in editable toolbar list");
        SendMessageW(window.seekSlider_, WM_LBUTTONDOWN, MK_LBUTTON, point(x(10000)));
        SendMessageW(window.seekSlider_, WM_MOUSEMOVE, MK_LBUTTON, point(x(12000)));
        const auto movedB = PreviewTimeFromChannelX(x(12000), timeline, 20000);
        SendMessageW(window.seekSlider_, WM_LBUTTONUP, 0, point(x(12000)));
        checks.Expect(window.removalIntervals_[0].startMs == 8000 && window.removalIntervals_[0].endMs == movedB &&
            window.playbackLoop_.StartMs() == movedA && fixture.backend.seekCalls == originalSeeks,
            L"saved B is draggable and current A-B remains independent");
        SendMessageW(window.seekSlider_, WM_LBUTTONDOWN, MK_LBUTTON, point(x(movedB)));
        SendMessageW(window.seekSlider_, WM_MOUSEMOVE, MK_LBUTTON, point(x(15000)));
        checks.Expect(window.removalIntervals_[0].endMs == movedB,
            L"saved marker drag cannot overlap another saved range");
        SendMessageW(window.seekSlider_, WM_LBUTTONUP, 0, point(x(15000)));
        SendMessageW(window.seekSlider_, WM_LBUTTONDOWN, MK_LBUTTON, point(x(8000)));
        SendMessageW(window.seekSlider_, WM_MOUSEMOVE, MK_LBUTTON, point(x(6000)));
        checks.Expect(window.removalIntervals_[0].startMs == 8000,
            L"saved range cannot expand across the current unsaved A-B");
        SendMessageW(window.seekSlider_, WM_MOUSEMOVE, MK_LBUTTON, point(x(9000)));
        window.HandleHotKey(VK_ESCAPE, false, false);
        checks.Expect(!window.markerDragging_ && window.removalIntervals_[0].startMs == 8000,
            L"Escape rolls back an edited saved endpoint");
        SendMessageW(window.savedIntervalList_, CB_SETCURSEL, 1, 0);
        SendMessageW(window.removeSavedIntervalButton_, BM_CLICK, 0, 0);
        checks.Expect(window.removalIntervals_.size() == 1 && window.removalIntervals_[0].startMs == 8000 &&
            SendMessageW(window.savedIntervalList_, CB_GETCOUNT, 0, 0) == 1 &&
            window.playbackLoop_.StartMs() == movedA && fixture.backend.seekCalls == originalSeeks,
            L"remove-from-list button removes only the selected saved range without seeking or clearing draft");
        window.RemoveSavedInterval();
        checks.Expect(window.removalIntervals_.empty() && !IsWindowEnabled(window.removeSavedIntervalButton_),
            L"removing the last saved range disables its list controls");
        const LPARAM channelClick = MAKELPARAM(static_cast<WORD>(x(movedA)), static_cast<WORD>(timeline.bottom + 2));
        SendMessageW(window.seekSlider_, WM_LBUTTONDOWN, MK_LBUTTON, channelClick);
        checks.Expect(window.seekDragging_ && !window.markerDragging_,
            L"clicking channel at the A timestamp still starts ordinary seek");
        SendMessageW(window.seekSlider_, WM_LBUTTONUP, 0, channelClick);
        checks.Expect(fixture.backend.seekCalls > originalSeeks, L"ordinary channel click still commits a seek");
        window.EnterFullscreen();
        checks.Expect(GetParent(window.savedIntervalList_) == window.fullscreenControlBar_ &&
            GetParent(window.removeSavedIntervalButton_) == window.fullscreenControlBar_,
            L"new saved interval controls follow fullscreen toolbar parent");
        const auto full = window.SeekTimelineRect();
        const auto fullA = IntervalTimelineX(window.playbackLoop_.StartMs(), 20000, full);
        checks.Expect(window.BeginMarkerDrag(fullA), L"marker dragging survives fullscreen resize");
        window.ExitFullscreen();
        checks.Expect(!window.markerDragging_ && GetParent(window.savedIntervalList_) == window.controlBar_,
            L"fullscreen transition cancels marker capture and returns list to docked toolbar");
        checks.Expect(window.BeginMarkerDrag(x(window.playbackLoop_.StartMs())), L"begin marker before media reset");
        window.ResetMediaUiState();
        checks.Expect(!window.markerDragging_ && window.playbackLoop_.StartMs() == -1 &&
            window.removalIntervals_.empty(), L"media reset cancels markers and clears both active and saved ranges");
    }

    static void SavedIntervalPainting(Checks& checks) {
        Fixture fixture;
        fixture.backend.backendState = 4; // paused
        checks.Expect(fixture.Create(), L"create saved-interval pixel fixture");
        auto& window = fixture.window;
        if (window.window_ == nullptr) return;
        struct Pixels final {
            HDC dc = CreateCompatibleDC(nullptr);
            HBITMAP bitmap = nullptr;
            HGDIOBJ previous = nullptr;
            Pixels(const int width, const int height) {
                if (dc == nullptr) return;
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = width;
                info.bmiHeader.biHeight = -height;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB;
                void* bits = nullptr;
                bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
                if (bitmap != nullptr) previous = SelectObject(dc, bitmap);
            }
            ~Pixels() {
                if (previous != nullptr && previous != HGDI_ERROR) SelectObject(dc, previous);
                if (bitmap != nullptr) DeleteObject(bitmap);
                if (dc != nullptr) DeleteDC(dc);
            }
            bool Valid() const {
                return dc != nullptr && bitmap != nullptr &&
                    previous != nullptr && previous != HGDI_ERROR;
            }
            void Fill(const RECT& bounds, const COLORREF color) const {
                SetDCBrushColor(dc, color);
                FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            }
        };
        HIGHCONTRASTW highContrast{};
        highContrast.cbSize = sizeof(highContrast);
        const bool highContrastOn = SystemParametersInfoW(
            SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0) != FALSE &&
            (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
        const COLORREF savedColor = highContrastOn
            ? GetSysColor(COLOR_HIGHLIGHT) : RGB(255, 153, 0);
        constexpr COLORREF untouchedColor = RGB(21, 27, 31);
        const auto sampleX = [](const RECT& bounds, const int percent) {
            return bounds.left + (bounds.right - bounds.left) * percent / 100;
        };
        {
            const RECT range{0, 0, 1000, 8};
            Pixels draft(1000, 8);
            checks.Expect(draft.Valid(), L"allocate draft A-B timeline pixels");
            if (draft.Valid()) {
                const COLORREF active = highContrastOn ? GetSysColor(COLOR_HIGHLIGHT) : RGB(0, 160, 230);
                window.playbackLoop_.SetStart(4000, 20000);
                window.RefreshControls();
                draft.Fill(range, untouchedColor);
                window.DrawCurrentInterval(draft.dc, range);
                window.DrawTimelineMarkers(draft.dc, range, false);
                checks.Expect(GetPixel(draft.dc, 200, 4) == active && GetPixel(draft.dc, 250, 4) == untouchedColor,
                    L"standalone A is visible before choosing B and before saving");
                window.playbackLoop_.SetEnd(7000, 20000);
                draft.Fill(range, untouchedColor);
                window.DrawCurrentInterval(draft.dc, range);
                checks.Expect(GetPixel(draft.dc, 250, 4) == active && window.removalIntervals_.empty(),
                    L"complete unsaved A-B paints its full interval on timeline");
                window.playbackLoop_.ClearSegment();
            }
        }
        window.removalIntervals_ = {{2000, 4000}, {8000, 10000}};
        fixture.backend.positionMs = 6000;
        window.RefreshControls();
        const auto channelPaintMatches = [&](const HWND parent, const bool hasSaved) {
            RECT client{}, channel{}, thumb{};
            GetClientRect(window.seekSlider_, &client);
            SendMessageW(window.seekSlider_, TBM_GETCHANNELRECT, 0,
                reinterpret_cast<LPARAM>(&channel));
            SendMessageW(window.seekSlider_, TBM_GETTHUMBRECT, 0,
                reinterpret_cast<LPARAM>(&thumb));
            RECT timeline = TrackbarPointerRange(channel, thumb);
            if (timeline.bottom - timeline.top > 2) {
                ++timeline.top;
                --timeline.bottom;
            }
            if (client.right <= 0 || client.bottom <= 0 ||
                timeline.right <= timeline.left || timeline.bottom <= timeline.top) return false;
            Pixels surface(client.right, client.bottom);
            if (!surface.Valid()) return false;
            surface.Fill(client, untouchedColor);
            NMCUSTOMDRAW draw{};
            draw.hdr.hwndFrom = window.seekSlider_;
            draw.hdr.idFrom = IDC_SEEK_SLIDER;
            draw.hdr.code = NM_CUSTOMDRAW;
            draw.hdc = surface.dc;
            draw.dwDrawStage = CDDS_PREPAINT;
            const bool stages = SendMessageW(parent, WM_NOTIFY, IDC_SEEK_SLIDER,
                reinterpret_cast<LPARAM>(&draw)) == (CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT);
            draw.dwItemSpec = TBCD_CHANNEL;
            draw.dwDrawStage = CDDS_ITEMPREPAINT;
            const bool postRequested = SendMessageW(parent, WM_NOTIFY, IDC_SEEK_SLIDER,
                reinterpret_cast<LPARAM>(&draw)) == CDRF_NOTIFYPOSTPAINT;
            draw.dwDrawStage = CDDS_ITEMPOSTPAINT;
            SendMessageW(parent, WM_NOTIFY, IDC_SEEK_SLIDER, reinterpret_cast<LPARAM>(&draw));
            const int row = timeline.top + (timeline.bottom - timeline.top) / 2;
            const COLORREF expected = hasSaved ? savedColor : untouchedColor;
            const bool paintMatches =
                GetPixel(surface.dc, sampleX(timeline, 15), row) == expected &&
                GetPixel(surface.dc, sampleX(timeline, 45), row) == expected &&
                GetPixel(surface.dc, sampleX(timeline, 30), row) == untouchedColor &&
                GetPixel(surface.dc, sampleX(timeline, 75), row) == untouchedColor;
            return stages && postRequested && paintMatches &&
                GetDCBrushColor(surface.dc) == untouchedColor;
        };
        checks.Expect(channelPaintMatches(window.controlBar_, true),
            L"docked native channel customdraw paints two saved intervals while preserving gaps and DC state");
        RECT originalBounds{}, originalSeek{}, scaledSeek{};
        GetWindowRect(window.window_, &originalBounds);
        GetClientRect(window.seekSlider_, &originalSeek);
        const int originalDpi = window.dpi_;
        const int scaledDpi = originalDpi * 3 / 2;
        const RECT suggested{originalBounds.left, originalBounds.top,
            originalBounds.left + (originalBounds.right - originalBounds.left) * 3 / 2,
            originalBounds.top + (originalBounds.bottom - originalBounds.top) * 3 / 2};
        SendMessageW(window.window_, WM_DPICHANGED, MAKEWPARAM(scaledDpi, scaledDpi),
            reinterpret_cast<LPARAM>(&suggested));
        GetClientRect(window.seekSlider_, &scaledSeek);
        checks.Expect(window.dpi_ == scaledDpi && scaledSeek.right > originalSeek.right &&
            channelPaintMatches(window.controlBar_, true),
            L"actual DPI/suggested-window resize reprojects saved marks through the native thumb travel range");
        SendMessageW(window.window_, WM_DPICHANGED, MAKEWPARAM(originalDpi, originalDpi),
            reinterpret_cast<LPARAM>(&originalBounds));
        window.EnterFullscreen();
        checks.Expect(GetParent(window.seekSlider_) == window.fullscreenControlBar_ &&
            channelPaintMatches(window.fullscreenControlBar_, true),
            L"fullscreen channel customdraw keeps saved marks at the resized trackbar positions");
        window.ExitFullscreen();

        ShowWindow(window.window_, SW_SHOWNA);
        window.SetToolbarManuallyHidden(true);
        window.RefreshControls();
        RECT videoClient{}, stripClient{};
        GetClientRect(window.videoWindow_, &videoClient);
        GetClientRect(window.progressStrip_, &stripClient);
        Pixels compact(videoClient.right, (std::max)(1L, stripClient.bottom));
        checks.Expect(compact.Valid(), L"allocate compact saved-interval pixel surface");
        if (!compact.Valid()) {
            ShowWindow(window.window_, SW_HIDE);
            return;
        }
        compact.Fill(videoClient, untouchedColor);
        SendMessageW(window.progressStrip_, WM_PRINTCLIENT,
            reinterpret_cast<WPARAM>(compact.dc), PRF_CLIENT);
        const int row = stripClient.bottom / 2;
        COLORREF transparent = 0;
        BYTE alpha = 0;
        DWORD flags = 0;
        const bool colorKey = GetLayeredWindowAttributes(
            window.progressStrip_, &transparent, &alpha, &flags) != FALSE &&
            transparent == RGB(1, 0, 1) &&
            (flags & (LWA_ALPHA | LWA_COLORKEY)) == (LWA_ALPHA | LWA_COLORKEY);
        checks.Expect(IsWindowVisible(window.progressStrip_) &&
            stripClient.right == videoClient.right && colorKey &&
            GetPixel(compact.dc, sampleX(stripClient, 5), row) == RGB(255, 0, 0) &&
            GetPixel(compact.dc, sampleX(stripClient, 15), row) == savedColor &&
            GetPixel(compact.dc, sampleX(stripClient, 25), row) == RGB(255, 0, 0) &&
            GetPixel(compact.dc, sampleX(stripClient, 45), row) == savedColor &&
            GetPixel(compact.dc, sampleX(stripClient, 70), row) == RGB(1, 0, 1),
            L"compact popup prints full-width saved marks over red playback with transparent future gaps");

        fixture.backend.positionMs = 0;
        window.RefreshControls();
        compact.Fill(stripClient, untouchedColor);
        SendMessageW(window.progressStrip_, WM_PRINTCLIENT,
            reinterpret_cast<WPARAM>(compact.dc), PRF_CLIENT);
        checks.Expect(IsWindowVisible(window.progressStrip_) && window.progressCompletedWidth_ == 0 &&
            GetPixel(compact.dc, sampleX(stripClient, 5), row) == RGB(1, 0, 1) &&
            GetPixel(compact.dc, sampleX(stripClient, 15), row) == savedColor &&
            GetPixel(compact.dc, sampleX(stripClient, 45), row) == savedColor,
            L"saved compact intervals remain visible before any video progress is completed");

        window.ResetMediaUiState();
        fixture.backend.positionMs = 6000;
        window.RefreshControls();
        RECT clearedStrip{};
        GetClientRect(window.progressStrip_, &clearedStrip);
        compact.Fill(stripClient, untouchedColor);
        SendMessageW(window.progressStrip_, WM_PRINTCLIENT,
            reinterpret_cast<WPARAM>(compact.dc), PRF_CLIENT);
        checks.Expect(window.removalIntervals_.empty() &&
            clearedStrip.right == videoClient.right * 6000 / fixture.backend.durationMs &&
            GetPixel(compact.dc, sampleX(stripClient, 15), row) == RGB(255, 0, 0) &&
            GetPixel(compact.dc, sampleX(stripClient, 45), row) == untouchedColor &&
            channelPaintMatches(window.controlBar_, false),
            L"media reset repaints away saved marks and restores completed-width compact progress");
        fixture.backend.positionMs = 0;
        window.RefreshControls();
        checks.Expect(!IsWindowVisible(window.progressStrip_),
            L"empty saved queue and zero playback progress hide the compact popup again");
        ShowWindow(window.window_, SW_HIDE);
    }

    static void PlaybackLoopRestartLifecycle(Checks& checks) {
        Fixture whole;
        checks.Expect(whole.Create(), L"create whole-video restart fixture");
        auto& wholeWindow = whole.window;
        if (wholeWindow.window_ == nullptr) return;
        SendMessageW(wholeWindow.loopVideoButton_, BM_CLICK, 0, 0);
        const std::uint32_t wholeGeneration = wholeWindow.player_.Generation();
        const int stopsBeforeEnd = whole.backend.stopCalls;
        const int seeksBeforeEnd = whole.backend.seekCalls;
        whole.backend.backendState = 5; // input-death Stopped notification
        whole.backend.positionMs = 0;
        wholeWindow.HandlePlayerEvent(PlayerEvent::EndReached, wholeGeneration);
        checks.Expect(whole.backend.playCalls == 1 &&
            whole.backend.stopCalls == stopsBeforeEnd + 1 &&
            whole.backend.seekCalls == seeksBeforeEnd + 1 &&
            wholeWindow.playbackSnapshot_.state == PlaybackState::Opening &&
            wholeWindow.playbackLoop_.Mode() == LoopMode::WholeVideo,
            L"whole-video loop explicitly restarts once after synthetic natural-end Stopped state");
        wholeWindow.RefreshControls();
        wholeWindow.RefreshControls();
        checks.Expect(whole.backend.playCalls == 1 &&
            wholeWindow.playbackSnapshot_.state == PlaybackState::Playing,
            L"Playing acknowledgement clears restart pending state without a repeat storm");
        SendMessageW(wholeWindow.stopButton_, BM_CLICK, 0, 0);
        const int playsAfterStop = whole.backend.playCalls;
        whole.backend.backendState = 5;
        wholeWindow.HandlePlayerEvent(PlayerEvent::EndReached, wholeGeneration);
        checks.Expect(whole.backend.playCalls == playsAfterStop &&
            !wholeWindow.loopPlaybackRequested_ &&
            wholeWindow.playbackLoop_.Mode() == LoopMode::WholeVideo,
            L"manual Stop wins over a late EndReached without discarding loop preference");

        Fixture segment;
        checks.Expect(segment.Create(), L"create segment restart fixture");
        auto& segmentWindow = segment.window;
        if (segmentWindow.window_ == nullptr) return;
        segment.backend.positionMs = 2000;
        segmentWindow.RefreshControls();
        SendMessageW(segmentWindow.loopStartButton_, BM_CLICK, 0, 0);
        segment.backend.positionMs = 6000;
        segmentWindow.RefreshControls();
        SendMessageW(segmentWindow.loopEndButton_, BM_CLICK, 0, 0);
        segmentWindow.RefreshControls();
        const std::uint32_t segmentGeneration = segmentWindow.player_.Generation();
        const int playsBeforeSegmentEnd = segment.backend.playCalls;
        const int seeksBeforeSegmentEnd = segment.backend.seekCalls;
        segment.backend.backendState = 5;
        segment.backend.positionMs = 0;
        segmentWindow.HandlePlayerEvent(PlayerEvent::EndReached, segmentGeneration);
        const bool restartedBeforeA = segment.backend.playCalls == playsBeforeSegmentEnd + 1 &&
            segment.backend.seekCalls == seeksBeforeSegmentEnd + 1 &&
            segment.backend.positionMs == 0 &&
            segmentWindow.playbackSnapshot_.state == PlaybackState::Opening;
        segmentWindow.RefreshControls();
        const int seeksAfterDeferredA = segment.backend.seekCalls;
        const bool deferredOnce = seeksAfterDeferredA == seeksBeforeSegmentEnd + 2 &&
            segment.backend.positionMs == 2000 &&
            segment.backend.playCalls == playsBeforeSegmentEnd + 1;
        // One in-range sample can be the optimistic cached seek position.
        // A transient backend zero after it must not start a second seek.
        segmentWindow.RefreshControls();
        segment.backend.positionMs = 0;
        segmentWindow.RefreshControls();
        const bool transientZeroDidNotReseek =
            segment.backend.seekCalls == seeksAfterDeferredA &&
            segment.backend.positionMs == 0;
        segment.backend.positionMs = 2000;
        segmentWindow.RefreshControls();
        segment.backend.positionMs = 2200;
        segmentWindow.RefreshControls();
        segmentWindow.RefreshControls();
        checks.Expect(restartedBeforeA && deferredOnce && transientZeroDidNotReseek &&
            segment.backend.seekCalls == seeksAfterDeferredA &&
            segment.backend.positionMs == 2200,
            L"segment restart ignores transient zero and settles only after A advances in range");

        segment.backend.backendState = 5;
        segment.backend.positionMs = 0;
        segmentWindow.HandlePlayerEvent(PlayerEvent::EndReached, segmentGeneration);
        const int seeksAfterSecondRestart = segment.backend.seekCalls;
        segment.backend.backendState = 4;
        segment.backend.positionMs = 0;
        segmentWindow.RefreshControls();
        const bool pendingPaused = segment.backend.seekCalls == seeksAfterSecondRestart &&
            segmentWindow.playbackLoop_.Mode() == LoopMode::Segment &&
            !segmentWindow.loopPlaybackRequested_;
        segment.backend.backendState = 3;
        segmentWindow.seekDragging_ = true;
        segmentWindow.RefreshControls();
        const bool dragPaused = segment.backend.seekCalls == seeksAfterSecondRestart;
        segmentWindow.seekDragging_ = false;
        segmentWindow.RefreshControls();
        checks.Expect(pendingPaused && dragPaused &&
            segment.backend.seekCalls == seeksAfterSecondRestart + 1 &&
            segment.backend.positionMs == 2000,
            L"Pause and seek drag suppress pending A-B until both have ended");
        segmentWindow.RefreshControls();

        segment.backend.backendState = 5;
        segment.backend.positionMs = 0;
        segmentWindow.HandlePlayerEvent(PlayerEvent::EndReached, segmentGeneration);
        const int seeksBeforeCancel = segment.backend.seekCalls;
        const int playsBeforeCancel = segment.backend.playCalls;
        SendMessageW(segmentWindow.loopSegmentButton_, BM_CLICK, 0, 0);
        segmentWindow.RefreshControls();
        checks.Expect(segmentWindow.playbackLoop_.Mode() == LoopMode::Off &&
            SendMessageW(segmentWindow.loopSegmentButton_, BM_GETCHECK, 0, 0) == BST_UNCHECKED &&
            segment.backend.seekCalls == seeksBeforeCancel &&
            segment.backend.playCalls == playsBeforeCancel,
            L"turning A-B off cancels its deferred seek and cannot restart again");
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
                input.compactMode = true;
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

    static void PreviewImmediatePresentation(Checks& checks) {
        Fixture fixture;
        checks.Expect(fixture.Create(), L"create immediate preview presentation fixture");
        auto& window = fixture.window;
        if (window.window_ == nullptr) return;
        ShowWindow(window.window_, SW_SHOWNA);
        window.previewInitialized_ = true;
        const auto generation = window.player_.Generation();
        window.previewRequestId_ = 99;
        window.previewRequestGeneration_ = generation;
        window.previewRequestTimestampMs_ = 5000;
        window.previewHoverTimeMs_ = 5200;
        RECT owner{};
        GetWindowRect(window.window_, &owner);
        window.previewAnchorScreen_ = {owner.left + 200, owner.top + 200};
        auto frame = std::make_shared<PreviewFrame>();
        frame->mediaGeneration = generation;
        frame->timestampMs = 5000;
        frame->width = frame->height = 2;
        frame->pitch = 8;
        frame->pixels.assign(16, 0);
        const PreviewResult ready{generation, 99, 5000, frame};
        checks.Expect(!window.PresentPreviewResult(ready, true) &&
            !window.previewPopup_.IsVisible(),
            L"late asynchronous preview cannot reopen a hidden popup");
        checks.Expect(window.PresentPreviewResult(ready, false) &&
            window.previewPopup_.IsVisible() && window.previewDisplayedFrame_ == frame &&
            window.previewDisplayedRequestId_ == 99 &&
            window.previewHoverTimeMs_ == 5200,
            L"immediate cached frame appears without placeholder or changing precise hover time");
        checks.Expect(!window.PresentPreviewResult(ready, true),
            L"posted cache notification cannot display a result twice");
        window.previewRequestId_ = 100;
        window.previewRequestTimestampMs_ = 10000;
        window.previewHoverTimeMs_ = 10200;
        window.previewDisplayedRequestId_ = 0;
        window.ShowPendingSeekPreview(60000);
        checks.Expect(window.previewPopup_.IsVisible() &&
            window.previewDisplayedFrame_ == frame && frame->timestampMs == 5000 &&
            window.previewHoverTimeMs_ == 10200 && window.previewDisplayedRequestId_ == 0,
            L"next bucket keeps the previous image with its true timestamp and precise new hover");
        checks.Expect(!window.CanReusePreviewRequest(generation, 10000),
            L"approximate fallback never masquerades as a completed current request");
        window.previewPopup_.ShowFrameAt(window.previewAnchorScreen_, 96, 10200, frame, true);
        checks.Expect(window.previewPopup_.IsUpdating(),
            L"pending fallback is explicitly marked as updating");
        window.HandlePreviewResult();
        checks.Expect(!window.previewPopup_.IsUpdating() && window.previewDisplayedFrame_ == frame,
            L"finished failure keeps approximate image but clears updating badge");
        auto fresh = std::make_shared<PreviewFrame>(*frame);
        fresh->timestampMs = 10000;
        checks.Expect(window.PresentPreviewResult({generation, 100, 10000, fresh}, true) &&
            window.previewDisplayedFrame_ == fresh && window.previewDisplayedRequestId_ == 100 &&
            !window.previewPopup_.IsUpdating(),
            L"matching new frame replaces fallback and clears pending presentation");
        auto stale = std::make_shared<PreviewFrame>(*frame);
        stale->mediaGeneration = generation + 1;
        window.previewDisplayedFrame_ = stale;
        window.ShowPendingSeekPreview(60000);
        checks.Expect(!window.previewDisplayedFrame_ && !window.previewPopup_.IsUpdating(),
            L"fallback discards frames from another media generation");
        window.HideSeekPreview();
        window.ShowPendingSeekPreview(60000);
        checks.Expect(!window.previewPopup_.IsVisible(),
            L"cancelled hover cannot reopen popup through fallback presentation");
        window.previewRequestId_ = 99;
        window.previewRequestGeneration_ = generation;
        window.previewRequestTimestampMs_ = 5000;
        window.previewDisplayedRequestId_ = 0;
        PreviewResult invalid = ready;
        invalid.timestampMs = 10000;
        checks.Expect(!window.PresentPreviewResult(invalid, false),
            L"wrong preview bucket is rejected by immediate presentation");
        invalid = ready;
        ++invalid.mediaGeneration;
        checks.Expect(!window.PresentPreviewResult(invalid, false),
            L"other-file cached frame cannot be shown");
        window.HideSeekPreview();
        checks.Expect(!window.PresentPreviewResult(ready, false) &&
            !window.previewPopup_.IsVisible() && !window.previewDisplayedFrame_,
            L"hover cancellation invalidates even the immediate presentation path");
        ShowWindow(window.window_, SW_HIDE);
    }

    static void PreviewWorkerPresentation(Checks& checks) {
        const auto state = std::make_shared<ControlledPreview>();
        checks.Expect(state->entered && state->release && state->returned,
            L"create controlled preview worker barriers");
        if (!state->entered || !state->release || !state->returned) return;
        Fixture fixture([state] { return std::make_unique<ControlledPreviewBackend>(state); });
        checks.Expect(fixture.Create(), L"create injected preview worker UI fixture");
        auto& window = fixture.window;
        if (!window.window_) return;
        ShowWindow(window.window_, SW_SHOWNA);
        window.previewInitialized_ = window.previewEngine_.Initialize(window.window_);
        checks.Expect(window.previewInitialized_, L"initialize actual preview worker notifications");
        if (!window.previewInitialized_) return;
        window.previewEngine_.SetPrefetchEnabled(false);
        window.previewEngine_.SetMedia(L"C:\\preview-fixture.mp4", window.player_.Generation());
        const int initialSeeks = fixture.backend.seekCalls;
        const std::int64_t initialPosition = fixture.backend.positionMs;
        const auto hover = [&window](const std::int64_t timestampMs) {
            RECT channel{};
            RECT thumb{};
            SendMessageW(window.seekSlider_, TBM_GETCHANNELRECT, 0,
                reinterpret_cast<LPARAM>(&channel));
            SendMessageW(window.seekSlider_, TBM_GETTHUMBRECT, 0,
                reinterpret_cast<LPARAM>(&thumb));
            const RECT range = TrackbarPointerRange(channel, thumb);
            const int x = range.left + static_cast<int>(
                (range.right - range.left) * timestampMs / window.player_.DurationMs());
            window.HandleSeekPointer(x);
        };
        const auto entered = [&] {
            const bool signalled = WaitForSingleObject(state->entered, 3000) == WAIT_OBJECT_0;
            checks.Expect(signalled, L"actual UI preview request reaches controlled decoder barrier");
            return signalled;
        };

        state->Arm(PreviewDecodeStatus::RuntimeFailure);
        hover(5200);
        if (!entered()) return;
        checks.Expect(window.previewEngine_.IsRequestPending(window.previewRequestId_) &&
            window.previewPopup_.IsVisible() && window.previewPopup_.IsWaitingForFrame() &&
            !window.previewPopup_.IsUpdating() && !window.previewDisplayedFrame_,
            L"cold in-flight worker shows genuine waiting state without fabricated image");
        const auto coldRequest = window.previewRequestId_;
        SetEvent(state->release);
        checks.Expect(PumpPreviewUntil(window, [&] {
            return window.previewEngine_.GetRequestState(coldRequest) ==
                PreviewRequestState::RuntimeFailure && !window.previewPopup_.IsWaitingForFrame();
        }) && window.previewPopup_.IsVisible() && !window.previewDisplayedFrame_,
            L"actual terminal cold failure notification replaces waiting with unavailable state");

        state->Arm(PreviewDecodeStatus::Ready);
        hover(5200);
        if (!entered()) return;
        checks.Expect(window.previewRequestId_ != coldRequest &&
            window.previewPopup_.IsWaitingForFrame(),
            L"hover after actual failure starts fresh request and restores waiting state");
        SetEvent(state->release);
        checks.Expect(PumpPreviewUntil(window, [&] {
            return window.previewDisplayedRequestId_ == window.previewRequestId_ &&
                window.previewDisplayedFrame_ && window.previewDisplayedFrame_->timestampMs == 5000;
        }) && !window.previewPopup_.IsWaitingForFrame() && !window.previewPopup_.IsUpdating(),
            L"actual ready notification displays decoded frame and clears waiting state");
        const auto first = window.previewDisplayedFrame_;

        state->Arm(PreviewDecodeStatus::Ready);
        hover(10200);
        if (!entered()) return;
        checks.Expect(first && window.previewDisplayedFrame_ == first &&
            first->mediaGeneration == window.player_.Generation() && first->timestampMs == 5000 &&
            window.previewDisplayedRequestId_ == 0 && window.previewPopup_.IsUpdating() &&
            !window.previewPopup_.IsWaitingForFrame(),
            L"actual pending next bucket keeps same-file fallback with updating badge");
        const auto warmRequest = window.previewRequestId_;
        const auto calls = state->decodeCalls.load();
        hover(10100);
        checks.Expect(window.previewRequestId_ == warmRequest &&
            state->decodeCalls.load() == calls && window.previewPopup_.IsUpdating(),
            L"same-bucket UI motion reuses genuine in-flight request without clearing updating badge");
        SetEvent(state->release);
        checks.Expect(PumpPreviewUntil(window, [&] {
            return window.previewDisplayedRequestId_ == warmRequest &&
                window.previewDisplayedFrame_ && window.previewDisplayedFrame_->timestampMs == 10000;
        }) && window.previewDisplayedFrame_ != first && !window.previewPopup_.IsUpdating(),
            L"actual new ready result replaces approximate fallback and clears updating badge");
        const auto second = window.previewDisplayedFrame_;

        state->Arm(PreviewDecodeStatus::RuntimeFailure);
        hover(15200);
        if (!entered()) return;
        checks.Expect(window.previewDisplayedFrame_ == second && window.previewPopup_.IsUpdating(),
            L"genuine pending failure attempt preserves last available image");
        const auto failedRequest = window.previewRequestId_;
        SetEvent(state->release);
        checks.Expect(PumpPreviewUntil(window, [&] {
            return window.previewEngine_.GetRequestState(failedRequest) ==
                PreviewRequestState::RuntimeFailure && !window.previewPopup_.IsUpdating();
        }) && window.previewDisplayedFrame_ == second &&
            window.previewDisplayedRequestId_ == 0 && !window.previewPopup_.IsWaitingForFrame(),
            L"actual terminal warm failure keeps approximate frame but clears pending badge");

        state->Arm(PreviewDecodeStatus::Ready);
        hover(19000);
        if (!entered()) return;
        const auto cancelledRequest = window.previewRequestId_;
        window.HideSeekPreview();
        checks.Expect(!window.previewPopup_.IsVisible() && !window.previewPopup_.IsUpdating() &&
            !window.previewPopup_.IsWaitingForFrame(),
            L"hiding genuine in-flight preview clears all popup activity indicators");
        SetEvent(state->release);
        checks.Expect(WaitForSingleObject(state->returned, 3000) == WAIT_OBJECT_0 &&
            state->cancellationObserved.load(),
            L"hidden hover cancellation is observed by actual injected decoder");
        window.previewEngine_.Shutdown();
        PumpPreviewUntil(window, [] { return true; });
        SendMessageW(window.window_, kPreviewFrameReadyMessage, 0, 0);
        checks.Expect(window.previewEngine_.GetRequestState(cancelledRequest) ==
            PreviewRequestState::Cancelled && !window.previewPopup_.IsVisible() &&
            !window.previewDisplayedFrame_ && window.previewRequestId_ == 0,
            L"queued and late real notification path never reopens cancelled preview");
        checks.Expect(fixture.backend.seekCalls == initialSeeks &&
            fixture.backend.positionMs == initialPosition,
            L"pending ready failure and cancellation preview transitions never seek main player");
        ShowWindow(window.window_, SW_HIDE);
    }

    static void ZoomNavigation(Checks& checks) {
        Fixture fixture;
        Fixture other;
        checks.Expect(fixture.Create() && other.Create(), L"create zoom navigation fixtures");
        auto& window = fixture.window;
        if (window.window_ == nullptr || other.window.window_ == nullptr) return;
        ShowWindow(window.window_, SW_SHOWNA);
        const VideoCrop initial{160, 90, 320, 180};
        window.ApplyAreaZoom(initial);
        fixture.backend.backendState = 4; // Navigation works on paused frames too.
        window.RefreshControls();
        const auto generation = window.player_.Generation();
        const auto position = fixture.backend.positionMs;
        const auto readCrop = [&window]() {
            VideoCrop crop{};
            window.player_.GetVideoCrop(crop);
            return crop;
        };
        window.HandleHotKey(VK_RIGHT, false, false);
        const auto right = readCrop();
        checks.Expect(right.x > initial.x && right.y == initial.y &&
            right.width == initial.width && right.height == initial.height,
            L"right arrow moves the camera right without resizing crop");
        window.HandleHotKey(VK_DOWN, false, true);
        checks.Expect(readCrop().y > initial.y && fixture.backend.volume == 100 &&
            fixture.backend.seekCalls == 0 && fixture.backend.positionMs == position,
            L"held zoom arrow pans without changing volume or playback position");
        window.HandleHotKey(VK_LEFT, true, false);
        checks.Expect(fixture.backend.seekCalls == 1,
            L"Ctrl+arrow keeps transport seek while zoom is applied");
        const int seeks = fixture.backend.seekCalls;

        window.ApplyAreaZoom(initial);
        VideoDimensions source{};
        VideoCrop crop{};
        GeometryRect content{};
        window.CurrentZoomViewport(source, crop, content);
        POINT center{content.left + content.Width() / 2, content.top + content.Height() / 2};
        POINT clientCenter = center;
        ScreenToClient(window.videoWindow_, &clientCenter);
        const auto clientParam = [](POINT point) { return MAKELPARAM(point.x, point.y); };
        SendMessageW(window.videoWindow_, WM_LBUTTONDOWN, MK_LBUTTON, clientParam(clientCenter));
        checks.Expect(window.zoomPanDragging_ && GetCapture() == window.videoWindow_ &&
            GetFocus() == window.videoWindow_, L"video mouse-down starts captured pan with video focus");
        POINT dragged = clientCenter;
        dragged.x += content.Width() / 4;
        SendMessageW(window.videoWindow_, WM_MOUSEMOVE, MK_LBUTTON, clientParam(dragged));
        checks.Expect(readCrop().x < initial.x && readCrop().width == initial.width,
            L"dragging picture right reveals source content to the left");
        SendMessageW(window.videoWindow_, WM_MOUSEMOVE, MK_LBUTTON, clientParam(clientCenter));
        checks.Expect(HasCrop(window, initial), L"returning a drag to its start has no rounding drift");
        SendMessageW(window.videoWindow_, WM_LBUTTONUP, 0, clientParam(dragged));
        checks.Expect(!window.zoomPanDragging_ && GetCapture() != window.videoWindow_ &&
            readCrop().x < initial.x, L"mouse-up applies final pan position and releases capture");

        window.ApplyAreaZoom(initial);
        const WPARAM halfWheel = MAKEWPARAM(0, 60);
        SendMessageW(window.videoWindow_, WM_MOUSEWHEEL, halfWheel, clientParam(center));
        checks.Expect(HasCrop(window, initial), L"high-resolution half notch accumulates without crop drift");
        SendMessageW(window.videoWindow_, WM_MOUSEWHEEL, halfWheel, clientParam(center));
        const VideoCrop zoomed = readCrop();
        checks.Expect(zoomed.width < initial.width && zoomed.height < initial.height &&
            window.zoomState_ == ZoomState::Applied, L"positive wheel notch zooms in through video message routing");
        const auto sourceCenterX = zoomed.x + zoomed.width / 2;
        checks.Expect(sourceCenterX >= 318 && sourceCenterX <= 322,
            L"wheel zoom retains the source point beneath the cursor");
        SetFocus(window.seekSlider_);
        MSG focusedTrackbarWheel{};
        focusedTrackbarWheel.hwnd = window.seekSlider_;
        focusedTrackbarWheel.message = WM_MOUSEWHEEL;
        focusedTrackbarWheel.wParam = MAKEWPARAM(0, 120);
        focusedTrackbarWheel.lParam = clientParam(center);
        checks.Expect(window.ProcessZoomMouseMessage(focusedTrackbarWheel) &&
            readCrop().width < zoomed.width && fixture.backend.seekCalls == seeks,
            L"message loop routes wheel over video before focused seek control consumes it");
        focusedTrackbarWheel.hwnd = other.window.seekSlider_;
        checks.Expect(!window.ProcessZoomMouseMessage(focusedTrackbarWheel),
            L"zoom wheel routing never handles another player instance");
        const int callsBeforeIgnoredWheel = fixture.backend.cropCalls;
        RECT bar{};
        GetWindowRect(window.controlBar_, &bar);
        SendMessageW(window.window_, WM_MOUSEWHEEL, MAKEWPARAM(0, 120),
            MAKELPARAM(bar.left + 10, bar.top + 10));
        checks.Expect(fixture.backend.cropCalls == callsBeforeIgnoredWheel,
            L"wheel above the toolbar cannot zoom the focused video");
        window.CurrentZoomViewport(source, crop, content);
        RECT viewport{};
        GetWindowRect(window.videoWindow_, &viewport);
        POINT blackBar{viewport.left, viewport.top};
        checks.Expect(!window.HandleZoomWheel(120, blackBar),
            L"letterbox pixels do not become wheel zoom anchors");
        for (int index = 0; index < 60; ++index) {
            window.CurrentZoomViewport(source, crop, content);
            POINT anchor{content.left + content.Width() / 2, content.top + content.Height() / 2};
            window.HandleZoomWheel(-120, anchor);
        }
        const auto initialLimit = readCrop();
        checks.Expect(initialLimit == VideoCrop{0, 0, source.width, source.height} &&
            window.zoomSelectionCrop_ == initial && window.zoomState_ == ZoomState::Applied,
            L"maximum wheel zoom-out expands a source-aspect selection to the full source");
        window.CurrentZoomViewport(source, crop, content);
        center = {content.left + content.Width() / 2, content.top + content.Height() / 2};
        const int callsAtLimit = fixture.backend.cropCalls;
        window.HandleZoomWheel(-120, center);
        checks.Expect(HasCrop(window, initialLimit) &&
            fixture.backend.cropCalls == callsAtLimit &&
            window.zoomSelectionCrop_ == initial && window.zoomState_ == ZoomState::Applied,
            L"extra outward wheel at full-source limit is a stable applied-mode no-op");
        SendMessageW(window.window_, WM_MOUSEWHEEL, MAKEWPARAM(0, 120), clientParam(center));
        const auto zoomedFromLimit = readCrop();
        checks.Expect(zoomedFromLimit.width < initialLimit.width &&
            zoomedFromLimit.height < initialLimit.height &&
            window.zoomSelectionCrop_ == initial && window.zoomState_ == ZoomState::Applied,
            L"wheel zooms inward from the expanded limit without replacing selection metadata");

        const VideoCrop panSelection{40, 60, 200, 100};
        window.ApplyAreaZoom(panSelection);
        for (int index = 0; index < 60; ++index) {
            window.CurrentZoomViewport(source, crop, content);
            POINT anchor{content.left + content.Width() / 2, content.top + content.Height() / 2};
            window.HandleZoomWheel(-120, anchor);
        }
        const auto panLimit = readCrop();
        checks.Expect(panLimit.width == source.width && panLimit.height == 320 &&
            static_cast<std::uint64_t>(panLimit.width) * panSelection.height ==
                static_cast<std::uint64_t>(panLimit.height) * panSelection.width &&
            window.zoomSelectionCrop_ == panSelection,
            L"non-source-aspect selection expands to the largest same-aspect source crop");
        window.CurrentZoomViewport(source, crop, content);
        center = {content.left + content.Width() / 2, content.top + content.Height() / 2};
        window.HandleHotKey(VK_DOWN, false, false);
        const auto pannedLimit = readCrop();
        const int callsAtPannedLimit = fixture.backend.cropCalls;
        window.HandleZoomWheel(-120, center);
        checks.Expect(pannedLimit.y > panLimit.y &&
            pannedLimit.width == panLimit.width && pannedLimit.height == panLimit.height &&
            HasCrop(window, pannedLimit) && fixture.backend.cropCalls == callsAtPannedLimit &&
            window.zoomSelectionCrop_ == panSelection && window.zoomState_ == ZoomState::Applied,
            L"same-aspect zoom limit still pans through source without resize or metadata reset");

        const auto startPan = [&]() {
            window.CurrentZoomViewport(source, crop, content);
            POINT anchor{content.left + content.Width() / 2, content.top + content.Height() / 2};
            return window.BeginZoomPan(anchor);
        };
        checks.Expect(startPan(), L"start a new pan before cancellation");
        ReleaseCapture();
        const auto afterCaptureLoss = readCrop();
        window.UpdateZoomPan({0, 0});
        checks.Expect(!window.zoomPanDragging_ && HasCrop(window, afterCaptureLoss),
            L"lost capture cancels pan and cannot move crop on stale input");
        startPan();
        SendMessageW(window.videoWindow_, WM_CANCELMODE, 0, 0);
        checks.Expect(!window.zoomPanDragging_ && GetCapture() != window.videoWindow_,
            L"video cancel-mode releases pan capture");
        startPan();
        SendMessageW(window.window_, WM_SIZE, SIZE_MINIMIZED, 0);
        checks.Expect(!window.zoomPanDragging_ && GetCapture() != window.videoWindow_,
            L"minimize cancels active pan");
        SendMessageW(window.window_, WM_SIZE, SIZE_RESTORED, 0);
        startPan();
        SendMessageW(window.window_, WM_SIZE, SIZE_RESTORED, 0);
        checks.Expect(!window.zoomPanDragging_, L"resize discards the old pan coordinate transform");
        startPan();
        window.SetToolbarManuallyHidden(true);
        checks.Expect(!window.zoomPanDragging_ && window.toolbarManuallyHidden_ &&
            window.zoomSelectionCrop_ == panSelection,
            L"compact toolbar layout cancels pan without resetting selected zoom limit");
        checks.Expect(fixture.backend.seekCalls == seeks &&
            window.player_.Generation() == generation && !other.window.player_.IsVideoCropped(),
            L"mouse navigation leaves seek, media generation and other player unchanged");
        startPan();
        window.ResetAreaZoom();
        checks.Expect(!window.zoomPanDragging_ && GetCapture() != window.videoWindow_ &&
            window.zoomState_ == ZoomState::None && !window.zoomSelectionCrop_.IsValid(),
            L"reset zoom releases drag, clears original selection and restores normal input mode");
        window.HandleHotKey(VK_RIGHT, false, false);
        window.HandleHotKey(VK_DOWN, false, false);
        checks.Expect(fixture.backend.seekCalls == seeks + 1 && fixture.backend.volume == 95,
            L"ordinary arrow seek and volume return after zoom reset");
        const int resetCropCalls = fixture.backend.cropCalls;
        checks.Expect(!window.HandleZoomWheel(120, center) &&
            fixture.backend.cropCalls == resetCropCalls, L"wheel outside zoom mode cannot change crop");
        const VideoCrop nextSelection{40, 60, 240, 160};
        window.ApplyAreaZoom(nextSelection);
        window.CurrentZoomViewport(source, crop, content);
        center = {content.left + content.Width() / 2, content.top + content.Height() / 2};
        window.HandleZoomWheel(-120, center);
        const auto grownNextSelection = readCrop();
        checks.Expect(grownNextSelection.width > nextSelection.width &&
            grownNextSelection.height > nextSelection.height &&
            static_cast<std::uint64_t>(grownNextSelection.width) * nextSelection.height ==
                static_cast<std::uint64_t>(grownNextSelection.height) * nextSelection.width &&
            window.zoomSelectionCrop_ == nextSelection,
            L"new selection grows outward at its own aspect while replacing old metadata");
        window.ResetMediaUiState();
        checks.Expect(!window.zoomSelectionCrop_.IsValid() &&
            window.zoomState_ == ZoomState::None,
            L"media change clears original selection rather than leaking to next file");
        ShowWindow(window.window_, SW_HIDE);
    }

    static void VideoDisplayResize(Checks& checks) {
        Fixture fixture;
        checks.Expect(fixture.Create(), L"create video-display resize fixture");
        auto& window = fixture.window;
        if (window.window_ == nullptr) return;

        ShowWindow(window.window_, SW_SHOWNA);

        const auto sameDisplay = [](const VideoDisplayState& left,
                                     const VideoDisplayState& right) {
            const auto nearlyEqual = [](const double a, const double b) {
                const double difference = a - b;
                return difference > -0.000000001 && difference < 0.000000001;
            };
            return nearlyEqual(left.scale, right.scale) &&
                nearlyEqual(left.centerX, right.centerX) &&
                nearlyEqual(left.centerY, right.centerY);
        };
        const auto frameMatchesCrop = [](const RECT& value, const VideoCrop crop) {
            const std::int64_t width = value.right - value.left;
            const std::int64_t height = value.bottom - value.top;
            std::int64_t difference = width * crop.height - height * crop.width;
            if (difference < 0) difference = -difference;
            return width > 0 && height > 0 &&
                difference <= 2LL * (std::max)(crop.width, crop.height);
        };
        const auto messagePoint = [&](const POINT screen) {
            POINT client = screen;
            ScreenToClient(window.videoWindow_, &client);
            return MAKELPARAM(client.x, client.y);
        };
        const auto drag = [&](const POINT start, const POINT finish) {
            SendMessageW(window.videoWindow_, WM_LBUTTONDOWN, MK_LBUTTON,
                messagePoint(start));
            SendMessageW(window.videoWindow_, WM_MOUSEMOVE, MK_LBUTTON,
                messagePoint(finish));
            SendMessageW(window.videoWindow_, WM_LBUTTONUP, 0,
                messagePoint(finish));
        };

        const LONG_PTR viewportStyle = GetWindowLongPtrW(window.videoWindow_, GWL_STYLE);
        const LONG_PTR renderStyle = GetWindowLongPtrW(window.videoRenderWindow_, GWL_STYLE);
        const LONG_PTR requiredRenderStyle =
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
        checks.Expect(GetParent(window.videoRenderWindow_) == window.videoWindow_ &&
            GetAncestor(window.videoRenderWindow_, GA_ROOTOWNER) == window.window_ &&
            GetDlgCtrlID(window.videoRenderWindow_) == IDC_VIDEO_RENDER_SURFACE &&
            (renderStyle & requiredRenderStyle) == requiredRenderStyle &&
            (viewportStyle & WS_CLIPCHILDREN) != 0,
            L"render HWND is a clipped child of the fixed video viewport");

        fixture.backend.positionMs = 2000;
        window.RefreshControls();
        SendMessageW(window.loopStartButton_, BM_CLICK, 0, 0);
        fixture.backend.positionMs = 6000;
        window.RefreshControls();
        SendMessageW(window.loopEndButton_, BM_CLICK, 0, 0);
        window.RefreshControls();
        const std::uint32_t generation = window.player_.Generation();
        const int loopSeeks = fixture.backend.seekCalls;

        VideoDimensions video{};
        GeometryRect viewport{};
        window.CurrentVideoViewport(video, viewport);
        GeometryRect content = ComputeVideoContentRect(video, viewport);
        POINT edge{content.right - 1, content.top + content.Height() / 2};
        checks.Expect(window.VideoResizeEdgeAt(edge) == VideoEdge::Right,
            L"pillarboxed fitted video exposes its visible right resize edge");
        SendMessageW(window.videoWindow_, WM_LBUTTONDBLCLK, MK_LBUTTON,
            messagePoint(edge));
        const VideoCrop fullCrop{0, 0, video.width, video.height};
        RECT beforeZero{};
        GetWindowRect(window.videoRenderWindow_, &beforeZero);
        const VideoDisplayState beforeZeroState = window.videoDisplayState_;
        window.UpdateVideoResize(edge);
        RECT afterZero{};
        GetWindowRect(window.videoRenderWindow_, &afterZero);
        checks.Expect(!window.fullscreen_ && window.videoResizeDragging_ &&
            window.videoDisplayActive_ && window.zoomState_ == ZoomState::Applied &&
            HasCrop(window, fullCrop) && window.zoomSelectionCrop_ == fullCrop &&
            beforeZero.left == afterZero.left && beforeZero.top == afterZero.top &&
            beforeZero.right == afterZero.right && beforeZero.bottom == afterZero.bottom &&
            sameDisplay(beforeZeroState, window.videoDisplayState_),
            L"edge double-click starts full-source resize without fullscreen or zero-delta drift");
        SendMessageW(window.videoWindow_, WM_CANCELMODE, 0, 0);
        checks.Expect(!window.ZoomGestureActive() && GetCapture() != window.videoWindow_,
            L"cancel-mode releases video resize capture while keeping applied zoom");

        VideoCrop crop{};
        window.CurrentZoomViewport(video, crop, content);
        edge = {content.right - 1, content.top + content.Height() / 2};
        const int cropCallsBeforeResize = fixture.backend.cropCalls;
        drag(edge, {viewport.right + 200, edge.y});
        RECT filled{};
        GetWindowRect(window.videoRenderWindow_, &filled);
        window.player_.GetVideoCrop(crop);
        checks.Expect(filled.left >= viewport.left && filled.left - viewport.left <= 2 &&
            filled.right <= viewport.right && viewport.right - filled.right <= 2 &&
            filled.top == beforeZero.top && filled.bottom == beforeZero.bottom &&
            crop.x == 0 && crop.width == video.width && crop.y > 0 &&
            crop.height < video.height && frameMatchesCrop(filled, crop) &&
            window.zoomSelectionCrop_ == crop && window.videoDisplayActive_ &&
            window.zoomState_ == ZoomState::Applied && !window.ZoomGestureActive() &&
            fixture.backend.cropCalls == cropCallsBeforeResize + 1,
            L"horizontal source-bound drag widens only the frame and crops the other source axis");

        window.ResetAreaZoom();
        const VideoCrop selection{80, 40, 320, 180};
        window.ApplyAreaZoom(selection);
        window.CurrentZoomViewport(video, crop, content);
        edge = {content.right - 1, content.top + content.Height() / 2};
        // Before custom presentation starts the render host fills the viewport;
        // compare the actual fitted picture, not its letterbox-containing host.
        const RECT horizontalStart{content.left, content.top, content.right, content.bottom};
        SendMessageW(window.videoWindow_, WM_LBUTTONDOWN, MK_LBUTTON,
            messagePoint(edge));
        SendMessageW(window.videoWindow_, WM_MOUSEMOVE, MK_LBUTTON,
            messagePoint({edge.x + 40, edge.y}));
        RECT horizontalGrown{};
        GetWindowRect(window.videoRenderWindow_, &horizontalGrown);
        VideoCrop horizontalCrop{};
        window.player_.GetVideoCrop(horizontalCrop);
        checks.Expect(horizontalGrown.right - horizontalGrown.left >
                horizontalStart.right - horizontalStart.left &&
            std::abs((horizontalGrown.bottom - horizontalGrown.top) -
                (horizontalStart.bottom - horizontalStart.top)) <= 2 &&
            horizontalCrop.x < selection.x && horizontalCrop.width > selection.width &&
            horizontalCrop.y == selection.y && horizontalCrop.height == selection.height &&
            frameMatchesCrop(horizontalGrown, horizontalCrop) &&
            window.zoomSelectionCrop_ == horizontalCrop,
            L"right-edge drag reveals more selected source while leaving frame height unchanged");
        SendMessageW(window.videoWindow_, WM_MOUSEMOVE, MK_LBUTTON,
            messagePoint(edge));
        RECT horizontalRestored{};
        GetWindowRect(window.videoRenderWindow_, &horizontalRestored);
        checks.Expect(HasCrop(window, selection) && window.zoomSelectionCrop_ == selection &&
            horizontalRestored.left == horizontalStart.left &&
            horizontalRestored.top == horizontalStart.top &&
            horizontalRestored.right == horizontalStart.right &&
            horizontalRestored.bottom == horizontalStart.bottom &&
            window.videoResizeDragging_,
            L"returning to the mouse-down point restores exact crop, frame and aspect reference");
        SendMessageW(window.videoWindow_, WM_LBUTTONUP, 0, messagePoint(edge));

        window.ResetAreaZoom();
        window.ApplyAreaZoom(selection);
        window.CurrentZoomViewport(video, crop, content);
        POINT bottom{content.left + content.Width() / 2, content.bottom - 1};
        const RECT verticalStart{content.left, content.top, content.right, content.bottom};
        drag(bottom, {bottom.x, bottom.y - 60});
        RECT verticalShrunk{};
        GetWindowRect(window.videoRenderWindow_, &verticalShrunk);
        VideoCrop verticalCrop{};
        window.player_.GetVideoCrop(verticalCrop);
        checks.Expect(std::abs((verticalShrunk.right - verticalShrunk.left) -
                (verticalStart.right - verticalStart.left)) <= 2 &&
            verticalShrunk.bottom - verticalShrunk.top <
                verticalStart.bottom - verticalStart.top &&
            verticalCrop.x == selection.x && verticalCrop.width == selection.width &&
            verticalCrop.y > selection.y && verticalCrop.height < selection.height &&
            frameMatchesCrop(verticalShrunk, verticalCrop) &&
            window.zoomSelectionCrop_ == verticalCrop,
            L"bottom-edge drag changes only frame height and matching source height");

        window.ResetAreaZoom();
        window.ApplyAreaZoom(selection);
        window.CurrentZoomViewport(video, crop, content);
        POINT corner{content.right - 1, content.top};
        const RECT cornerStart{content.left, content.top, content.right, content.bottom};
        checks.Expect(window.VideoResizeEdgeAt(corner) == VideoEdge::TopRight,
            L"visible corner exposes a free two-axis area resize grip");
        drag(corner, {corner.x + 50, corner.y + 60});
        RECT cornerFrame{};
        GetWindowRect(window.videoRenderWindow_, &cornerFrame);
        VideoCrop cornerCrop{};
        window.player_.GetVideoCrop(cornerCrop);
        checks.Expect(cornerFrame.right - cornerFrame.left >
                cornerStart.right - cornerStart.left &&
            cornerFrame.bottom - cornerFrame.top <
                cornerStart.bottom - cornerStart.top &&
            cornerCrop.width > selection.width && cornerCrop.height < selection.height &&
            static_cast<std::uint64_t>(cornerCrop.width) * selection.height !=
                static_cast<std::uint64_t>(cornerCrop.height) * selection.width &&
            frameMatchesCrop(cornerFrame, cornerCrop) &&
            window.zoomSelectionCrop_ == cornerCrop,
            L"corner drag changes width and height independently without distorting decoded pixels");

        window.CurrentZoomViewport(video, crop, content);
        const POINT wheelPoint{content.left + content.Width() / 4,
            content.top + content.Height() / 2};
        const VideoCrop expectedWheel = ZoomVideoCrop(
            video, crop, 1.0 / 1.12,
            static_cast<double>(wheelPoint.x - content.left) / content.Width(),
            static_cast<double>(wheelPoint.y - content.top) / content.Height(),
            {cornerCrop.width, cornerCrop.height});
        checks.Expect(window.HandleZoomWheel(WHEEL_DELTA, wheelPoint) &&
            HasCrop(window, expectedWheel) && window.zoomSelectionCrop_ == cornerCrop &&
            window.videoDisplayActive_,
            L"wheel navigation retains the aspect established by free corner resize");
        const VideoDisplayState preserved = window.videoDisplayState_;
        const VideoCrop preservedCrop = expectedWheel;
        const VideoCrop preservedReference = window.zoomSelectionCrop_;

        window.SetToolbarManuallyHidden(true);
        ShowWindow(window.window_, SW_HIDE);
        window.EnterFullscreen();
        const bool fullscreenPreserved = window.fullscreen_ &&
            window.videoDisplayActive_ && sameDisplay(preserved, window.videoDisplayState_) &&
            HasCrop(window, preservedCrop) &&
            window.zoomSelectionCrop_ == preservedReference;
        window.ExitFullscreen();
        ShowWindow(window.window_, SW_SHOWNA);
        window.SetToolbarManuallyHidden(false);
        checks.Expect(fullscreenPreserved && !window.fullscreen_ &&
            !window.toolbarManuallyHidden_ && window.videoDisplayActive_ &&
            sameDisplay(preserved, window.videoDisplayState_) &&
            HasCrop(window, preservedCrop) &&
            window.zoomSelectionCrop_ == preservedReference &&
            GetParent(window.videoRenderWindow_) == window.videoWindow_ &&
            window.playbackLoop_.Mode() == LoopMode::Segment &&
            window.playbackLoop_.StartMs() == 2000 && window.playbackLoop_.EndMs() == 6000 &&
            fixture.backend.seekCalls == loopSeeks && window.player_.Generation() == generation,
            L"toolbar/fullscreen roundtrip preserves resized area, A-B loop and media identity");

        window.ResetAreaZoom();
        RECT renderClient{};
        GetWindowRect(window.videoRenderWindow_, &renderClient);
        MapWindowPoints(nullptr, window.videoWindow_,
            reinterpret_cast<POINT*>(&renderClient), 2);
        RECT viewportClient{};
        GetClientRect(window.videoWindow_, &viewportClient);
        checks.Expect(!window.videoDisplayActive_ &&
            sameDisplay(window.videoDisplayState_, VideoDisplayState{}) &&
            window.zoomState_ == ZoomState::None && !window.player_.IsVideoCropped() &&
            renderClient.left == viewportClient.left && renderClient.top == viewportClient.top &&
            renderClient.right == viewportClient.right && renderClient.bottom == viewportClient.bottom,
            L"explicit zoom reset restores the render child to the fixed viewport");

        window.ApplyAreaZoom(selection);
        window.CurrentZoomViewport(video, crop, content);
        window.BeginVideoResize({content.right - 1, content.top + content.Height() / 2});
        window.EndZoomPan();
        window.ResetMediaUiState();
        checks.Expect(!window.videoDisplayActive_ &&
            sameDisplay(window.videoDisplayState_, VideoDisplayState{}) &&
            window.zoomState_ == ZoomState::None && !window.player_.IsVideoCropped() &&
            window.playbackLoop_.Mode() == LoopMode::Off,
            L"new-media reset clears presentation transform together with crop and loop state");

        window.ApplyAreaZoom(selection);
        window.CurrentZoomViewport(video, crop, content);
        window.BeginVideoResize({content.right - 1, content.top + content.Height() / 2});
        window.EndZoomPan();
        fixture.backend.backendState = 7; // libvlc_Error
        window.RefreshControls();
        checks.Expect(!window.videoDisplayActive_ &&
            sameDisplay(window.videoDisplayState_, VideoDisplayState{}) &&
            window.zoomState_ == ZoomState::None && !window.player_.IsVideoCropped(),
            L"polled playback error clears presentation transform and crop");
        ShowWindow(window.window_, SW_HIDE);
    }

    static void Run(int& passed, int& failed) {
        Checks checks{passed, failed};
        ApplicationIcon(checks);
        Ownership(checks);
        ManualToolbarHiding(checks);
        CaptureCancellation(checks);
        MouseFocusRecovery(checks);
        MinimizeAndModal(checks);
        ReparentRollback(checks);
        ZoomAndEscape(checks);
        ZoomNavigation(checks);
        VideoDisplayResize(checks);
        RepeatedIndependentControllers(checks);
        ReentrantCropGetter(checks);
        PlaybackLoopControls(checks);
        RemovalIntervalControls(checks);
        TimelineEditing(checks);
        RemovalOverlapGuards(checks);
        SavedIntervalPainting(checks);
        PlaybackLoopRestartLifecycle(checks);
        SnapshotProgress(checks);
        PreviewRequestReuse(checks);
        PreviewImmediatePresentation(checks);
        PreviewWorkerPresentation(checks);
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
