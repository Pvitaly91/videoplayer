#pragma once

#include "targetver.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ExportTypes.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace videoplayer {

// Saved ranges are expressed in the original video's nonnegative timeline.
// A shared endpoint is allowed: only a position strictly inside is occupied.
inline bool IsInsideSavedInterval(
    const std::int64_t positionMs,
    const std::vector<VideoInterval>& intervals) noexcept {
    for (const VideoInterval& interval : intervals) {
        if (interval.startMs >= 0 && interval.endMs > interval.startMs &&
            positionMs > interval.startMs && positionMs < interval.endMs) {
            return true;
        }
    }
    return false;
}

// Reject positive-duration overlap, including containment. Adjacent ranges do
// not overlap, and malformed ranges cannot block a valid selection.
inline bool OverlapsSavedInterval(
    const VideoInterval candidate,
    const std::vector<VideoInterval>& intervals) noexcept {
    if (candidate.startMs < 0 || candidate.endMs <= candidate.startMs) {
        return false;
    }
    for (const VideoInterval& interval : intervals) {
        if (interval.startMs >= 0 && interval.endMs > interval.startMs &&
            (std::max)(candidate.startMs, interval.startMs) <
                (std::min)(candidate.endMs, interval.endMs)) {
            return true;
        }
    }
    return false;
}

namespace interval_timeline_detail {

struct ScaledPosition final {
    std::uint64_t quotient = 0;
    std::uint64_t remainder = 0;
};

// Exact numerator * width / denominator, including its remainder, without a
// potentially overflowing multiplication. The numerator is in [0, denominator]
// and the width is at most INT_MAX. The denominator is at most INT64_MAX.
inline ScaledPosition ScalePosition(
    const std::uint64_t numerator,
    const std::uint64_t denominator,
    const std::uint32_t width) noexcept {
    if (numerator == denominator) {
        return {width, 0};
    }
    ScaledPosition scaled{};
    for (int bit = 31; bit >= 0; --bit) {
        scaled.quotient *= 2;
        if (scaled.remainder >= denominator - scaled.remainder) {
            scaled.remainder -= denominator - scaled.remainder;
            ++scaled.quotient;
        } else {
            scaled.remainder += scaled.remainder;
        }
        if ((width & (std::uint32_t{1} << bit)) != 0U) {
            if (scaled.remainder >= denominator - numerator) {
                scaled.remainder -= denominator - numerator;
                ++scaled.quotient;
            } else {
                scaled.remainder += numerator;
            }
        }
    }
    return scaled;
}

}  // namespace interval_timeline_detail

inline LONG IntervalTimelineX(const std::int64_t positionMs,
    const std::int64_t durationMs, const RECT& timeline) noexcept {
    const auto width = static_cast<std::int64_t>(timeline.right) - timeline.left;
    if (durationMs <= 0 || width <= 0 || width > (std::numeric_limits<int>::max)()) return timeline.left;
    const auto position = std::clamp(positionMs, std::int64_t{0}, durationMs);
    const auto scaled = interval_timeline_detail::ScalePosition(
        static_cast<std::uint64_t>(position), static_cast<std::uint64_t>(durationMs),
        static_cast<std::uint32_t>(width));
    const auto rounded = scaled.quotient +
        (scaled.remainder >= static_cast<std::uint64_t>(durationMs) - scaled.remainder ? 1 : 0);
    return static_cast<LONG>(timeline.left + static_cast<std::int64_t>(rounded));
}

// Clips the range to the video and returns its occupied pixels in [left, right).
// Left uses floor, right uses ceil, so even a subpixel range is visible. Wider
// than INT_MAX timelines and malformed/fully clipped ranges return an empty RECT.
inline RECT IntervalTimelineRect(
    const VideoInterval interval,
    const std::int64_t durationMs,
    const RECT& timeline) noexcept {
    const std::int64_t width =
        static_cast<std::int64_t>(timeline.right) - timeline.left;
    if (durationMs <= 0 || interval.endMs <= interval.startMs || width <= 0 ||
        width > (std::numeric_limits<int>::max)() ||
        timeline.bottom <= timeline.top) {
        return {};
    }
    const std::int64_t start = (std::max)(std::int64_t{0}, interval.startMs);
    const std::int64_t end = (std::min)(durationMs, interval.endMs);
    if (end <= start) {
        return {};
    }
    const auto first = interval_timeline_detail::ScalePosition(
        static_cast<std::uint64_t>(start),
        static_cast<std::uint64_t>(durationMs),
        static_cast<std::uint32_t>(width));
    const auto last = interval_timeline_detail::ScalePosition(
        static_cast<std::uint64_t>(end),
        static_cast<std::uint64_t>(durationMs),
        static_cast<std::uint32_t>(width));
    const std::int64_t rightOffset = static_cast<std::int64_t>(last.quotient) +
        (last.remainder != 0 ? 1 : 0);
    return {
        static_cast<LONG>(static_cast<std::int64_t>(timeline.left) +
            static_cast<std::int64_t>(first.quotient)),
        timeline.top,
        static_cast<LONG>(static_cast<std::int64_t>(timeline.left) + rightOffset),
        timeline.bottom};
}

}  // namespace videoplayer
