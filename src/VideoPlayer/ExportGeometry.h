#pragma once

#include "ExportTypes.h"

namespace videoplayer {

// Maps the visible intersection of the presentation and clipping viewport
// back into decoded source coordinates. Overlay toolbars are deliberately not
// part of either rectangle, so they never remove pixels from an export.
ExportSourceRegion ComputeExportSourceRegion(
    VideoDimensions source,
    VideoCrop currentCrop,
    GeometryRect presentation,
    GeometryRect viewport,
    unsigned horizontalAlignment = 2,
    unsigned verticalAlignment = 2) noexcept;

GeometryRect ComputeExportPresentationRect(
    VideoCrop crop,
    GeometryRect renderBounds,
    Rational sampleAspectRatio,
    int rotationDegrees) noexcept;

ExportSourceRegion ComputeExportSourceRegionTransformed(
    VideoDimensions source,
    VideoCrop currentCrop,
    GeometryRect presentation,
    GeometryRect viewport,
    int rotationDegrees,
    unsigned horizontalAlignment = 2,
    unsigned verticalAlignment = 2) noexcept;

}  // namespace videoplayer
