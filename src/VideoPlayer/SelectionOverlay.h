#pragma once

#include "targetver.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "VideoGeometry.h"

#include <cstdint>
#include <string>

namespace videoplayer {

inline constexpr UINT kSelectionOverlayMessage = WM_APP + 0x35U;

enum class SelectionOverlayEvent : std::uintptr_t {
    Completed = 1U,
    Cancelled,
    CaptureLost,
};

struct SelectionOverlayResult final {
    GeometryRect overlayRect{};
    GeometryRect screenRect{};
};

class SelectionOverlay final {
public:
    SelectionOverlay() = default;
    ~SelectionOverlay();

    SelectionOverlay(const SelectionOverlay&) = delete;
    SelectionOverlay& operator=(const SelectionOverlay&) = delete;
    SelectionOverlay(SelectionOverlay&&) = delete;
    SelectionOverlay& operator=(SelectionOverlay&&) = delete;

    // Creates a hidden, owned, non-activating popup. notificationMessage is
    // delivered synchronously to notificationWindow with SelectionOverlayEvent
    // in wParam. For Completed only, lParam points to a SelectionOverlayResult
    // that the receiver must copy before returning from its window procedure.
    bool Create(
        HINSTANCE instance,
        HWND owner,
        HWND notificationWindow,
        std::wstring& error,
        UINT notificationMessage = kSelectionOverlayMessage);

    // screenBounds uses right/bottom-exclusive screen coordinates and should
    // be the fitted video content rectangle, not the full letterboxed viewport.
    bool Begin(GeometryRect screenBounds, int minimumSelectionPixels) noexcept;

    // Programmatic cancellation (Escape, new media, shutdown). It intentionally
    // sends no notification because the owner initiated the state change.
    void Cancel() noexcept;
    void Destroy() noexcept;

    bool IsCreated() const noexcept;
    bool IsActive() const noexcept;
    bool IsDragging() const noexcept;
    HWND WindowHandle() const noexcept;

private:
    static LRESULT CALLBACK StaticWindowProc(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam);

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    bool RegisterWindowClass(std::wstring& error) const;
    GeometryPoint PointFromMessage(LPARAM lParam) const noexcept;
    GeometryPoint ClampToClient(GeometryPoint point) const noexcept;
    void UpdateDrag(GeometryPoint point) noexcept;
    void FinishDrag(GeometryPoint point) noexcept;
    void FinishCancellation(SelectionOverlayEvent event) noexcept;
    void HideAndReset() noexcept;
    void Notify(SelectionOverlayEvent event, const SelectionOverlayResult* result) const noexcept;
    void Paint() const noexcept;

    HINSTANCE instance_ = nullptr;
    HWND owner_ = nullptr;
    HWND notificationWindow_ = nullptr;
    HWND window_ = nullptr;
    UINT notificationMessage_ = kSelectionOverlayMessage;
    GeometryRect screenBounds_{};
    GeometryPoint dragAnchor_{};
    GeometryPoint dragCurrent_{};
    GeometryRect selection_{};
    int minimumSelectionPixels_ = 1;
    bool active_ = false;
    bool dragging_ = false;
    bool hasSelection_ = false;
};

}  // namespace videoplayer
