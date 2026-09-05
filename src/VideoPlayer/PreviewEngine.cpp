#include "PreviewEngine.h"

#include "LibVlcRuntime.h"
#include "PreviewCache.h"
#include "PreviewMath.h"
#include "PrivacyPolicy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <malloc.h>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace videoplayer {
namespace {

constexpr std::size_t kPreviewBufferCount = 3;
constexpr std::size_t kPreviewBufferAlignment = 32;
constexpr auto kDecodeThrottle = std::chrono::milliseconds(120);
constexpr auto kFrameWaitTimeout = std::chrono::milliseconds(3000);
constexpr auto kCancellationPollInterval = std::chrono::milliseconds(40);
constexpr auto kRetryBackoff = std::chrono::milliseconds(80);
constexpr unsigned int kMaximumDecodeAttempts = 2;

std::int64_t PreviewDecodeTarget(const PreviewDecodeRequest& request) noexcept {
    // At duration itself there is no following frame. Decode the final small
    // interval so a request at the right edge can capture the final image.
    const auto finalFrameTime = (std::max)(std::int64_t{0}, request.durationMs - 200);
    return (std::max)(std::int64_t{0}, (std::min)(request.timestampMs, finalFrameTime));
}

bool PassPreviewPrerollBarrier(
    bool& discardedFirstFrame,
    const std::uint64_t displayedSequence,
    std::uint64_t& afterSequence) noexcept {
    if (discardedFirstFrame) {
        return true;
    }
    discardedFirstFrame = true;
    afterSequence = displayedSequence;
    return false;
}

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

std::size_t AlignUp32(const std::size_t value) noexcept {
    constexpr std::size_t mask = kPreviewBufferAlignment - 1;
    if (value > (std::numeric_limits<std::size_t>::max)() - mask) {
        return 0;
    }
    return (value + mask) & ~mask;
}

class AlignedBuffer final {
public:
    AlignedBuffer() noexcept = default;
    ~AlignedBuffer() {
        Reset();
    }

    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    bool Resize(const std::size_t size) noexcept {
        Reset();
        if (size == 0) {
            return false;
        }
        data_ = static_cast<std::uint8_t*>(
            ::_aligned_malloc(size, kPreviewBufferAlignment));
        if (data_ == nullptr) {
            return false;
        }
        size_ = size;
        std::memset(data_, 0, size_);
        return true;
    }

    void Reset() noexcept {
        if (data_ != nullptr) {
            ::_aligned_free(data_);
            data_ = nullptr;
        }
        size_ = 0;
    }

    std::uint8_t* Data() noexcept {
        return data_;
    }

    const std::uint8_t* Data() const noexcept {
        return data_;
    }

    std::size_t Size() const noexcept {
        return size_;
    }

private:
    std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

struct CapturedFrame final {
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int pitch = 0;
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> pixels;
};

enum class FrameWaitResult {
    Ready,
    Cancelled,
    TimedOut,
};

class PreviewVideoContext final {
public:
    unsigned Configure(
        char* chroma,
        unsigned int* width,
        unsigned int* height,
        unsigned int* pitches,
        unsigned int* lines) noexcept {
        if (chroma == nullptr || width == nullptr || height == nullptr ||
            pitches == nullptr || lines == nullptr) {
            return 0;
        }
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            ClearBuffersLocked();
            const SIZE fitted = FitPreviewSize(*width, *height);
            if (fitted.cx <= 0 || fitted.cy <= 0) {
                return 0;
            }

            width_ = static_cast<unsigned int>(fitted.cx);
            height_ = static_cast<unsigned int>(fitted.cy);
            const std::size_t tightPitch = static_cast<std::size_t>(width_) * 4U;
            const std::size_t alignedPitch = AlignUp32(tightPitch);
            const std::size_t alignedLines = AlignUp32(height_);
            if (alignedPitch == 0 || alignedLines == 0 ||
                alignedPitch > (std::numeric_limits<unsigned int>::max)() ||
                alignedLines > (std::numeric_limits<unsigned int>::max)() ||
                alignedLines > (std::numeric_limits<std::size_t>::max)() / alignedPitch) {
                ClearBuffersLocked();
                return 0;
            }

            pitch_ = static_cast<unsigned int>(alignedPitch);
            lines_ = static_cast<unsigned int>(alignedLines);
            const std::size_t bufferBytes = alignedPitch * alignedLines;
            for (AlignedBuffer& buffer : buffers_) {
                if (!buffer.Resize(bufferBytes)) {
                    ClearBuffersLocked();
                    return 0;
                }
            }
            compactPixels_.assign(
                static_cast<std::size_t>(width_) * height_ * 4U,
                std::uint8_t{0});
            nextBuffer_ = 0;
            std::memcpy(chroma, "RV32", 4);
            *width = width_;
            *height = height_;
            pitches[0] = pitch_;
            lines[0] = lines_;
            return static_cast<unsigned int>(kPreviewBufferCount);
        } catch (...) {
            try {
                std::lock_guard<std::mutex> lock(mutex_);
                ClearBuffersLocked();
            } catch (...) {
            }
            return 0;
        }
    }

