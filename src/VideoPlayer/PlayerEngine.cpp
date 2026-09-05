#include "PlayerEngine.h"

#include "LibVlcRuntime.h"
#include "PlaybackMath.h"
#include "PrivacyPolicy.h"
#include "Utf8.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <thread>
#include <utility>

namespace videoplayer {
namespace {

struct CallbackContext final {
    std::atomic<bool> shuttingDown{false};
    std::atomic<unsigned int> callbacksInFlight{0};
    std::atomic<HWND> notificationWindow{nullptr};
    std::atomic<PlaybackState> state{PlaybackState::Stopped};
    std::atomic<std::int64_t> positionMs{0};
    std::atomic<std::int64_t> durationMs{0};
    std::atomic<bool> seekable{false};
    std::atomic<std::uint32_t> generation{0};
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

void __cdecl LibVlcEventCallback(const libvlc_event_t* event, void* opaque) {
    auto* context = static_cast<CallbackContext*>(opaque);
    if (context == nullptr) {
        return;
    }

    context->callbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
    if (context->shuttingDown.load(std::memory_order_acquire) || event == nullptr) {
        context->callbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    PlayerEvent notification{};
    bool recognized = true;
    switch (event->type) {
        case libvlc_MediaPlayerOpening:
            context->state.store(PlaybackState::Opening, std::memory_order_release);
            notification = PlayerEvent::Opening;
            break;
        case libvlc_MediaPlayerPlaying:
            context->state.store(PlaybackState::Playing, std::memory_order_release);
            notification = PlayerEvent::Playing;
            break;
        case libvlc_MediaPlayerPaused:
            context->state.store(PlaybackState::Paused, std::memory_order_release);
            notification = PlayerEvent::Paused;
            break;
        case libvlc_MediaPlayerStopped:
            context->positionMs.store(0, std::memory_order_release);
            context->state.store(PlaybackState::Stopped, std::memory_order_release);
            notification = PlayerEvent::Stopped;
            break;
        case libvlc_MediaPlayerEndReached:
            context->positionMs.store(
                context->durationMs.load(std::memory_order_acquire),
                std::memory_order_release);
            context->state.store(PlaybackState::Ended, std::memory_order_release);
            notification = PlayerEvent::EndReached;
            break;
        case libvlc_MediaPlayerEncounteredError:
            context->state.store(PlaybackState::Error, std::memory_order_release);
            notification = PlayerEvent::EncounteredError;
            break;
        case libvlc_MediaPlayerLengthChanged:
            notification = PlayerEvent::LengthChanged;
            break;
        case libvlc_MediaPlayerSeekableChanged:
            notification = PlayerEvent::SeekableChanged;
            break;
        default:
            recognized = false;
            break;
    }

    if (recognized && !context->shuttingDown.load(std::memory_order_acquire)) {
        const HWND window = context->notificationWindow.load(std::memory_order_acquire);
        if (window != nullptr) {
            ::PostMessageW(
                window,
                kPlayerEventMessage,
                static_cast<WPARAM>(notification),
                static_cast<LPARAM>(context->generation.load(std::memory_order_acquire)));
        }
    }

    context->callbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
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

    void DetachEventsLocked() noexcept {
        if (eventManager == nullptr || !runtime.IsLoaded()) {
            attachedEventCount = 0;
            eventManager = nullptr;
            return;
        }
        const LibVlcRuntime::Api& api = runtime.Functions();
        for (std::size_t index = 0; index < attachedEventCount; ++index) {
            api.eventDetach(
                eventManager,
                kObservedEvents[index],
                &LibVlcEventCallback,
                context.get());
        }
        attachedEventCount = 0;
        eventManager = nullptr;
    }

    void WaitForCallbacksLocked() const noexcept {
        while (context->callbacksInFlight.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
    }

    void ResetVideoCropLocked() noexcept {
        videoCropped = false;
        if (!player || !runtime.IsLoaded()) {
            return;
        }

        const LibVlcRuntime::Api& api = runtime.Functions();
        api.videoSetCropGeometry(player.get(), nullptr);
        api.videoSetScale(player.get(), 0.0F);
    }

    void ShutdownLocked() noexcept {
        context->shuttingDown.store(true, std::memory_order_release);
        context->notificationWindow.store(nullptr, std::memory_order_release);
        DetachEventsLocked();
        WaitForCallbacksLocked();

        if (player && runtime.IsLoaded()) {
            const LibVlcRuntime::Api& api = runtime.Functions();
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
        instance.reset();
        runtime.Unload();

        initialized.store(false, std::memory_order_release);
        videoWindow = nullptr;
        context->positionMs.store(0, std::memory_order_release);
        context->durationMs.store(0, std::memory_order_release);
        context->seekable.store(false, std::memory_order_release);
        context->state.store(PlaybackState::Stopped, std::memory_order_release);
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
    LibVlcRuntime runtime;
    InstancePtr instance;
    PlayerPtr player;
    MediaPtr media;
    std::unique_ptr<FileMediaSource> mediaSource;
    libvlc_event_manager_t* eventManager = nullptr;
    std::size_t attachedEventCount = 0;
    mutable std::mutex mutex;
    std::atomic<bool> initialized{false};
    std::atomic<int> volume{100};
    std::atomic<bool> muted{false};
    bool videoCropped = false;
    HWND videoWindow = nullptr;
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
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (videoWindow == nullptr || notificationWindow == nullptr) {
        error = L"PlayerEngine requires valid video and notification windows.";
        return false;
    }

    if (impl_->initialized.load(std::memory_order_acquire)) {
        impl_->videoWindow = videoWindow;
        impl_->context->notificationWindow.store(notificationWindow, std::memory_order_release);
        impl_->runtime.Functions().mediaPlayerSetHwnd(impl_->player.get(), videoWindow);
        return true;
    }

    impl_->context->shuttingDown.store(false, std::memory_order_release);
    impl_->context->notificationWindow.store(notificationWindow, std::memory_order_release);
    impl_->context->state.store(PlaybackState::Stopped, std::memory_order_release);
    impl_->context->positionMs.store(0, std::memory_order_release);
    impl_->context->durationMs.store(0, std::memory_order_release);
    impl_->context->seekable.store(false, std::memory_order_release);

    if (!impl_->runtime.Load(error)) {
        impl_->context->notificationWindow.store(nullptr, std::memory_order_release);
        return false;
    }

    const LibVlcRuntime::Api& api = impl_->runtime.Functions();
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

    impl_->eventManager = api.mediaPlayerEventManager(impl_->player.get());
    if (impl_->eventManager == nullptr) {
        error = L"libvlc_media_player_event_manager failed.";
        impl_->ShutdownLocked();
        return false;
    }

    for (const libvlc_event_e eventType : Impl::kObservedEvents) {
        if (api.eventAttach(
                impl_->eventManager,
                eventType,
                &LibVlcEventCallback,
                impl_->context.get()) != 0) {
            error = L"libvlc_event_attach failed.";
            impl_->ShutdownLocked();
            return false;
        }
        ++impl_->attachedEventCount;
    }

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

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized.load(std::memory_order_acquire) || !impl_->player) {
        error = L"PlayerEngine is not initialized.";
        return false;
    }

    const LibVlcRuntime::Api& api = impl_->runtime.Functions();
    // Crop geometry is a persistent media-player variable in LibVLC. Clear it
    // before every media replacement, including after a prior failed open.
    impl_->ResetVideoCropLocked();
    if (impl_->media) {
        api.mediaPlayerStop(impl_->player.get());
        api.mediaPlayerSetMedia(impl_->player.get(), nullptr);
        impl_->media.reset();
        impl_->mediaSource.reset();
    }

    impl_->context->state.store(PlaybackState::Stopped, std::memory_order_release);
    impl_->context->positionMs.store(0, std::memory_order_release);
    impl_->context->durationMs.store(0, std::memory_order_release);
    impl_->context->seekable.store(false, std::memory_order_release);
    impl_->context->generation.fetch_add(1, std::memory_order_acq_rel);

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
        impl_->context->state.store(PlaybackState::Error, std::memory_order_release);
        error = VlcFailure(api, L"Could not create LibVLC media.");
        return false;
    }

    impl_->media = MediaPtr(rawMedia, MediaReleaser{api.mediaRelease});
    api.mediaPlayerSetMedia(impl_->player.get(), impl_->media.get());
    impl_->context->state.store(PlaybackState::Opening, std::memory_order_release);
    if (api.mediaPlayerPlay(impl_->player.get()) != 0) {
        impl_->ResetVideoCropLocked();
        api.mediaPlayerStop(impl_->player.get());
        api.mediaPlayerSetMedia(impl_->player.get(), nullptr);
        impl_->media.reset();
        impl_->mediaSource.reset();
        impl_->context->state.store(PlaybackState::Error, std::memory_order_release);
        error = VlcFailure(api, L"libvlc_media_player_play failed.");
        return false;
    }

    api.audioSetVolume(impl_->player.get(), impl_->volume.load(std::memory_order_acquire));
    api.audioSetMute(
        impl_->player.get(),
        impl_->muted.load(std::memory_order_acquire) ? 1 : 0);
    return true;
}

bool PlayerEngine::Play(std::wstring& error) {
    error.clear();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized.load(std::memory_order_acquire) || !impl_->player ||
        !impl_->media) {
        error = L"No media is open.";
        return false;
    }

    const LibVlcRuntime::Api& api = impl_->runtime.Functions();
    if (impl_->context->state.load(std::memory_order_acquire) == PlaybackState::Error) {
        impl_->ResetVideoCropLocked();
    }
    if (impl_->context->state.load(std::memory_order_acquire) == PlaybackState::Ended) {
        api.mediaPlayerStop(impl_->player.get());
        api.mediaPlayerSetTime(impl_->player.get(), 0);
        impl_->context->positionMs.store(0, std::memory_order_release);
        impl_->context->state.store(PlaybackState::Opening, std::memory_order_release);
    }

    if (api.mediaPlayerPlay(impl_->player.get()) != 0) {
        impl_->ResetVideoCropLocked();
        impl_->context->state.store(PlaybackState::Error, std::memory_order_release);
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
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->initialized.load(std::memory_order_acquire) && impl_->player &&
            impl_->media) {
            impl_->runtime.Functions().mediaPlayerSetPause(impl_->player.get(), 1);
        }
    } catch (...) {
    }
}

void PlayerEngine::Stop() noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->initialized.load(std::memory_order_acquire) && impl_->player &&
            impl_->media) {
            const LibVlcRuntime::Api& api = impl_->runtime.Functions();
            api.mediaPlayerStop(impl_->player.get());
            api.mediaPlayerSetTime(impl_->player.get(), 0);
            impl_->context->positionMs.store(0, std::memory_order_release);
            impl_->context->state.store(PlaybackState::Stopped, std::memory_order_release);
        }
    } catch (...) {
    }
}

