#include "ToolbarVisibility.h"

#include "VideoGeometry.h"

#include <algorithm>

namespace videoplayer {
namespace {

ToolbarVisibilityDecision FinishDecision(
    const ToolbarVisibilityInput& input,
    ToolbarVisibilityDecision decision,
    const bool visible) noexcept {
    decision.shouldBeVisible = visible;
    if (visible != input.currentlyVisible) {
        decision.action = visible
            ? ToolbarVisibilityAction::Show
            : ToolbarVisibilityAction::Hide;
    }
    return decision;
}

}  // namespace

int ToolbarActivationHeight(const unsigned dpi) noexcept {
    return ScaleLogicalPixels(kToolbarActivationHeightLogicalPixels, dpi);
}

ToolbarVisibilityDecision EvaluateToolbarVisibility(
    const ToolbarVisibilityInput& input) noexcept {
    ToolbarVisibilityDecision decision{};
    const int activationHeight = ToolbarActivationHeight(input.dpi);
    const int activationTop = (std::max)(0, input.clientHeight - activationHeight);
    decision.inBottomActivationZone =
        input.cursorInsideClient && input.clientHeight > 0 &&
        input.cursorY >= activationTop && input.cursorY < input.clientHeight;
    decision.interactionBlocksHide =
        input.pointerOverToolbar || input.seekDragging || input.volumeDragging ||
        input.previewVisible || input.controlHasFocus;

    if (!input.fullscreen) {
        return FinishDecision(input, decision, true);
    }

    // Area selection must include the video under the fullscreen toolbar.
    // This overrides pause/focus/mouse reveal rules until selection ends.
    if (input.zoomSelecting) {
        return FinishDecision(input, decision, false);
    }

    // Fullscreen transitions and every non-playing state normally leave the
    // controls visible.
    if (!input.wasFullscreen || !input.playing || decision.interactionBlocksHide) {
        return FinishDecision(input, decision, true);
    }

    if (!input.currentlyVisible) {
        const bool reveal = input.cursorMoved && decision.inBottomActivationZone;
        return FinishDecision(input, decision, reveal);
    }

    // A movement observed on this timer tick is itself fresh interaction. The
    // caller should also persist nowMs as its new lastInteractionMs.
    if (input.cursorMoved) {
        return FinishDecision(input, decision, true);
    }

    const std::uint64_t idleMs = input.nowMs >= input.lastInteractionMs
        ? input.nowMs - input.lastInteractionMs
        : 0U;
    return FinishDecision(input, decision, idleMs < kToolbarHideDelayMs);
}

}  // namespace videoplayer