    void Cleanup() noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            ClearBuffersLocked();
        } catch (...) {
        }
        condition_.notify_all();
    }

    void* Lock(void** planes) noexcept {
        if (planes == nullptr) {
            return nullptr;
        }
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (width_ == 0 || height_ == 0 || buffers_[nextBuffer_].Data() == nullptr) {
                planes[0] = nullptr;
                return nullptr;
            }
            const std::size_t index = nextBuffer_;
            nextBuffer_ = (nextBuffer_ + 1) % kPreviewBufferCount;
            bufferTokens_[index].mediaGeneration = captureMediaGeneration_;
            bufferTokens_[index].requestId = captureRequestId_;
            bufferTokens_[index].attempt = captureAttempt_;
            planes[0] = buffers_[index].Data();
            return reinterpret_cast<void*>(static_cast<std::uintptr_t>(index + 1));
        } catch (...) {
            planes[0] = nullptr;
            return nullptr;
        }
    }

    void Unlock(void* picture) noexcept {
        const std::uintptr_t token = reinterpret_cast<std::uintptr_t>(picture);
        if (token == 0 || token > kPreviewBufferCount) {
            return;
        }
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            const AlignedBuffer& source = buffers_[token - 1];
            if (width_ == 0 || height_ == 0 || source.Data() == nullptr ||
                compactPixels_.empty()) {
                return;
            }
            const std::size_t tightPitch = static_cast<std::size_t>(width_) * 4U;
            for (unsigned int row = 0; row < height_; ++row) {
                std::memcpy(
                    compactPixels_.data() + (static_cast<std::size_t>(row) * tightPitch),
                    source.Data() + (static_cast<std::size_t>(row) * pitch_),
                    tightPitch);
            }
            capturedMediaGeneration_ = bufferTokens_[token - 1].mediaGeneration;
            capturedRequestId_ = bufferTokens_[token - 1].requestId;
            capturedAttempt_ = bufferTokens_[token - 1].attempt;
            ++frameSequence_;
        } catch (...) {
            return;
        }
        condition_.notify_all();
    }

    std::uint64_t Sequence() const noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            return frameSequence_;
        } catch (...) {
            return 0;
        }
    }

    void SetCaptureToken(
        const std::uint32_t mediaGeneration,
        const std::uint64_t requestId,
        const unsigned int attempt) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            captureMediaGeneration_ = mediaGeneration;
            captureRequestId_ = requestId;
            captureAttempt_ = attempt;
        } catch (...) {
        }
    }

    FrameWaitResult WaitForFrame(
        const std::uint64_t afterSequence,
        const std::chrono::steady_clock::time_point deadline,
        const std::uint64_t requestId,
        const std::uint32_t mediaGeneration,
        const unsigned int attempt,
        const PreviewCancellation& cancellation,
        const PreviewDecodeRequest& request,
        CapturedFrame& destination) noexcept {
        try {
            std::unique_lock<std::mutex> lock(mutex_);
            std::uint64_t observedSequence = afterSequence;
            for (;;) {
                while (frameSequence_ <= observedSequence) {
                    if (cancellation.IsCancelled(request)) {
                        return FrameWaitResult::Cancelled;
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline) {
                        return FrameWaitResult::TimedOut;
                    }
                    condition_.wait_until(
                        lock,
                        (std::min)(deadline, now + kCancellationPollInterval));
                }
                if (cancellation.IsCancelled(request)) {
                    return FrameWaitResult::Cancelled;
                }
                if (capturedMediaGeneration_ == mediaGeneration &&
                    capturedRequestId_ == requestId &&
                    capturedAttempt_ == attempt) {
                    break;
                }
                // A frame whose lock callback ran before this request was
                // activated must never be relabelled as the new request.
                observedSequence = frameSequence_;
            }
            if (width_ == 0 || height_ == 0 || compactPixels_.empty()) {
                return FrameWaitResult::TimedOut;
            }
            destination.width = width_;
            destination.height = height_;
            destination.pitch = width_ * 4U;
            destination.sequence = frameSequence_;
            destination.pixels = compactPixels_;
            return FrameWaitResult::Ready;
        } catch (...) {
            return FrameWaitResult::TimedOut;
        }
    }

    void Wake() noexcept {
        condition_.notify_all();
    }

