#pragma once

#include "PreviewFrame.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace videoplayer {

class PreviewEngine final {
public:
    PreviewEngine();
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
    void Shutdown() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace videoplayer
