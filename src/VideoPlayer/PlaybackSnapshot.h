#pragma once

#include "PlaybackMath.h"
#include "PlaybackState.h"

#include <mutex>

namespace videoplayer {

struct PlaybackSnapshot final {
    std::uint32_t generation = 0;
    PlaybackState state = PlaybackState::Stopped;
    std::int64_t positionMs = 0;
    std::int64_t durationMs = 0;
    bool seekable = false;
};

// This is the shared state used by the actual LibVLC callback and UI poll.
// Its mutex protects only a small value: never call LibVLC, Win32, or wait for
// callbacks while holding it. Event generations are immutable at attachment.
class PlaybackSnapshotStore final {
public:
    PlaybackSnapshot Read() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return value_;
    }

    std::uint32_t BeginMedia() {
        const std::lock_guard<std::mutex> lock(mutex_);
        const std::uint32_t generation = value_.generation + 1U;
        value_ = {};
        value_.generation = generation;
        return generation;
    }

    bool SetState(const std::uint32_t generation, const PlaybackState state) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (value_.generation != generation) {
            return false;
        }
        value_.state = state;
        NormalizePosition();
        return true;
    }

    PlaybackSnapshot Observe(
        const std::uint32_t generation,
        const PlaybackState state,
        const std::int64_t positionMs,
        const std::int64_t durationMs,
        const bool seekable) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (value_.generation == generation) {
            value_.state = state;
            if (durationMs >= 0) {
                value_.durationMs = durationMs;
            }
            if (positionMs >= 0) {
                value_.positionMs = positionMs;
            }
            value_.seekable = seekable;
            NormalizePosition();
        }
        return value_;
    }

    void Seek(const std::uint32_t generation, const std::int64_t positionMs) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (value_.generation == generation && value_.seekable) {
            value_.positionMs = ClampTime(positionMs, value_.durationMs);
        }
    }

private:
    void NormalizePosition() noexcept {
        if (value_.state == PlaybackState::Stopped) {
            value_.positionMs = 0;
        } else if (value_.state == PlaybackState::Ended) {
            value_.positionMs = value_.durationMs;
        } else {
            value_.positionMs = ClampTime(value_.positionMs, value_.durationMs);
        }
    }

    mutable std::mutex mutex_;
    PlaybackSnapshot value_{};
};

}  // namespace videoplayer
