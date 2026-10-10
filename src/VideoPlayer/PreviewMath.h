#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <vector>

namespace videoplayer {

inline constexpr std::int64_t kPreviewTimestampQuantumMs = 5'000;
inline constexpr std::int64_t kPreviewMaximumIntervals = 120;
inline constexpr unsigned int kPreviewMaximumWidth = 320;
inline constexpr unsigned int kPreviewMaximumHeight = 180;

// At most 120 preview intervals, with a five-second minimum. Seeking stays exact.
std::int64_t PreviewTimestampQuantum(std::int64_t durationMs) noexcept;
// Every preview bucket, including both endpoints, ordered coarse-to-fine so
// background warming covers the whole timeline before filling smaller gaps.
std::vector<std::int64_t> BuildPreviewWarmupTimestamps(std::int64_t durationMs);
std::int64_t QuantizePreviewTimestamp(
    std::int64_t timestampMs,
    std::int64_t durationMs) noexcept;
std::int64_t QuantizePreviewTimestamp(
    std::int64_t timestampMs,
    std::int64_t durationMs,
    std::int64_t quantumMs) noexcept;

std::int64_t PreviewTimeFromChannelX(
    int mouseX,
    const RECT& channel,
    std::int64_t durationMs) noexcept;

// TBM_GETCHANNELRECT reports the outside channel edges, while the thumb
// center can only travel inside those edges by half of its width.
RECT TrackbarPointerRange(
    const RECT& channel,
    const RECT& thumb) noexcept;

int TrackbarPositionFromPointerX(
    int mouseX,
    const RECT& pointerRange,
    int minimum,
    int maximum) noexcept;

SIZE FitPreviewSize(
    unsigned int sourceWidth,
    unsigned int sourceHeight,
    unsigned int maximumWidth = kPreviewMaximumWidth,
    unsigned int maximumHeight = kPreviewMaximumHeight) noexcept;

int ScalePreviewPixels(int logicalPixels, int dpi) noexcept;

RECT PlacePreviewPopup(
    POINT anchorScreen,
    SIZE popupSize,
    const RECT& monitorBounds,
    int verticalGap) noexcept;

}  // namespace videoplayer