private:
    struct BufferToken final {
        std::uint32_t mediaGeneration = 0;
        std::uint64_t requestId = 0;
        unsigned int attempt = 0;
    };

    void ClearBuffersLocked() noexcept {
        for (AlignedBuffer& buffer : buffers_) {
            buffer.Reset();
        }
        compactPixels_.clear();
        width_ = 0;
        height_ = 0;
        pitch_ = 0;
        lines_ = 0;
        nextBuffer_ = 0;
        capturedMediaGeneration_ = 0;
        capturedRequestId_ = 0;
        capturedAttempt_ = 0;
        for (BufferToken& token : bufferTokens_) {
            token = {};
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::array<AlignedBuffer, kPreviewBufferCount> buffers_;
    std::array<BufferToken, kPreviewBufferCount> bufferTokens_{};
    std::vector<std::uint8_t> compactPixels_;
    unsigned int width_ = 0;
    unsigned int height_ = 0;
    unsigned int pitch_ = 0;
    unsigned int lines_ = 0;
    std::size_t nextBuffer_ = 0;
    std::uint64_t frameSequence_ = 0;
    std::uint32_t captureMediaGeneration_ = 0;
    std::uint64_t captureRequestId_ = 0;
    unsigned int captureAttempt_ = 0;
    std::uint32_t capturedMediaGeneration_ = 0;
    std::uint64_t capturedRequestId_ = 0;
    unsigned int capturedAttempt_ = 0;
};

unsigned __cdecl ConfigureVideo(
    void** opaque,
    char* chroma,
    unsigned int* width,
    unsigned int* height,
    unsigned int* pitches,
    unsigned int* lines) noexcept {
    auto* context = opaque != nullptr
        ? static_cast<PreviewVideoContext*>(*opaque)
        : nullptr;
    return context != nullptr
        ? context->Configure(chroma, width, height, pitches, lines)
        : 0;
}

void __cdecl CleanupVideo(void* opaque) noexcept {
    auto* context = static_cast<PreviewVideoContext*>(opaque);
    if (context != nullptr) {
        context->Cleanup();
    }
}

void* __cdecl LockVideo(void* opaque, void** planes) noexcept {
    auto* context = static_cast<PreviewVideoContext*>(opaque);
    return context != nullptr ? context->Lock(planes) : nullptr;
}

void __cdecl DisplayVideo(
    void* opaque,
    void* picture) noexcept {
    auto* context = static_cast<PreviewVideoContext*>(opaque);
    if (context != nullptr) {
        context->Unlock(picture);
    }
}

// The UI can wake the active frame wait without owning or racing the worker's
// callback context. Each player publishes a distinct shared context here;
// the relay itself retains only a weak reference.
class PreviewContextWakeRelay final {
public:
    void Set(const std::shared_ptr<PreviewVideoContext>& context) noexcept {
        try {
            const std::lock_guard<std::mutex> lock(mutex_);
            context_ = context;
        } catch (...) {
        }
    }

    void Clear(const std::shared_ptr<PreviewVideoContext>& context) noexcept {
        try {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (context_.lock() == context) {
                context_.reset();
            }
        } catch (...) {
        }
    }

    void Wake() noexcept {
        std::shared_ptr<PreviewVideoContext> context;
        try {
            const std::lock_guard<std::mutex> lock(mutex_);
            context = context_.lock();
        } catch (...) {
            return;
        }
        if (context) {
            context->Wake();
        }
    }

#if defined(VIDEOPLAYER_TESTING)
    bool TargetsForTesting(
        const std::shared_ptr<PreviewVideoContext>& context) noexcept {
        try {
            const std::lock_guard<std::mutex> lock(mutex_);
            return context_.lock() == context;
        } catch (...) {
            return false;
        }
    }
#endif

private:
    std::mutex mutex_;
    std::weak_ptr<PreviewVideoContext> context_;
};

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

using DecodeRequest = PreviewDecodeRequest;
using DecodeStatus = PreviewDecodeStatus;

class WorkerResources final : public PreviewBackend {
public:
    explicit WorkerResources(PreviewContextWakeRelay& wakeRelay)
        : instance_(nullptr, InstanceReleaser{}),
          player_(nullptr, PlayerReleaser{}),
          media_(nullptr, MediaReleaser{}),
          wakeRelay_(wakeRelay) {
    }

    ~WorkerResources() {
        Shutdown();
    }

    WorkerResources(const WorkerResources&) = delete;
    WorkerResources& operator=(const WorkerResources&) = delete;

    DecodeStatus Decode(
        const DecodeRequest& request,
        const PreviewCancellation& cancellation,
        PreviewFramePtr& result) override {
        result.reset();
        try {
            if (cancellation.IsCancelled(request)) {
                return DecodeStatus::Cancelled;
            }
            // Stop and release the old player, not only its input: LibVLC may
            // retain a vout on a stopped player. Its delayed cleanup must not
            // clear a new input's callback buffers.
            ReleaseMedia();
            if (!EnsurePlayer()) {
                return DecodeStatus::RuntimeFailure;
            }
            if (!EnsureMedia(request)) {
                return DecodeStatus::RetryableFailure;
            }

            PreviewVideoContext& videoContext = *videoContext_;
            // Invalidate on every return path, including cancellation,
            // timeout and exceptions. A callback from a released player owns
            // a different context and can never acquire this request's token.
            struct CaptureReset final {
                PreviewVideoContext& context;
                ~CaptureReset() { context.SetCaptureToken(0, 0, 0); }
            } captureReset{videoContext};
            videoContext.SetCaptureToken(0, 0, 0);

            const LibVlcRuntime::Api& api = runtime_.Functions();
            api.audioSetVolume(player_.get(), 0);
            api.audioSetMute(player_.get(), 1);

            const auto deadline = std::chrono::steady_clock::now() + kFrameWaitTimeout;
            std::uint64_t sequence = videoContext.Sequence();
            // EnsureMedia drains the prior input and sets start-time before
            // playback. An asynchronous set_time can report the new timeline
            // while still delivering old pixels, so it is deliberately absent
            // here. Every callback now belongs to this request's fresh input.
            videoContext.SetCaptureToken(request.mediaGeneration, request.requestId, 1);
            if (api.mediaPlayerPlay(player_.get()) != 0) {
                return DecodeStatus::RetryableFailure;
            }
            CapturedFrame captured;
            bool discardedPrerollFrame = false;
            for (;;) {
                const FrameWaitResult waitResult = videoContext.WaitForFrame(
                    sequence,
                    deadline,
                    request.requestId,
                    request.mediaGeneration,
                    1,
                    cancellation,
                    request,
                    captured);
                if (waitResult == FrameWaitResult::Cancelled) {
                    api.mediaPlayerSetPause(player_.get(), 1);
                    return DecodeStatus::Cancelled;
                }
                if (waitResult == FrameWaitResult::TimedOut) {
                    const auto finalState = api.mediaPlayerGetState(player_.get());
                    // Only a running input with a populated timeline and an
                    // explicit zero video-track count is negatively cached.
                    // An opening decoder, failed seek, or lone timeout is not.
                    const bool confirmedNoVideo = finalState == libvlc_Playing &&
                        api.mediaPlayerGetTime(player_.get()) >= 1000 &&
                        api.videoGetTrackCount(player_.get()) == 0;
                    api.mediaPlayerSetPause(player_.get(), 1);
                    return confirmedNoVideo ? DecodeStatus::UnsupportedMedia
                        : DecodeStatus::RetryableFailure;
                }

                // This displayed frame belongs to the fresh player's own
                // callback context. Do not qualify its pixels with get_time:
                // LibVLC's cached clock can lag burned-in displayed PTS by
                // several seconds or remain zero while the frame is correct.
                // LibVLC can still display one common preroll buffer inside a
                // fresh input before the start-time target. Never publish that
                // first buffer as an exact thumbnail; wait for the next display
                // callback. If there is no second frame, the bounded attempt
                // fails with a placeholder instead of mislabelling old pixels.
                if (!PassPreviewPrerollBarrier(
                        discardedPrerollFrame, captured.sequence, sequence)) {
                    captured = {};
                    continue;
                }
                break;
            }
            api.mediaPlayerSetPause(player_.get(), 1);

            auto mutableFrame = std::make_shared<PreviewFrame>();
            mutableFrame->mediaGeneration = request.mediaGeneration;
            mutableFrame->timestampMs = request.timestampMs;
            mutableFrame->width = captured.width;
            mutableFrame->height = captured.height;
            mutableFrame->pitch = captured.pitch;
            mutableFrame->topDown = true;
            mutableFrame->pixels = std::move(captured.pixels);
            if (!mutableFrame->IsValid()) {
                return DecodeStatus::RetryableFailure;
            }
            result = std::move(mutableFrame);
            return DecodeStatus::Ready;
        } catch (...) {
            return DecodeStatus::RuntimeFailure;
        }
    }

    void ReleaseMedia() noexcept override {
        const std::shared_ptr<PreviewVideoContext> context = videoContext_;
        if (context) {
            context->SetCaptureToken(0, 0, 0);
        }
        try {
            if (player_ && runtime_.IsLoaded()) {
                const LibVlcRuntime::Api& api = runtime_.Functions();
                api.mediaPlayerStop(player_.get());
                api.mediaPlayerSetMedia(player_.get(), nullptr);
            }
            player_.reset();
            // Player release is the callback lifetime boundary. Only after it
            // returns may this player's private opaque/buffers be retired.
            wakeRelay_.Clear(context);
            if (context) {
                context->Cleanup();
            }
            videoContext_.reset();
            media_.reset();
            mediaSource_.reset();
            loadedPath_.clear();
            loadedGeneration_ = 0;
        } catch (...) {
        }
    }

    void Shutdown() noexcept {
        ReleaseMedia();
        // LibVLC 3 has no documented callback-detach operation: lock/setup
        // callbacks are required to be non-null. Releasing the stopped player
        // synchronously closes its vout while videoContext_ is still alive.
        instance_.reset();
        runtime_.Unload();
    }

private:
    bool EnsurePlayer() {
        if (player_) {
            return true;
        }
        std::wstring ignoredError;
        if (!runtime_.Load(ignoredError)) {
            return false;
        }
        const LibVlcRuntime::Api& api = runtime_.Functions();

        if (!instance_) {
            std::array<const char*, kPrivateLibVlcArguments.size() + 2> arguments{};
            std::copy(kPrivateLibVlcArguments.begin(), kPrivateLibVlcArguments.end(),
                arguments.begin());
            arguments[kPrivateLibVlcArguments.size()] = "--no-audio";
            // Thumbnails are CPU RV32 buffers. GPU decoder/converter surfaces
            // provide no benefit here and complicate repeated vmem restarts.
            arguments.back() = "--avcodec-hw=none";
            libvlc_instance_t* rawInstance = api.newInstance(
                static_cast<int>(arguments.size()), arguments.data());
            if (rawInstance == nullptr) {
                return false;
            }
            instance_ = InstancePtr(rawInstance, InstanceReleaser{api.releaseInstance});
        }

        auto context = std::make_shared<PreviewVideoContext>();
        libvlc_media_player_t* rawPlayer = api.mediaPlayerNew(instance_.get());
        if (rawPlayer == nullptr) {
            return false;
        }
        player_ = PlayerPtr(rawPlayer, PlayerReleaser{api.mediaPlayerRelease});
        videoContext_ = std::move(context);
        api.videoSetCallbacks(
            player_.get(),
            &LockVideo,
            nullptr,
            &DisplayVideo,
            videoContext_.get());
        api.videoSetFormatCallbacks(
            player_.get(),
            &ConfigureVideo,
            &CleanupVideo);
        wakeRelay_.Set(videoContext_);
        api.audioSetVolume(player_.get(), 0);
        api.audioSetMute(player_.get(), 1);
        return true;
    }

    bool EnsureMedia(const DecodeRequest& request) {
        // Decode has already released the preceding player/vout. A fresh
        // player/input also recovers Ended/Stopped/Error without set_pause(0).
        if (request.path.empty() || !player_ || !instance_) {
            return false;
        }

        const LibVlcRuntime::Api& api = runtime_.Functions();
        mediaSource_ = std::make_unique<FileMediaSource>(request.path);
        libvlc_media_t* rawMedia = api.mediaNewCallbacks(
            instance_.get(),
            &OpenFileMedia,
            &ReadFileMedia,
            &SeekFileMedia,
            &CloseFileMedia,
            mediaSource_.get());
        if (rawMedia == nullptr) {
            mediaSource_.reset();
            return false;
        }
        media_ = MediaPtr(rawMedia, MediaReleaser{api.mediaRelease});
        const auto target = PreviewDecodeTarget(request);
        const auto milliseconds = std::to_string(1000 + target % 1000).substr(1);
        const std::string startTime = ":start-time=" +
            std::to_string(target / 1000) + "." + milliseconds;
        api.mediaAddOption(media_.get(), startTime.c_str());
        api.mediaPlayerSetMedia(player_.get(), media_.get());
        loadedPath_ = request.path;
        loadedGeneration_ = request.mediaGeneration;
        return true;
    }

    LibVlcRuntime runtime_;
    InstancePtr instance_;
    PlayerPtr player_;
    MediaPtr media_;
    std::unique_ptr<FileMediaSource> mediaSource_;
    std::shared_ptr<PreviewVideoContext> videoContext_;
    PreviewContextWakeRelay& wakeRelay_;
    std::wstring loadedPath_;
    std::uint32_t loadedGeneration_ = 0;
};

}  // namespace

