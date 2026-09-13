#include "ExportGeometry.h"

#include <algorithm>
#include <cmath>

namespace videoplayer {
namespace {

bool CropInside(const VideoDimensions source, const VideoCrop crop) noexcept {
    return source.IsValid() && crop.IsValid() && crop.x < source.width &&
        crop.y < source.height && crop.width <= source.width - crop.x &&
        crop.height <= source.height - crop.y;
}
unsigned AlignDown(const unsigned value, const unsigned alignment) noexcept {
    return alignment <= 1 ? value : value - value % alignment;
}
unsigned AlignUp(const unsigned value, const unsigned alignment) noexcept {
    return alignment <= 1 || value % alignment == 0
        ? value : value + alignment - value % alignment;
}

ExportSourceRegion AlignRegion(
    const VideoDimensions source, const double x, const double y,
    const double right, const double bottom,
    const unsigned horizontalAlignment, const unsigned verticalAlignment) noexcept {
    ExportSourceRegion result{};
    result.x = x;
    result.y = y;
    result.width = right - x;
    result.height = bottom - y;
    if (!std::isfinite(result.x) || !std::isfinite(result.y) ||
        !std::isfinite(result.width) || !std::isfinite(result.height) ||
        result.width <= 0.0 || result.height <= 0.0) return {};
    unsigned left = static_cast<unsigned>((std::max)(0.0, std::floor(result.x)));
    unsigned top = static_cast<unsigned>((std::max)(0.0, std::floor(result.y)));
    unsigned alignedRight = static_cast<unsigned>((std::min)(
        static_cast<double>(source.width), std::ceil(right)));
    unsigned alignedBottom = static_cast<unsigned>((std::min)(
        static_cast<double>(source.height), std::ceil(bottom)));
    left = AlignDown(left, horizontalAlignment);
    top = AlignDown(top, verticalAlignment);
    const unsigned requestedRight = AlignUp(alignedRight, horizontalAlignment);
    const unsigned requestedBottom = AlignUp(alignedBottom, verticalAlignment);
    alignedRight = requestedRight <= source.width
        ? requestedRight : AlignDown(source.width, horizontalAlignment);
    alignedBottom = requestedBottom <= source.height
        ? requestedBottom : AlignDown(source.height, verticalAlignment);
    if (alignedRight <= left || alignedBottom <= top) return {};
    result.aligned = {left, top, alignedRight - left, alignedBottom - top};
    result.valid = CropInside(source, result.aligned);
    return result;
}

int NormalizeRotation(int rotation) noexcept {
    rotation %= 360;
    if (rotation < 0) rotation += 360;
    if (rotation >= 315 || rotation < 45) return 0;
    if (rotation < 135) return 90;
    if (rotation < 225) return 180;
    return 270;
}

}  // namespace

ExportSourceRegion ComputeExportSourceRegion(
    const VideoDimensions source,
    const VideoCrop currentCrop,
    const GeometryRect presentation,
    const GeometryRect viewport,
    const unsigned horizontalAlignment,
    const unsigned verticalAlignment) noexcept {
    if (!CropInside(source, currentCrop) || presentation.IsEmpty() ||
        viewport.IsEmpty() || horizontalAlignment == 0 || verticalAlignment == 0) {
        return {};
    }
    const GeometryRect visible = IntersectRectangles(presentation, viewport);
    if (visible.IsEmpty()) return {};

    const double sx = static_cast<double>(currentCrop.width) / presentation.Width();
    const double sy = static_cast<double>(currentCrop.height) / presentation.Height();
    const double x = currentCrop.x + (visible.left - presentation.left) * sx;
    const double y = currentCrop.y + (visible.top - presentation.top) * sy;
    const double right = currentCrop.x + (visible.right - presentation.left) * sx;
    const double bottom = currentCrop.y + (visible.bottom - presentation.top) * sy;
    return AlignRegion(source, x, y, right, bottom,
        horizontalAlignment, verticalAlignment);
}

GeometryRect ComputeExportPresentationRect(
    const VideoCrop crop,
    const GeometryRect renderBounds,
    const Rational sampleAspectRatio,
    const int rotationDegrees) noexcept {
    if (!crop.IsValid() || renderBounds.IsEmpty()) return {};
    double sar = 1.0;
    if (sampleAspectRatio.numerator > 0 && sampleAspectRatio.denominator > 0) {
        sar = static_cast<double>(sampleAspectRatio.numerator) /
            static_cast<double>(sampleAspectRatio.denominator);
    }
    double displayWidth = crop.width * sar;
    double displayHeight = crop.height;
    const int rotation = NormalizeRotation(rotationDegrees);
    if (rotation == 90 || rotation == 270) std::swap(displayWidth, displayHeight);
    if (!std::isfinite(displayWidth) || !std::isfinite(displayHeight) ||
        displayWidth <= 0.0 || displayHeight <= 0.0) return {};
    const double scale = (std::min)(renderBounds.Width() / displayWidth,
        renderBounds.Height() / displayHeight);
    const int width = (std::max)(1, static_cast<int>(std::floor(displayWidth * scale + 0.5)));
    const int height = (std::max)(1, static_cast<int>(std::floor(displayHeight * scale + 0.5)));
    const int left = renderBounds.left + (renderBounds.Width() - width) / 2;
    const int top = renderBounds.top + (renderBounds.Height() - height) / 2;
    return {left, top, left + width, top + height};
}

ExportSourceRegion ComputeExportSourceRegionTransformed(
    const VideoDimensions source,
    const VideoCrop currentCrop,
    const GeometryRect presentation,
    const GeometryRect viewport,
    const int rotationDegrees,
    const unsigned horizontalAlignment,
    const unsigned verticalAlignment) noexcept {
    const int rotation = NormalizeRotation(rotationDegrees);
    if (rotation == 0) {
        return ComputeExportSourceRegion(source, currentCrop, presentation, viewport,
            horizontalAlignment, verticalAlignment);
    }
    if (!CropInside(source, currentCrop) || presentation.IsEmpty() || viewport.IsEmpty() ||
        horizontalAlignment == 0 || verticalAlignment == 0) return {};
    const GeometryRect visible = IntersectRectangles(presentation, viewport);
    if (visible.IsEmpty()) return {};
    const double u0 = static_cast<double>(visible.left - presentation.left) / presentation.Width();
    const double u1 = static_cast<double>(visible.right - presentation.left) / presentation.Width();
    const double v0 = static_cast<double>(visible.top - presentation.top) / presentation.Height();
    const double v1 = static_cast<double>(visible.bottom - presentation.top) / presentation.Height();
    double x = 0.0, y = 0.0, right = 0.0, bottom = 0.0;
    if (rotation == 90) {
        x = currentCrop.x + v0 * currentCrop.width;
        right = currentCrop.x + v1 * currentCrop.width;
        y = currentCrop.y + (1.0 - u1) * currentCrop.height;
        bottom = currentCrop.y + (1.0 - u0) * currentCrop.height;
    } else if (rotation == 180) {
        x = currentCrop.x + (1.0 - u1) * currentCrop.width;
        right = currentCrop.x + (1.0 - u0) * currentCrop.width;
        y = currentCrop.y + (1.0 - v1) * currentCrop.height;
        bottom = currentCrop.y + (1.0 - v0) * currentCrop.height;
    } else {
        x = currentCrop.x + (1.0 - v1) * currentCrop.width;
        right = currentCrop.x + (1.0 - v0) * currentCrop.width;
        y = currentCrop.y + u0 * currentCrop.height;
        bottom = currentCrop.y + u1 * currentCrop.height;
    }
    return AlignRegion(source, x, y, right, bottom,
        horizontalAlignment, verticalAlignment);
}

}  // namespace videoplayer
