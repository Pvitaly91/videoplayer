#include "PlaybackMath.h"

#include <limits>

namespace videoplayer {

std::int64_t ClampTime(const std::int64_t valueMs, const std::int64_t durationMs) noexcept {
    if (valueMs <= 0) {
        return 0;
    }
    if (durationMs >= 0 && valueMs > durationMs) {
        return durationMs;
    }
    return valueMs;
}

std::int64_t SeekBy(
    const std::int64_t currentMs,
    const std::int64_t deltaMs,
    const std::int64_t durationMs) noexcept {
    std::int64_t result;
    if (deltaMs > 0 && currentMs > (std::numeric_limits<std::int64_t>::max)() - deltaMs) {
        result = (std::numeric_limits<std::int64_t>::max)();
    } else if (
        deltaMs < 0 &&
        currentMs < (std::numeric_limits<std::int64_t>::min)() - deltaMs) {
        result = (std::numeric_limits<std::int64_t>::min)();
    } else {
        result = currentMs + deltaMs;
    }
    return ClampTime(result, durationMs);
}

int ClampVolume(const int volume) noexcept {
    if (volume < 0) {
        return 0;
    }
    if (volume > 100) {
        return 100;
    }
    return volume;
}

}  // namespace videoplayer
