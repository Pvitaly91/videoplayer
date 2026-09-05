#pragma once

#include <cstdint>

namespace videoplayer {

struct ProgressStripInput final {
    bool fullscreen = false;
    bool toolbarVisible = true;
    bool hasMedia = false;
    int availableWidth = 0;
    std::int64_t positionMs = 0;
    std::int64_t durationMs = 0;
};

struct ProgressStripDecision final {
    bool visible = false;
    int completedWidth = 0;
};

// Determines whether the compact fullscreen progress strip is needed and how
// many pixels of its available width represent completed playback.
ProgressStripDecision EvaluateProgressStrip(
    const ProgressStripInput& input) noexcept;

}  // namespace videoplayer
