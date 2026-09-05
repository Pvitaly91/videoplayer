#pragma once

#include "PreviewFrame.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace videoplayer {

enum class PreviewDecodeStatus {
    Ready, Cancelled, RetryableFailure, UnsupportedMedia, RuntimeFailure,
};

enum class PreviewWorkerState {
    NotStarted, Running, Recovering, Stopping, Stopped, Faulted,
};

enum class PreviewRequestState {
    Idle, Pending, Decoding, Ready, Cancelled, RetryableFailure,
    UnsupportedMedia, RuntimeFailure,
};

struct PreviewDecodeRequest final {
    std::wstring path;
    std::uint32_t mediaGeneration = 0;
    std::uint64_t requestId = 0;
    std::uint64_t cancelEpoch = 0;
    std::int64_t timestampMs = 0;
    std::int64_t durationMs = 0;
};

// The worker owns this view and all backend resources until Decode returns.
// A replacement request cancels an in-flight decode as well as media/epoch changes.
struct PreviewCancellation final {
    const std::atomic<bool>& stopping;
    const std::atomic<std::uint64_t>& cancelEpoch;
    const std::atomic<std::uint32_t>& mediaGeneration;
    const std::atomic<std::uint64_t>& latestRequestId;

    bool IsCancelled(const PreviewDecodeRequest& request) const noexcept {
        return stopping.load(std::memory_order_acquire) ||
            cancelEpoch.load(std::memory_order_acquire) != request.cancelEpoch ||
            mediaGeneration.load(std::memory_order_acquire) != request.mediaGeneration ||
            latestRequestId.load(std::memory_order_acquire) != request.requestId;
    }
};

// A narrow injection seam: tests exercise the production worker, coalescing,
// cache, retry, publication and shutdown using a deterministic decoder.
class PreviewBackend {
public:
    virtual ~PreviewBackend() = default;
    virtual PreviewDecodeStatus Decode(
        const PreviewDecodeRequest& request,
        const PreviewCancellation& cancellation,
        PreviewFramePtr& result) = 0;
    virtual void ReleaseMedia() noexcept = 0;
};

using PreviewBackendFactory = std::function<std::unique_ptr<PreviewBackend>()>;

#if defined(VIDEOPLAYER_TESTING)
bool VerifyPreviewCaptureIsolationForTesting();
#endif

class PreviewEngine final {
public:
    PreviewEngine();
    explicit PreviewEngine(PreviewBackendFactory backendFactory);
    ~PreviewEngine();

    PreviewEngine(const PreviewEngine&) = delete;
    PreviewEngine& operator=(const PreviewEngine&) = delete;
    PreviewEngine(PreviewEngine&&) = delete;
    PreviewEngine& operator=(PreviewEngine&&) = delete;

    // This only records the UI notification target. The worker, LibVLC
    // runtime, instance and player remain uncreated until the first cache miss.
    bool Initialize(
        HWND notificationWindow,
        UINT resultMessage = kPreviewFrameReadyMessage) noexcept;

    // ioPath must be the validated path also supplied to PlayerEngine. It may
    // use the Win32 extended prefix; preview input always uses read callbacks.
    void SetMedia(
        const std::wstring& ioPath,
        std::uint32_t mediaGeneration) noexcept;

    // Returns the monotonically increasing request ID the UI must retain.
    // A new request replaces any older request that has not started decoding.
    std::uint64_t RequestFrame(
        std::int64_t timestampMs,
        std::int64_t durationMs) noexcept;

    void CancelRequests() noexcept;

    // Call on the UI thread after kPreviewFrameReadyMessage. At most one result
    // is retained, so an abandoned/destroyed window cannot create a leak.
    std::optional<PreviewResult> TakeLatestResult() noexcept;

    bool IsInitialized() const noexcept;
    bool HasWorker() const noexcept;
    PreviewWorkerState GetWorkerState() const noexcept;
    PreviewRequestState GetRequestState(std::uint64_t requestId) const noexcept;
    bool IsRequestPending(std::uint64_t requestId) const noexcept;
    void Shutdown() noexcept;

#if defined(VIDEOPLAYER_TESTING)
    // Exercise the outer worker exception boundary, including actual thread
    // exit/rejoin, independently of the inner backend Decode catch.
    void FailNextWorkerPublicationForTesting() noexcept;
    bool WorkerHasExitedForTesting() const noexcept;
    unsigned int WorkerLaunchCountForTesting() const noexcept;
#endif

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace videoplayer