#if defined(VIDEOPLAYER_TESTING)
bool VerifyPreviewCaptureIsolationForTesting() {
    bool discardedPrerollFrame = false;
    std::uint64_t barrierSequence = 7;
    if (PassPreviewPrerollBarrier(discardedPrerollFrame, 8, barrierSequence) ||
        barrierSequence != 8 ||
        !PassPreviewPrerollBarrier(discardedPrerollFrame, 9, barrierSequence) ||
        barrierSequence != 8) {
        return false;
    }

    PreviewVideoContext context;
    char chroma[4]{};
    unsigned int width = 2, height = 2, pitch = 0, lines = 0;
    if (context.Configure(chroma, &width, &height, &pitch, &lines) == 0) {
        return false;
    }
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> epoch{1}, latest{2};
    std::atomic<std::uint32_t> generation{1};
    PreviewCancellation cancellation{stopping, epoch, generation, latest};
    PreviewDecodeRequest request{L"A", 1, 2, 1, 1000, 10000};
    context.SetCaptureToken(1, 1, 1);
    void* oldPlane = nullptr;
    void* oldPicture = context.Lock(&oldPlane);
    context.SetCaptureToken(0, 0, 0);
    context.SetCaptureToken(1, 2, 1);
    context.Unlock(oldPicture);
    CapturedFrame frame;
    const auto stale = context.WaitForFrame(0, std::chrono::steady_clock::now(),
        2, 1, 1, cancellation, request, frame);
    if (stale != FrameWaitResult::TimedOut || !frame.pixels.empty()) {
        return false;
    }
    const auto sequence = context.Sequence();
    void* newPlane = nullptr;
    void* newPicture = context.Lock(&newPlane);
    context.Unlock(newPicture);
    if (context.WaitForFrame(sequence, std::chrono::steady_clock::now(),
        2, 1, 1, cancellation, request, frame) != FrameWaitResult::Ready) {
        return false;
    }
    // A seek retry with the same request ID must reject the earlier attempt.
    oldPicture = context.Lock(&oldPlane);
    const auto retrySequence = context.Sequence();
    context.SetCaptureToken(0, 0, 0);
    context.SetCaptureToken(1, 2, 2);
    context.Unlock(oldPicture);
    frame = {};
    if (context.WaitForFrame(retrySequence, std::chrono::steady_clock::now(),
            2, 1, 2, cancellation, request, frame) != FrameWaitResult::TimedOut ||
        !frame.pixels.empty()) {
        return false;
    }

    auto oldContext = std::make_shared<PreviewVideoContext>();
    auto currentContext = std::make_shared<PreviewVideoContext>();
    width = height = 2;
    if (oldContext->Configure(chroma, &width, &height, &pitch, &lines) == 0) {
        return false;
    }
    width = height = 2;
    if (currentContext->Configure(chroma, &width, &height, &pitch, &lines) == 0) {
        return false;
    }
    PreviewContextWakeRelay relay;
    relay.Set(oldContext);
    oldContext->SetCaptureToken(1, 2, 1);
    void* latePlane = nullptr;
    void* latePicture = oldContext->Lock(&latePlane);
    relay.Set(currentContext);
    relay.Clear(oldContext);
    if (!relay.TargetsForTesting(currentContext)) {
        return false;
    }
    oldContext->Unlock(latePicture);
    const auto currentSequence = currentContext->Sequence();
    frame = {};
    if (currentContext->WaitForFrame(currentSequence,
            std::chrono::steady_clock::now(), 2, 1, 1,
            cancellation, request, frame) != FrameWaitResult::TimedOut ||
        !frame.pixels.empty()) {
        return false;
    }
    currentContext->SetCaptureToken(1, 2, 1);
    void* currentPlane = nullptr;
    void* currentPicture = currentContext->Lock(&currentPlane);
    currentContext->Unlock(currentPicture);
    return currentContext->WaitForFrame(currentSequence,
        std::chrono::steady_clock::now(), 2, 1, 1,
        cancellation, request, frame) == FrameWaitResult::Ready;
}
#endif

