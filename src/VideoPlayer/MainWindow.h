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
#include "PreviewEngine.h"
#include "PreviewPopup.h"
#include "SelectionOverlay.h"
#include "VideoGeometry.h"
#include "ZoomState.h"

#include <cstdint>
#include <string>

namespace videoplayer {

class MainWindow final {
public:
    MainWindow() = default;
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    bool Create(HINSTANCE instance, std::wstring& error);
    void Show(int showCommand) const;
    int RunMessageLoop();
    bool OpenFile(const std::wstring& path);

private:
    static LRESULT CALLBACK StaticWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK StaticVideoProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK StaticControlBarProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
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
    LRESULT HandleSeekSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    bool RegisterWindowClasses(std::wstring& error) const;
    bool CreateChildWindows();
    void ApplySystemFont();
    void LayoutChildren(int width, int height);
    int Scale(int value) const noexcept;

    bool ProcessKeyboardMessage(const MSG& message);
    bool HandleHotKey(UINT key, bool controlDown, bool repeated);
    void ShowOpenDialog();
    void HandleDroppedFiles(HDROP drop);

    void TogglePlayback();
    void StopPlayback();
    void SeekBy(std::int64_t deltaMs);
    void CommitSeekFromSlider();
    void UpdateSeekLabel();
    void HandleSeekPointer(int mouseX);
    void HideSeekPreview(bool cancelRequest = true) noexcept;
    void HandlePreviewResult();
    void SetVolumeFromSlider(int volume);
    void AdjustVolume(int delta);
    void ToggleMute();

    void ToggleFullscreen();
    void EnterFullscreen();
    void ExitFullscreen();
    void SetControlBarVisible(bool visible);
    void RegisterInteraction(bool pointerMoved = false);
    void UpdateControlBarVisibility(bool pointerMoved = false);
    void FinishMouseControlInteraction() noexcept;

    void ToggleAreaZoom();
    void BeginAreaZoom();
    void ResetAreaZoom();
    void HandleZoomEscape();
    void HandleSelectionOverlay(SelectionOverlayEvent event, const SelectionOverlayResult* result);
    bool CurrentVideoViewport(VideoDimensions& video, GeometryRect& viewportScreen) const noexcept;

    void HandlePlayerEvent(PlayerEvent event, std::uint32_t generation);
    void RefreshControls();
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
    HWND controlBar_ = nullptr;
    HWND openButton_ = nullptr;
    HWND playButton_ = nullptr;
    HWND stopButton_ = nullptr;
    HWND seekSlider_ = nullptr;
    HWND currentTimeLabel_ = nullptr;
    HWND durationLabel_ = nullptr;
    HWND muteButton_ = nullptr;
    HWND volumeSlider_ = nullptr;
    HWND zoomButton_ = nullptr;
    HWND fullscreenButton_ = nullptr;
    HFONT uiFont_ = nullptr;
    bool ownsFont_ = false;
    int dpi_ = 96;

    PlayerEngine player_;
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
    VideoDimensions selectionVideo_{};
    GeometryRect selectionViewportScreen_{};

    bool fullscreen_ = false;
    bool controlBarVisible_ = true;
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
};

}  // namespace videoplayer
