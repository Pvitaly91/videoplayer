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
constexpr std::int64_t kAcceptedSeekToleranceMs = 2500;

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
        const std::atomic<bool>& stopping,
        const std::atomic<std::uint64_t>& cancelEpoch,
        const std::uint64_t requestCancelEpoch,
        const std::atomic<std::uint32_t>& activeMediaGeneration,
        CapturedFrame& destination) noexcept {
        try {
            std::unique_lock<std::mutex> lock(mutex_);
            std::uint64_t observedSequence = afterSequence;
            for (;;) {
                while (frameSequence_ <= observedSequence) {
                    if (stopping.load(std::memory_order_acquire) ||
                        cancelEpoch.load(std::memory_order_acquire) != requestCancelEpoch ||
                        activeMediaGeneration.load(std::memory_order_acquire) != mediaGeneration) {
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
                if (stopping.load(std::memory_order_acquire) ||
                    cancelEpoch.load(std::memory_order_acquire) != requestCancelEpoch ||
                    activeMediaGeneration.load(std::memory_order_acquire) != mediaGeneration) {
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

void __cdecl UnlockVideo(
    void* opaque,
    void* picture,
    void* const*) noexcept {
    auto* context = static_cast<PreviewVideoContext*>(opaque);
    if (context != nullptr) {
        context->Unlock(picture);
    }
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

struct DecodeRequest final {
    std::wstring path;
    std::uint32_t mediaGeneration = 0;
    std::uint64_t requestId = 0;
    std::uint64_t cancelEpoch = 0;
    std::int64_t timestampMs = 0;
    std::int64_t durationMs = 0;
};

enum class DecodeStatus {
    Ready,
    Cancelled,
    Failed,
};

class WorkerResources final {
public:
    explicit WorkerResources(PreviewVideoContext& videoContext)
        : instance_(nullptr, InstanceReleaser{}),
          player_(nullptr, PlayerReleaser{}),
          media_(nullptr, MediaReleaser{}),
          videoContext_(videoContext) {
    }

    ~WorkerResources() {
        Shutdown();
    }

    WorkerResources(const WorkerResources&) = delete;
    WorkerResources& operator=(const WorkerResources&) = delete;

    DecodeStatus Decode(
        const DecodeRequest& request,
        const std::atomic<bool>& stopping,
        const std::atomic<std::uint64_t>& cancelEpoch,
        const std::atomic<std::uint32_t>& activeMediaGeneration,
        PreviewFramePtr& result) noexcept {
        result.reset();
        try {
            if (IsCancelled(request, stopping, cancelEpoch, activeMediaGeneration)) {
                return DecodeStatus::Cancelled;
            }
            if (!EnsurePlayer() || !EnsureMedia(request)) {
                return DecodeStatus::Failed;
            }

            const LibVlcRuntime::Api& api = runtime_.Functions();
            api.audioSetVolume(player_.get(), 0);
            api.audioSetMute(player_.get(), 1);

            std::int64_t decodeTarget = request.timestampMs;
            if (request.durationMs > 0) {
                decodeTarget = (std::min)(
                    decodeTarget,
                    request.durationMs > 1 ? request.durationMs - 1 : std::int64_t{0});
            }
            decodeTarget = (std::max)(std::int64_t{0}, decodeTarget);

            if (!started_) {
                if (api.mediaPlayerPlay(player_.get()) != 0) {
                    return DecodeStatus::Failed;
                }
                started_ = true;
            } else {
                api.mediaPlayerSetPause(player_.get(), 0);
            }

            const auto deadline = std::chrono::steady_clock::now() + kFrameWaitTimeout;
            api.mediaPlayerSetTime(player_.get(), static_cast<libvlc_time_t>(decodeTarget));
            std::uint64_t sequence = videoContext_.Sequence();
            // Activate the token only after the asynchronous seek request.
            // A buffer already locked by LibVLC keeps the previous token and
            // is rejected by WaitForFrame.
            videoContext_.SetCaptureToken(
                request.mediaGeneration,
                request.requestId,
                1);
            CapturedFrame captured;
            unsigned int seekAttempt = 1;
            for (;;) {
                const FrameWaitResult waitResult = videoContext_.WaitForFrame(
                    sequence,
                    deadline,
                    request.requestId,
                    request.mediaGeneration,
                    seekAttempt,
                    stopping,
                    cancelEpoch,
                    request.cancelEpoch,
                    activeMediaGeneration,
                    captured);
                if (waitResult == FrameWaitResult::Cancelled) {
                    api.mediaPlayerSetPause(player_.get(), 1);
                    return DecodeStatus::Cancelled;
                }
                if (waitResult == FrameWaitResult::TimedOut) {
                    api.mediaPlayerSetPause(player_.get(), 1);
                    return DecodeStatus::Failed;
                }

                const libvlc_time_t reportedTime = api.mediaPlayerGetTime(player_.get());
                const std::int64_t distance = reportedTime >= 0
                    ? (std::max)(
                        static_cast<std::int64_t>(reportedTime),
                        decodeTarget) -
                        (std::min)(
                            static_cast<std::int64_t>(reportedTime),
                            decodeTarget)
                    : 0;
                if (reportedTime < 0 || distance <= kAcceptedSeekToleranceMs) {
                    break;
                }
                if (seekAttempt >= 3) {
                    api.mediaPlayerSetPause(player_.get(), 1);
                    return DecodeStatus::Failed;
                }
                // Deactivate the old attempt before the retry seek. A buffer
                // already locked for that attempt can finish, but its token
                // cannot satisfy the next WaitForFrame call.
                videoContext_.SetCaptureToken(0, 0, 0);
                ++seekAttempt;
                sequence = videoContext_.Sequence();
                api.mediaPlayerSetTime(
                    player_.get(),
                    static_cast<libvlc_time_t>(decodeTarget));
                videoContext_.SetCaptureToken(
                    request.mediaGeneration,
                    request.requestId,
                    seekAttempt);
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
                return DecodeStatus::Failed;
            }
            result = std::move(mutableFrame);
            return DecodeStatus::Ready;
        } catch (...) {
            return DecodeStatus::Failed;
        }
    }

    void ReleaseMedia() noexcept {
        try {
            if (player_ && runtime_.IsLoaded()) {
                const LibVlcRuntime::Api& api = runtime_.Functions();
                api.mediaPlayerStop(player_.get());
                api.mediaPlayerSetMedia(player_.get(), nullptr);
            }
            media_.reset();
            mediaSource_.reset();
            loadedPath_.clear();
            loadedGeneration_ = 0;
            started_ = false;
        } catch (...) {
        }
    }

    void WakeVideoWait() noexcept {
        videoContext_.Wake();
    }

    void Shutdown() noexcept {
        ReleaseMedia();
        // LibVLC 3 has no documented callback-detach operation: lock/setup
        // callbacks are required to be non-null. Releasing the stopped player
        // synchronously closes its vout while videoContext_ is still alive.
        player_.reset();
        videoContext_.Cleanup();
        instance_.reset();
        runtime_.Unload();
    }

private:
    static bool IsCancelled(
        const DecodeRequest& request,
        const std::atomic<bool>& stopping,
        const std::atomic<std::uint64_t>& cancelEpoch,
        const std::atomic<std::uint32_t>& activeMediaGeneration) noexcept {
        return stopping.load(std::memory_order_acquire) ||
            cancelEpoch.load(std::memory_order_acquire) != request.cancelEpoch ||
            activeMediaGeneration.load(std::memory_order_acquire) !=
                request.mediaGeneration;
    }

    bool EnsurePlayer() {
        if (player_) {
            return true;
        }
        std::wstring ignoredError;
        if (!runtime_.Load(ignoredError)) {
            return false;
        }
        const LibVlcRuntime::Api& api = runtime_.Functions();

        std::array<const char*, kPrivateLibVlcArguments.size() + 1> arguments{};
        std::copy(
            kPrivateLibVlcArguments.begin(),
            kPrivateLibVlcArguments.end(),
            arguments.begin());
        arguments.back() = "--no-audio";
        libvlc_instance_t* rawInstance = api.newInstance(
            static_cast<int>(arguments.size()),
            arguments.data());
        if (rawInstance == nullptr) {
            return false;
        }
        instance_ = InstancePtr(rawInstance, InstanceReleaser{api.releaseInstance});

        libvlc_media_player_t* rawPlayer = api.mediaPlayerNew(instance_.get());
        if (rawPlayer == nullptr) {
            return false;
        }
        player_ = PlayerPtr(rawPlayer, PlayerReleaser{api.mediaPlayerRelease});
        api.videoSetCallbacks(
            player_.get(),
            &LockVideo,
            &UnlockVideo,
            nullptr,
            &videoContext_);
        api.videoSetFormatCallbacks(
            player_.get(),
            &ConfigureVideo,
            &CleanupVideo);
        api.audioSetVolume(player_.get(), 0);
        api.audioSetMute(player_.get(), 1);
        return true;
    }

    bool EnsureMedia(const DecodeRequest& request) {
        if (media_ && loadedGeneration_ == request.mediaGeneration &&
            loadedPath_ == request.path) {
            return true;
        }
        ReleaseMedia();
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
    PreviewVideoContext& videoContext_;
    std::wstring loadedPath_;
    std::uint32_t loadedGeneration_ = 0;
    bool started_ = false;
};

}  // namespace

class PreviewEngine::Impl final {
public:
    void WorkerMain() noexcept {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        WorkerResources resources(videoContext);
        try {
            for (;;) {
                DecodeRequest request;
                bool releaseOnly = false;
                std::uint64_t releaseSerial = 0;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    condition.wait(lock, [this] {
                        return stopping.load(std::memory_order_acquire) ||
                            pending.has_value() || mediaResetRequested;
                    });
                    if (stopping.load(std::memory_order_acquire)) {
                        break;
                    }

                    if (!pending.has_value() && mediaResetRequested) {
                        mediaResetRequested = false;
                        releaseOnly = true;
                        releaseSerial = mediaResetSerial;
                    } else {
                        const auto now = std::chrono::steady_clock::now();
                        if (now < nextDecodeAllowed) {
                            condition.wait_until(lock, nextDecodeAllowed, [this] {
                                return stopping.load(std::memory_order_acquire);
                            });
                            if (stopping.load(std::memory_order_acquire)) {
                                break;
                            }
                        }
                        if (!pending.has_value()) {
                            continue;
                        }
                        request = *pending;
                        pending.reset();
                        mediaResetRequested = false;
                        nextDecodeAllowed =
                            std::chrono::steady_clock::now() + kDecodeThrottle;
                    }
                }

                if (releaseOnly) {
                    resources.ReleaseMedia();
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        completedMediaResetSerial = (std::max)(
                            completedMediaResetSerial, releaseSerial);
                    }
                    condition.notify_all();
                    continue;
                }

                PreviewFramePtr frame = cache.Find(
                    request.mediaGeneration,
                    request.timestampMs);
                DecodeStatus status = DecodeStatus::Ready;
                if (!frame) {
                    status = resources.Decode(
                        request,
                        stopping,
                        cancelEpoch,
                        activeMediaGeneration,
                        frame);
                    if (status == DecodeStatus::Ready && frame) {
                        cache.Put(frame);
                    }
                }

                if (status == DecodeStatus::Failed) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (activeMediaGeneration.load(std::memory_order_acquire) ==
                            request.mediaGeneration &&
                        latestRequestId.load(std::memory_order_acquire) ==
                            request.requestId) {
                        failedGeneration = request.mediaGeneration;
                    }
                } else if (status == DecodeStatus::Ready && frame) {
                    Publish(request, std::move(frame));
                }
            }
        } catch (...) {
        }
        resources.Shutdown();
        {
            std::lock_guard<std::mutex> lock(mutex);
            completedMediaResetSerial = (std::max)(
                completedMediaResetSerial, mediaResetSerial);
        }
        condition.notify_all();
    }

    void Publish(const DecodeRequest& request, PreviewFramePtr frame) noexcept {
        HWND targetWindow = nullptr;
        UINT targetMessage = 0;
        try {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!initialized || !accepting ||
                    activeMediaGeneration.load(std::memory_order_acquire) !=
                        request.mediaGeneration ||
                    cancelEpoch.load(std::memory_order_acquire) !=
                        request.cancelEpoch ||
                    request.requestId <= latestPublishedRequestId) {
                    return;
                }
                latestPublishedRequestId = request.requestId;
                readyResult = PreviewResult{
                    request.mediaGeneration,
                    request.requestId,
                    request.timestampMs,
                    std::move(frame)};
                targetWindow = notificationWindow;
                targetMessage = resultMessage;
            }
            if (targetWindow != nullptr && targetMessage != 0) {
                ::PostMessageW(targetWindow, targetMessage, 0, 0);
            }
        } catch (...) {
        }
    }

    mutable std::mutex mutex;
    std::mutex shutdownMutex;
    std::condition_variable condition;
    PreviewVideoContext videoContext;
    PreviewCache cache{24};
    std::thread worker;
    std::optional<DecodeRequest> pending;
    std::optional<PreviewResult> readyResult;
    std::wstring mediaPath;
    HWND notificationWindow = nullptr;
    UINT resultMessage = kPreviewFrameReadyMessage;
    std::chrono::steady_clock::time_point nextDecodeAllowed{};
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> latestRequestId{0};
    std::atomic<std::uint64_t> cancelEpoch{0};
    std::atomic<std::uint32_t> activeMediaGeneration{0};
    std::uint64_t nextRequestId = 0;
    std::uint64_t latestPublishedRequestId = 0;
    std::uint64_t mediaResetSerial = 0;
    std::uint64_t completedMediaResetSerial = 0;
    std::uint32_t failedGeneration = 0;
    bool initialized = false;
    bool accepting = false;
    bool workerStarted = false;
    bool mediaResetRequested = false;
};

PreviewEngine::PreviewEngine()
    : impl_(std::make_unique<Impl>()) {
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
        std::uint64_t resetSerial = 0;
        bool waitForRelease = false;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->mediaPath = ioPath;
            impl_->activeMediaGeneration.store(
                mediaGeneration,
                std::memory_order_release);
            impl_->cancelEpoch.fetch_add(1, std::memory_order_acq_rel);
            ++impl_->nextRequestId;
            impl_->latestRequestId.store(
                impl_->nextRequestId,
                std::memory_order_release);
            impl_->pending.reset();
            impl_->readyResult.reset();
            impl_->latestPublishedRequestId = 0;
            impl_->failedGeneration = 0;
            impl_->mediaResetRequested = true;
            resetSerial = ++impl_->mediaResetSerial;
            waitForRelease = ioPath.empty() && impl_->workerStarted;
            if (!impl_->workerStarted) {
                impl_->completedMediaResetSerial = resetSerial;
            }
        }
        impl_->cache.ClearForMedia(mediaGeneration);
        impl_->condition.notify_all();
        impl_->videoContext.Wake();
        if (waitForRelease) {
            std::unique_lock<std::mutex> lock(impl_->mutex);
            impl_->condition.wait(lock, [implementation = impl_.get(), resetSerial] {
                return implementation->completedMediaResetSerial >= resetSerial ||
                    implementation->stopping.load(std::memory_order_acquire);
            });
        }
    } catch (...) {
    }
}

