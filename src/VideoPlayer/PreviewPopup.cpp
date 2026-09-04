#include "PreviewPopup.h"

#include "PreviewMath.h"
#include "TimeFormatter.h"

#include <algorithm>
#include <limits>
#include <string>

namespace videoplayer {
namespace {

constexpr wchar_t kPreviewPopupClassName[] = L"VideoPlayer.PreviewPopup";
constexpr COLORREF kPopupBackground = RGB(18, 18, 20);
constexpr COLORREF kImageBackground = RGB(5, 5, 6);
constexpr COLORREF kBorderColor = RGB(205, 205, 210);
constexpr COLORREF kTextColor = RGB(245, 245, 247);
constexpr COLORREF kPlaceholderColor = RGB(175, 175, 180);

std::wstring Win32Error(const wchar_t* prefix) {
    return std::wstring(prefix) + L" (Win32 error " +
        std::to_wstring(::GetLastError()) + L").";
}

void FillSolidRect(const HDC dc, const RECT& rectangle, const COLORREF color) noexcept {
    const HBRUSH brush = ::CreateSolidBrush(color);
    if (brush != nullptr) {
        ::FillRect(dc, &rectangle, brush);
        ::DeleteObject(brush);
    }
}

}  // namespace

PreviewPopup::~PreviewPopup() {
    Destroy();
}

bool PreviewPopup::Create(
    const HWND owner,
    const HINSTANCE instance,
    std::wstring& error) noexcept {
    error.clear();
    if (window_ != nullptr) {
        return true;
    }
    if (owner == nullptr || !::IsWindow(owner)) {
        error = L"PreviewPopup requires a valid owner window.";
        return false;
    }

    try {
        owner_ = owner;
        instance_ = instance != nullptr ? instance :
            reinterpret_cast<HINSTANCE>(::GetModuleHandleW(nullptr));
        if (!RegisterWindowClass(error)) {
            owner_ = nullptr;
            instance_ = nullptr;
            return false;
        }

        window_ = ::CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            kPreviewPopupClassName,
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
            error = Win32Error(L"Could not create the seek preview popup");
            owner_ = nullptr;
            instance_ = nullptr;
            return false;
        }
        return true;
    } catch (...) {
        error = L"Unexpected error while creating the seek preview popup.";
        Destroy();
        return false;
    }
}

void PreviewPopup::ShowPlaceholderAt(
    const POINT anchorScreen,
    const int dpi,
    const std::int64_t timestampMs) noexcept {
    frame_.reset();
    timestampMs_ = (std::max)(std::int64_t{0}, timestampMs);
    PositionAndShow(anchorScreen, dpi);
}

void PreviewPopup::ShowFrameAt(
    const POINT anchorScreen,
    const int dpi,
    const std::int64_t timestampMs,
    PreviewFramePtr frame) noexcept {
    frame_ = frame && frame->IsValid() &&
            frame->width <= kPreviewMaximumWidth &&
            frame->height <= kPreviewMaximumHeight
        ? std::move(frame)
        : PreviewFramePtr{};
    timestampMs_ = (std::max)(std::int64_t{0}, timestampMs);
    PositionAndShow(anchorScreen, dpi);
}

void PreviewPopup::Hide() noexcept {
    if (window_ != nullptr && ::IsWindowVisible(window_)) {
        ::ShowWindow(window_, SW_HIDE);
    }
    frame_.reset();
}

void PreviewPopup::Destroy() noexcept {
    frame_.reset();
    if (window_ != nullptr) {
        const HWND window = window_;
        if (::IsWindow(window)) {
            ::DestroyWindow(window);
        }
        if (window_ == window) {
            window_ = nullptr;
        }
    }
    owner_ = nullptr;
    instance_ = nullptr;
}

bool PreviewPopup::IsVisible() const noexcept {
    return window_ != nullptr && ::IsWindowVisible(window_) != FALSE;
}

HWND PreviewPopup::Window() const noexcept {
    return window_;
}