class PreviewEngine::Impl final {
public:
    explicit Impl(PreviewBackendFactory factory = {})
        : backendFactory(std::move(factory)) {
    }

    PreviewCancellation Cancellation() const noexcept {
        return {stopping, cancelEpoch, activeMediaGeneration, latestRequestId};
    }

    bool IsCurrentLocked(const DecodeRequest& request) const noexcept {
        return initialized && accepting && !Cancellation().IsCancelled(request);
    }

    void Notify() {
        HWND target = nullptr;
        UINT message = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (initialized && accepting) {
                target = notificationWindow;
                message = resultMessage;
            }
        }
        if (target != nullptr && message != 0) {
            ::PostMessageW(target, message, 0, 0);
        }
    }

    void Publish(const DecodeRequest& request, PreviewFramePtr frame) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!IsCurrentLocked(request) || !frame || !frame->IsValid() ||
                frame->mediaGeneration != request.mediaGeneration ||
                frame->timestampMs != request.timestampMs) {
                return;
            }
            // Cache and publication are both guarded by current epoch/request.
            // Cancelled/stale frames never poison the current media cache.
            cache.Put(frame);
            requestState = PreviewRequestState::Ready;
            readyResult = PreviewResult{request.mediaGeneration,
                request.requestId, request.timestampMs, std::move(frame)};
        }
        Notify();
    }

    void CompleteFailure(const DecodeRequest& request, const DecodeStatus status) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!IsCurrentLocked(request)) {
                return;
            }
            readyResult.reset();
            switch (status) {
            case DecodeStatus::Cancelled:
                requestState = PreviewRequestState::Cancelled;
                break;
            case DecodeStatus::UnsupportedMedia:
                requestState = PreviewRequestState::UnsupportedMedia;
                unsupportedGeneration = request.mediaGeneration;
                break;
            case DecodeStatus::RuntimeFailure:
                requestState = PreviewRequestState::RuntimeFailure;
                break;
            default:
                requestState = PreviewRequestState::RetryableFailure;
                break;
            }
        }
        // Notification is a wake-up, not a heap-owned result. Failure leaves
        // no frame; UI queries state and permits the next explicit hover.
        Notify();
    }

    void WorkerMain() noexcept {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        std::unique_ptr<PreviewBackend> backend;
        try {
            for (;;) {
                DecodeRequest request;
                bool releaseOnly = false;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    condition.wait(lock, [this] {
                        return stopping.load(std::memory_order_acquire) ||
                            pending.has_value() || mediaResetRequested;
                    });
                    if (stopping.load(std::memory_order_acquire)) {
                        break;
                    }
                    if (mediaResetRequested) {
                        mediaResetRequested = false;
                        releaseOnly = true;
                    } else {
                        condition.wait_until(lock, nextDecodeAllowed, [this] {
                            return stopping.load(std::memory_order_acquire) ||
                                mediaResetRequested;
                        });
                        if (stopping.load(std::memory_order_acquire)) {
                            break;
                        }
                        if (mediaResetRequested || !pending.has_value()) {
                            continue;
                        }
                        request = std::move(*pending);
                        pending.reset();
                        requestState = PreviewRequestState::Decoding;
                        workerState = backend ? PreviewWorkerState::Running
                            : PreviewWorkerState::Recovering;
                        nextDecodeAllowed =
                            std::chrono::steady_clock::now() + kDecodeThrottle;
                    }
                }
                if (releaseOnly) {
                    if (backend) {
                        backend->ReleaseMedia();
                    }
                    continue;
                }

                DecodeStatus status = DecodeStatus::Cancelled;
                PreviewFramePtr frame;
                try {
                    for (unsigned int attempt = 0;
                        attempt < kMaximumDecodeAttempts; ++attempt) {
                        if (Cancellation().IsCancelled(request)) {
                            status = DecodeStatus::Cancelled;
                            break;
                        }
                        if (!backend) {
                            backend = backendFactory ? backendFactory()
                                : std::make_unique<WorkerResources>(videoWakeRelay);
                        }
                        if (!backend) {
                            status = DecodeStatus::RuntimeFailure;
                            break;
                        }
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            if (!stopping.load(std::memory_order_acquire)) {
                                workerState = PreviewWorkerState::Running;
                            }
                        }
                        frame.reset();
                        status = backend->Decode(request, Cancellation(), frame);
                        if (Cancellation().IsCancelled(request)) {
                            frame.reset();
                            status = DecodeStatus::Cancelled;
                            break;
                        }
                        if (status == DecodeStatus::Ready && (!frame ||
                            !frame->IsValid() ||
                            frame->mediaGeneration != request.mediaGeneration ||
                            frame->timestampMs != request.timestampMs)) {
                            frame.reset();
                            status = DecodeStatus::RetryableFailure;
                        }
                        if (status != DecodeStatus::RetryableFailure) {
                            break;
                        }
                        // Only preview media restarts. Source and callback
                        // buffers remain alive until synchronous stop drains.
                        backend->ReleaseMedia();
                        if (attempt + 1 >= kMaximumDecodeAttempts) {
                            break;
                        }
                        std::unique_lock<std::mutex> lock(mutex);
                        if (!stopping.load(std::memory_order_acquire)) {
                            workerState = PreviewWorkerState::Recovering;
                        }
                        condition.wait_for(lock, kRetryBackoff, [this, &request] {
                            return Cancellation().IsCancelled(request);
                        });
                    }
                } catch (...) {
                    status = DecodeStatus::RuntimeFailure;
                    frame.reset();
                }

                if (status == DecodeStatus::RuntimeFailure) {
                    // Cleanup stays on the worker. Next explicit request uses
                    // this same thread and creates a fresh preview backend.
                    backend.reset();
                }
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (!stopping.load(std::memory_order_acquire)) {
                        workerState = status == DecodeStatus::RuntimeFailure
                            ? PreviewWorkerState::Faulted : PreviewWorkerState::Running;
                    }
                }
                if (status == DecodeStatus::Ready) {
#if defined(VIDEOPLAYER_TESTING)
                    if (failNextPublicationForTesting.exchange(false, std::memory_order_acq_rel)) {
                        // Deliberately cover a fatal outer-loop exception,
                        // rather than the per-request Decode catch.
                        throw std::bad_alloc{};
                    }
#endif
                    Publish(request, std::move(frame));
                } else {
                    CompleteFailure(request, status);
                }
            }
        } catch (...) {
            // An exception outside Decode cannot leave a live-looking queue.
            {
                std::lock_guard<std::mutex> lock(mutex);
                pending.reset();
                readyResult.reset();
                requestState = PreviewRequestState::RuntimeFailure;
                workerState = PreviewWorkerState::Faulted;
                workerExiting = true;
            }
            Notify();
        }
        // Sources/players/callbacks die before DLL unload. No mutex is held.
        backend.reset();
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping.load(std::memory_order_acquire)) {
                workerState = PreviewWorkerState::Stopped;
            }
            workerExited = true;
        }
        condition.notify_all();
    }

    mutable std::mutex mutex;
    std::mutex shutdownMutex;
    std::condition_variable condition;
    PreviewContextWakeRelay videoWakeRelay;
    PreviewCache cache{24};
    PreviewBackendFactory backendFactory;
    std::thread worker;
    std::optional<DecodeRequest> pending;
    std::optional<PreviewResult> readyResult;
    std::optional<std::uint32_t> unsupportedGeneration;
    std::wstring mediaPath;
    HWND notificationWindow = nullptr;
    UINT resultMessage = kPreviewFrameReadyMessage;
    std::chrono::steady_clock::time_point nextDecodeAllowed{};
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> latestRequestId{0};
    std::atomic<std::uint64_t> cancelEpoch{0};
    std::atomic<std::uint32_t> activeMediaGeneration{0};
    std::uint64_t nextRequestId = 0;
    PreviewWorkerState workerState = PreviewWorkerState::NotStarted;
    PreviewRequestState requestState = PreviewRequestState::Idle;
    bool initialized = false;
    bool accepting = false;
    bool workerExited = true;
    bool workerExiting = false;
    bool mediaResetRequested = false;
