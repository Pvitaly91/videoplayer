#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>

namespace videoplayer {

inline constexpr std::int64_t kPreviewTimestampQuantumMs = 750;
inline constexpr unsigned int kPreviewMaximumWidth = 320;
inline constexpr unsigned int kPreviewMaximumHeight = 180;

std::int64_t QuantizePreviewTimestamp(
    std::int64_t timestampMs,
    std::int64_t durationMs,
    std::int64_t quantumMs = kPreviewTimestampQuantumMs) noexcept;

std::int64_t PreviewTimeFromChannelX(
    int mouseX,
    const RECT& channel,
    std::int64_t durationMs) noexcept;

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
