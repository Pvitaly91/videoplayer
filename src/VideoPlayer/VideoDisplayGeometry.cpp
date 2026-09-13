#include "VideoDisplayGeometry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace videoplayer {
namespace {

constexpr double kMinimumScale = 0.25;
constexpr double kMaximumScale = 8.0;
constexpr int kMaximumCanvasDimension = 16'384;

int ScaledExtent(const int fittedExtent, const double scale) noexcept {
    if (fittedExtent <= 0 || !std::isfinite(scale) || scale <= 0.0) {
        return 0;
    }
    const double requested = static_cast<double>(fittedExtent) * scale;
    if (!std::isfinite(requested) ||
        requested >= static_cast<double>(kMaximumCanvasDimension)) {
        return kMaximumCanvasDimension;
    }
    return (std::clamp)(
        static_cast<int>(std::floor(requested + 0.5)),
        1,
        kMaximumCanvasDimension);
}

double MaximumCanvasScale(const GeometryRect fitted) noexcept {
    const int width = fitted.Width();
    const int height = fitted.Height();
    if (width <= 0 || height <= 0) {
        return 0.0;
    }
    return (std::min)({
        kMaximumScale,
        static_cast<double>(kMaximumCanvasDimension) / width,
        static_cast<double>(kMaximumCanvasDimension) / height});
}

double NormalizeCenter(
    const double requested,
    const int displayExtent,
    const int viewportExtent) noexcept {
    if (displayExtent <= viewportExtent) {
        return 0.5;
    }
    double center = std::isfinite(requested) ? requested : 0.5;
    center = (std::clamp)(center, 0.0, 1.0);
    const double minimum = static_cast<double>(viewportExtent) /
        (2.0 * static_cast<double>(displayExtent));
    return (std::clamp)(center, minimum, 1.0 - minimum);
}

std::int64_t FloorToInt64(const double value) noexcept {
    if (!std::isfinite(value)) {
        return 0;
    }
    if (value >= static_cast<double>((std::numeric_limits<std::int64_t>::max)())) {
        return (std::numeric_limits<std::int64_t>::max)();
    }
    if (value <= static_cast<double>((std::numeric_limits<std::int64_t>::min)())) {
        return (std::numeric_limits<std::int64_t>::min)();
    }
    return static_cast<std::int64_t>(std::floor(value));
}

int SaturateToInt(const std::int64_t value) noexcept {
    if (value > static_cast<std::int64_t>((std::numeric_limits<int>::max)())) {
        return (std::numeric_limits<int>::max)();
    }
    if (value < static_cast<std::int64_t>((std::numeric_limits<int>::min)())) {
        return (std::numeric_limits<int>::min)();
    }
    return static_cast<int>(value);
}

std::int64_t ClampOrigin(
    std::int64_t origin,
    const int viewportLow,
    const int viewportHigh,
    const int viewportExtent,
    const int displayExtent) noexcept {
    if (displayExtent < viewportExtent) {
        return static_cast<std::int64_t>(viewportLow) +
            (viewportExtent - displayExtent) / 2;
    }
    const std::int64_t lowest =
        static_cast<std::int64_t>(viewportHigh) - displayExtent;
    const std::int64_t highest = viewportLow;
    return (std::clamp)(origin, lowest, highest);
}

bool PointInsideViewport(
    const GeometryPoint point,
    const GeometryRect viewport) noexcept {
    return point.x >= viewport.left && point.x < viewport.right &&
        point.y >= viewport.top && point.y < viewport.bottom;
}

bool NearCoordinate(const int first, const int second, const int tolerance) noexcept {
    const std::int64_t difference =
        static_cast<std::int64_t>(first) - static_cast<std::int64_t>(second);
    return difference >= -static_cast<std::int64_t>(tolerance) &&
        difference <= static_cast<std::int64_t>(tolerance);
}

bool WithinExpandedSpan(
    const int value,
    const int low,
    const int high,
    const int tolerance) noexcept {
    return static_cast<std::int64_t>(value) >=
            static_cast<std::int64_t>(low) - tolerance &&
        static_cast<std::int64_t>(value) <=
            static_cast<std::int64_t>(high) + tolerance;
}

int ClampMouseCoordinate(const int value, const int low, const int high) noexcept {
    return (std::clamp)(value, low, high);
}

bool CropWithinVideo(
    const VideoDimensions video,
    const VideoCrop crop) noexcept {
    return video.IsValid() && crop.IsValid() &&
        crop.x < video.width && crop.y < video.height &&
        crop.width <= video.width - crop.x &&
        crop.height <= video.height - crop.y;
}

unsigned RoundEvenExtent(const double requested, const unsigned limit) noexcept {
    if (!std::isfinite(requested) || requested <= 0.0 || limit == 0U) {
        return 0U;
    }
    if (requested >= static_cast<double>(limit) - 0.5) {
        return limit;
    }
    unsigned rounded = static_cast<unsigned>(std::floor(requested + 0.5));
    rounded = (std::clamp)(rounded, 1U, limit);
    if (rounded == limit || rounded == 1U || (rounded & 1U) == 0U) {
        return rounded;
    }
    const unsigned lower = rounded - 1U;
    const unsigned upper = rounded + 1U;
    if (upper > limit) {
        return lower;
    }
    return requested - lower < upper - requested ? lower : upper;
}

unsigned PlaceCropAroundCenter(
    const double center,
    const unsigned extent,
    const unsigned sourceExtent) noexcept {
    if (!std::isfinite(center) || extent == 0U || extent > sourceExtent) {
        return 0U;
    }
    const unsigned maximumOrigin = sourceExtent - extent;
    const double requested = center - static_cast<double>(extent) / 2.0;
    if (requested <= 0.0) {
        return 0U;
    }
    if (requested >= static_cast<double>(maximumOrigin)) {
        return maximumOrigin;
    }
    const unsigned rounded = static_cast<unsigned>(std::floor(requested + 0.5));
    if ((rounded & 1U) == 0U || rounded == maximumOrigin) {
        return rounded;
    }
    const unsigned lower = rounded - 1U;
    const unsigned upper = rounded + 1U;
    if (upper > maximumOrigin) {
        return lower;
    }
    return requested - lower <= upper - requested ? lower : upper;
}

}  // namespace