void PlayerEngine::Seek(const std::int64_t milliseconds) noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->initialized.load(std::memory_order_acquire) || !impl_->player ||
            !impl_->media) {
            return;
        }

        const LibVlcRuntime::Api& api = impl_->runtime.Functions();
        const bool seekable = api.mediaPlayerIsSeekable(impl_->player.get()) != 0;
        impl_->context->seekable.store(seekable, std::memory_order_release);
        if (!seekable) {
            return;
        }

        std::int64_t duration =
            impl_->context->durationMs.load(std::memory_order_acquire);
        const libvlc_time_t reportedLength = api.mediaPlayerGetLength(impl_->player.get());
        if (reportedLength >= 0) {
            duration = static_cast<std::int64_t>(reportedLength);
            impl_->context->durationMs.store(duration, std::memory_order_release);
        }

        const std::int64_t target = ClampTime(milliseconds, duration);
        api.mediaPlayerSetTime(impl_->player.get(), static_cast<libvlc_time_t>(target));
        impl_->context->positionMs.store(target, std::memory_order_release);
    } catch (...) {
    }
}

void PlayerEngine::SetVolume(const int volume) noexcept {
    const int clamped = ClampVolume(volume);
    impl_->volume.store(clamped, std::memory_order_release);
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->initialized.load(std::memory_order_acquire) && impl_->player) {
            impl_->runtime.Functions().audioSetVolume(impl_->player.get(), clamped);
        }
    } catch (...) {
    }
}