#if defined(VIDEOPLAYER_TESTING)
    std::atomic<bool> failNextPublicationForTesting{false};
    unsigned int workerLaunchCountForTesting = 0;
#endif
};

PreviewEngine::PreviewEngine()
    : PreviewEngine(PreviewBackendFactory{}) {
}

PreviewEngine::PreviewEngine(PreviewBackendFactory backendFactory)
    : impl_(std::make_unique<Impl>(std::move(backendFactory))) {
}

PreviewEngine::~PreviewEngine() {
    Shutdown();
}

bool PreviewEngine::Initialize(
    const HWND notificationWindow,
    const UINT resultMessage) noexcept {
    if (notificationWindow == nullptr || !::IsWindow(notificationWindow) ||
        resultMessage < WM_APP) {
        return false;
    }
    try {
        std::lock_guard<std::mutex> shutdownLock(impl_->shutdownMutex);
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->notificationWindow = notificationWindow;
        impl_->resultMessage = resultMessage;
        impl_->stopping.store(false, std::memory_order_release);
        impl_->initialized = true;
        impl_->accepting = true;
        return true;
    } catch (...) {
        return false;
    }
}

void PreviewEngine::SetMedia(
    const std::wstring& ioPath,
    const std::uint32_t mediaGeneration) noexcept {
    try {
        std::wstring nextPath(ioPath);
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->mediaPath = std::move(nextPath);
            impl_->activeMediaGeneration.store(mediaGeneration, std::memory_order_release);
            impl_->cancelEpoch.fetch_add(1, std::memory_order_acq_rel);
            impl_->latestRequestId.store(++impl_->nextRequestId, std::memory_order_release);
            impl_->pending.reset();
            impl_->readyResult.reset();
            impl_->requestState = PreviewRequestState::Idle;
            impl_->unsupportedGeneration.reset();
            impl_->mediaResetRequested = true;
            impl_->cache.ClearForMedia(mediaGeneration);
        }
        impl_->condition.notify_all();
        impl_->videoWakeRelay.Wake();
        // No decoder cleanup or wait on the UI thread. DecodeRequest owns its
        // path; FileMediaSource remains alive until worker stop/release ends.
    } catch (...) {
        // Allocation failure must not keep the previous file eligible for
        // preview after the main player has already switched media.
        try {
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->mediaPath.clear();
                impl_->activeMediaGeneration.store(mediaGeneration, std::memory_order_release);
                impl_->cancelEpoch.fetch_add(1, std::memory_order_acq_rel);
                impl_->latestRequestId.store(++impl_->nextRequestId, std::memory_order_release);
                impl_->pending.reset();
                impl_->readyResult.reset();
                impl_->requestState = PreviewRequestState::RuntimeFailure;
                impl_->mediaResetRequested = true;
                impl_->cache.ClearForMedia(mediaGeneration);
            }
            impl_->condition.notify_all();
            impl_->videoWakeRelay.Wake();
            impl_->Notify();
        } catch (...) {
            // Synchronization itself is unavailable; cooperative shutdown
            // still observes these atomics without requiring this mutex.
            impl_->cancelEpoch.fetch_add(1, std::memory_order_acq_rel);
            impl_->condition.notify_all();
        }
    }
}

