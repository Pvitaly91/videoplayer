#include "PlayerEngine.h"

#include "LibVlcRuntime.h"
#include "PlaybackMath.h"
#include "PrivacyPolicy.h"
#include "Utf8.h"
#if defined(VIDEOPLAYER_TESTING)
#include "PlayerEngineTestAccess.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <utility>

namespace videoplayer {
namespace {

struct CallbackContext final {
    std::atomic<bool> shuttingDown{false};
    std::atomic<HWND> notificationWindow{nullptr};
    PlaybackSnapshotStore playback;
    std::mutex callbacksMutex;
    std::condition_variable callbacksChanged;
    unsigned int callbacksInFlight = 0;
};

struct EventBinding final {
    CallbackContext* context = nullptr;
    const std::uint32_t generation;
};

class CallbackLease final {
public:
    explicit CallbackLease(CallbackContext& context) : context_(context) {
        const std::lock_guard<std::mutex> lock(context_.callbacksMutex);
        ++context_.callbacksInFlight;
    }
    ~CallbackLease() {
        const std::lock_guard<std::mutex> lock(context_.callbacksMutex);
        --context_.callbacksInFlight;
        context_.callbacksChanged.notify_all();
    }
private:
    CallbackContext& context_;
};

// LibVLC's HWND operations can synchronously dispatch Win32 messages. A
// getter reached from those messages must not wait on this thread's own
// engine operation or call the backend recursively.
class EngineOperation final {
public:
    EngineOperation(std::mutex& mutex, std::atomic<DWORD>& owner, const bool wait = true)
        : lock_(mutex, std::defer_lock), owner_(owner) {
        const DWORD thread = ::GetCurrentThreadId();
        if (owner_.load(std::memory_order_acquire) == thread) {
            return;
        }
        if (wait) {
            lock_.lock();
        } else if (!lock_.try_lock()) {
            return;
        }
        owner_.store(thread, std::memory_order_release);
    }
    ~EngineOperation() {
        if (lock_.owns_lock()) {
            owner_.store(0, std::memory_order_release);
        }
    }
    explicit operator bool() const noexcept { return lock_.owns_lock(); }
private:
    std::unique_lock<std::mutex> lock_;
    std::atomic<DWORD>& owner_;
};

struct FileMediaSource final {
    explicit FileMediaSource(std::wstring sourcePath)
        : path(std::move(sourcePath)) {
    }

    std::wstring path;
};

struct FileMediaStream final {
    HANDLE file = INVALID_HANDLE_VALUE;
};

int __cdecl OpenFileMedia(void* opaque, void** data, std::uint64_t* size) noexcept {
    const auto* source = static_cast<const FileMediaSource*>(opaque);
    if (source == nullptr || data == nullptr || size == nullptr) {
        return -1;
    }

    const HANDLE file = ::CreateFileW(
        source->path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return -1;
    }

    LARGE_INTEGER fileSize{};
    if (::GetFileSizeEx(file, &fileSize) == FALSE || fileSize.QuadPart < 0) {
        ::CloseHandle(file);
        return -1;
    }

    auto* stream = new (std::nothrow) FileMediaStream{};
    if (stream == nullptr) {
        ::CloseHandle(file);
        return -1;
    }

    stream->file = file;
    *data = stream;
    *size = static_cast<std::uint64_t>(fileSize.QuadPart);
    return 0;
}

std::intptr_t __cdecl ReadFileMedia(
    void* opaque,
    unsigned char* buffer,
    const std::size_t length) noexcept {
    const auto* stream = static_cast<const FileMediaStream*>(opaque);
    if (stream == nullptr || stream->file == INVALID_HANDLE_VALUE ||
        (buffer == nullptr && length != 0)) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }

    const std::size_t maximumRead = (std::min)(
        static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()),
        static_cast<std::size_t>((std::numeric_limits<std::intptr_t>::max)()));
    const DWORD requested = static_cast<DWORD>((std::min)(length, maximumRead));
    DWORD bytesRead = 0;
    if (::ReadFile(stream->file, buffer, requested, &bytesRead, nullptr) == FALSE) {
        return -1;
    }
    return static_cast<std::intptr_t>(bytesRead);
}