void PlayerEngine::SetMuted(const bool muted) noexcept {
    impl_->muted.store(muted, std::memory_order_release);
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->initialized.load(std::memory_order_acquire) && impl_->player) {
            impl_->runtime.Functions().audioSetMute(impl_->player.get(), muted ? 1 : 0);
        }
    } catch (...) {
    }
}

void PlayerEngine::SetVideoWindow(const HWND videoWindow) noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->videoWindow = videoWindow;
        if (impl_->initialized.load(std::memory_order_acquire) && impl_->player) {
            impl_->runtime.Functions().mediaPlayerSetHwnd(impl_->player.get(), videoWindow);
        }
    } catch (...) {
    }
}

std::int64_t PlayerEngine::PositionMs() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->initialized.load(std::memory_order_acquire) || !impl_->player ||
            !impl_->media) {
            return 0;
        }

        const PlaybackState state =
            impl_->context->state.load(std::memory_order_acquire);
        if (state == PlaybackState::Stopped) {
            return 0;
        }
        if (state == PlaybackState::Ended) {
            return impl_->context->durationMs.load(std::memory_order_acquire);
        }

        const libvlc_time_t value =
            impl_->runtime.Functions().mediaPlayerGetTime(impl_->player.get());
        if (value < 0) {
            return impl_->context->positionMs.load(std::memory_order_acquire);
        }
        const std::int64_t result = ClampTime(
            static_cast<std::int64_t>(value),
            impl_->context->durationMs.load(std::memory_order_acquire));
        impl_->context->positionMs.store(result, std::memory_order_release);
        return result;
    } catch (...) {
        return impl_->context->positionMs.load(std::memory_order_acquire);
    }
}

