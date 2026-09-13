#pragma once

#include "PreviewFrame.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <string>

namespace videoplayer {

// UI-thread-only owned popup. WS_EX_TOOLWINDOW and an owner keep it out of the
// taskbar and Alt+Tab; WS_EX_NOACTIVATE prevents seek preview from taking focus.
class PreviewPopup final {
public:
    PreviewPopup() noexcept = default;
    ~PreviewPopup();

    PreviewPopup(const PreviewPopup&) = delete;
    PreviewPopup& operator=(const PreviewPopup&) = delete;

    bool Create(HWND owner, HINSTANCE instance, std::wstring& error) noexcept;
    void ShowPlaceholderAt(
        POINT anchorScreen,
        int dpi,
        std::int64_t timestampMs,
        bool pending = true) noexcept;
    void ShowFrameAt(
        POINT anchorScreen,
        int dpi,
        std::int64_t timestampMs,
        PreviewFramePtr frame,
        bool updating = false) noexcept;
    void Hide() noexcept;
    void Destroy() noexcept;

    bool IsVisible() const noexcept;
    bool IsUpdating() const noexcept { return updating_; }
    bool IsWaitingForFrame() const noexcept { return !frame_ && placeholderPending_; }
    HWND Window() const noexcept;

private:
    static LRESULT CALLBACK StaticWindowProc(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam) noexcept;
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) noexcept;

    bool RegisterWindowClass(std::wstring& error) noexcept;
    void PositionAndShow(POINT anchorScreen, int dpi) noexcept;
    void Paint() noexcept;

    HINSTANCE instance_ = nullptr;
    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    PreviewFramePtr frame_;
    bool updating_ = false;
    bool placeholderPending_ = false;
    std::int64_t timestampMs_ = 0;
    int dpi_ = USER_DEFAULT_SCREEN_DPI;
};

}  // namespace videoplayer