int __cdecl SeekFileMedia(void* opaque, const std::uint64_t offset) noexcept {
    const auto* stream = static_cast<const FileMediaStream*>(opaque);
    if (stream == nullptr || stream->file == INVALID_HANDLE_VALUE ||
        offset > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
        return -1;
    }

    LARGE_INTEGER target{};
    target.QuadPart = static_cast<LONGLONG>(offset);
    return ::SetFilePointerEx(stream->file, target, nullptr, FILE_BEGIN) != FALSE
        ? 0
        : -1;
}

void __cdecl CloseFileMedia(void* opaque) noexcept {
    auto* stream = static_cast<FileMediaStream*>(opaque);
    if (stream == nullptr) {
        return;
    }
    if (stream->file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(stream->file);
    }
    delete stream;
}

bool RequiresWin32FileCallbacks(const std::wstring& path) noexcept {
    return path.rfind(L"\\\\?\\", 0) == 0 || path.rfind(L"\\\\.\\", 0) == 0;
}

void __cdecl LibVlcEventCallback(const libvlc_event_t* event, void* opaque) noexcept {
    const auto* binding = static_cast<const EventBinding*>(opaque);
    if (binding == nullptr || binding->context == nullptr) {
        return;
    }
    CallbackContext* const context = binding->context;
    try {
        const CallbackLease lease(*context);
        if (context->shuttingDown.load(std::memory_order_acquire) || event == nullptr) {
            return;
        }

        PlayerEvent notification{};
        bool recognized = true;
        bool stateChanged = true;
        PlaybackState state = PlaybackState::Stopped;
        switch (event->type) {
            case libvlc_MediaPlayerOpening:
                state = PlaybackState::Opening;
                notification = PlayerEvent::Opening;
                break;
            case libvlc_MediaPlayerPlaying:
                state = PlaybackState::Playing;
                notification = PlayerEvent::Playing;
                break;
            case libvlc_MediaPlayerPaused:
                state = PlaybackState::Paused;
                notification = PlayerEvent::Paused;
                break;
            case libvlc_MediaPlayerStopped:
                state = PlaybackState::Stopped;
                notification = PlayerEvent::Stopped;
                break;
            case libvlc_MediaPlayerEndReached:
                state = PlaybackState::Ended;
                notification = PlayerEvent::EndReached;
                break;
            case libvlc_MediaPlayerEncounteredError:
                state = PlaybackState::Error;
                notification = PlayerEvent::EncounteredError;
                break;
            case libvlc_MediaPlayerLengthChanged:
                stateChanged = false;
                notification = PlayerEvent::LengthChanged;
                break;
            case libvlc_MediaPlayerSeekableChanged:
                stateChanged = false;
                notification = PlayerEvent::SeekableChanged;
                break;
            default:
                recognized = false;
                break;
        }

        const bool current = recognized && (stateChanged
            ? context->playback.SetState(binding->generation, state)
            : context->playback.Read().generation == binding->generation);
        if (current && !context->shuttingDown.load(std::memory_order_acquire)) {
            const HWND window = context->notificationWindow.load(std::memory_order_acquire);
            if (window != nullptr) {
                ::PostMessageW(
                    window,
                    kPlayerEventMessage,
                    static_cast<WPARAM>(notification),
                    static_cast<LPARAM>(binding->generation));
            }
        }
    } catch (...) {
        // Never unwind into LibVLC. A later UI poll reads the real backend;
        // an exceptional callback cannot alter another media generation.
        ::OutputDebugStringW(L"VideoPlayer: playback event processing failed.\n");
    }
}

PlaybackState ObservedPlaybackState(const libvlc_state_t state) noexcept {
    switch (state) {
        case libvlc_Opening:
        case libvlc_Buffering:
            return PlaybackState::Opening;
        case libvlc_Playing: return PlaybackState::Playing;
        case libvlc_Paused: return PlaybackState::Paused;
        case libvlc_Ended: return PlaybackState::Ended;
        case libvlc_Error: return PlaybackState::Error;
        default: return PlaybackState::Stopped;
    }
}

