#pragma once

#include "../../src/VideoPlayer/PlaybackSnapshot.h"
#include "FakePlayerBackend.h"

template <typename Runner>
void RunPlaybackSnapshotTests(Runner& tests) {
    using namespace videoplayer;
    PlaybackSnapshotStore store;
    const std::uint32_t first = store.BeginMedia();
    PlaybackSnapshot snapshot = store.Observe(first, PlaybackState::Playing, 1250, 10000, true);
    tests.True(snapshot.generation == first && snapshot.state == PlaybackState::Playing &&
        snapshot.positionMs == 1250 && snapshot.durationMs == 10000 && snapshot.seekable,
        L"Playback snapshot publishes one coherent main-media observation");

    store.SetState(first, PlaybackState::Paused);
    tests.Equal(store.Read().positionMs, std::int64_t{1250}, L"Pause keeps the observed position");
    snapshot = store.Observe(first, PlaybackState::Paused, -1, -1, true);
    tests.True(snapshot.positionMs == 1250 && snapshot.durationMs == 10000,
        L"Temporarily unavailable backend values preserve the last observation");
    store.SetState(first, PlaybackState::Playing);
    snapshot = store.Observe(first, PlaybackState::Playing, 1750, 10000, true);
    tests.Equal(snapshot.positionMs, std::int64_t{1750}, L"Play advances only from backend observation");
    store.Seek(first, 6000);
    tests.Equal(store.Read().positionMs, std::int64_t{6000}, L"Accepted seek is visible immediately");
    store.Seek(first, 20000);
    tests.Equal(store.Read().positionMs, std::int64_t{10000}, L"Seek is clamped to current duration");
    store.SetState(first, PlaybackState::Stopped);
    tests.Equal(store.Read().positionMs, std::int64_t{0}, L"Stop resets actual progress to zero");
    store.SetState(first, PlaybackState::Ended);
    tests.Equal(store.Read().positionMs, std::int64_t{10000}, L"EndReached reports the known duration");

    const std::uint32_t second = store.BeginMedia();
    snapshot = store.Read();
    tests.True(snapshot.generation != first && snapshot.state == PlaybackState::Stopped &&
        snapshot.positionMs == 0 && snapshot.durationMs == 0 && !snapshot.seekable,
        L"A to B resets all snapshot fields at one generation boundary");
    store.Observe(second, PlaybackState::Playing, 400, 2500, true);
    tests.True(!store.SetState(first, PlaybackState::Stopped),
        L"Detached A Stopped callback is rejected while B plays");
    tests.True(!store.SetState(first, PlaybackState::Ended),
        L"Detached A EndReached callback cannot freeze B progress");
    tests.True(!store.SetState(first, PlaybackState::Error),
        L"Detached A error callback cannot fail current B");
    snapshot = store.Observe(first, PlaybackState::Ended, 10000, 10000, false);
    tests.True(snapshot.generation == second && snapshot.state == PlaybackState::Playing &&
        snapshot.positionMs == 400 && snapshot.durationMs == 2500 && snapshot.seekable,
        L"Stale A poll cannot replace B position duration or seekability");
    store.Seek(first, 2000);
    tests.Equal(store.Read().positionMs, std::int64_t{400}, L"Stale seek cannot change current media");

    const std::uint32_t third = store.BeginMedia();
    store.Observe(third, PlaybackState::Playing, 300, 10000, true);
    tests.True(!store.SetState(first, PlaybackState::Stopped) &&
        !store.SetState(second, PlaybackState::Ended),
        L"A to B to A keeps distinct callback identities even for the same file");
    snapshot = store.Read();
    tests.True(snapshot.generation == third && snapshot.positionMs == 300,
        L"Reopened A retains its own current progress after stale events");

    PlaybackSnapshotStore independent;
    const std::uint32_t other = independent.BeginMedia();
    independent.Observe(other, PlaybackState::Paused, 750, 3000, false);
    independent.Seek(other, 1000);
    tests.Equal(independent.Read().positionMs, std::int64_t{750},
        L"Unseekable media does not accept a seek target");
    tests.True(store.Read().positionMs == 300 && independent.Read().positionMs == 750,
        L"Independent playback stores do not share progress state");

    videoplayer::tests::FakePlayerBackend backendA;
    videoplayer::tests::FakePlayerBackend backendB;
    PlayerEngine engine;
    backendA.Install(engine);
    const std::uint32_t attachedA = engine.Generation();
    backendB.positionMs = 750;
    backendB.durationMs = 3000;
    backendB.Install(engine);
    const std::uint32_t attachedB = engine.Generation();
    PlayerEngineTestAccess::DeliverEvent(engine, libvlc_MediaPlayerStopped, attachedA);
    snapshot = engine.CachedSnapshot();
    tests.True(snapshot.generation == attachedB && snapshot.state == PlaybackState::Playing &&
        snapshot.positionMs == 750 && snapshot.durationMs == 3000,
        L"Production callback rejects detached player A Stopped event");
    PlayerEngineTestAccess::DeliverEvent(engine, libvlc_MediaPlayerEndReached, attachedA);
    tests.Equal(engine.CachedSnapshot().positionMs, std::int64_t{750},
        L"Production callback rejects stale EndReached before the next timer poll");
    PlayerEngineTestAccess::DeliverEvent(engine, libvlc_MediaPlayerPaused, attachedB);
    tests.Equal(engine.CachedSnapshot().state, PlaybackState::Paused,
        L"Production callback accepts current media event");
    backendA.positionMs = 500;
    backendA.backendState = 3;
    backendA.Install(engine);
    PlayerEngineTestAccess::DeliverEvent(engine, libvlc_MediaPlayerStopped, attachedA);
    PlayerEngineTestAccess::DeliverEvent(engine, libvlc_MediaPlayerEncounteredError, attachedB);
    snapshot = engine.Snapshot();
    tests.True(snapshot.generation != attachedA && snapshot.generation != attachedB &&
        snapshot.state == PlaybackState::Playing && snapshot.positionMs == 500,
        L"Production A to B to A callback lifecycle retains current backend progress");
}
