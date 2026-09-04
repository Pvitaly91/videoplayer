#pragma once

#include <cstdint>

namespace videoplayer {

enum class ZoomState : std::uint8_t {
    None,
    Selecting,
    Applied,
};

enum class ZoomEscapeAction : std::uint8_t {
    None,
    CancelSelection,
    ResetCrop,
    ExitFullscreen,
};

struct ZoomEscapeDecision final {
    ZoomEscapeAction action = ZoomEscapeAction::None;
    ZoomState nextState = ZoomState::None;
    bool exitFullscreen = false;
};

// Priority: cancel an active selection, reset an applied crop, then leave
// fullscreen. This makes the documented two-Escape flow explicit and testable.
constexpr ZoomEscapeDecision EvaluateZoomEscape(
    const ZoomState state,
    const bool fullscreen) noexcept {
    return state == ZoomState::Selecting
        ? ZoomEscapeDecision{ZoomEscapeAction::CancelSelection, ZoomState::None, false}
        : state == ZoomState::Applied
            ? ZoomEscapeDecision{ZoomEscapeAction::ResetCrop, ZoomState::None, false}
            : fullscreen
                ? ZoomEscapeDecision{ZoomEscapeAction::ExitFullscreen, ZoomState::None, true}
                : ZoomEscapeDecision{};
}

}  // namespace videoplayer
