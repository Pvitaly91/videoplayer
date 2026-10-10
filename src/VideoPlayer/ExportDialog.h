#pragma once

#include "ExportTypes.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <functional>

namespace videoplayer {

bool HasActiveExportDialog(HWND owner) noexcept;
void ShowExportDialog(HWND owner, HINSTANCE instance, ExportSnapshot snapshot);
using CommitSourceRemoval = std::function<bool(
    const ExportSnapshot&, const std::wstring&, std::wstring&)>;
using UpdateRemovalIntervals =
    std::function<void(const std::vector<VideoInterval>&)>;
void ShowRemoveSegmentDialog(HWND owner, HINSTANCE instance,
    ExportSnapshot snapshot, CommitSourceRemoval commitSource,
    UpdateRemovalIntervals updateIntervals = {});

}  // namespace videoplayer