VideoDisplayState NormalizeVideoDisplayState(
    const VideoDimensions cropSize,
    const GeometryRect viewport,
    const VideoDisplayState state) noexcept {
    const GeometryRect fitted = ComputeVideoContentRect(cropSize, viewport);
    const int viewportWidth = viewport.Width();
    const int viewportHeight = viewport.Height();
    if (fitted.IsEmpty() || viewportWidth <= 0 || viewportHeight <= 0) {
        return {};
    }

    const double maximumScale = MaximumCanvasScale(fitted);
    if (!std::isfinite(maximumScale) || maximumScale <= 0.0) {
        return {};
    }
    // Viewports wider than 65536 pixels cannot simultaneously satisfy the
    // ordinary 0.25 lower bound and the hard canvas allocation bound. Safety
    // wins for that otherwise-unrepresentable input.
    const double minimumScale = (std::min)(kMinimumScale, maximumScale);
    const double requestedScale = std::isfinite(state.scale) ? state.scale : 1.0;

    VideoDisplayState result{};
    result.scale = (std::clamp)(requestedScale, minimumScale, maximumScale);
    const int displayWidth = ScaledExtent(fitted.Width(), result.scale);
    const int displayHeight = ScaledExtent(fitted.Height(), result.scale);
    result.centerX = NormalizeCenter(
        state.centerX, displayWidth, viewportWidth);
    result.centerY = NormalizeCenter(
        state.centerY, displayHeight, viewportHeight);
    return result;
}

GeometryRect ComputeVideoDisplayRect(
    const VideoDimensions cropSize,
    const GeometryRect viewport,
    const VideoDisplayState state) noexcept {
    const GeometryRect fitted = ComputeVideoContentRect(cropSize, viewport);
    const int viewportWidth = viewport.Width();
    const int viewportHeight = viewport.Height();
    if (fitted.IsEmpty() || viewportWidth <= 0 || viewportHeight <= 0) {
        return {};
    }

    const VideoDisplayState normalized =
        NormalizeVideoDisplayState(cropSize, viewport, state);
    const int displayWidth = ScaledExtent(fitted.Width(), normalized.scale);
    const int displayHeight = ScaledExtent(fitted.Height(), normalized.scale);
    if (displayWidth <= 0 || displayHeight <= 0) {
        return {};
    }

    const double viewportCenterX = static_cast<double>(viewport.left) +
        static_cast<double>(viewportWidth) / 2.0;
    const double viewportCenterY = static_cast<double>(viewport.top) +
        static_cast<double>(viewportHeight) / 2.0;
    std::int64_t left = FloorToInt64(
        viewportCenterX - normalized.centerX * displayWidth);
    std::int64_t top = FloorToInt64(
        viewportCenterY - normalized.centerY * displayHeight);
    left = ClampOrigin(
        left, viewport.left, viewport.right, viewportWidth, displayWidth);
    top = ClampOrigin(
        top, viewport.top, viewport.bottom, viewportHeight, displayHeight);

    return {
        SaturateToInt(left),
        SaturateToInt(top),
        SaturateToInt(left + displayWidth),
        SaturateToInt(top + displayHeight)};
}

