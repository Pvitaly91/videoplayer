#pragma once

#include "PlaybackSnapshot.h"

#include <cstdint>

namespace videoplayer {

enum class LoopMode : std::uint8_t {
    Off,
    WholeVideo,
    Segment,
};

enum class LoopActionKind : std::uint8_t {
    None,
    Seek,
    Play,
};

struct LoopAction final {
    LoopActionKind kind = LoopActionKind::None;
    std::int64_t positionMs = 0;
};

constexpr bool operator==(const LoopAction left, const LoopAction right) noexcept {
    return left.kind == right.kind && left.positionMs == right.positionMs;
}

// A deterministic controller for loop configuration and asynchronous playback
// acknowledgement. It owns no player, UI, timer, or thread; callers execute the
// returned action and feed later PlaybackSnapshot values back to Update().
class PlaybackLoop final {
public:
    LoopMode Mode() const noexcept { return mode_; }
    std::int64_t StartMs() const noexcept { return startMs_; }
    std::int64_t EndMs() const noexcept { return endMs_; }

    bool HasSegment(const std::int64_t durationMs) const noexcept {
        return durationMs > 0 && startMs_ >= 0 && startMs_ < durationMs &&
            endMs_ > startMs_ && endMs_ <= durationMs &&
            endMs_ - startMs_ >= kMinimumSegmentMs;
    }

    void ToggleWholeVideo() noexcept {
        CancelPending();
        mode_ = mode_ == LoopMode::WholeVideo
            ? LoopMode::Off
            : LoopMode::WholeVideo;
    }

    bool SetStart(
        const std::int64_t positionMs,
        const std::int64_t durationMs) noexcept {
        if (durationMs <= 0) {
            return false;
        }
        CancelPending();
        startMs_ = positionMs <= 0
            ? 0
            : positionMs >= durationMs ? durationMs - 1 : positionMs;
        endMs_ = kUnsetMarkerMs;
        mode_ = LoopMode::Off;
        return true;
    }

    bool SetEnd(
        const std::int64_t positionMs,
        const std::int64_t durationMs) noexcept {
        if (durationMs <= 0 || startMs_ < 0 || startMs_ >= durationMs ||
            positionMs <= startMs_ || positionMs > durationMs ||
            positionMs - startMs_ < kMinimumSegmentMs) {
            return false;
        }
        CancelPending();
        endMs_ = positionMs;
        mode_ = LoopMode::Segment;
        return true;
    }

    bool ToggleSegment(const std::int64_t durationMs) noexcept {
        // Switching off must always work, even if a length update has made
        // the stored endpoint invalid or duration is temporarily unavailable.
        if (mode_ == LoopMode::Segment) {
            CancelPending();
            mode_ = LoopMode::Off;
            return true;
        }
        if (!HasSegment(durationMs)) {
            return false;
        }
        CancelPending();
        mode_ = LoopMode::Segment;
        return true;
    }

    void ClearSegment() noexcept {
        if (mode_ == LoopMode::Segment) {
            mode_ = LoopMode::Off;
            CancelPending();
        }
        startMs_ = kUnsetMarkerMs;
        endMs_ = kUnsetMarkerMs;
    }

    void Reset() noexcept {
        ResetConfiguration();
    }

    void CancelPending() noexcept {
        pending_ = PendingAction::None;
        pendingSinceMs_ = 0;
        restartSeekAwaitingProgress_ = false;
        restartSeekAckPositionMs_ = kUnsetMarkerMs;
    }