LRESULT CALLBACK PreviewPopup::StaticWindowProc(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) noexcept {
    PreviewPopup* self = reinterpret_cast<PreviewPopup*>(
        ::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = create != nullptr
            ? static_cast<PreviewPopup*>(create->lpCreateParams)
            : nullptr;
        if (self != nullptr) {
            self->window_ = window;
            ::SetWindowLongPtrW(
                window,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(self));
        }
    }
    if (self == nullptr) {
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
    const LRESULT result = self->HandleMessage(message, wParam, lParam);
    if (message == WM_NCDESTROY) {
        ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        if (self->window_ == window) {
            self->window_ = nullptr;
        }
    }
    return result;
}

LRESULT PreviewPopup::HandleMessage(
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) noexcept {
    switch (message) {
    case WM_PAINT:
        Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    default:
        return ::DefWindowProcW(window_, message, wParam, lParam);
    }
}

bool PreviewPopup::RegisterWindowClass(std::wstring& error) noexcept {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    if (::GetClassInfoExW(instance_, kPreviewPopupClassName, &windowClass) != FALSE) {
        return true;
    }

    windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &PreviewPopup::StaticWindowProc;
    windowClass.hInstance = instance_;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kPreviewPopupClassName;
    if (::RegisterClassExW(&windowClass) == 0 &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error = Win32Error(L"Could not register the seek preview popup class");
        return false;
    }
    return true;
}

void PreviewPopup::PositionAndShow(const POINT anchorScreen, const int dpi) noexcept {
    if (window_ == nullptr) {
        return;
    }
    dpi_ = dpi > 0 ? dpi : USER_DEFAULT_SCREEN_DPI;

    const int padding = (std::max)(1, ScalePreviewPixels(6, dpi_));
    const int imageWidth = (std::max)(1, ScalePreviewPixels(320, dpi_));
    const int imageHeight = (std::max)(1, ScalePreviewPixels(180, dpi_));
    const int timestampHeight = (std::max)(1, ScalePreviewPixels(26, dpi_));
    const SIZE popupSize{
        imageWidth + (2 * padding),
        imageHeight + timestampHeight + (2 * padding)};

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    const HMONITOR monitor = ::MonitorFromPoint(anchorScreen, MONITOR_DEFAULTTONEAREST);
    if (::GetMonitorInfoW(monitor, &monitorInfo) == FALSE) {
        monitorInfo.rcMonitor = RECT{
            0,
            0,
            ::GetSystemMetrics(SM_CXSCREEN),
            ::GetSystemMetrics(SM_CYSCREEN)};
    }
    const RECT popup = PlacePreviewPopup(
        anchorScreen,
        popupSize,
        monitorInfo.rcMonitor,
        (std::max)(1, ScalePreviewPixels(8, dpi_)));
    ::SetWindowPos(
        window_,
        HWND_TOP,
        popup.left,
        popup.top,
        popup.right - popup.left,
        popup.bottom - popup.top,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    ::InvalidateRect(window_, nullptr, FALSE);
    if (!::IsWindowVisible(window_)) {
        ::ShowWindow(window_, SW_SHOWNOACTIVATE);
    }
}

void PreviewPopup::Paint() noexcept {
    if (window_ == nullptr) {
        return;
    }
    PAINTSTRUCT paint{};
    const HDC dc = ::BeginPaint(window_, &paint);
    if (dc == nullptr) {
        return;
    }

    RECT client{};
    ::GetClientRect(window_, &client);
    FillSolidRect(dc, client, kPopupBackground);

    const int border = (std::max)(1, ScalePreviewPixels(1, dpi_));
    const int padding = (std::max)(1, ScalePreviewPixels(6, dpi_));
    const int timestampHeight = (std::max)(1, ScalePreviewPixels(26, dpi_));
    RECT imageArea{
        client.left + padding,
        client.top + padding,
        client.right - padding,
        client.bottom - padding - timestampHeight};
    FillSolidRect(dc, imageArea, kImageBackground);

    if (frame_ && frame_->IsValid()) {
        const SIZE fitted = FitPreviewSize(
            frame_->width,
            frame_->height,
            static_cast<unsigned int>((std::max)(0L, imageArea.right - imageArea.left)),
            static_cast<unsigned int>((std::max)(0L, imageArea.bottom - imageArea.top)));
        const int destinationWidth = fitted.cx;
        const int destinationHeight = fitted.cy;
        const int destinationX = imageArea.left +
            ((imageArea.right - imageArea.left - destinationWidth) / 2);
        const int destinationY = imageArea.top +
            ((imageArea.bottom - imageArea.top - destinationHeight) / 2);

        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(bitmap.bmiHeader);
        bitmap.bmiHeader.biWidth = static_cast<LONG>(frame_->width);
        bitmap.bmiHeader.biHeight = frame_->topDown
            ? -static_cast<LONG>(frame_->height)
            : static_cast<LONG>(frame_->height);
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        ::SetStretchBltMode(dc, HALFTONE);
        ::SetBrushOrgEx(dc, 0, 0, nullptr);
        ::StretchDIBits(
            dc,
            destinationX,
            destinationY,
            destinationWidth,
            destinationHeight,
            0,
            0,
            static_cast<int>(frame_->width),
            static_cast<int>(frame_->height),
            frame_->pixels.data(),
            &bitmap,
            DIB_RGB_COLORS,
            SRCCOPY);
    } else {
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, kPlaceholderColor);
        const HFONT font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
        const HGDIOBJ oldFont = font != nullptr ? ::SelectObject(dc, font) : nullptr;
        RECT placeholder = imageArea;
        ::DrawTextW(
            dc,
            L"Очікування кадру…",
            -1,
            &placeholder,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (oldFont != nullptr) {
            ::SelectObject(dc, oldFont);
        }
    }

    RECT timestampArea{
        client.left + padding,
        imageArea.bottom,
        client.right - padding,
        client.bottom - padding};
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, kTextColor);
    const HFONT font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    const HGDIOBJ oldFont = font != nullptr ? ::SelectObject(dc, font) : nullptr;
    const std::wstring timestamp = FormatTime(timestampMs_);
    ::DrawTextW(
        dc,
        timestamp.c_str(),
        static_cast<int>((std::min)(
            timestamp.size(),
            static_cast<std::size_t>((std::numeric_limits<int>::max)()))),
        &timestampArea,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (oldFont != nullptr) {
        ::SelectObject(dc, oldFont);
    }

    const HBRUSH borderBrush = ::CreateSolidBrush(kBorderColor);
    if (borderBrush != nullptr) {
        for (int index = 0; index < border; ++index) {
            RECT outline{
                client.left + index,
                client.top + index,
                client.right - index,
                client.bottom - index};
            ::FrameRect(dc, &outline, borderBrush);
        }
        ::DeleteObject(borderBrush);
    }
    ::EndPaint(window_, &paint);
}

}  // namespace videoplayer