VideoEdge HitTestVideoEdge(
    const GeometryRect contentRect,
    const GeometryRect viewport,
    const GeometryPoint point,
    const int tolerance) noexcept {
    if (contentRect.IsEmpty() || viewport.IsEmpty() || tolerance < 0 ||
        !PointInsideViewport(point, viewport)) {
        return VideoEdge::None;
    }
    const GeometryRect visible = IntersectRectangles(contentRect, viewport);
    if (visible.IsEmpty()) {
        return VideoEdge::None;
    }

    // GeometryRect is half-open, so the final visible device pixel is one less
    // than right/bottom. Clipped viewport borders deliberately remain resize
    // grips: an oversized presentation must always retain a way to shrink.
    const int leftEdge = visible.left;
    const int rightEdge = visible.right - 1;
    const int topEdge = visible.top;
    const int bottomEdge = visible.bottom - 1;
    bool nearLeft = NearCoordinate(point.x, leftEdge, tolerance) &&
        WithinExpandedSpan(point.y, visible.top, visible.bottom, tolerance);
    bool nearRight = NearCoordinate(point.x, rightEdge, tolerance) &&
        WithinExpandedSpan(point.y, visible.top, visible.bottom, tolerance);
    bool nearTop = NearCoordinate(point.y, topEdge, tolerance) &&
        WithinExpandedSpan(point.x, visible.left, visible.right, tolerance);
    bool nearBottom = NearCoordinate(point.y, bottomEdge, tolerance) &&
        WithinExpandedSpan(point.x, visible.left, visible.right, tolerance);

    // A large DPI-scaled tolerance can overlap opposite sides of a small
    // visible rectangle. Select the nearest side on each axis deterministically.
    if (nearLeft && nearRight) {
        const std::int64_t leftDistance = std::abs(
            static_cast<std::int64_t>(point.x) - leftEdge);
        const std::int64_t rightDistance = std::abs(
            static_cast<std::int64_t>(point.x) - rightEdge);
        if (leftDistance <= rightDistance) {
            nearRight = false;
        } else {
            nearLeft = false;
        }
    }
    if (nearTop && nearBottom) {
        const std::int64_t topDistance = std::abs(
            static_cast<std::int64_t>(point.y) - topEdge);
        const std::int64_t bottomDistance = std::abs(
            static_cast<std::int64_t>(point.y) - bottomEdge);
        if (topDistance <= bottomDistance) {
            nearBottom = false;
        } else {
            nearTop = false;
        }
    }

    if (nearTop && nearLeft) {
        return VideoEdge::TopLeft;
    }
    if (nearTop && nearRight) {
        return VideoEdge::TopRight;
    }
    if (nearBottom && nearLeft) {
        return VideoEdge::BottomLeft;
    }
    if (nearBottom && nearRight) {
        return VideoEdge::BottomRight;
    }
    if (nearLeft) {
        return VideoEdge::Left;
    }
    if (nearRight) {
        return VideoEdge::Right;
    }
    if (nearTop) {
        return VideoEdge::Top;
    }
    if (nearBottom) {
        return VideoEdge::Bottom;
    }
    return VideoEdge::None;
}

