#pragma once

#include "ExportTypes.h"

#include <atomic>
#include <string>

namespace videoplayer {

MediaProbeResult ParseFfprobeFlat(const std::string& flatOutput);
MediaProbeResult ProbeMediaFile(
    const std::wstring& ffprobePath,
    const std::wstring& sourcePath,
    std::atomic_bool* cancellation = nullptr);
std::wstring DescribeMediaProbe(const MediaProbeResult& probe);

}  // namespace videoplayer