std::wstring Utf8Diagnostic(const char* message) {
    if (message == nullptr || *message == '\0') {
        return {};
    }
    const std::size_t sourceLength = std::char_traits<char>::length(message);
    if (sourceLength > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const int length = static_cast<int>(sourceLength);
    const int required = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        message,
        length,
        nullptr,
        0);
    if (required <= 0) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    const int written = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        message,
        length,
        result.data(),
        required);
    return written == required ? result : std::wstring{};
}

std::wstring VlcFailure(
    const LibVlcRuntime::Api& api,
    const std::wstring_view fallback) {
    std::wstring result(fallback);
    const std::wstring detail = Utf8Diagnostic(api.errorMessage());
    if (!detail.empty()) {
        result += L" ";
        result += detail;
    }
    return result;
}

struct InstanceReleaser final {
    LibVlcRelease release = nullptr;
    void operator()(libvlc_instance_t* instance) const noexcept {
        if (instance != nullptr && release != nullptr) {
            release(instance);
        }
    }
};

struct PlayerReleaser final {
    LibVlcMediaPlayerRelease release = nullptr;
    void operator()(libvlc_media_player_t* player) const noexcept {
        if (player != nullptr && release != nullptr) {
            release(player);
        }
    }
};

struct MediaReleaser final {
    LibVlcMediaRelease release = nullptr;
    void operator()(libvlc_media_t* media) const noexcept {
        if (media != nullptr && release != nullptr) {
            release(media);
        }
    }
};

using InstancePtr = std::unique_ptr<libvlc_instance_t, InstanceReleaser>;
using PlayerPtr = std::unique_ptr<libvlc_media_player_t, PlayerReleaser>;
using MediaPtr = std::unique_ptr<libvlc_media_t, MediaReleaser>;

}  // namespace

class PlayerEngine::Impl final {
public:
    Impl()
        : context(std::make_unique<CallbackContext>()),
          instance(nullptr, InstanceReleaser{}),
          player(nullptr, PlayerReleaser{}),
          media(nullptr, MediaReleaser{}) {
    }

    const LibVlcRuntime::Api& Functions() const noexcept {
#if defined(VIDEOPLAYER_TESTING)
        if (testBackend) {
            return testApi;
        }
#endif
        return runtime.Functions();
    }

    bool BackendLoaded() const noexcept {
#if defined(VIDEOPLAYER_TESTING)
        if (testBackend) {
            return true;
        }
#endif
        return runtime.IsLoaded();
    }

    bool AttachEventsLocked(const std::uint32_t generation, std::wstring& error) {
        const LibVlcRuntime::Api& api = Functions();
        eventBinding = std::make_unique<EventBinding>(EventBinding{context.get(), generation});
        eventManager = api.mediaPlayerEventManager(player.get());
        if (eventManager == nullptr) {
            error = L"libvlc_media_player_event_manager failed.";
            return false;
        }
        for (const libvlc_event_e eventType : kObservedEvents) {
            if (api.eventAttach(
                    eventManager, eventType, &LibVlcEventCallback, eventBinding.get()) != 0) {
                error = L"libvlc_event_attach failed.";
                DetachEventsLocked();
                return false;
            }
            ++attachedEventCount;
        }
        return true;
    }

    void DetachEventsLocked() noexcept {
        if (eventManager == nullptr || !BackendLoaded()) {
            attachedEventCount = 0;
            eventManager = nullptr;
            return;
        }
        const LibVlcRuntime::Api& api = Functions();
        for (std::size_t index = 0; index < attachedEventCount; ++index) {
            api.eventDetach(
                eventManager,
                kObservedEvents[index],
                &LibVlcEventCallback,
                eventBinding.get());
        }
        attachedEventCount = 0;
        eventManager = nullptr;
    }

    void WaitForCallbacksLocked() const {
        std::unique_lock<std::mutex> lock(context->callbacksMutex);
        context->callbacksChanged.wait(lock, [this] {
            return context->callbacksInFlight == 0;
        });
    }

    void ResetVideoCropLocked() noexcept {
        {
            const std::lock_guard<std::mutex> lock(cropMutex);
            videoCrop = {};
        }
        if (!player || !BackendLoaded()) {
            return;
        }

        const LibVlcRuntime::Api& api = Functions();
        api.videoSetCropGeometry(player.get(), nullptr);
        api.videoSetScale(player.get(), 0.0F);
    }

