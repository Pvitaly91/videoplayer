#pragma once

#include <cstdint>

namespace videoplayer {

inline constexpr std::uint64_t kToolbarHideDelayMs = 1200U;
inline constexpr int kToolbarActivationHeightLogicalPixels = 100;
inline constexpr int kToolbarRestoreActivationHeightLogicalPixels = 48;

enum class ToolbarVisibilityAction : std::uint8_t {
    None,
    Show,
    Hide,
};

struct ToolbarVisibilityInput final {
    bool wasFullscreen = false;
    bool fullscreen = false;
    bool manuallyHidden = false;
    bool playing = false;
    bool currentlyVisible = true;
    bool restoreCurrentlyVisible = false;
    bool ownedPopupsAllowed = true;

    bool cursorInsideClient = false;
    bool cursorMoved = false;
    bool pointerOverToolbar = false;
    bool pointerOverRestore = false;
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
    bool shouldRestoreBeVisible = false;
    bool inBottomActivationZone = false;
    bool inRestoreActivationZone = false;
    bool interactionBlocksHide = false;
    ToolbarVisibilityAction action = ToolbarVisibilityAction::None;
    ToolbarVisibilityAction restoreAction = ToolbarVisibilityAction::None;
};

int ToolbarActivationHeight(unsigned dpi) noexcept;
int ToolbarRestoreActivationHeight(unsigned dpi) noexcept;

// Pure state decision. The caller owns the periodic toolbar timer and applies
// ShowWindow only when the corresponding action is Show or Hide.
ToolbarVisibilityDecision EvaluateToolbarVisibility(
    const ToolbarVisibilityInput& input) noexcept;

}  // namespace videoplayer
