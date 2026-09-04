#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>

namespace videoplayer {

enum class PlaybackState : std::uint8_t {
    Stopped,
    Opening,
    Playing,
    Paused,
    Ended,
    Error,
};

enum class PlayerEvent : std::uintptr_t {
    Opening = 1,
    Playing,
    Paused,
    Stopped,
    EndReached,
    EncounteredError,
    LengthChanged,
    SeekableChanged,
};

inline constexpr UINT kPlayerEventMessage = WM_APP + 0x31;

}  // namespace videoplayer
