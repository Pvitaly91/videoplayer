#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace videoplayer {

inline constexpr UINT kPreviewFrameReadyMessage = WM_APP + 0x32;

// Pixels are an immutable, tightly packed RV32 image. On little-endian Windows
// RV32 has the B, G, R, X byte order expected by a 32-bit BI_RGB DIB.
struct PreviewFrame final {
    std::uint32_t mediaGeneration = 0;
    std::int64_t timestampMs = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int pitch = 0;
    bool topDown = true;
    std::vector<std::uint8_t> pixels;

    bool IsValid() const noexcept {
        if (width == 0 || height == 0 ||
            width > (std::numeric_limits<unsigned int>::max)() / 4U) {
            return false;
        }
        if (pitch < width * 4U) {
            return false;
        }
        if (height > (std::numeric_limits<std::size_t>::max)() / pitch) {
            return false;
        }
        return pixels.size() >= static_cast<std::size_t>(pitch) * height;
    }
};

using PreviewFramePtr = std::shared_ptr<const PreviewFrame>;

// Results remain owned by PreviewEngine until the UI takes them. PostMessageW
// is used only as a wake-up, so a destroyed HWND can never leak a heap pointer.
struct PreviewResult final {
    std::uint32_t mediaGeneration = 0;
    std::uint64_t requestId = 0;
    std::int64_t timestampMs = 0;
    PreviewFramePtr frame;
};

}  // namespace videoplayer
