#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace videoplayer {

// Minimal LibVLC 3.x C ABI declarations. The official Windows runtime
// archives do not ship SDK headers, and the application deliberately has no
// load-time dependency on an import library.
struct libvlc_instance_t;
struct libvlc_media_t;
struct libvlc_media_player_t;
struct libvlc_event_manager_t;

using libvlc_time_t = std::int64_t;

struct libvlc_event_t final {
    int type;
};

enum libvlc_event_e : int {
    libvlc_MediaPlayerOpening = 0x102,
    libvlc_MediaPlayerPlaying = 0x104,
    libvlc_MediaPlayerPaused = 0x105,
    libvlc_MediaPlayerStopped = 0x106,
    libvlc_MediaPlayerEndReached = 0x109,
    libvlc_MediaPlayerEncounteredError = 0x10A,
    libvlc_MediaPlayerSeekableChanged = 0x10D,
    libvlc_MediaPlayerLengthChanged = 0x111,
};

using LibVlcCallback = void(__cdecl*)(const libvlc_event_t*, void*);
using LibVlcNew = libvlc_instance_t*(__cdecl*)(int, const char* const*);
using LibVlcRelease = void(__cdecl*)(libvlc_instance_t*);
using LibVlcGetVersion = const char*(__cdecl*)();
using LibVlcErrorMessage = const char*(__cdecl*)();
using LibVlcMediaNewPath = libvlc_media_t*(__cdecl*)(libvlc_instance_t*, const char*);
using LibVlcMediaOpenCallback = int(__cdecl*)(void*, void**, std::uint64_t*);
using LibVlcMediaReadCallback =
    std::intptr_t(__cdecl*)(void*, unsigned char*, std::size_t);
using LibVlcMediaSeekCallback = int(__cdecl*)(void*, std::uint64_t);
using LibVlcMediaCloseCallback = void(__cdecl*)(void*);
using LibVlcMediaNewCallbacks = libvlc_media_t*(__cdecl*)(
    libvlc_instance_t*,
    LibVlcMediaOpenCallback,
    LibVlcMediaReadCallback,
    LibVlcMediaSeekCallback,
    LibVlcMediaCloseCallback,
    void*);
using LibVlcMediaRelease = void(__cdecl*)(libvlc_media_t*);
using LibVlcMediaPlayerNew = libvlc_media_player_t*(__cdecl*)(libvlc_instance_t*);
using LibVlcMediaPlayerRelease = void(__cdecl*)(libvlc_media_player_t*);
using LibVlcMediaPlayerSetMedia = void(__cdecl*)(libvlc_media_player_t*, libvlc_media_t*);
using LibVlcMediaPlayerSetHwnd = void(__cdecl*)(libvlc_media_player_t*, void*);
using LibVlcVideoSetKeyInput = void(__cdecl*)(libvlc_media_player_t*, unsigned);
using LibVlcVideoSetMouseInput = void(__cdecl*)(libvlc_media_player_t*, unsigned);
using LibVlcVideoLockCallback = void*(__cdecl*)(void*, void**);
using LibVlcVideoUnlockCallback =
    void(__cdecl*)(void*, void*, void* const*);
using LibVlcVideoDisplayCallback = void(__cdecl*)(void*, void*);
using LibVlcVideoFormatCallback =
    unsigned(__cdecl*)(void**, char*, unsigned*, unsigned*, unsigned*, unsigned*);
using LibVlcVideoCleanupCallback = void(__cdecl*)(void*);
using LibVlcVideoSetCallbacks = void(__cdecl*)(
    libvlc_media_player_t*,
    LibVlcVideoLockCallback,
    LibVlcVideoUnlockCallback,
    LibVlcVideoDisplayCallback,
    void*);
using LibVlcVideoSetFormatCallbacks = void(__cdecl*)(
    libvlc_media_player_t*,
    LibVlcVideoFormatCallback,
    LibVlcVideoCleanupCallback);
using LibVlcVideoGetSize =
    int(__cdecl*)(libvlc_media_player_t*, unsigned, unsigned*, unsigned*);
using LibVlcVideoSetCropGeometry =
    void(__cdecl*)(libvlc_media_player_t*, const char*);