std::uint64_t PreviewEngine::RequestFrame(
    const std::int64_t timestampMs,
    const std::int64_t durationMs) noexcept {
    try {
        DecodeRequest request;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            ++impl_->nextRequestId;
            request.requestId = impl_->nextRequestId;
            impl_->latestRequestId.store(request.requestId, std::memory_order_release);
            request.mediaGeneration =
                impl_->activeMediaGeneration.load(std::memory_order_acquire);
            request.cancelEpoch = impl_->cancelEpoch.load(std::memory_order_acquire);
            request.timestampMs = QuantizePreviewTimestamp(timestampMs, durationMs);
            request.durationMs = (std::max)(std::int64_t{0}, durationMs);
            request.path = impl_->mediaPath;
            impl_->pending.reset();

            if (!impl_->initialized || !impl_->accepting || request.path.empty() ||
                request.durationMs <= 0 ||
                impl_->failedGeneration == request.mediaGeneration) {
                return request.requestId;
            }
        }

        PreviewFramePtr cached = impl_->cache.Find(
            request.mediaGeneration,
            request.timestampMs);
        if (cached) {
            impl_->Publish(request, std::move(cached));
            return request.requestId;
        }

        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (!impl_->initialized || !impl_->accepting ||
                impl_->latestRequestId.load(std::memory_order_acquire) !=
                    request.requestId ||
                impl_->activeMediaGeneration.load(std::memory_order_acquire) !=
                    request.mediaGeneration) {
                return request.requestId;
            }
            impl_->pending = request;
            if (!impl_->workerStarted) {
                try {
                    impl_->worker = std::thread([implementation = impl_.get()] {
                        implementation->WorkerMain();
                    });
                    impl_->workerStarted = true;
                } catch (...) {
                    impl_->pending.reset();
                    impl_->failedGeneration = request.mediaGeneration;
                    return request.requestId;
                }
            }
        }
        impl_->condition.notify_all();
        return request.requestId;
    } catch (...) {
        return impl_->latestRequestId.load(std::memory_order_acquire);
    }
}

