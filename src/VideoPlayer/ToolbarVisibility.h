#pragma once

#include <cstdint>

namespace videoplayer {

inline constexpr std::uint64_t kToolbarHideDelayMs = 1200U;
inline constexpr int kToolbarActivationHeightLogicalPixels = 100;

enum class ToolbarVisibilityAction : std::uint8_t {
    None,
    Show,
    Hide,
};

struct ToolbarVisibilityInput final {
    bool wasFullscreen = false;
    bool fullscreen = false;
    bool playing = false;
    bool currentlyVisible = true;

    bool cursorInsideClient = false;
    bool cursorMoved = false;
    bool pointerOverToolbar = false;
    bool seekDragging = false;
    bool volumeDragging = false;
    bool previewVisible = false;
    bool zoomSelecting = false;
    bool controlHasFocus = false;

    int cursorY = 0;
    int clientHeight = 0;
    unsigned dpi = 96U;
    std::uint64_t nowMs = 0U;
    std::uint64_t lastInteractionMs = 0U;
};

struct ToolbarVisibilityDecision final {
    bool shouldBeVisible = true;
    bool inBottomActivationZone = false;
    bool interactionBlocksHide = false;
    ToolbarVisibilityAction action = ToolbarVisibilityAction::None;
};

int ToolbarActivationHeight(unsigned dpi) noexcept;

// Pure state decision. The caller owns the 100 ms fullscreen timer and applies
// ShowWindow only when action is Show or Hide.
ToolbarVisibilityDecision EvaluateToolbarVisibility(
    const ToolbarVisibilityInput& input) noexcept;

}  // namespace videoplayer
