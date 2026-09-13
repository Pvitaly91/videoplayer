#pragma once

#include "ExportTypes.h"

namespace videoplayer {

std::wstring FormatExportSeconds(std::int64_t milliseconds);
std::wstring ExportMuxerForPath(const std::wstring& path);
ExportPlan BuildExportPlan(
    const ExportSnapshot& snapshot,
    const MediaProbeResult& probe,
    const ExportOptions& options,
    const ExportSourceRegion& visibleRegion);

}  // namespace videoplayer
