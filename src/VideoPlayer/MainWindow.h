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

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleVideoMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    bool RegisterWindowClasses(std::wstring& error) const;
    bool CreateChildWindows();
    void ApplySystemFont();
    void LayoutChildren(int width, int height) const;
    int Scale(int value) const noexcept;

    bool ProcessKeyboardMessage(const MSG& message);
    bool HandleHotKey(UINT key, bool controlDown, bool repeated);
    void ShowOpenDialog();
    void HandleDroppedFiles(HDROP drop);

    void TogglePlayback();
    void StopPlayback();
    void SeekBy(std::int64_t deltaMs);
    void CommitSeekFromSlider();
    void UpdateSeekPreview();
    void SetVolumeFromSlider(int volume);
    void AdjustVolume(int delta);
    void ToggleMute();
    void ToggleFullscreen();
    void EnterFullscreen();
    void ExitFullscreen();

    void HandlePlayerEvent(PlayerEvent event, std::uint32_t generation);
    void RefreshControls();
    void UpdateExecutionState(bool playing);
    void RestoreExecutionState();
    void ShowMediaError();
    void SetPromptVisible(bool visible);
    void UpdateWindowTitle();

    static int TimeToSlider(std::int64_t positionMs, std::int64_t durationMs) noexcept;
    static std::int64_t SliderToTime(int sliderPosition, std::int64_t durationMs) noexcept;

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND videoWindow_ = nullptr;
    HWND openButton_ = nullptr;
    HWND playButton_ = nullptr;
    HWND stopButton_ = nullptr;
    HWND seekSlider_ = nullptr;
    HWND currentTimeLabel_ = nullptr;
    HWND durationLabel_ = nullptr;
    HWND muteButton_ = nullptr;
    HWND volumeSlider_ = nullptr;
    HWND fullscreenButton_ = nullptr;
    HFONT uiFont_ = nullptr;
    bool ownsFont_ = false;
    int dpi_ = 96;

    PlayerEngine player_;
    bool playerInitialized_ = false;
    bool hasMedia_ = false;
    bool promptVisible_ = true;
    bool seekDragging_ = false;
    bool mediaErrorShown_ = false;
    bool muted_ = false;
    int displayedVolume_ = 100;
    int lastNonZeroVolume_ = 100;
    std::wstring currentPath_;
    std::wstring creationError_;

    bool fullscreen_ = false;
    LONG_PTR savedStyle_ = 0;
    LONG_PTR savedExtendedStyle_ = 0;
    WINDOWPLACEMENT savedPlacement_{sizeof(WINDOWPLACEMENT)};
    bool executionStateActive_ = false;
};

}  // namespace videoplayer
