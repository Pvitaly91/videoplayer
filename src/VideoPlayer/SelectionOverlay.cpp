#include "targetver.h"
#include "SelectionOverlay.h"

#include <windowsx.h>

#include <algorithm>

namespace videoplayer {
namespace {

constexpr wchar_t kSelectionOverlayClassName[] = L"VideoPlayer.SelectionOverlay";
constexpr wchar_t kSelectionOverlayCreateError[] =
    L"Не вдалося створити вікно вибору області відео.";
constexpr COLORREF kTransparencyKey = RGB(255, 0, 255);
constexpr COLORREF kDimColor = RGB(12, 12, 12);
constexpr COLORREF kBorderColor = RGB(245, 245, 245);
constexpr BYTE kOverlayOpacity = 155U;
constexpr int kBorderWidth = 2;

bool IsClassAlreadyRegistered() noexcept {
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

}  // namespace

SelectionOverlay::~SelectionOverlay() {
    Destroy();
}

bool SelectionOverlay::Create(
    const HINSTANCE instance,
    const HWND owner,
    const HWND notificationWindow,
    std::wstring& error,
    const UINT notificationMessage) {
    if (window_ != nullptr) {
        return true;
    }
    if (instance == nullptr || owner == nullptr || notificationWindow == nullptr ||
        notificationMessage < WM_APP) {
        error = kSelectionOverlayCreateError;
        return false;
    }

    instance_ = instance;
    owner_ = owner;
    notificationWindow_ = notificationWindow;
    notificationMessage_ = notificationMessage;
    if (!RegisterWindowClass(error)) {
        return false;
    }

    window_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kSelectionOverlayClassName,
        L"",
        WS_POPUP,
        0,
        0,
        0,
        0,
        owner_,
        nullptr,
        instance_,
        this);
    if (window_ == nullptr) {
        error = kSelectionOverlayCreateError;
        return false;
    }