void PreviewEngine::CancelRequests() noexcept {
    try {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            ++impl_->nextRequestId;
            impl_->latestRequestId.store(
                impl_->nextRequestId,
                std::memory_order_release);
            impl_->cancelEpoch.fetch_add(1, std::memory_order_acq_rel);
            impl_->pending.reset();
            impl_->readyResult.reset();
        }
        impl_->condition.notify_all();
        impl_->videoContext.Wake();
    } catch (...) {
    }
}

std::optional<PreviewResult> PreviewEngine::TakeLatestResult() noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->readyResult.has_value()) {
            return std::nullopt;
        }
        std::optional<PreviewResult> result = std::move(impl_->readyResult);
        impl_->readyResult.reset();
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

bool PreviewEngine::IsInitialized() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->initialized;
    } catch (...) {
        return false;
    }
}

bool PreviewEngine::HasWorker() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->workerStarted;
    } catch (...) {
        return false;
    }
}

void PreviewEngine::Shutdown() noexcept {
    if (!impl_) {
        return;
    }
    try {
        std::lock_guard<std::mutex> shutdownLock(impl_->shutdownMutex);
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (!impl_->initialized && !impl_->worker.joinable()) {
                impl_->cache.Clear();
                return;
            }
            impl_->accepting = false;
            impl_->initialized = false;
            impl_->stopping.store(true, std::memory_order_release);
            impl_->cancelEpoch.fetch_add(1, std::memory_order_acq_rel);
            impl_->activeMediaGeneration.fetch_add(1, std::memory_order_acq_rel);
            ++impl_->nextRequestId;
            impl_->latestRequestId.store(
                impl_->nextRequestId,
                std::memory_order_release);
            impl_->pending.reset();
            impl_->readyResult.reset();
            impl_->latestPublishedRequestId = 0;
            impl_->mediaResetRequested = false;
            impl_->notificationWindow = nullptr;
        }
        impl_->condition.notify_all();
        impl_->videoContext.Wake();
        if (impl_->worker.joinable()) {
            impl_->worker.join();
        }
        impl_->cache.Clear();
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->workerStarted = false;
            impl_->mediaPath.clear();
            impl_->failedGeneration = 0;
        }
    } catch (...) {
    }
}

}  // namespace videoplayer
