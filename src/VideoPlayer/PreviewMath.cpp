#include "PreviewMath.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace videoplayer {
namespace {

LONG ClampLongLong(const long long value, const LONG minimum, const LONG maximum) noexcept {
    return static_cast<LONG>((std::max)(
        static_cast<long long>(minimum),
        (std::min)(value, static_cast<long long>(maximum))));
}

}  // namespace

std::int64_t QuantizePreviewTimestamp(
    const std::int64_t timestampMs,
    const std::int64_t durationMs,
    const std::int64_t quantumMs) noexcept {
    if (durationMs <= 0 || quantumMs <= 0) {
        return 0;
    }

    const std::int64_t clamped = (std::max)(
        std::int64_t{0},
        (std::min)(timestampMs, durationMs));
    if (clamped == durationMs) {
        return durationMs;
    }

    const std::int64_t quotient = clamped / quantumMs;
    const std::int64_t remainder = clamped % quantumMs;
    std::int64_t quantized = quotient * quantumMs;
    const std::int64_t roundingThreshold =
        (quantumMs / 2) + (quantumMs % 2);
    if (remainder >= roundingThreshold &&
        quantized <= (std::numeric_limits<std::int64_t>::max)() - quantumMs) {
        quantized += quantumMs;
    }
    return (std::min)(quantized, durationMs);
}

std::int64_t PreviewTimeFromChannelX(
    const int mouseX,
    const RECT& channel,
    const std::int64_t durationMs) noexcept {
    if (durationMs <= 0 || channel.right <= channel.left) {
        return 0;
    }

    const LONG clampedX = (std::max)(
        channel.left,
        (std::min)(static_cast<LONG>(mouseX), channel.right));
    const std::int64_t offset =
        static_cast<std::int64_t>(clampedX) - channel.left;
    const std::int64_t width =
        static_cast<std::int64_t>(channel.right) - channel.left;
    if (offset <= 0) {
        return 0;
    }
    if (offset >= width) {
        return durationMs;
    }

    // Split the product so multi-day media cannot overflow int64_t.
    const std::int64_t whole = durationMs / width;
    const std::int64_t remainder = durationMs % width;
    const std::uint64_t fractional =
        (static_cast<std::uint64_t>(remainder) *
            static_cast<std::uint64_t>(offset)) /
        static_cast<std::uint64_t>(width);
    return (whole * offset) + static_cast<std::int64_t>(fractional);
}

SIZE FitPreviewSize(
    const unsigned int sourceWidth,
    const unsigned int sourceHeight,
    const unsigned int maximumWidth,
    const unsigned int maximumHeight) noexcept {
    SIZE result{};
    if (sourceWidth == 0 || sourceHeight == 0 ||
        maximumWidth == 0 || maximumHeight == 0) {
        return result;
    }

    const std::uint64_t sourceAcrossMaximumHeight =
        static_cast<std::uint64_t>(sourceWidth) * maximumHeight;
    const std::uint64_t sourceAcrossMaximumWidth =
        static_cast<std::uint64_t>(sourceHeight) * maximumWidth;
    if (sourceAcrossMaximumHeight >= sourceAcrossMaximumWidth) {
        result.cx = static_cast<LONG>(maximumWidth);
        result.cy = static_cast<LONG>((std::max)(
            std::uint64_t{1},
            (static_cast<std::uint64_t>(sourceHeight) * maximumWidth) /
                sourceWidth));
    } else {
        result.cy = static_cast<LONG>(maximumHeight);
        result.cx = static_cast<LONG>((std::max)(
            std::uint64_t{1},
            (static_cast<std::uint64_t>(sourceWidth) * maximumHeight) /
                sourceHeight));
    }
    return result;
}

int ScalePreviewPixels(const int logicalPixels, const int dpi) noexcept {
    const int safeDpi = dpi > 0 ? dpi : USER_DEFAULT_SCREEN_DPI;
    const long long scaled =
        (static_cast<long long>(logicalPixels) * safeDpi) /
        USER_DEFAULT_SCREEN_DPI;
    return static_cast<int>((std::max)(
        static_cast<long long>((std::numeric_limits<int>::min)()),
        (std::min)(
            scaled,
            static_cast<long long>((std::numeric_limits<int>::max)()))));
}

RECT PlacePreviewPopup(
    const POINT anchorScreen,
    const SIZE popupSize,
    const RECT& monitorBounds,
    const int verticalGap) noexcept {
    const LONG monitorWidth = (std::max)(0L, monitorBounds.right - monitorBounds.left);
    const LONG monitorHeight = (std::max)(0L, monitorBounds.bottom - monitorBounds.top);
    const LONG width = (std::max)(0L, (std::min)(popupSize.cx, monitorWidth));
    const LONG height = (std::max)(0L, (std::min)(popupSize.cy, monitorHeight));

    const long long maximumLeft = static_cast<long long>(monitorBounds.right) - width;
    const long long proposedLeft = static_cast<long long>(anchorScreen.x) - width / 2;
    const LONG left = ClampLongLong(
        proposedLeft,
        monitorBounds.left,
        static_cast<LONG>(maximumLeft));

    long long proposedTop = static_cast<long long>(anchorScreen.y) - verticalGap - height;
    if (proposedTop < monitorBounds.top) {
        proposedTop = static_cast<long long>(anchorScreen.y) + verticalGap;
    }
    const long long maximumTop = static_cast<long long>(monitorBounds.bottom) - height;
    const LONG top = ClampLongLong(
        proposedTop,
        monitorBounds.top,
        static_cast<LONG>(maximumTop));
    return RECT{left, top, left + width, top + height};
}

}  // namespace videoplayer