std::uint64_t PreviewEngine::RequestFrame(
    const std::int64_t timestampMs,
    const std::int64_t durationMs) noexcept {
    try {
        // Serialize thread creation/join with Shutdown, never with decoding.
        std::lock_guard<std::mutex> shutdownLock(impl_->shutdownMutex);
        DecodeRequest request;
        PreviewFramePtr cached;
        bool rejected = false;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            request.requestId = ++impl_->nextRequestId;
            impl_->latestRequestId.store(request.requestId, std::memory_order_release);
            request.mediaGeneration = impl_->activeMediaGeneration.load(std::memory_order_acquire);
            request.cancelEpoch = impl_->cancelEpoch.load(std::memory_order_acquire);
            request.timestampMs = QuantizePreviewTimestamp(timestampMs, durationMs);
            request.durationMs = (std::max)(std::int64_t{0}, durationMs);
            request.path = impl_->mediaPath;
            impl_->pending.reset();
            impl_->readyResult.reset();
            impl_->requestState = PreviewRequestState::Cancelled;
            if (!impl_->initialized || !impl_->accepting || request.path.empty() ||
                request.durationMs <= 0) {
                rejected = true;
            } else if (impl_->workerExiting && !impl_->workerExited) {
                impl_->requestState = PreviewRequestState::RuntimeFailure;
                rejected = true;
            } else if (impl_->unsupportedGeneration == request.mediaGeneration) {
                impl_->requestState = PreviewRequestState::UnsupportedMedia;
                rejected = true;
            } else {
                cached = impl_->cache.Find(request.mediaGeneration, request.timestampMs);
                impl_->requestState = PreviewRequestState::Pending;
            }
        }
        impl_->videoWakeRelay.Wake();
        if (rejected) {
            impl_->condition.notify_all();
            impl_->Notify();
            return request.requestId;
        }
        if (cached) {
            impl_->Publish(request, std::move(cached));
            return request.requestId;
        }
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (!impl_->IsCurrentLocked(request)) {
                return request.requestId;
            }
            impl_->pending = request;
            if (impl_->workerExited) {
                // Set only after backend/callback destruction; this join
                // cannot wait for a decoder or for its cleanup.
                if (impl_->worker.joinable()) {
                    impl_->worker.join();
                }
                try {
                    impl_->workerExited = false;
                    impl_->workerExiting = false;
                    impl_->workerState = PreviewWorkerState::Recovering;
                    impl_->worker = std::thread([implementation = impl_.get()] {
                        implementation->WorkerMain();
                    });
#if defined(VIDEOPLAYER_TESTING)
                    ++impl_->workerLaunchCountForTesting;
#endif
                } catch (...) {
                    impl_->workerExited = true;
                    impl_->workerState = PreviewWorkerState::Faulted;
                    impl_->pending.reset();
                    impl_->requestState = PreviewRequestState::RuntimeFailure;
                }
            } else if (impl_->workerState == PreviewWorkerState::Faulted) {
                impl_->workerState = PreviewWorkerState::Recovering;
            }
        }
        impl_->condition.notify_all();
        impl_->Notify();
        return request.requestId;
    } catch (...) {
        try {
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->pending.reset();
                impl_->readyResult.reset();
                impl_->requestState = PreviewRequestState::RuntimeFailure;
                impl_->cancelEpoch.fetch_add(1, std::memory_order_acq_rel);
            }
            impl_->condition.notify_all();
            impl_->videoWakeRelay.Wake();
            impl_->Notify();
        } catch (...) {
            impl_->cancelEpoch.fetch_add(1, std::memory_order_acq_rel);
            impl_->condition.notify_all();
        }
        return impl_->latestRequestId.load(std::memory_order_acquire);
    }
}

