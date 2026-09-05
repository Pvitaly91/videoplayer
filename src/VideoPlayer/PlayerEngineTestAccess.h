#pragma once

#if !defined(VIDEOPLAYER_TESTING)
#error PlayerEngineTestAccess is available only to regression builds.
#endif

#include "LibVlcRuntime.h"
#include "PlayerEngine.h"

namespace videoplayer {

// Inject only the C API and opaque resources. Tests exercise the production
// seek/crop/snapshot paths; the engine does not own the fake resource pointers.
struct PlayerEngineTestAccess final {
    static void Install(
        PlayerEngine& engine,
        const LibVlcRuntime::Api& api,
        libvlc_media_player_t* player,
        libvlc_media_t* media,
        const PlaybackSnapshot& snapshot);
    static void DeliverEvent(
        PlayerEngine& engine,
        libvlc_event_e type,
        std::uint32_t attachedGeneration);
};

}  // namespace videoplayer