    if (SetLayeredWindowAttributes(
            window_, kTransparencyKey, kOverlayOpacity, LWA_COLORKEY | LWA_ALPHA) == FALSE) {
        error = kSelectionOverlayCreateError;
        Destroy();
        return false;
    }
    return true;
}

bool SelectionOverlay::Begin(
    const GeometryRect screenBounds,
    const int minimumSelectionPixels) noexcept {
    if (window_ == nullptr || !IsWindow(window_) || !IsWindow(owner_) ||
        IsWindowVisible(owner_) == FALSE || IsIconic(owner_) != FALSE ||
        IsWindowEnabled(owner_) == FALSE) {
        return false;
    }

    const GeometryRect normalizedBounds = NormalizeRectangle(screenBounds);
    if (normalizedBounds.IsEmpty()) {
        return false;
    }

    HideAndReset();
    screenBounds_ = normalizedBounds;
    minimumSelectionPixels_ = (std::max)(1, minimumSelectionPixels);
    active_ = true;

    const BOOL positioned = SetWindowPos(
        window_,
        HWND_TOP,
        screenBounds_.left,
        screenBounds_.top,
        screenBounds_.Width(),
        screenBounds_.Height(),
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (positioned == FALSE) {
        HideAndReset();
        return false;
    }
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

void SelectionOverlay::Cancel() noexcept {
    HideAndReset();
}

void SelectionOverlay::Destroy() noexcept {
    HideAndReset();
    if (window_ != nullptr && IsWindow(window_)) {
        DestroyWindow(window_);
    }
    window_ = nullptr;
    owner_ = nullptr;
    notificationWindow_ = nullptr;
    instance_ = nullptr;
}

bool SelectionOverlay::IsCreated() const noexcept {
    return window_ != nullptr && IsWindow(window_) != FALSE;
}

bool SelectionOverlay::IsActive() const noexcept {
    return active_;
}

bool SelectionOverlay::IsDragging() const noexcept {
    return dragging_;
}

HWND SelectionOverlay::WindowHandle() const noexcept {
    return window_;
}

LRESULT CALLBACK SelectionOverlay::StaticWindowProc(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    SelectionOverlay* self = reinterpret_cast<SelectionOverlay*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* const create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<SelectionOverlay*>(create->lpCreateParams);
        if (self != nullptr) {
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
    }
    if (self == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return self->HandleMessage(message, wParam, lParam);
}

LRESULT SelectionOverlay::HandleMessage(
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;
    case WM_LBUTTONDOWN:
        if (active_) {
            dragAnchor_ = ClampToClient(PointFromMessage(lParam));
            dragCurrent_ = dragAnchor_;
            selection_ = {};
            hasSelection_ = false;
            dragging_ = true;
            SetCapture(window_);
            if (GetCapture() != window_) {
                dragging_ = false;
                FinishCancellation(SelectionOverlayEvent::CaptureLost);
                return 0;
            }
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (active_ && dragging_) {
            UpdateDrag(PointFromMessage(lParam));
        }
        return 0;
    case WM_LBUTTONUP:
        if (active_ && dragging_) {
            FinishDrag(PointFromMessage(lParam));
        }
        return 0;
    case WM_RBUTTONDOWN:
        if (active_) {
            FinishCancellation(SelectionOverlayEvent::Cancelled);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (active_ && dragging_ && reinterpret_cast<HWND>(lParam) != window_) {
            dragging_ = false;
            FinishCancellation(SelectionOverlayEvent::CaptureLost);
        }
        return 0;
    case WM_CANCELMODE:
        if (active_) {
            FinishCancellation(SelectionOverlayEvent::Cancelled);
        }
        return 0;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_CLOSE:
        if (active_) {
            FinishCancellation(SelectionOverlayEvent::Cancelled);
        }
        return 0;
    case WM_NCDESTROY: {
        const HWND destroyedWindow = window_;
        SetWindowLongPtrW(destroyedWindow, GWLP_USERDATA, 0);
        const LRESULT result = DefWindowProcW(destroyedWindow, message, wParam, lParam);
        window_ = nullptr;
        active_ = false;
        dragging_ = false;
        hasSelection_ = false;
        return result;
    }
    default:
        break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

bool SelectionOverlay::RegisterWindowClass(std::wstring& error) const {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = StaticWindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    windowClass.lpszClassName = kSelectionOverlayClassName;
    if (RegisterClassExW(&windowClass) == 0 && !IsClassAlreadyRegistered()) {
        error = kSelectionOverlayCreateError;
        return false;
    }
    return true;
}

GeometryPoint SelectionOverlay::PointFromMessage(const LPARAM lParam) const noexcept {
    return {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
}

GeometryPoint SelectionOverlay::ClampToClient(GeometryPoint point) const noexcept {
    RECT client{};
    if (window_ == nullptr || GetClientRect(window_, &client) == FALSE) {
        return {};
    }
    const int clientLeft = static_cast<int>(client.left);
    const int clientTop = static_cast<int>(client.top);
    const int clientRight = static_cast<int>(client.right);
    const int clientBottom = static_cast<int>(client.bottom);
    point.x = (std::max)(clientLeft, (std::min)(point.x, clientRight));
    point.y = (std::max)(clientTop, (std::min)(point.y, clientBottom));
    return point;
}

void SelectionOverlay::UpdateDrag(const GeometryPoint point) noexcept {
    dragCurrent_ = ClampToClient(point);
    selection_ = NormalizeRectangle(dragAnchor_, dragCurrent_);
    hasSelection_ = !selection_.IsEmpty();
    InvalidateRect(window_, nullptr, FALSE);
}

void SelectionOverlay::FinishDrag(const GeometryPoint point) noexcept {
    UpdateDrag(point);
    dragging_ = false;
    if (GetCapture() == window_) {
        ReleaseCapture();
    }

    if (selection_.Width() < minimumSelectionPixels_ ||
        selection_.Height() < minimumSelectionPixels_) {
        FinishCancellation(SelectionOverlayEvent::Cancelled);
        return;
    }

    SelectionOverlayResult result{};
    result.overlayRect = selection_;
    result.screenRect = OffsetRectangle(
        selection_, screenBounds_.left, screenBounds_.top);
    HideAndReset();
    Notify(SelectionOverlayEvent::Completed, &result);
}

void SelectionOverlay::FinishCancellation(const SelectionOverlayEvent event) noexcept {
    HideAndReset();
    Notify(event, nullptr);
}

void SelectionOverlay::HideAndReset() noexcept {
    active_ = false;
    dragging_ = false;
    hasSelection_ = false;
    selection_ = {};
    if (window_ != nullptr && GetCapture() == window_) {
        ReleaseCapture();
    }
    if (window_ != nullptr && IsWindowVisible(window_) != FALSE) {
        ShowWindow(window_, SW_HIDE);
    }
}

void SelectionOverlay::Notify(
    const SelectionOverlayEvent event,
    const SelectionOverlayResult* const result) const noexcept {
    if (notificationWindow_ == nullptr || IsWindow(notificationWindow_) == FALSE) {
        return;
    }
    SendMessageW(
        notificationWindow_,
        notificationMessage_,
        static_cast<WPARAM>(event),
        reinterpret_cast<LPARAM>(result));
}

void SelectionOverlay::Paint() const noexcept {
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(window_, &paint);
    if (dc == nullptr) {
        return;
    }

    RECT client{};
    GetClientRect(window_, &client);
    const HBRUSH dimBrush = CreateSolidBrush(kDimColor);
    if (dimBrush != nullptr) {
        FillRect(dc, &client, dimBrush);
        DeleteObject(dimBrush);
    }

    if (hasSelection_ && !selection_.IsEmpty()) {
        RECT selectionRect{
            selection_.left,
            selection_.top,
            selection_.right,
            selection_.bottom};
        const HBRUSH transparentBrush = CreateSolidBrush(kTransparencyKey);
        if (transparentBrush != nullptr) {
            FillRect(dc, &selectionRect, transparentBrush);
            DeleteObject(transparentBrush);
        }

        const HPEN borderPen = CreatePen(PS_SOLID, kBorderWidth, kBorderColor);
        if (borderPen != nullptr) {
            const HGDIOBJ previousPen = SelectObject(dc, borderPen);
            const HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(
                dc,
                selectionRect.left,
                selectionRect.top,
                selectionRect.right,
                selectionRect.bottom);
            SelectObject(dc, previousBrush);
            SelectObject(dc, previousPen);
            DeleteObject(borderPen);
        }
    }
    EndPaint(window_, &paint);
}

}  // namespace videoplayer