void PreviewEngine::CancelRequests() noexcept {
    try {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->latestRequestId.store(++impl_->nextRequestId, std::memory_order_release);
            impl_->cancelEpoch.fetch_add(1, std::memory_order_acq_rel);
            impl_->pending.reset();
            impl_->readyResult.reset();
            impl_->requestState = PreviewRequestState::Cancelled;
        }
        impl_->condition.notify_all();
        impl_->videoWakeRelay.Wake();
    } catch (...) {
    }
}

std::optional<PreviewResult> PreviewEngine::TakeLatestResult() noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        std::optional<PreviewResult> result = std::move(impl_->readyResult);
        impl_->readyResult.reset();
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

bool PreviewEngine::IsInitialized() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->initialized;
}

bool PreviewEngine::HasWorker() const noexcept {
    const auto state = GetWorkerState();
    return state == PreviewWorkerState::Running || state == PreviewWorkerState::Recovering;
}

PreviewWorkerState PreviewEngine::GetWorkerState() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->workerState;
}

PreviewRequestState PreviewEngine::GetRequestState(const std::uint64_t requestId) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return requestId != 0 && requestId == impl_->latestRequestId.load(std::memory_order_acquire)
        ? impl_->requestState : PreviewRequestState::Cancelled;
}

bool PreviewEngine::IsRequestPending(const std::uint64_t requestId) const noexcept {
    const auto state = GetRequestState(requestId);
    return state == PreviewRequestState::Pending || state == PreviewRequestState::Decoding;
}

#if defined(VIDEOPLAYER_TESTING)
void PreviewEngine::FailNextWorkerPublicationForTesting() noexcept {
    impl_->failNextPublicationForTesting.store(true, std::memory_order_release);
}

bool PreviewEngine::WorkerHasExitedForTesting() const noexcept {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->workerExited;
}

unsigned int PreviewEngine::WorkerLaunchCountForTesting() const noexcept {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->workerLaunchCountForTesting;
}
#endif

void PreviewEngine::Shutdown() noexcept {
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> shutdownLock(impl_->shutdownMutex);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->accepting = false;
        impl_->initialized = false;
        impl_->stopping.store(true, std::memory_order_release);
        impl_->cancelEpoch.fetch_add(1, std::memory_order_acq_rel);
        impl_->activeMediaGeneration.fetch_add(1, std::memory_order_acq_rel);
        impl_->latestRequestId.store(++impl_->nextRequestId, std::memory_order_release);
        impl_->pending.reset();
        impl_->readyResult.reset();
        impl_->requestState = PreviewRequestState::Cancelled;
        impl_->workerState = PreviewWorkerState::Stopping;
        impl_->mediaResetRequested = false;
        impl_->notificationWindow = nullptr;
    }
    impl_->condition.notify_all();
    impl_->videoWakeRelay.Wake();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->cache.Clear();
        impl_->workerExited = true;
        impl_->workerState = PreviewWorkerState::Stopped;
        impl_->mediaPath.clear();
        impl_->unsupportedGeneration.reset();
    }
}

}  // namespace videoplayer