    LoopAction Update(
        const PlaybackSnapshot& snapshot,
        const std::uint64_t nowMs,
        const bool suspended = false) noexcept {
        if (!generationBound_) {
            generationBound_ = true;
            generation_ = snapshot.generation;
        } else if (snapshot.generation != generation_) {
            ResetConfiguration();
            generationBound_ = true;
            generation_ = snapshot.generation;
            return {};
        }

        if (snapshot.state == PlaybackState::Paused) {
            CancelPending();
            return {};
        }
        if (snapshot.state == PlaybackState::Error) {
            ResetConfiguration();
            return {};
        }
        if (suspended) {
            return {};
        }
        if (mode_ == LoopMode::Off) {
            CancelPending();
            return {};
        }

        switch (pending_) {
        case PendingAction::WholeRestart:
            if (snapshot.state == PlaybackState::Playing) {
                CancelPending();
                return {};
            }
            if (PendingTimedOut(nowMs)) {
                DisableAfterTimeout();
            }
            return {};

        case PendingAction::SegmentRestart:
            if (snapshot.state == PlaybackState::Playing && snapshot.seekable &&
                HasSegment(snapshot.durationMs)) {
                BeginPending(PendingAction::SegmentSeek, nowMs, true);
                return {LoopActionKind::Seek, startMs_};
            }
            if (PendingTimedOut(nowMs)) {
                DisableAfterTimeout();
            }
            return {};

        case PendingAction::SegmentSeek:
            if (snapshot.state == PlaybackState::Playing && snapshot.seekable &&
                HasSegment(snapshot.durationMs)) {
                if (snapshot.positionMs >= startMs_ && snapshot.positionMs < endMs_) {
                    if (!restartSeekAwaitingProgress_) {
                        CancelPending();
                    } else if (restartSeekAckPositionMs_ == kUnsetMarkerMs) {
                        restartSeekAckPositionMs_ = snapshot.positionMs;
                    } else if (snapshot.positionMs > restartSeekAckPositionMs_) {
                        CancelPending();
                    }
                    if (PendingTimedOut(nowMs)) {
                        DisableAfterTimeout();
                    }
                    return {};
                }
                if (restartSeekAwaitingProgress_ && snapshot.positionMs < startMs_) {
                    // Restart can briefly expose the requested A position and then
                    // regress to the new input's real zero. Require fresh forward
                    // progress after that regression before releasing the guard.
                    restartSeekAckPositionMs_ = kUnsetMarkerMs;
                }
            }
            if (snapshot.state == PlaybackState::Ended &&
                HasSegment(snapshot.durationMs)) {
                BeginPending(PendingAction::SegmentRestart, nowMs);
                return {LoopActionKind::Play, 0};
            }
            if (PendingTimedOut(nowMs)) {
                DisableAfterTimeout();
            }
            return {};

        case PendingAction::None:
        default:
            break;
        }

        if (mode_ == LoopMode::WholeVideo) {
            if (snapshot.state == PlaybackState::Ended) {
                BeginPending(PendingAction::WholeRestart, nowMs);
                return {LoopActionKind::Play, 0};
            }
            return {};
        }

        if (!HasSegment(snapshot.durationMs)) {
            return {};
        }
        if (snapshot.state == PlaybackState::Ended) {
            BeginPending(PendingAction::SegmentRestart, nowMs);
            return {LoopActionKind::Play, 0};
        }
        if (snapshot.state == PlaybackState::Playing && snapshot.seekable &&
            (snapshot.positionMs < startMs_ || snapshot.positionMs >= endMs_)) {
            BeginPending(PendingAction::SegmentSeek, nowMs);
            return {LoopActionKind::Seek, startMs_};
        }
        return {};
    }

private:
    static constexpr std::int64_t kUnsetMarkerMs = -1;
    static constexpr std::int64_t kMinimumSegmentMs = 500;
    static constexpr std::uint64_t kPendingTimeoutMs = 5000;

    enum class PendingAction : std::uint8_t {
        None,
        WholeRestart,
        SegmentRestart,
        SegmentSeek,
    };

    void BeginPending(
        const PendingAction pending,
        const std::uint64_t nowMs,
        const bool restartSeekAwaitingProgress = false) noexcept {
        pending_ = pending;
        pendingSinceMs_ = nowMs;
        restartSeekAwaitingProgress_ =
            pending == PendingAction::SegmentSeek && restartSeekAwaitingProgress;
        restartSeekAckPositionMs_ = kUnsetMarkerMs;
    }

    bool PendingTimedOut(const std::uint64_t nowMs) const noexcept {
        return pending_ != PendingAction::None && nowMs >= pendingSinceMs_ &&
            nowMs - pendingSinceMs_ >= kPendingTimeoutMs;
    }

    void DisableAfterTimeout() noexcept {
        mode_ = LoopMode::Off;
        CancelPending();
    }

    void ResetConfiguration() noexcept {
        mode_ = LoopMode::Off;
        startMs_ = kUnsetMarkerMs;
        endMs_ = kUnsetMarkerMs;
        CancelPending();
    }

    LoopMode mode_ = LoopMode::Off;
    std::int64_t startMs_ = kUnsetMarkerMs;
    std::int64_t endMs_ = kUnsetMarkerMs;
    PendingAction pending_ = PendingAction::None;
    std::uint64_t pendingSinceMs_ = 0;
    bool restartSeekAwaitingProgress_ = false;
    std::int64_t restartSeekAckPositionMs_ = kUnsetMarkerMs;
    bool generationBound_ = false;
    std::uint32_t generation_ = 0;
};

}  // namespace videoplayer
