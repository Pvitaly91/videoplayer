#pragma once

#include "VideoGeometry.h"

#include <cstdint>

namespace videoplayer {

// scale is relative to the ordinary fitted crop rectangle. centerX/centerY are
// normalized positions inside that crop which remain under the viewport center.
struct VideoDisplayState final {
    double scale = 1.0;
    double centerX = 0.5;
    double centerY = 0.5;
};

enum class VideoEdge : std::uint8_t {
    None,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

struct VideoAreaResizeResult final {
    bool valid = false;
    VideoCrop crop{};
    VideoDisplayState display{};
};

// Sanitizes scale and normalized center coordinates for the fitted crop and
// viewport. Oversized axes cannot expose a blank gap; smaller axes stay centered.
VideoDisplayState NormalizeVideoDisplayState(
    VideoDimensions cropSize,
    GeometryRect viewport,
    VideoDisplayState state) noexcept;

// Returns the presentation rectangle for the cropped video. Its dimensions are
// bounded to 16384 device pixels per axis.
GeometryRect ComputeVideoDisplayRect(
    VideoDimensions cropSize,
    GeometryRect viewport,
    VideoDisplayState state) noexcept;

// Hit-tests the visible content border, including clipped viewport edges.
// tolerance is already expressed in device pixels by the caller.
VideoEdge HitTestVideoEdge(
    GeometryRect contentRect,
    GeometryRect viewport,
    GeometryPoint point,
    int tolerance) noexcept;

// Resizes uniformly about the fixed viewport center using the immutable drag
// origin. startGripRect is its visible intersection with the viewport at
// mouse-down, so clipped edges stay usable for shrinking an oversized image.
VideoDisplayState ResizeVideoDisplay(
    VideoDisplayState startState,
    VideoDimensions cropSize,
    GeometryRect viewport,
    GeometryRect startGripRect,
    VideoEdge edge,
    GeometryPoint startMouse,
    GeometryPoint currentMouse) noexcept;

// Resizes the visible video area with independent screen-space axes while
// deriving a same-aspect source crop and one uniform pixel density. The result
// can therefore change aspect without stretching decoded pixels.
VideoAreaResizeResult ResizeVideoArea(
    VideoDimensions video,
    VideoCrop startCrop,
    VideoDisplayState startState,
    GeometryRect viewport,
    GeometryRect startContent,
    VideoEdge edge,
    GeometryPoint startMouse,
    GeometryPoint currentMouse) noexcept;

}  // namespace videoplayer