VideoDisplayState ResizeVideoDisplay(
    const VideoDisplayState startState,
    const VideoDimensions cropSize,
    const GeometryRect viewport,
    const GeometryRect startGripRect,
    const VideoEdge edge,
    const GeometryPoint startMouse,
    const GeometryPoint currentMouse) noexcept {
    const VideoDisplayState normalizedStart =
        NormalizeVideoDisplayState(cropSize, viewport, startState);
    const GeometryRect fitted = ComputeVideoContentRect(cropSize, viewport);
    const int gripWidth = startGripRect.Width();
    const int gripHeight = startGripRect.Height();
    if (fitted.IsEmpty() || viewport.IsEmpty() || startGripRect.IsEmpty() ||
        gripWidth <= 0 || gripHeight <= 0 || edge == VideoEdge::None) {
        return normalizedStart;
    }

    const GeometryPoint current{
        ClampMouseCoordinate(currentMouse.x, viewport.left, viewport.right),
        ClampMouseCoordinate(currentMouse.y, viewport.top, viewport.bottom)};
    double horizontalOutward = 0.0;
    double verticalOutward = 0.0;
    bool horizontal = false;
    bool vertical = false;
    switch (edge) {
    case VideoEdge::Left:
        horizontal = true;
        horizontalOutward = static_cast<double>(
            static_cast<std::int64_t>(startMouse.x) - current.x);
        break;
    case VideoEdge::Right:
        horizontal = true;
        horizontalOutward = static_cast<double>(
            static_cast<std::int64_t>(current.x) - startMouse.x);
        break;
    case VideoEdge::Top:
        vertical = true;
        verticalOutward = static_cast<double>(
            static_cast<std::int64_t>(startMouse.y) - current.y);
        break;
    case VideoEdge::Bottom:
        vertical = true;
        verticalOutward = static_cast<double>(
            static_cast<std::int64_t>(current.y) - startMouse.y);
        break;
    case VideoEdge::TopLeft:
        horizontal = true;
        vertical = true;
        horizontalOutward = static_cast<double>(
            static_cast<std::int64_t>(startMouse.x) - current.x);
        verticalOutward = static_cast<double>(
            static_cast<std::int64_t>(startMouse.y) - current.y);
        break;
    case VideoEdge::TopRight:
        horizontal = true;
        vertical = true;
        horizontalOutward = static_cast<double>(
            static_cast<std::int64_t>(current.x) - startMouse.x);
        verticalOutward = static_cast<double>(
            static_cast<std::int64_t>(startMouse.y) - current.y);
        break;
    case VideoEdge::BottomLeft:
        horizontal = true;
        vertical = true;
        horizontalOutward = static_cast<double>(
            static_cast<std::int64_t>(startMouse.x) - current.x);
        verticalOutward = static_cast<double>(
            static_cast<std::int64_t>(current.y) - startMouse.y);
        break;
    case VideoEdge::BottomRight:
        horizontal = true;
        vertical = true;
        horizontalOutward = static_cast<double>(
            static_cast<std::int64_t>(current.x) - startMouse.x);
        verticalOutward = static_cast<double>(
            static_cast<std::int64_t>(current.y) - startMouse.y);
        break;
    case VideoEdge::None:
    default:
        return normalizedStart;
    }

    double factor = 1.0;
    if (horizontal && vertical) {
        const double width = static_cast<double>(gripWidth);
        const double height = static_cast<double>(gripHeight);
        factor += 2.0 *
            (horizontalOutward * width + verticalOutward * height) /
            (width * width + height * height);
    } else if (horizontal) {
        factor += 2.0 * horizontalOutward / gripWidth;
    } else {
        factor += 2.0 * verticalOutward / gripHeight;
    }

    const double canvasMaximum = MaximumCanvasScale(fitted);
    if (!std::isfinite(canvasMaximum) || canvasMaximum <= 0.0) {
        return normalizedStart;
    }
    const double coverScale = (std::max)(
        static_cast<double>(viewport.Width()) / fitted.Width(),
        static_cast<double>(viewport.Height()) / fitted.Height());
    const double operationMaximum = (std::min)(
        canvasMaximum,
        (std::max)(normalizedStart.scale, coverScale));
    const double operationMinimum = (std::min)(kMinimumScale, canvasMaximum);
    const double requestedScale = normalizedStart.scale * factor;

    VideoDisplayState result = normalizedStart;
    result.scale = std::isfinite(requestedScale)
        ? (std::clamp)(requestedScale, operationMinimum, operationMaximum)
        : normalizedStart.scale;
    return NormalizeVideoDisplayState(cropSize, viewport, result);
}

