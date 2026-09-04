#include "VideoGeometry.h"

#include <algorithm>
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

std::string FormatVideoCropGeometry(const VideoCrop& crop) {
    if (!crop.IsValid()) {
        return {};
    }
    return std::to_string(crop.width) + "x" + std::to_string(crop.height) +
        "+" + std::to_string(crop.x) + "+" + std::to_string(crop.y);
}

}  // namespace videoplayer
