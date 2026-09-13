#pragma once

#include "PreviewFrame.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>

namespace videoplayer {

class PreviewCache final {
public:
    explicit PreviewCache(std::size_t capacity = 24) noexcept;

    PreviewCache(const PreviewCache&) = delete;
    PreviewCache& operator=(const PreviewCache&) = delete;

    PreviewFramePtr Find(
        std::uint32_t mediaGeneration,
        std::int64_t quantizedTimestampMs) noexcept;
    // Returns the nearest valid frame within the inclusive distance bound,
    // preserving its actual timestamp. Ties prefer the earlier frame; hits
    // promote the frame to MRU. Negative distance bounds always miss.
    PreviewFramePtr FindNearest(
        std::uint32_t mediaGeneration,
        std::int64_t timestampMs,
        std::int64_t maximumDistanceMs) noexcept;
    void Put(PreviewFramePtr frame) noexcept;
    void ClearForMedia(std::uint32_t mediaGeneration) noexcept;
    void Clear() noexcept;

    std::size_t Size() const noexcept;
    std::size_t Capacity() const noexcept;
    std::uint32_t MediaGeneration() const noexcept;

private:
    struct Entry final {
        PreviewFramePtr frame;
    };

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::list<Entry> entries_;
    std::uint32_t mediaGeneration_ = 0;
};

}  // namespace videoplayer
