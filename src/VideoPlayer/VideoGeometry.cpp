#include "VideoGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace videoplayer {
namespace {

int SaturateToInt(const std::int64_t value) noexcept {
    if (value > static_cast<std::int64_t>((std::numeric_limits<int>::max)())) {
        return (std::numeric_limits<int>::max)();
    }
    if (value < static_cast<std::int64_t>((std::numeric_limits<int>::min)())) {
        return (std::numeric_limits<int>::min)();
    }
    return static_cast<int>(value);
}

int PositiveExtent(const int low, const int high) noexcept {
    if (high <= low) {
        return 0;
    }
    return SaturateToInt(static_cast<std::int64_t>(high) - low);
}

unsigned ClampToUnsigned(const std::uint64_t value, const unsigned maximum) noexcept {
    return value > maximum ? maximum : static_cast<unsigned>(value);
}

std::uint64_t DivideRounded(
    const std::uint64_t numerator,
    const std::uint64_t denominator) noexcept {
    if (denominator == 0U) {
        return 0U;
    }
    return (numerator + (denominator / 2U)) / denominator;
}

std::uint64_t DivideCeiling(
    const std::uint64_t numerator,
    const std::uint64_t denominator) noexcept {
    if (denominator == 0U || numerator == 0U) {
        return 0U;
    }
    return 1U + ((numerator - 1U) / denominator);
}

std::pair<unsigned, unsigned> AlignRangeToEvenPixels(
    unsigned start,
    unsigned end,
    const unsigned limit) noexcept {
    start = (std::min)(start, limit);
    end = (std::min)(end, limit);
    if (end <= start || limit < 2U) {
        return {start, end};
    }

    const unsigned evenStart = start & ~1U;
    const std::uint64_t roundedEnd = (static_cast<std::uint64_t>(end) + 1U) & ~std::uint64_t{1U};
    const unsigned greatestEvenBoundary = limit & ~1U;
    const unsigned evenEnd = static_cast<unsigned>((std::min)(
        roundedEnd,
        static_cast<std::uint64_t>(greatestEvenBoundary)));

    // For an odd-sized source and a selection containing only its final pixel,
    // no all-even range can still contain the selected source interval.
    if (evenEnd <= evenStart) {
        return {start, end};
    }
    return {evenStart, evenEnd};
}

constexpr unsigned kMinimumInteractiveCropSide = 16U;

bool IsCropWithinVideo(
    const VideoDimensions video,
    const VideoCrop crop) noexcept {
    return video.IsValid() && crop.IsValid() &&
        crop.x < video.width && crop.y < video.height &&
        crop.width <= video.width - crop.x &&
        crop.height <= video.height - crop.y;
}

unsigned RoundAndClampOffset(
    const unsigned origin,
    const unsigned extent,
    const unsigned limit,
    const double delta) noexcept {
    const unsigned maximumOrigin = limit - extent;
    if (delta >= static_cast<double>(maximumOrigin - origin)) {
        return maximumOrigin;
    }
    if (delta <= -static_cast<double>(origin)) {
        return 0U;
    }

    const double roundedDelta = delta < 0.0
        ? std::ceil(delta - 0.5)
        : std::floor(delta + 0.5);
    const std::int64_t candidate = static_cast<std::int64_t>(origin) +
        static_cast<std::int64_t>(roundedDelta);
    const unsigned roundedOrigin = static_cast<unsigned>((std::clamp)(
        candidate,
        std::int64_t{0},
        static_cast<std::int64_t>(maximumOrigin)));
    if ((roundedOrigin & 1U) == 0U || roundedOrigin == maximumOrigin ||
        roundedDelta == 0.0) {
        return roundedOrigin;
    }

    // Preserve a clamped source edge. Away from an edge, choose the closest
    // chroma-friendly even origin to the requested continuous source point.
    const unsigned lower = roundedOrigin - 1U;
    const unsigned upper = roundedOrigin + 1U;
    if (upper > maximumOrigin) {
        return lower;
    }
    const double requested = static_cast<double>(origin) + delta;
    const double lowerDistance = requested - static_cast<double>(lower);
    const double upperDistance = static_cast<double>(upper) - requested;
    if (lowerDistance < upperDistance) {
        return lower;
    }
    if (upperDistance < lowerDistance) {
        return upper;
    }
    return delta > 0.0 ? upper : lower;
}

unsigned ScaleCropExtent(
    const unsigned current,
    const unsigned maximum,
    const double scale) noexcept {
    const bool zoomingIn = scale < 1.0;
    // The caller enforces the minimum with one aspect-preserving scale.
    // A per-axis 16-pixel floor here would stretch very narrow selections
    // when their other axis reaches the source edge first.
    constexpr unsigned minimum = 1U;

    const double scaled = static_cast<double>(current) * scale;
    unsigned target = maximum;
    if (std::isfinite(scaled) && scaled < static_cast<double>(maximum)) {
        const double rounded = std::floor(scaled + 0.5);
        target = rounded <= 0.0
            ? 0U
            : static_cast<unsigned>(rounded);
    }
    if (zoomingIn) {
        target = (std::clamp)(target, minimum, current);
    } else {
        target = (std::clamp)(target, (std::max)(minimum, current), maximum);
    }

    if (target == maximum || (target & 1U) == 0U) {
        return target;
    }

    // Keep each integer step in the requested zoom direction. Full odd-sized
    // maximum extents remain reachable instead of being permanently rounded inward.
    if (zoomingIn) {
        if (target > minimum) {
            return target - 1U;
        }
        return target + 1U <= current ? target + 1U : target;
    }
    if (target < maximum) {
        return target + 1U;
    }
    return target;
}

unsigned PlaceScaledCropAxis(
    const unsigned origin,
    const unsigned oldExtent,
    const unsigned newExtent,
    const unsigned sourceExtent,
    const double anchor) noexcept {
    if (oldExtent == newExtent) {
        return origin;
    }

    const double anchoredSourcePoint = static_cast<double>(origin) +
        anchor * static_cast<double>(oldExtent);
    const double requestedOrigin = anchoredSourcePoint -
        anchor * static_cast<double>(newExtent);
    const unsigned maximumOrigin = sourceExtent - newExtent;
    if (requestedOrigin <= 0.0) {
        return 0U;
    }
    if (requestedOrigin >= static_cast<double>(maximumOrigin)) {
        return maximumOrigin;
    }

    const unsigned roundedOrigin = static_cast<unsigned>(
        std::floor(requestedOrigin + 0.5));
    if ((roundedOrigin & 1U) == 0U || roundedOrigin == maximumOrigin) {
        return roundedOrigin;
    }
    const unsigned lower = roundedOrigin - 1U;
    const unsigned upper = roundedOrigin + 1U;
    if (upper > maximumOrigin) {
        return lower;
    }
    return requestedOrigin - static_cast<double>(lower) <=
            static_cast<double>(upper) - requestedOrigin
        ? lower
        : upper;
}

}  // namespace