std::int64_t PlayerEngine::DurationMs() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->initialized.load(std::memory_order_acquire) && impl_->player &&
            impl_->media) {
            const libvlc_time_t value =
                impl_->runtime.Functions().mediaPlayerGetLength(impl_->player.get());
            if (value >= 0) {
                impl_->context->durationMs.store(
                    static_cast<std::int64_t>(value),
                    std::memory_order_release);
            }
        }
    } catch (...) {
    }
    return impl_->context->durationMs.load(std::memory_order_acquire);
}

bool PlayerEngine::GetVideoSize(VideoDimensions& dimensions) const noexcept {
    dimensions = {};
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->initialized.load(std::memory_order_acquire) || !impl_->player ||
            !impl_->media) {
            return false;
        }

        unsigned width = 0;
        unsigned height = 0;
        if (impl_->runtime.Functions().videoGetSize(
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

        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->initialized.load(std::memory_order_acquire) || !impl_->player ||
            !impl_->media) {
            return false;
        }

        if (impl_->context->state.load(std::memory_order_acquire) ==
            PlaybackState::Error) {
            impl_->ResetVideoCropLocked();
            return false;
        }

        const LibVlcRuntime::Api& api = impl_->runtime.Functions();
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
        impl_->videoCropped = true;
        return true;
    } catch (...) {
        return false;
    }
}

void PlayerEngine::ResetVideoCrop() noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->ResetVideoCropLocked();
    } catch (...) {
    }
}

bool PlayerEngine::IsVideoCropped() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->videoCropped &&
            impl_->context->state.load(std::memory_order_acquire) ==
                PlaybackState::Error) {
            // Do not invoke LibVLC reentrantly from its event callback. The UI
            // observes the error and reaches this safe, serialized reset path.
            impl_->ResetVideoCropLocked();
        }
        return impl_->videoCropped;
    } catch (...) {
        return false;
    }
}

bool PlayerEngine::IsSeekable() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->initialized.load(std::memory_order_acquire) && impl_->player &&
            impl_->media) {
            const bool value =
                impl_->runtime.Functions().mediaPlayerIsSeekable(impl_->player.get()) != 0;
            impl_->context->seekable.store(value, std::memory_order_release);
        }
    } catch (...) {
    }
    return impl_->context->seekable.load(std::memory_order_acquire);
}

bool PlayerEngine::IsMuted() const noexcept {
    return impl_->muted.load(std::memory_order_acquire);
}

int PlayerEngine::Volume() const noexcept {
    return impl_->volume.load(std::memory_order_acquire);
}

PlaybackState PlayerEngine::State() const noexcept {
    return impl_->context->state.load(std::memory_order_acquire);
}

std::uint32_t PlayerEngine::Generation() const noexcept {
    return impl_->context->generation.load(std::memory_order_acquire);
}

bool PlayerEngine::HasMedia() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->initialized.load(std::memory_order_acquire) &&
            impl_->player && impl_->media;
    } catch (...) {
        return false;
    }
}

bool PlayerEngine::IsInitialized() const noexcept {
    return impl_->initialized.load(std::memory_order_acquire);
}

void PlayerEngine::Shutdown() noexcept {
    if (!impl_) {
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->ShutdownLocked();
    } catch (...) {
    }
}

}  // namespace videoplayer
