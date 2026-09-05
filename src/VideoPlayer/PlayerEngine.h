#pragma once

#include "PlaybackSnapshot.h"
#include "VideoGeometry.h"

#include <cstdint>
#include <memory>
#include <string>

namespace videoplayer {

class PlayerEngine final {
public:
    PlayerEngine();
    ~PlayerEngine();

    PlayerEngine(const PlayerEngine&) = delete;
    PlayerEngine& operator=(const PlayerEngine&) = delete;
    PlayerEngine(PlayerEngine&&) = delete;
    PlayerEngine& operator=(PlayerEngine&&) = delete;

    bool Initialize(HWND videoWindow, HWND notificationWindow, std::wstring& error);
    bool Open(const std::wstring& path, std::wstring& error);
    bool Play(std::wstring& error);
    void Pause() noexcept;
    void Stop() noexcept;
    void Seek(std::int64_t milliseconds) noexcept;
    void SetVolume(int volume) noexcept;
    void SetMuted(bool muted) noexcept;
    void SetVideoWindow(HWND videoWindow) noexcept;
    PlaybackSnapshot Snapshot() const noexcept;
    PlaybackSnapshot CachedSnapshot() const noexcept;
    std::int64_t PositionMs() const noexcept;
    std::int64_t DurationMs() const noexcept;
    bool GetVideoSize(VideoDimensions& dimensions) const noexcept;
    bool ApplyVideoCrop(const VideoCrop& crop) noexcept;
    void ResetVideoCrop() noexcept;
    bool IsVideoCropped() const noexcept;
    bool GetVideoCrop(VideoCrop& crop) const noexcept;
    bool IsSeekable() const noexcept;
    bool IsMuted() const noexcept;
    int Volume() const noexcept;
    PlaybackState State() const noexcept;
    std::uint32_t Generation() const noexcept;
    bool HasMedia() const noexcept;
    bool IsInitialized() const noexcept;
    void Shutdown() noexcept;

private:
#if defined(VIDEOPLAYER_TESTING)
    friend struct PlayerEngineTestAccess;
#endif
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace videoplayer