int GeometryRect::Width() const noexcept {
    return PositiveExtent(left, right);
}

int GeometryRect::Height() const noexcept {
    return PositiveExtent(top, bottom);
}

bool GeometryRect::IsEmpty() const noexcept {
    return right <= left || bottom <= top;
}

bool operator==(const GeometryPoint& left, const GeometryPoint& right) noexcept {
    return left.x == right.x && left.y == right.y;
}

bool operator==(const GeometryRect& left, const GeometryRect& right) noexcept {
    return left.left == right.left && left.top == right.top &&
        left.right == right.right && left.bottom == right.bottom;
}

bool operator==(const VideoDimensions& left, const VideoDimensions& right) noexcept {
    return left.width == right.width && left.height == right.height;
}

bool operator==(const VideoCrop& left, const VideoCrop& right) noexcept {
    return left.x == right.x && left.y == right.y &&
        left.width == right.width && left.height == right.height;
}

int ScaleLogicalPixels(const int logicalPixels, unsigned dpi) noexcept {
    if (logicalPixels <= 0) {
        return 0;
    }
    if (dpi == 0U) {
        dpi = 96U;
    }
    const std::uint64_t scaled =
        (static_cast<std::uint64_t>(logicalPixels) * dpi + 48U) / 96U;
    return scaled > static_cast<std::uint64_t>((std::numeric_limits<int>::max)())
        ? (std::numeric_limits<int>::max)()
        : static_cast<int>(scaled);
}

GeometryRect NormalizeRectangle(
    const GeometryPoint first,
    const GeometryPoint second) noexcept {
    return {
        (std::min)(first.x, second.x),
        (std::min)(first.y, second.y),
        (std::max)(first.x, second.x),
        (std::max)(first.y, second.y)};
}

GeometryRect NormalizeRectangle(const GeometryRect rectangle) noexcept {
    return NormalizeRectangle(
        {rectangle.left, rectangle.top},
        {rectangle.right, rectangle.bottom});
}

GeometryRect IntersectRectangles(
    const GeometryRect first,
    const GeometryRect second) noexcept {
    GeometryRect intersection{
        (std::max)(first.left, second.left),
        (std::max)(first.top, second.top),
        (std::min)(first.right, second.right),
        (std::min)(first.bottom, second.bottom)};
    if (intersection.IsEmpty()) {
        return {};
    }
    return intersection;
}

