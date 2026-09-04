#include "PreviewCache.h"

#include "PreviewMath.h"

#include <algorithm>
#include <utility>

namespace videoplayer {

PreviewCache::PreviewCache(const std::size_t capacity) noexcept
    : capacity_(capacity) {
}

PreviewFramePtr PreviewCache::Find(
    const std::uint32_t mediaGeneration,
    const std::int64_t quantizedTimestampMs) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mediaGeneration != mediaGeneration_) {
            return {};
        }
        const auto found = std::find_if(
            entries_.begin(),
            entries_.end(),
            [mediaGeneration, quantizedTimestampMs](const Entry& entry) {
                return entry.frame &&
                    entry.frame->mediaGeneration == mediaGeneration &&
                    entry.frame->timestampMs == quantizedTimestampMs;
            });
        if (found == entries_.end()) {
            return {};
        }
        PreviewFramePtr result = found->frame;
        entries_.splice(entries_.begin(), entries_, found);
        return result;
    } catch (...) {
        return {};
    }
}

void PreviewCache::Put(PreviewFramePtr frame) noexcept {
    if (!frame || !frame->IsValid() || capacity_ == 0 ||
        frame->width > kPreviewMaximumWidth ||
        frame->height > kPreviewMaximumHeight ||
        frame->pitch != frame->width * 4U) {
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frame->mediaGeneration != mediaGeneration_) {
            return;
        }
        const auto found = std::find_if(
            entries_.begin(),
            entries_.end(),
            [&frame](const Entry& entry) {
                return entry.frame &&
                    entry.frame->mediaGeneration == frame->mediaGeneration &&
                    entry.frame->timestampMs == frame->timestampMs;
            });
        if (found != entries_.end()) {
            found->frame = std::move(frame);
            entries_.splice(entries_.begin(), entries_, found);
        } else {
            entries_.push_front(Entry{std::move(frame)});
        }
        while (entries_.size() > capacity_) {
            entries_.pop_back();
        }
    } catch (...) {
    }
}

void PreviewCache::ClearForMedia(const std::uint32_t mediaGeneration) noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        mediaGeneration_ = mediaGeneration;
    } catch (...) {
    }
}

void PreviewCache::Clear() noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        mediaGeneration_ = 0;
    } catch (...) {
    }
}

std::size_t PreviewCache::Size() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    } catch (...) {
        return 0;
    }
}

std::size_t PreviewCache::Capacity() const noexcept {
    return capacity_;
}

std::uint32_t PreviewCache::MediaGeneration() const noexcept {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        return mediaGeneration_;
    } catch (...) {
        return 0;
    }
}

}  // namespace videoplayer
