#pragma once

#include <cstdint>
#include <string>

namespace videoplayer {

struct GeometryPoint final {
    int x = 0;
    int y = 0;
};

struct GeometryRect final {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    int Width() const noexcept;
    int Height() const noexcept;
    bool IsEmpty() const noexcept;
};

struct VideoDimensions final {
    unsigned width = 0;
    unsigned height = 0;

    bool IsValid() const noexcept { return width != 0U && height != 0U; }
};

struct NormalizedVideoRect final {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
};

struct VideoCrop final {
    unsigned x = 0;
    unsigned y = 0;
    unsigned width = 0;
    unsigned height = 0;

    bool IsValid() const noexcept { return width != 0U && height != 0U; }
};

struct VideoCropMapping final {
    bool valid = false;
    GeometryRect contentRect{};
    GeometryRect clippedSelection{};
    NormalizedVideoRect normalizedSelection{};
    VideoCrop crop{};
};

bool operator==(const GeometryPoint& left, const GeometryPoint& right) noexcept;
bool operator==(const GeometryRect& left, const GeometryRect& right) noexcept;
bool operator==(const VideoDimensions& left, const VideoDimensions& right) noexcept;
bool operator==(const VideoCrop& left, const VideoCrop& right) noexcept;

// Converts a logical-pixel distance at 96 DPI to device pixels using rounded
// integer arithmetic. Invalid DPI values fall back to 96 DPI.
int ScaleLogicalPixels(int logicalPixels, unsigned dpi) noexcept;

GeometryRect NormalizeRectangle(GeometryPoint first, GeometryPoint second) noexcept;
GeometryRect NormalizeRectangle(GeometryRect rectangle) noexcept;
GeometryRect IntersectRectangles(GeometryRect first, GeometryRect second) noexcept;
GeometryRect OffsetRectangle(GeometryRect rectangle, int deltaX, int deltaY) noexcept;

// Returns the actual auto-fit video rectangle inside viewport, including the
// centering offsets introduced by letterboxing or pillarboxing.
GeometryRect ComputeVideoContentRect(
    VideoDimensions video,
    GeometryRect viewport) noexcept;

// Clips selection to the fitted video content, enforces a minimum on-screen
// size, maps it to decoded source pixels, and aligns crop coordinates to even
// pixels whenever the source bounds make that possible.
VideoCropMapping MapSelectionToVideoCrop(
    VideoDimensions video,
    GeometryRect viewport,
    GeometryRect selection,
    int minimumSelectionPixels) noexcept;

// LibVLC 3.x internally consumes the first two crop values as absolute
// right/bottom coordinates even though its public syntax documents extents.
std::string FormatLibVlc3CropGeometry(const VideoCrop& crop);

}  // namespace videoplayer