GeometryRect OffsetRectangle(
    const GeometryRect rectangle,
    const int deltaX,
    const int deltaY) noexcept {
    return {
        SaturateToInt(static_cast<std::int64_t>(rectangle.left) + deltaX),
        SaturateToInt(static_cast<std::int64_t>(rectangle.top) + deltaY),
        SaturateToInt(static_cast<std::int64_t>(rectangle.right) + deltaX),
        SaturateToInt(static_cast<std::int64_t>(rectangle.bottom) + deltaY)};
}

GeometryRect ComputeVideoContentRect(
    const VideoDimensions video,
    const GeometryRect viewport) noexcept {
    const int viewportWidth = viewport.Width();
    const int viewportHeight = viewport.Height();
    if (!video.IsValid() || viewportWidth <= 0 || viewportHeight <= 0) {
        return {};
    }

    int displayWidth = viewportWidth;
    int displayHeight = viewportHeight;
    const std::uint64_t widthLimitedLeft =
        static_cast<std::uint64_t>(viewportWidth) * video.height;
    const std::uint64_t widthLimitedRight =
        static_cast<std::uint64_t>(viewportHeight) * video.width;
    if (widthLimitedLeft <= widthLimitedRight) {
        displayHeight = static_cast<int>((std::max)(std::uint64_t{1U}, DivideRounded(
            static_cast<std::uint64_t>(video.height) * static_cast<unsigned>(viewportWidth),
            video.width)));
        displayHeight = (std::min)(displayHeight, viewportHeight);
    } else {
        displayWidth = static_cast<int>((std::max)(std::uint64_t{1U}, DivideRounded(
            static_cast<std::uint64_t>(video.width) * static_cast<unsigned>(viewportHeight),
            video.height)));
        displayWidth = (std::min)(displayWidth, viewportWidth);
    }

    const int horizontalInset = (viewportWidth - displayWidth) / 2;
    const int verticalInset = (viewportHeight - displayHeight) / 2;
    const int contentLeft = SaturateToInt(
        static_cast<std::int64_t>(viewport.left) + horizontalInset);
    const int contentTop = SaturateToInt(
        static_cast<std::int64_t>(viewport.top) + verticalInset);
    return {
        contentLeft,
        contentTop,
        SaturateToInt(static_cast<std::int64_t>(contentLeft) + displayWidth),
        SaturateToInt(static_cast<std::int64_t>(contentTop) + displayHeight)};
}

VideoCropMapping MapSelectionToVideoCrop(
    const VideoDimensions video,
    const GeometryRect viewport,
    const GeometryRect selection,
    const int minimumSelectionPixels) noexcept {
    VideoCropMapping result{};
    result.contentRect = ComputeVideoContentRect(video, viewport);
    if (!video.IsValid() || result.contentRect.IsEmpty()) {
        return result;
    }

    const GeometryRect normalizedSelection = NormalizeRectangle(selection);
    result.clippedSelection = IntersectRectangles(normalizedSelection, result.contentRect);
    const int requiredSize = (std::max)(1, minimumSelectionPixels);
    if (result.clippedSelection.Width() < requiredSize ||
        result.clippedSelection.Height() < requiredSize) {
        return result;
    }

    const unsigned displayWidth = static_cast<unsigned>(result.contentRect.Width());
    const unsigned displayHeight = static_cast<unsigned>(result.contentRect.Height());
    const unsigned relativeLeft = static_cast<unsigned>(
        result.clippedSelection.left - result.contentRect.left);
    const unsigned relativeTop = static_cast<unsigned>(
        result.clippedSelection.top - result.contentRect.top);
    const unsigned relativeRight = static_cast<unsigned>(
        result.clippedSelection.right - result.contentRect.left);
    const unsigned relativeBottom = static_cast<unsigned>(
        result.clippedSelection.bottom - result.contentRect.top);

    result.normalizedSelection = {
        static_cast<double>(relativeLeft) / displayWidth,
        static_cast<double>(relativeTop) / displayHeight,
        static_cast<double>(relativeRight) / displayWidth,
        static_cast<double>(relativeBottom) / displayHeight};

    unsigned sourceLeft = ClampToUnsigned(
        (static_cast<std::uint64_t>(relativeLeft) * video.width) / displayWidth,
        video.width);
    unsigned sourceTop = ClampToUnsigned(
        (static_cast<std::uint64_t>(relativeTop) * video.height) / displayHeight,
        video.height);
    unsigned sourceRight = ClampToUnsigned(DivideCeiling(
        static_cast<std::uint64_t>(relativeRight) * video.width,
        displayWidth), video.width);
    unsigned sourceBottom = ClampToUnsigned(DivideCeiling(
        static_cast<std::uint64_t>(relativeBottom) * video.height,
        displayHeight), video.height);

    const auto horizontal = AlignRangeToEvenPixels(sourceLeft, sourceRight, video.width);
    const auto vertical = AlignRangeToEvenPixels(sourceTop, sourceBottom, video.height);
    sourceLeft = horizontal.first;
    sourceRight = horizontal.second;
    sourceTop = vertical.first;
    sourceBottom = vertical.second;
    if (sourceRight <= sourceLeft || sourceBottom <= sourceTop) {
        return result;
    }

    result.crop = {
        sourceLeft,
        sourceTop,
        sourceRight - sourceLeft,
        sourceBottom - sourceTop};
    result.valid = result.crop.IsValid();
    return result;
}

