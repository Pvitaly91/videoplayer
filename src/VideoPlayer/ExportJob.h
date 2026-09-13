#pragma once

#include "ExportTypes.h"

#include <atomic>
#include <functional>
#include <string>

namespace videoplayer {

struct ExportJobResult final {
    bool success = false;
    bool cancelled = false;
    std::wstring finalPath;
    std::wstring partialPath;
    std::wstring diagnostics;
};

using ExportProgress = std::function<void(int, const std::wstring&)>;

ExportFileIdentity CaptureExportFileIdentity(const std::wstring& path);

ExportJobResult RunExportJob(
    const std::wstring& ffmpegPath,
    const std::wstring& ffprobePath,
    const ExportSnapshot& snapshot,
    const ExportOptions& options,
    const ExportPlan& plan,
    bool overwriteConfirmed,
    std::atomic_bool& cancellation,
    ExportProgress progress = {});

}  // namespace videoplayer
