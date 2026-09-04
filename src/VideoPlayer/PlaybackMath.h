#pragma once

#include <cstdint>

namespace videoplayer {

std::int64_t ClampTime(std::int64_t valueMs, std::int64_t durationMs) noexcept;
std::int64_t SeekBy(
    std::int64_t currentMs,
    std::int64_t deltaMs,
    std::int64_t durationMs) noexcept;
int ClampVolume(int volume) noexcept;

}  // namespace videoplayer