VideoCrop PanVideoCrop(
    const VideoDimensions video,
    const VideoCrop crop,
    const double deltaX,
    const double deltaY) noexcept {
    if (!IsCropWithinVideo(video, crop) ||
        !std::isfinite(deltaX) || !std::isfinite(deltaY)) {
        return crop;
    }
    if (deltaX == 0.0 && deltaY == 0.0) {
        return crop;
    }

    return {
        RoundAndClampOffset(crop.x, crop.width, video.width, deltaX),
        RoundAndClampOffset(crop.y, crop.height, video.height, deltaY),
        crop.width,
        crop.height};
}

VideoCrop ZoomVideoCrop(
    const VideoDimensions video,
    const VideoCrop crop,
    const double scale,
    const double anchorX,
    const double anchorY,
    const VideoDimensions aspectReference) noexcept {
    if (!IsCropWithinVideo(video, crop) || !std::isfinite(scale) ||
        scale <= 0.0 || !std::isfinite(anchorX) || !std::isfinite(anchorY) ||
        anchorX < 0.0 || anchorX > 1.0 ||
        anchorY < 0.0 || anchorY > 1.0) {
        return crop;
    }

    if (scale == 1.0) {
        return crop;
    }

    const VideoDimensions reference = aspectReference.IsValid()
        ? aspectReference : VideoDimensions{crop.width, crop.height};
    // Derive both extents from one scale and the original proportions. Merely
    // clamping each current axis to the source eventually changes a narrow
    // selection into the full frame, and repeated pixel rounding adds drift.
    const double currentScale = reference.width >= reference.height
        ? static_cast<double>(crop.width) / reference.width
        : static_cast<double>(crop.height) / reference.height;
    const double maximumScale = (std::min)(
        static_cast<double>(video.width) / reference.width,
        static_cast<double>(video.height) / reference.height);
    const double minimumScale = (std::max)(
        static_cast<double>(kMinimumInteractiveCropSide) / reference.width,
        static_cast<double>(kMinimumInteractiveCropSide) / reference.height);
    double targetScale = currentScale * scale;
    if (scale < 1.0) {
        targetScale = (std::max)(targetScale, (std::min)(minimumScale, currentScale));
        if (targetScale >= currentScale) {
            return crop;
        }
    } else {
        targetScale = (std::max)(targetScale, minimumScale);
    }
    targetScale = (std::min)(targetScale, maximumScale);

    unsigned width = ScaleCropExtent(
        crop.width, video.width, reference.width * targetScale / crop.width);
    unsigned height = ScaleCropExtent(
        crop.height, video.height, reference.height * targetScale / crop.height);
    // Pixel alignment must not reverse either axis at a zoom boundary.
    width = scale < 1.0 ? (std::min)(width, crop.width) : (std::max)(width, crop.width);
    height = scale < 1.0 ? (std::min)(height, crop.height) : (std::max)(height, crop.height);
    return {
        PlaceScaledCropAxis(crop.x, crop.width, width, video.width, anchorX),
        PlaceScaledCropAxis(crop.y, crop.height, height, video.height, anchorY),
        width,
        height};
}

std::string FormatLibVlc3CropGeometry(const VideoCrop& crop) {
    if (!crop.IsValid()) {
        return {};
    }

    const std::uint64_t right =
        static_cast<std::uint64_t>(crop.x) + crop.width;
    const std::uint64_t bottom =
        static_cast<std::uint64_t>(crop.y) + crop.height;
    const std::uint64_t maximum = (std::numeric_limits<unsigned>::max)();
    if (right > maximum || bottom > maximum) {
        return {};
    }

    // VLC 3.0's crop-window path forwards these two parsed values as the
    // absolute right/bottom edges. Supplying extents loses the selected
    // right and bottom portions whenever x or y is non-zero.
    return std::to_string(right) + "x" + std::to_string(bottom) +
        "+" + std::to_string(crop.x) + "+" + std::to_string(crop.y);
}

}  // namespace videoplayer