    void ShutdownLocked() {
        context->shuttingDown.store(true, std::memory_order_release);
        context->notificationWindow.store(nullptr, std::memory_order_release);
        DetachEventsLocked();
        WaitForCallbacksLocked();

        if (player && BackendLoaded()) {
            const LibVlcRuntime::Api& api = Functions();
            ResetVideoCropLocked();
            api.mediaPlayerStop(player.get());
            api.mediaPlayerSetMedia(player.get(), nullptr);
        }
        media.reset();
        mediaSource.reset();
        player.reset();
        // Releasing the player joins its event-producing worker threads. Wait
        // once more for a callback that may have been selected immediately
        // before event_detach acquired the LibVLC event-manager lock.
        WaitForCallbacksLocked();
        eventBinding.reset();
        instance.reset();
        runtime.Unload();

        initialized.store(false, std::memory_order_release);
        hasMedia.store(false, std::memory_order_release);
        videoWindow = nullptr;
        context->playback.BeginMedia();
    }

    inline static constexpr std::array<libvlc_event_e, 8> kObservedEvents{
        libvlc_MediaPlayerOpening,
        libvlc_MediaPlayerPlaying,
        libvlc_MediaPlayerPaused,
        libvlc_MediaPlayerStopped,
        libvlc_MediaPlayerEndReached,
        libvlc_MediaPlayerEncounteredError,
        libvlc_MediaPlayerLengthChanged,
        libvlc_MediaPlayerSeekableChanged,
    };

    std::unique_ptr<CallbackContext> context;
    std::unique_ptr<EventBinding> eventBinding;
    LibVlcRuntime runtime;
    InstancePtr instance;
    PlayerPtr player;
    MediaPtr media;
    std::unique_ptr<FileMediaSource> mediaSource;
    libvlc_event_manager_t* eventManager = nullptr;
    std::size_t attachedEventCount = 0;
    mutable std::mutex mutex;
    mutable std::atomic<DWORD> mutexOwner{0};
    std::atomic<bool> initialized{false};
    std::atomic<bool> hasMedia{false};
    std::atomic<int> volume{100};
    std::atomic<bool> muted{false};
    VideoCrop videoCrop{};
    mutable std::mutex cropMutex;
    HWND videoWindow = nullptr;
#if defined(VIDEOPLAYER_TESTING)
    bool testBackend = false;
    LibVlcRuntime::Api testApi{};
#endif
};

PlayerEngine::PlayerEngine() : impl_(std::make_unique<Impl>()) {
}

PlayerEngine::~PlayerEngine() {
    Shutdown();
}

bool PlayerEngine::Initialize(
    const HWND videoWindow,
    const HWND notificationWindow,
    std::wstring& error) {
    error.clear();
    const EngineOperation lock(impl_->mutex, impl_->mutexOwner);
    if (!lock) {
        error = L"A playback operation is already in progress.";
        return false;
    }
    if (videoWindow == nullptr || notificationWindow == nullptr) {
        error = L"PlayerEngine requires valid video and notification windows.";
        return false;
    }

    if (impl_->initialized.load(std::memory_order_acquire)) {
        impl_->videoWindow = videoWindow;
        impl_->context->notificationWindow.store(notificationWindow, std::memory_order_release);
        impl_->Functions().mediaPlayerSetHwnd(impl_->player.get(), videoWindow);
        return true;
    }

    impl_->context->shuttingDown.store(false, std::memory_order_release);
    impl_->context->notificationWindow.store(notificationWindow, std::memory_order_release);

    if (!impl_->runtime.Load(error)) {
        impl_->context->notificationWindow.store(nullptr, std::memory_order_release);
        return false;
    }

    const LibVlcRuntime::Api& api = impl_->Functions();
    libvlc_instance_t* rawInstance = api.newInstance(
        static_cast<int>(kPrivateLibVlcArguments.size()),
        kPrivateLibVlcArguments.data());
    if (rawInstance == nullptr) {
        error = VlcFailure(api, L"libvlc_new failed.");
        impl_->ShutdownLocked();
        return false;
    }
    impl_->instance =
        InstancePtr(rawInstance, InstanceReleaser{api.releaseInstance});

    libvlc_media_player_t* rawPlayer = api.mediaPlayerNew(impl_->instance.get());
    if (rawPlayer == nullptr) {
        error = VlcFailure(api, L"libvlc_media_player_new failed.");
        impl_->ShutdownLocked();
        return false;
    }
    impl_->player = PlayerPtr(rawPlayer, PlayerReleaser{api.mediaPlayerRelease});

    impl_->videoWindow = videoWindow;
    api.mediaPlayerSetHwnd(impl_->player.get(), videoWindow);
    api.videoSetKeyInput(impl_->player.get(), 0U);
    api.videoSetMouseInput(impl_->player.get(), 0U);
    api.audioSetVolume(impl_->player.get(), impl_->volume.load(std::memory_order_acquire));
    api.audioSetMute(
        impl_->player.get(),
        impl_->muted.load(std::memory_order_acquire) ? 1 : 0);
    impl_->initialized.store(true, std::memory_order_release);
    return true;
}

