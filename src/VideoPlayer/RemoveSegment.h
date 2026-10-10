#pragma once

#include "ExportJob.h"
#include "ExportTypes.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace videoplayer {

struct RemovalPlan final {
    bool valid = false;
    std::vector<VideoInterval> requestedIntervals;
    std::vector<VideoInterval> actualIntervals;
    std::vector<VideoInterval> retainedIntervals;
    std::int64_t expectedDurationMs = 0;
    std::wstring muxer;
    std::wstring summary;
    std::wstring error;
};

// Reads keyframe packet timestamps without decoding/re-encoding the video.
std::vector<std::int64_t> ProbeVideoKeyframes(
    const std::wstring& ffprobePath, const std::wstring& sourcePath,
    const MediaProbeResult& probe, std::atomic_bool* cancellation,
    std::wstring& error);

RemovalPlan BuildRemovalPlan(const ExportSnapshot& snapshot,
    const MediaProbeResult& probe, const std::vector<std::int64_t>& keyframes,
    const std::wstring& destinationPath);

// When replacing the source, returns a verified .part file for the UI thread
// to commit after releasing the currently open player/preview handles.
ExportJobResult RunRemovalJob(
    const std::wstring& ffmpegPath, const std::wstring& ffprobePath,
    const ExportSnapshot& snapshot, const MediaProbeResult& probe,
    const RemovalPlan& plan, const std::wstring& destinationPath,
    bool overwriteConfirmed, bool replaceSource, std::atomic_bool& cancellation,
    ExportProgress progress = {});

}  // namespace videoplayer
