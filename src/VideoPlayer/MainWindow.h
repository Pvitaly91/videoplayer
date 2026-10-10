#pragma once

#include "targetver.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "PlayerEngine.h"
#include "ExportTypes.h"
#include "PlaybackLoop.h"
#include "PreviewEngine.h"
#include "PreviewPopup.h"
#include "SelectionOverlay.h"
#include "VideoGeometry.h"
#include "VideoDisplayGeometry.h"
#include "ZoomState.h"

#include <cstdint>
#include <string>
#include <vector>

namespace videoplayer {

class MainWindow final {
public:
    MainWindow() = default;
#if defined(VIDEOPLAYER_TESTING)
    explicit MainWindow(PreviewBackendFactory previewBackendFactory);
#endif
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool Create(HINSTANCE instance, std::wstring& error);
    void Show(int showCommand) const;
    int RunMessageLoop();
    bool OpenFile(const std::wstring& path);
    void OpenFiles(const std::vector<std::wstring>& paths);

private:
    friend struct MainWindowTestAccess;
    using SetParentFunction = HWND (WINAPI*)(HWND, HWND);
    static LRESULT CALLBACK StaticWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK StaticVideoProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK StaticControlBarProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK StaticToolbarRestoreProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK StaticProgressStripProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK StaticSeekSubclass(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId,
        DWORD_PTR referenceData);

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleVideoMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleControlBarMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleToolbarRestoreMessage(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleProgressStripMessage(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleSeekSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void DrawSavedIntervals(HDC dc, const RECT& timeline) const noexcept;
    void DrawCompactProgress(HDC dc, const RECT& bounds) const noexcept;
    void InvalidateIntervalTimeline() noexcept;

    bool RegisterWindowClasses(std::wstring& error) const;
    bool CreateChildWindows();
    void ApplySystemFont();
    void LayoutChildren(int width, int height);
    int Scale(int value) const noexcept;
    HWND ActiveControlBar() const noexcept;
    bool ReparentControlBarControls(HWND parent) noexcept;
    void ApplyFullscreenControlBarOpacity() noexcept;
    void UpdateProgressStrip() noexcept;
    void SetToolbarRestoreVisible(bool visible) noexcept;
    void UpdateToolbarVisibilityTimer() noexcept;
    bool CanShowOwnedPopups() const noexcept;
    void CancelTransientUi() noexcept;
    void RestoreUiAfterLifecycle();

    bool ProcessKeyboardMessage(const MSG& message);
    bool ProcessZoomMouseMessage(const MSG& message);
    bool HandleHotKey(UINT key, bool controlDown, bool repeated);
    void ShowOpenDialog();
    void ShowExportVideoDialog();
    void ShowRemoveSegmentVideoDialog();
    void AddRemovalInterval();
    bool CommitSourceRemoval(const ExportSnapshot& snapshot,
        const std::wstring& partialPath, std::wstring& error);
    void HandleDroppedFiles(HDROP drop);

    void TogglePlayback();
    void ToggleWholeVideoLoop();
    void SetLoopPoint(bool start);
    void ToggleSegmentLoop();
    void ClearLoopSegment();
    void ApplyPlaybackLoop();
    void UpdateLoopControls();
    void StopPlayback();
    void SeekBy(std::int64_t deltaMs);
    void CommitSeekFromSlider();
    void UpdateSeekLabel();
    bool SetSeekSliderFromPointer(int mouseX);
    void HandleSeekPointer(int mouseX);
    bool CanReusePreviewRequest(std::uint32_t generation, std::int64_t quantizedTimestamp) const noexcept;
    void ShowPendingSeekPreview(std::int64_t durationMs);
    void HideSeekPreview(bool cancelRequest = true) noexcept;
    void HandlePreviewResult();
    bool TryShowPreviewResult(bool requireVisible);
    bool PresentPreviewResult(const PreviewResult& result, bool requireVisible);
    void SetVolumeFromSlider(int volume);
    void AdjustVolume(int delta);
    void ToggleMute();

    void ToggleFullscreen();
    void EnterFullscreen();
    void ExitFullscreen();
    void ToggleManualToolbarVisibility();
    void SetToolbarManuallyHidden(bool hidden);
    void SetControlBarVisible(bool visible);
    void RegisterInteraction(bool pointerMoved = false);
    void UpdateControlBarVisibility(bool pointerMoved = false);
    void UpdateControlBarVisibilityFromCursor(
        bool pointerMoved, bool haveCursor, POINT cursorScreen);
    void FinishMouseControlInteraction() noexcept;
    void RestoreVideoFocusAfterMouseInteraction() noexcept;

    void ToggleAreaZoom();
    void BeginAreaZoom();
    bool ApplyAreaZoom(const VideoCrop& crop);
    bool ApplyZoomNavigation(const VideoCrop& crop);
    void ResetAreaZoom();
    void HandleZoomEscape();
    void HandleSelectionOverlay(SelectionOverlayEvent event, const SelectionOverlayResult* result);
    bool CurrentVideoViewport(VideoDimensions& video, GeometryRect& viewportScreen) const noexcept;
    bool CurrentZoomViewport(
        VideoDimensions& video, VideoCrop& crop, GeometryRect& contentScreen) const noexcept;
    bool IsZoomPointerTarget(POINT screenPoint, const GeometryRect& contentScreen) const noexcept;
    bool PanZoomByKeyboard(UINT key);
    bool HandleZoomWheel(int wheelDelta, POINT screenPoint);
    bool BeginZoomPan(POINT screenPoint);
    void UpdateZoomPan(POINT screenPoint);
    void EndZoomPan() noexcept;
    bool ZoomGestureActive() const noexcept;
    void UpdateVideoDisplayLayout() noexcept;
    void ResetVideoDisplay() noexcept;
    VideoEdge VideoResizeEdgeAt(POINT screenPoint) const noexcept;
    bool BeginVideoResize(POINT screenPoint);
    void UpdateVideoResize(POINT screenPoint);
    void SetVideoPointerCursor(POINT screenPoint) const noexcept;
    bool ApplyDisplayPan(VideoDimensions video, VideoCrop crop,
        VideoDisplayState state, GeometryRect viewport, GeometryRect content,
        double deltaX, double deltaY);

    void HandlePlayerEvent(PlayerEvent event, std::uint32_t generation);
    void RefreshControls();
    void RenderPlaybackSnapshot();
    void UpdateExecutionState(bool playing);
    void RestoreExecutionState();
    void ShowMediaError();
    void SetPromptVisible(bool visible);
    void UpdateWindowTitle();
    void ResetMediaUiState() noexcept;
    void ShutdownPlaybackComponents() noexcept;

    static int TimeToSlider(std::int64_t positionMs, std::int64_t durationMs) noexcept;
    static std::int64_t SliderToTime(int sliderPosition, std::int64_t durationMs) noexcept;

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND videoWindow_ = nullptr;
    // Fixed outer viewport clips this independently sized LibVLC render host.
    HWND videoRenderWindow_ = nullptr;
    HWND controlBar_ = nullptr;
    HWND fullscreenControlBar_ = nullptr;
    HWND progressStrip_ = nullptr;
    HWND toolbarRestorePopup_ = nullptr;
    HWND toolbarRestoreButton_ = nullptr;
    HWND openButton_ = nullptr;
    HWND playButton_ = nullptr;
    HWND stopButton_ = nullptr;
    HWND exportButton_ = nullptr;
    HWND removeSegmentButton_ = nullptr;
    HWND addRemovalButton_ = nullptr;
    HWND removalCountLabel_ = nullptr;
    HWND seekSlider_ = nullptr;
    HWND currentTimeLabel_ = nullptr;
    HWND durationLabel_ = nullptr;
    HWND muteButton_ = nullptr;
    HWND volumeSlider_ = nullptr;
    HWND zoomButton_ = nullptr;
    HWND fullscreenButton_ = nullptr;
    HWND hideToolbarButton_ = nullptr;
    HWND loopVideoButton_ = nullptr;
    HWND loopStartButton_ = nullptr;
    HWND loopEndButton_ = nullptr;
    HWND loopSegmentButton_ = nullptr;
    HWND loopClearButton_ = nullptr;
    HWND loopRangeLabel_ = nullptr;
    HFONT uiFont_ = nullptr;
    bool ownsFont_ = false;
    int dpi_ = 96;

    PlayerEngine player_;
    PlaybackSnapshot playbackSnapshot_{};
    PlaybackLoop playbackLoop_;
    std::vector<VideoInterval> removalIntervals_;
    std::int64_t timelineDurationMs_ = 0;
    int progressCompletedWidth_ = 0;
    bool applyingPlaybackLoop_ = false;
    bool loopPlaybackRequested_ = false;
    PreviewEngine previewEngine_;
    PreviewPopup previewPopup_;
    SelectionOverlay selectionOverlay_;
    bool playerInitialized_ = false;
    bool previewInitialized_ = false;
    bool hasMedia_ = false;
    bool promptVisible_ = true;
    bool seekDragging_ = false;
    bool seekMouseTracking_ = false;
    bool volumeDragging_ = false;
    bool mediaErrorShown_ = false;
    bool muted_ = false;
    int displayedVolume_ = 100;
    int lastNonZeroVolume_ = 100;
    std::wstring currentPath_;
    std::wstring creationError_;

    std::uint64_t previewRequestId_ = 0;
    std::uint64_t previewDisplayedRequestId_ = 0;
    std::uint32_t previewRequestGeneration_ = 0;
    std::int64_t previewRequestTimestampMs_ = -1;
    std::int64_t previewHoverTimeMs_ = 0;
    POINT previewAnchorScreen_{};
    PreviewFramePtr previewDisplayedFrame_;

    ZoomState zoomState_ = ZoomState::None;
    // The latest explicitly edited area proportions survive pan/wheel updates.
    VideoCrop zoomSelectionCrop_{};
    VideoDimensions selectionVideo_{};
    GeometryRect selectionViewportScreen_{};
    bool zoomPanDragging_ = false;
    POINT zoomPanAnchorScreen_{};
    VideoCrop zoomPanStartCrop_{};
    VideoDimensions zoomPanVideo_{};
    GeometryRect zoomPanContentScreen_{};
    GeometryRect zoomPanViewportScreen_{};
    VideoDisplayState zoomPanDisplayState_{};
    int zoomWheelRemainder_ = 0;
    bool videoDisplayActive_ = false;
    VideoDisplayState videoDisplayState_{};
    bool videoResizeDragging_ = false;
    VideoEdge videoResizeEdge_ = VideoEdge::None;
    GeometryPoint videoResizeAnchor_{};
    GeometryRect videoResizeViewport_{};
    GeometryRect videoResizeContent_{};
    VideoDimensions videoResizeVideo_{};
    VideoCrop videoResizeStartCrop_{};
    VideoCrop videoResizeAspectReference_{};
    VideoDisplayState videoResizeStartState_{};

    bool fullscreen_ = false;
    bool controlBarVisible_ = true;
    bool toolbarManuallyHidden_ = false;
    bool toolbarWasFullscreen_ = false;
    std::uint64_t lastInteractionMs_ = 0;
    POINT lastCursorScreen_{};
    bool hasLastCursor_ = false;
    bool mouseControlInteraction_ = false;
    LONG_PTR savedStyle_ = 0;
    LONG_PTR savedExtendedStyle_ = 0;
    WINDOWPLACEMENT savedPlacement_{sizeof(WINDOWPLACEMENT)};
    bool executionStateActive_ = false;
    bool shutdownComplete_ = false;
    bool applicationActive_ = true;
    bool minimized_ = false;
    bool cancellingTransient_ = false;
    bool restoringUi_ = false;
    bool pendingVideoRefocus_ = false;
    SetParentFunction setParent_ = ::SetParent;
};

}  // namespace videoplayer