bool PlayerEngine::Open(const std::wstring& path, std::wstring& error) {
    error.clear();
    if (path.empty()) {
        error = L"The media path is empty.";
        return false;
    }

    std::string utf8Path;
    if (!WideToUtf8(path, utf8Path)) {
        error = L"The media path is not valid UTF-16.";
        return false;
    }

    const EngineOperation lock(impl_->mutex, impl_->mutexOwner);
    if (!lock) {
        error = L"A playback operation is already in progress.";
        return false;
    }
    if (!impl_->initialized.load(std::memory_order_acquire) || !impl_->player) {
        error = L"PlayerEngine is not initialized.";
        return false;
    }

    const LibVlcRuntime::Api& api = impl_->Functions();
    // Invalidate the old identity BEFORE stopping it. A synchronous Stop
    // callback and already queued UI events retain their original generation.
    const std::uint32_t generation = impl_->context->playback.BeginMedia();
    impl_->hasMedia.store(false, std::memory_order_release);
    impl_->DetachEventsLocked();
    // Crop geometry is a persistent media-player variable in LibVLC. Clear it
    // before every media replacement, including after a prior failed open.
    impl_->ResetVideoCropLocked();
    if (impl_->media) {
        api.mediaPlayerStop(impl_->player.get());
        api.mediaPlayerSetMedia(impl_->player.get(), nullptr);
        impl_->media.reset();
        impl_->mediaSource.reset();
    }

    // Stop joins the old input; event_detach serializes with event dispatch.
    // Keep its immutable binding alive through both barriers.
    impl_->WaitForCallbacksLocked();
    impl_->eventBinding.reset();

    libvlc_media_t* rawMedia = nullptr;
    if (RequiresWin32FileCallbacks(path)) {
        impl_->mediaSource = std::make_unique<FileMediaSource>(path);
        rawMedia = api.mediaNewCallbacks(
            impl_->instance.get(),
            &OpenFileMedia,
            &ReadFileMedia,
            &SeekFileMedia,
            &CloseFileMedia,
            impl_->mediaSource.get());
    } else {
        rawMedia = api.mediaNewPath(impl_->instance.get(), utf8Path.c_str());
    }
    if (rawMedia == nullptr) {
        impl_->ResetVideoCropLocked();
        impl_->mediaSource.reset();
        impl_->context->playback.SetState(generation, PlaybackState::Error);
        error = VlcFailure(api, L"Could not create LibVLC media.");
        return false;
    }

    impl_->media = MediaPtr(rawMedia, MediaReleaser{api.mediaRelease});
    api.mediaPlayerSetMedia(impl_->player.get(), impl_->media.get());
    impl_->context->playback.SetState(generation, PlaybackState::Opening);
    if (!impl_->AttachEventsLocked(generation, error) ||
        api.mediaPlayerPlay(impl_->player.get()) != 0) {
        impl_->ResetVideoCropLocked();
        api.mediaPlayerStop(impl_->player.get());
        api.mediaPlayerSetMedia(impl_->player.get(), nullptr);
        impl_->media.reset();
        impl_->mediaSource.reset();
        impl_->context->playback.SetState(generation, PlaybackState::Error);
        if (error.empty()) {
            error = VlcFailure(api, L"libvlc_media_player_play failed.");
        }
        return false;
    }

    api.audioSetVolume(impl_->player.get(), impl_->volume.load(std::memory_order_acquire));
    api.audioSetMute(
        impl_->player.get(),
        impl_->muted.load(std::memory_order_acquire) ? 1 : 0);
    impl_->hasMedia.store(true, std::memory_order_release);
    return true;
}

