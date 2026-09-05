#pragma once

#include "../../src/VideoPlayer/PlayerEngineTestAccess.h"

#include <cstdint>
#include <functional>
#include <string>

namespace videoplayer::tests {

// A narrow fake of the LibVLC C boundary, not an alternative PlayerEngine.
// Production snapshot, seek, crop and lifecycle code still execute normally.
struct FakePlayerBackend final {
    std::int64_t positionMs = 2000;
    std::int64_t durationMs = 20000;
    VideoDimensions dimensions{640, 360};
    int backendState = 3; // libvlc_Playing
    bool seekable = true;
    int seekCalls = 0;
    int cropCalls = 0;
    int cropResetCalls = 0;
    int stopCalls = 0;
    int volume = 100;
    bool muted = false;
    std::string cropGeometry;
    std::function<void()> onCrop;

    static FakePlayerBackend& From(libvlc_media_player_t* player) noexcept {
        return *reinterpret_cast<FakePlayerBackend*>(player);
    }
    static libvlc_state_t __cdecl GetState(libvlc_media_player_t* player) {
        return static_cast<libvlc_state_t>(From(player).backendState);
    }
    static libvlc_time_t __cdecl GetTime(libvlc_media_player_t* player) {
        return From(player).positionMs;
    }
    static libvlc_time_t __cdecl GetLength(libvlc_media_player_t* player) {
        return From(player).durationMs;
    }
    static int __cdecl IsSeekable(libvlc_media_player_t* player) {
        return From(player).seekable ? 1 : 0;
    }
    static void __cdecl SetTime(libvlc_media_player_t* player, libvlc_time_t time) {
        auto& self = From(player);
        ++self.seekCalls;
        self.positionMs = time;
    }
    static int __cdecl VideoSize(
        libvlc_media_player_t* player, unsigned, unsigned* width, unsigned* height) {
        const auto& self = From(player);
        *width = self.dimensions.width;
        *height = self.dimensions.height;
        return 0;
    }
    static void __cdecl Crop(libvlc_media_player_t* player, const char* geometry) {
        auto& self = From(player);
        if (geometry == nullptr) {
            ++self.cropResetCalls;
        } else {
            ++self.cropCalls;
        }
        self.cropGeometry = geometry == nullptr ? "" : geometry;
        if (self.onCrop) {
            self.onCrop();
        }
    }
    static void __cdecl Scale(libvlc_media_player_t*, float) {}
    static void __cdecl SetMedia(libvlc_media_player_t*, libvlc_media_t*) {}
    static void __cdecl SetHwnd(libvlc_media_player_t*, void*) {}
    static void __cdecl SetInput(libvlc_media_player_t*, unsigned) {}
    static int __cdecl Play(libvlc_media_player_t* player) {
        From(player).backendState = 3;
        return 0;
    }
    static void __cdecl Pause(libvlc_media_player_t* player, int paused) {
        From(player).backendState = paused != 0 ? 4 : 3;
    }
    static void __cdecl Stop(libvlc_media_player_t* player) {
        auto& self = From(player);
        ++self.stopCalls;
        self.backendState = 5;
        self.positionMs = 0;
    }
    static int __cdecl Volume(libvlc_media_player_t* player, int volume) {
        From(player).volume = volume;
        return 0;
    }
    static void __cdecl Mute(libvlc_media_player_t* player, int muted) {
        From(player).muted = muted != 0;
    }
    static const char* __cdecl Error() { return "synthetic test backend"; }

    void Install(PlayerEngine& engine) {
        LibVlcRuntime::Api api{};
        api.mediaPlayerGetState = GetState;
        api.mediaPlayerGetTime = GetTime;
        api.mediaPlayerGetLength = GetLength;
        api.mediaPlayerIsSeekable = IsSeekable;
        api.mediaPlayerSetTime = SetTime;
        api.videoGetSize = VideoSize;
        api.videoSetCropGeometry = Crop;
        api.videoSetScale = Scale;
        api.mediaPlayerSetMedia = SetMedia;
        api.mediaPlayerSetHwnd = SetHwnd;
        api.videoSetKeyInput = SetInput;
        api.videoSetMouseInput = SetInput;
        api.mediaPlayerPlay = Play;
        api.mediaPlayerSetPause = Pause;
        api.mediaPlayerStop = Stop;
        api.audioSetVolume = Volume;
        api.audioSetMute = Mute;
        api.errorMessage = Error;
        PlaybackSnapshot snapshot{};
        snapshot.state = PlaybackState::Playing;
        snapshot.positionMs = positionMs;
        snapshot.durationMs = durationMs;
        snapshot.seekable = seekable;
        PlayerEngineTestAccess::Install(
            engine, api,
            reinterpret_cast<libvlc_media_player_t*>(this),
            reinterpret_cast<libvlc_media_t*>(this), snapshot);
    }
};

} // namespace videoplayer::tests