VideoAreaResizeResult ResizeVideoArea(
    const VideoDimensions video,
    const VideoCrop startCrop,
    const VideoDisplayState startState,
    const GeometryRect viewport,
    const GeometryRect startContent,
    const VideoEdge edge,
    const GeometryPoint startMouse,
    const GeometryPoint currentMouse) noexcept {
    VideoAreaResizeResult result{};
    if (!CropWithinVideo(video, startCrop) || viewport.IsEmpty() ||
        startContent.IsEmpty() || !std::isfinite(startState.scale) ||
        startState.scale <= 0.0 || !std::isfinite(startState.centerX) ||
        !std::isfinite(startState.centerY) || startState.centerX < 0.0 ||
        startState.centerX > 1.0 || startState.centerY < 0.0 ||
        startState.centerY > 1.0 || !PointInsideViewport(startMouse, viewport)) {
        return result;
    }

    const GeometryRect grip = IntersectRectangles(startContent, viewport);
    const int gripWidth = grip.Width();
    const int gripHeight = grip.Height();
    if (grip.IsEmpty() || gripWidth <= 0 || gripHeight <= 0) {
        return result;
    }

    const GeometryPoint current{
        ClampMouseCoordinate(currentMouse.x, viewport.left, viewport.right),
        ClampMouseCoordinate(currentMouse.y, viewport.top, viewport.bottom)};
    std::int64_t outwardX = 0;
    std::int64_t outwardY = 0;
    bool resizeWidth = false;
    bool resizeHeight = false;
    switch (edge) {
    case VideoEdge::Left:
        resizeWidth = true;
        outwardX = static_cast<std::int64_t>(startMouse.x) - current.x;
        break;
    case VideoEdge::Right:
        resizeWidth = true;
        outwardX = static_cast<std::int64_t>(current.x) - startMouse.x;
        break;
    case VideoEdge::Top:
        resizeHeight = true;
        outwardY = static_cast<std::int64_t>(startMouse.y) - current.y;
        break;
    case VideoEdge::Bottom:
        resizeHeight = true;
        outwardY = static_cast<std::int64_t>(current.y) - startMouse.y;
        break;
    case VideoEdge::TopLeft:
        resizeWidth = true;
        resizeHeight = true;
        outwardX = static_cast<std::int64_t>(startMouse.x) - current.x;
        outwardY = static_cast<std::int64_t>(startMouse.y) - current.y;
        break;
    case VideoEdge::TopRight:
        resizeWidth = true;
        resizeHeight = true;
        outwardX = static_cast<std::int64_t>(current.x) - startMouse.x;
        outwardY = static_cast<std::int64_t>(startMouse.y) - current.y;
        break;
    case VideoEdge::BottomLeft:
        resizeWidth = true;
        resizeHeight = true;
        outwardX = static_cast<std::int64_t>(startMouse.x) - current.x;
        outwardY = static_cast<std::int64_t>(current.y) - startMouse.y;
        break;
    case VideoEdge::BottomRight:
        resizeWidth = true;
        resizeHeight = true;
        outwardX = static_cast<std::int64_t>(current.x) - startMouse.x;
        outwardY = static_cast<std::int64_t>(current.y) - startMouse.y;
        break;
    case VideoEdge::None:
    default:
        return result;
    }

    const int minimumWidth = (std::min)(32, gripWidth);
    const int minimumHeight = (std::min)(32, gripHeight);
    const auto desiredExtent = [](
        const int start,
        const std::int64_t outward,
        const bool affected,
        const int minimum,
        const int maximum) noexcept {
        if (!affected) {
            return start;
        }
        const std::int64_t requested =
            static_cast<std::int64_t>(start) + outward * 2;
        return static_cast<int>((std::clamp)(
            requested,
            static_cast<std::int64_t>(minimum),
            static_cast<std::int64_t>(maximum)));
    };
    int desiredWidth = desiredExtent(
        gripWidth, outwardX, resizeWidth, minimumWidth, viewport.Width());
    int desiredHeight = desiredExtent(
        gripHeight, outwardY, resizeHeight, minimumHeight, viewport.Height());
    if (desiredWidth == gripWidth && desiredHeight == gripHeight) {
        result.valid = true;
        result.crop = startCrop;
        result.display = startState;
        return result;
    }

    // ComputeVideoDisplayRect deliberately keeps its public, legacy 0.25
    // minimum scale. Keep a freely-resized frame representable by that API so
    // normalization cannot silently enlarge the unaffected screen-space axis.
    const int scaleFloorWidth = static_cast<int>(std::ceil(
        static_cast<double>(viewport.Width()) * kMinimumScale));
    const int scaleFloorHeight = static_cast<int>(std::ceil(
        static_cast<double>(viewport.Height()) * kMinimumScale));
    if (resizeWidth && !resizeHeight && desiredHeight < scaleFloorHeight) {
        desiredWidth = (std::max)(desiredWidth, scaleFloorWidth);
    } else if (!resizeWidth && resizeHeight && desiredWidth < scaleFloorWidth) {
        desiredHeight = (std::max)(desiredHeight, scaleFloorHeight);
    } else if (resizeWidth && resizeHeight) {
        const double representedScale = (std::max)(
            static_cast<double>(desiredWidth) / viewport.Width(),
            static_cast<double>(desiredHeight) / viewport.Height());
        if (representedScale < kMinimumScale) {
            const double correction = kMinimumScale / representedScale;
            desiredWidth = (std::min)(
                viewport.Width(),
                static_cast<int>(std::ceil(desiredWidth * correction)));
            desiredHeight = (std::min)(
                viewport.Height(),
                static_cast<int>(std::ceil(desiredHeight * correction)));
        }
    }

    const double contentWidth = static_cast<double>(startContent.Width());
    const double contentHeight = static_cast<double>(startContent.Height());
    const double cropWidth = static_cast<double>(startCrop.width);
    const double cropHeight = static_cast<double>(startCrop.height);
    const double densityDenominator =
        cropWidth * cropWidth + cropHeight * cropHeight;
    double density =
        (contentWidth * cropWidth + contentHeight * cropHeight) /
        densityDenominator;
    if (!std::isfinite(density) || density <= 0.0) {
        return {};
    }

    const double sourceFitDensity = (std::max)(
        static_cast<double>(desiredWidth) / video.width,
        static_cast<double>(desiredHeight) / video.height);
    density = (std::max)(density, sourceFitDensity);

    // Keep ordinary crops at least 16 source pixels per side without enlarging
    // an already-tiny starting crop or violating the requested aspect/source.
    const double minimumCropWidth =
        static_cast<double>((std::min)(16U, startCrop.width));
    const double minimumCropHeight =
        static_cast<double>((std::min)(16U, startCrop.height));
    const double maximumDensityForMinimum = (std::min)(
        static_cast<double>(desiredWidth) / minimumCropWidth,
        static_cast<double>(desiredHeight) / minimumCropHeight);
    if (maximumDensityForMinimum >= sourceFitDensity) {
        density = (std::min)(density, maximumDensityForMinimum);
    }

    const unsigned newWidth = RoundEvenExtent(
        static_cast<double>(desiredWidth) / density, video.width);
    const unsigned newHeight = RoundEvenExtent(
        static_cast<double>(desiredHeight) / density, video.height);
    if (newWidth == 0U || newHeight == 0U) {
        return {};
    }

    const double viewportCenterX = static_cast<double>(viewport.left) +
        static_cast<double>(viewport.Width()) / 2.0;
    const double viewportCenterY = static_cast<double>(viewport.top) +
        static_cast<double>(viewport.Height()) / 2.0;
    const double visibleFractionX = (std::clamp)(
        (viewportCenterX - startContent.left) / contentWidth, 0.0, 1.0);
    const double visibleFractionY = (std::clamp)(
        (viewportCenterY - startContent.top) / contentHeight, 0.0, 1.0);
    const double sourceCenterX = startCrop.x + visibleFractionX * startCrop.width;
    const double sourceCenterY = startCrop.y + visibleFractionY * startCrop.height;
    const VideoCrop crop{
        PlaceCropAroundCenter(sourceCenterX, newWidth, video.width),
        PlaceCropAroundCenter(sourceCenterY, newHeight, video.height),
        newWidth,
        newHeight};
    if (!CropWithinVideo(video, crop)) {
        return {};
    }

    const GeometryRect fitted = ComputeVideoContentRect(
        {crop.width, crop.height}, viewport);
    if (fitted.IsEmpty()) {
        return {};
    }
    const double fittedWidth = static_cast<double>(fitted.Width());
    const double fittedHeight = static_cast<double>(fitted.Height());
    const double displayDenominator =
        fittedWidth * fittedWidth + fittedHeight * fittedHeight;
    double displayScale =
        (desiredWidth * fittedWidth + desiredHeight * fittedHeight) /
        displayDenominator;
    const double parentFitMaximum = (std::min)(
        static_cast<double>(viewport.Width()) / fitted.Width(),
        static_cast<double>(viewport.Height()) / fitted.Height());
    displayScale = (std::min)(displayScale, parentFitMaximum);
    if (!std::isfinite(displayScale) || displayScale <= 0.0) {
        return {};
    }

    result.valid = true;
    result.crop = crop;
    result.display = NormalizeVideoDisplayState(
        {crop.width, crop.height}, viewport, {displayScale, 0.5, 0.5});
    return result;
}

}  // namespace videoplayer