bool PlayerEngine::Play(std::wstring& error) {
    error.clear();
    const EngineOperation lock(impl_->mutex, impl_->mutexOwner);
    if (!lock) {
        error = L"A playback operation is already in progress.";
        return false;
    }
    if (!impl_->initialized.load(std::memory_order_acquire) || !impl_->player ||
        !impl_->media) {
        error = L"No media is open.";
        return false;
    }

    const LibVlcRuntime::Api& api = impl_->Functions();
    const PlaybackSnapshot snapshot = impl_->context->playback.Read();
    if (snapshot.state == PlaybackState::Error) {
        impl_->ResetVideoCropLocked();
    }
    if (snapshot.state == PlaybackState::Ended) {
        api.mediaPlayerStop(impl_->player.get());
        api.mediaPlayerSetTime(impl_->player.get(), 0);
        impl_->context->playback.SetState(snapshot.generation, PlaybackState::Opening);
        impl_->context->playback.Seek(snapshot.generation, 0);
    }

    if (api.mediaPlayerPlay(impl_->player.get()) != 0) {
        impl_->ResetVideoCropLocked();
        impl_->context->playback.SetState(snapshot.generation, PlaybackState::Error);
        error = VlcFailure(api, L"libvlc_media_player_play failed.");
        return false;
    }
    api.audioSetVolume(impl_->player.get(), impl_->volume.load(std::memory_order_acquire));
    api.audioSetMute(
        impl_->player.get(),
        impl_->muted.load(std::memory_order_acquire) ? 1 : 0);
    return true;
}

void PlayerEngine::Pause() noexcept {
    try {
        const EngineOperation lock(impl_->mutex, impl_->mutexOwner);
        if (lock && impl_->initialized.load(std::memory_order_acquire) && impl_->player &&
            impl_->media) {
            impl_->Functions().mediaPlayerSetPause(impl_->player.get(), 1);
        }
    } catch (...) {
    }
}

void PlayerEngine::Stop() noexcept {
    try {
        const EngineOperation lock(impl_->mutex, impl_->mutexOwner);
        if (lock && impl_->initialized.load(std::memory_order_acquire) && impl_->player &&
            impl_->media) {
            const LibVlcRuntime::Api& api = impl_->Functions();
            api.mediaPlayerStop(impl_->player.get());
            api.mediaPlayerSetTime(impl_->player.get(), 0);
            impl_->context->playback.SetState(
                impl_->context->playback.Read().generation, PlaybackState::Stopped);
        }
    } catch (...) {
    }
}

void PlayerEngine::Seek(const std::int64_t milliseconds) noexcept {
    try {
        const EngineOperation lock(impl_->mutex, impl_->mutexOwner);
        if (!lock || !impl_->initialized.load(std::memory_order_acquire) || !impl_->player ||
            !impl_->media) {
            return;
        }

        const LibVlcRuntime::Api& api = impl_->Functions();
        const bool seekable = api.mediaPlayerIsSeekable(impl_->player.get()) != 0;
        if (!seekable) {
            return;
        }

        const PlaybackSnapshot snapshot = impl_->context->playback.Read();
        std::int64_t duration = snapshot.durationMs;
        const libvlc_time_t reportedLength = api.mediaPlayerGetLength(impl_->player.get());
        if (reportedLength >= 0) {
            duration = static_cast<std::int64_t>(reportedLength);
        }

        const std::int64_t target = ClampTime(milliseconds, duration);
        impl_->context->playback.Observe(
            snapshot.generation, snapshot.state, snapshot.positionMs, duration, seekable);
        api.mediaPlayerSetTime(impl_->player.get(), static_cast<libvlc_time_t>(target));
        // Immediate feedback is the requested main-player seek, not invented
        // elapsed playback. The next timer snapshot uses the real backend.
        impl_->context->playback.Seek(snapshot.generation, target);
    } catch (...) {
    }
}