using LibVlcVideoSetScale = void(__cdecl*)(libvlc_media_player_t*, float);
using LibVlcMediaPlayerPlay = int(__cdecl*)(libvlc_media_player_t*);
using LibVlcMediaPlayerSetPause = void(__cdecl*)(libvlc_media_player_t*, int);
using LibVlcMediaPlayerStop = void(__cdecl*)(libvlc_media_player_t*);
using LibVlcMediaPlayerGetTime = libvlc_time_t(__cdecl*)(libvlc_media_player_t*);
using LibVlcMediaPlayerSetTime = void(__cdecl*)(libvlc_media_player_t*, libvlc_time_t);
using LibVlcMediaPlayerGetLength = libvlc_time_t(__cdecl*)(libvlc_media_player_t*);
using LibVlcMediaPlayerIsSeekable = int(__cdecl*)(libvlc_media_player_t*);
using LibVlcMediaPlayerEventManager =
    libvlc_event_manager_t*(__cdecl*)(libvlc_media_player_t*);
using LibVlcAudioSetVolume = int(__cdecl*)(libvlc_media_player_t*, int);
using LibVlcAudioSetMute = void(__cdecl*)(libvlc_media_player_t*, int);
using LibVlcEventAttach =
    int(__cdecl*)(libvlc_event_manager_t*, int, LibVlcCallback, void*);
using LibVlcEventDetach =
    void(__cdecl*)(libvlc_event_manager_t*, int, LibVlcCallback, void*);

class LibVlcRuntime final {
public:
    struct Api final {
        LibVlcNew newInstance = nullptr;
        LibVlcRelease releaseInstance = nullptr;
        LibVlcGetVersion getVersion = nullptr;
        LibVlcErrorMessage errorMessage = nullptr;
        LibVlcMediaNewPath mediaNewPath = nullptr;
        LibVlcMediaNewCallbacks mediaNewCallbacks = nullptr;
        LibVlcMediaRelease mediaRelease = nullptr;
        LibVlcMediaPlayerNew mediaPlayerNew = nullptr;
        LibVlcMediaPlayerRelease mediaPlayerRelease = nullptr;
        LibVlcMediaPlayerSetMedia mediaPlayerSetMedia = nullptr;
        LibVlcMediaPlayerSetHwnd mediaPlayerSetHwnd = nullptr;
        LibVlcVideoSetKeyInput videoSetKeyInput = nullptr;
        LibVlcVideoSetMouseInput videoSetMouseInput = nullptr;
        LibVlcVideoSetCallbacks videoSetCallbacks = nullptr;
        LibVlcVideoSetFormatCallbacks videoSetFormatCallbacks = nullptr;
        LibVlcVideoGetSize videoGetSize = nullptr;
        LibVlcVideoSetCropGeometry videoSetCropGeometry = nullptr;
        LibVlcVideoSetScale videoSetScale = nullptr;
        LibVlcMediaPlayerPlay mediaPlayerPlay = nullptr;
        LibVlcMediaPlayerSetPause mediaPlayerSetPause = nullptr;
        LibVlcMediaPlayerStop mediaPlayerStop = nullptr;
        LibVlcMediaPlayerGetTime mediaPlayerGetTime = nullptr;
        LibVlcMediaPlayerSetTime mediaPlayerSetTime = nullptr;
        LibVlcMediaPlayerGetLength mediaPlayerGetLength = nullptr;
        LibVlcMediaPlayerIsSeekable mediaPlayerIsSeekable = nullptr;
        LibVlcMediaPlayerEventManager mediaPlayerEventManager = nullptr;
        LibVlcAudioSetVolume audioSetVolume = nullptr;
        LibVlcAudioSetMute audioSetMute = nullptr;
        LibVlcEventAttach eventAttach = nullptr;
        LibVlcEventDetach eventDetach = nullptr;
    };

    LibVlcRuntime() noexcept = default;
    ~LibVlcRuntime();

    LibVlcRuntime(const LibVlcRuntime&) = delete;
    LibVlcRuntime& operator=(const LibVlcRuntime&) = delete;
    LibVlcRuntime(LibVlcRuntime&&) = delete;
    LibVlcRuntime& operator=(LibVlcRuntime&&) = delete;

    bool Load(std::wstring& error) noexcept;
    void Unload() noexcept;
    bool IsLoaded() const noexcept;
    const Api& Functions() const noexcept;

private:
    HMODULE coreModule_ = nullptr;
    HMODULE vlcModule_ = nullptr;
    Api api_{};
};

int RunLibVlcSelfTest(std::wstring& error) noexcept;

}  // namespace videoplayer