void PlayerEngine::SetVolume(const int volume) noexcept {
    const int clamped = ClampVolume(volume);
    impl_->volume.store(clamped, std::memory_order_release);
    try {
        const EngineOperation lock(impl_->mutex, impl_->mutexOwner);
        if (lock && impl_->initialized.load(std::memory_order_acquire) && impl_->player) {
            impl_->Functions().audioSetVolume(impl_->player.get(), clamped);
        }
    } catch (...) {
    }
}

void PlayerEngine::SetMuted(const bool muted) noexcept {
    impl_->muted.store(muted, std::memory_order_release);
    try {
        const EngineOperation lock(impl_->mutex, impl_->mutexOwner);
        if (lock && impl_->initialized.load(std::memory_order_acquire) && impl_->player) {
            impl_->Functions().audioSetMute(impl_->player.get(), muted ? 1 : 0);
        }
    } catch (...) {
    }
}

void PlayerEngine::SetVideoWindow(const HWND videoWindow) noexcept {
    try {
        const EngineOperation lock(impl_->mutex, impl_->mutexOwner);
        if (!lock) {
            return;
        }
        impl_->videoWindow = videoWindow;
        if (impl_->initialized.load(std::memory_order_acquire) && impl_->player) {
            impl_->Functions().mediaPlayerSetHwnd(impl_->player.get(), videoWindow);
        }
    } catch (...) {
    }
}

PlaybackSnapshot PlayerEngine::Snapshot() const noexcept {
    try {
        const EngineOperation lock(impl_->mutex, impl_->mutexOwner, false);
        const PlaybackSnapshot cached = impl_->context->playback.Read();
        if (!lock || !impl_->initialized.load(std::memory_order_acquire) || !impl_->player ||
            !impl_->media) {
            return cached;
        }

        const LibVlcRuntime::Api& api = impl_->Functions();
        const libvlc_time_t position = api.mediaPlayerGetTime(impl_->player.get());
        const libvlc_time_t duration = api.mediaPlayerGetLength(impl_->player.get());
        const bool seekable = api.mediaPlayerIsSeekable(impl_->player.get()) != 0;
        const PlaybackState state = ObservedPlaybackState(
            api.mediaPlayerGetState(impl_->player.get()));
        if (state == PlaybackState::Error && impl_->videoCrop.IsValid()) {
            impl_->ResetVideoCropLocked();
        }
        return impl_->context->playback.Observe(
            cached.generation, state, position, duration, seekable);
    } catch (...) {
        return {};
    }
}

PlaybackSnapshot PlayerEngine::CachedSnapshot() const noexcept {
    try {
        return impl_->context->playback.Read();
    } catch (...) {
        return {};
    }
}

std::int64_t PlayerEngine::PositionMs() const noexcept {
    return Snapshot().positionMs;
}

std::int64_t PlayerEngine::DurationMs() const noexcept {
    return Snapshot().durationMs;
}

bool PlayerEngine::GetVideoSize(VideoDimensions& dimensions) const noexcept {
    dimensions = {};
    try {
        const EngineOperation lock(impl_->mutex, impl_->mutexOwner, false);
        if (!lock || !impl_->initialized.load(std::memory_order_acquire) || !impl_->player ||
            !impl_->media) {
            return false;
        }

        unsigned width = 0;
        unsigned height = 0;
        if (impl_->Functions().videoGetSize(
                impl_->player.get(),
                0U,
                &width,
                &height) != 0 ||
            width == 0U || height == 0U) {
            return false;
        }

        dimensions = {width, height};
        return true;
    } catch (...) {
        dimensions = {};
        return false;
    }
}

bool PlayerEngine::ApplyVideoCrop(const VideoCrop& crop) noexcept {
    try {
        if (!crop.IsValid()) {
            return false;
        }

        const EngineOperation lock(impl_->mutex, impl_->mutexOwner);
        if (!lock || !impl_->initialized.load(std::memory_order_acquire) || !impl_->player ||
            !impl_->media) {
            return false;
        }

        if (impl_->context->playback.Read().state ==
            PlaybackState::Error) {
            impl_->ResetVideoCropLocked();
            return false;
        }

        const LibVlcRuntime::Api& api = impl_->Functions();
        unsigned decodedWidth = 0;
        unsigned decodedHeight = 0;
        if (api.videoGetSize(
                impl_->player.get(),
                0U,
                &decodedWidth,
                &decodedHeight) != 0 ||
            decodedWidth == 0U || decodedHeight == 0U) {
            return false;
        }

        const std::uint64_t right =
            static_cast<std::uint64_t>(crop.x) + crop.width;
        const std::uint64_t bottom =
            static_cast<std::uint64_t>(crop.y) + crop.height;
        if (right > decodedWidth || bottom > decodedHeight) {
            return false;
        }

        const std::string geometry = FormatLibVlc3CropGeometry(crop);
        if (geometry.empty()) {
            return false;
        }

        // LibVLC copies this string into the media-player crop variable.
        api.videoSetCropGeometry(impl_->player.get(), geometry.c_str());
        api.videoSetScale(impl_->player.get(), 0.0F);
        {
            const std::lock_guard<std::mutex> cropLock(impl_->cropMutex);
            impl_->videoCrop = crop;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void PlayerEngine::ResetVideoCrop() noexcept {
    try {
        const EngineOperation lock(impl_->mutex, impl_->mutexOwner);
        if (lock) {
            impl_->ResetVideoCropLocked();
        }
    } catch (...) {
    }
}

bool PlayerEngine::IsVideoCropped() const noexcept {
    VideoCrop crop{};
    return GetVideoCrop(crop);
}

bool PlayerEngine::GetVideoCrop(VideoCrop& crop) const noexcept {
    crop = {};
    try {
        const std::lock_guard<std::mutex> lock(impl_->cropMutex);
        crop = impl_->videoCrop;
        return crop.IsValid();
    } catch (...) {
        return false;
    }
}

bool PlayerEngine::IsSeekable() const noexcept {
    return Snapshot().seekable;
}

bool PlayerEngine::IsMuted() const noexcept {
    return impl_->muted.load(std::memory_order_acquire);
}

int PlayerEngine::Volume() const noexcept {
    return impl_->volume.load(std::memory_order_acquire);
}

PlaybackState PlayerEngine::State() const noexcept {
    return CachedSnapshot().state;
}

std::uint32_t PlayerEngine::Generation() const noexcept {
    return CachedSnapshot().generation;
}

bool PlayerEngine::HasMedia() const noexcept {
    return impl_->hasMedia.load(std::memory_order_acquire);
}

bool PlayerEngine::IsInitialized() const noexcept {
    return impl_->initialized.load(std::memory_order_acquire);
}

void PlayerEngine::Shutdown() noexcept {
    if (!impl_) {
        return;
    }
    try {
        const EngineOperation lock(impl_->mutex, impl_->mutexOwner);
        if (lock) {
            impl_->ShutdownLocked();
        }
    } catch (...) {
    }
}

#if defined(VIDEOPLAYER_TESTING)
void PlayerEngineTestAccess::Install(
    PlayerEngine& engine,
    const LibVlcRuntime::Api& api,
    libvlc_media_player_t* const player,
    libvlc_media_t* const media,
    const PlaybackSnapshot& snapshot) {
    const EngineOperation lock(engine.impl_->mutex, engine.impl_->mutexOwner);
    if (!lock) {
        return;
    }
    engine.impl_->ShutdownLocked();
    engine.impl_->testApi = api;
    engine.impl_->testBackend = true;
    engine.impl_->player = PlayerPtr(player, PlayerReleaser{});
    engine.impl_->media = MediaPtr(media, MediaReleaser{});
    engine.impl_->context->shuttingDown.store(false, std::memory_order_release);
    const std::uint32_t generation = engine.impl_->context->playback.Read().generation;
    engine.impl_->context->playback.Observe(
        generation, snapshot.state, snapshot.positionMs, snapshot.durationMs, snapshot.seekable);
    engine.impl_->hasMedia.store(media != nullptr, std::memory_order_release);
    engine.impl_->initialized.store(true, std::memory_order_release);
}

void PlayerEngineTestAccess::DeliverEvent(
    PlayerEngine& engine,
    const libvlc_event_e type,
    const std::uint32_t attachedGeneration) {
    EventBinding binding{engine.impl_->context.get(), attachedGeneration};
    const libvlc_event_t event{static_cast<int>(type)};
    LibVlcEventCallback(&event, &binding);
}
#endif

}  // namespace videoplayer
