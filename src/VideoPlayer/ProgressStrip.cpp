#include "ProgressStrip.h"

#include <algorithm>
#include <cstdint>

namespace videoplayer {
namespace {

// Computes floor(numerator * multiplier / denominator) without overflowing a
// 64-bit intermediate. numerator is smaller than denominator and multiplier
// is limited to a positive int by the public input contract.
std::uint64_t MultiplyFractionByWidth(
    const std::uint64_t numerator,
    const std::uint64_t denominator,
    const std::uint32_t multiplier) noexcept {
    std::uint64_t quotient = 0;
    std::uint64_t remainder = 0;

    for (int bit = 31; bit >= 0; --bit) {
        quotient *= 2;

        if (remainder >= denominator - remainder) {
            remainder -= denominator - remainder;
            ++quotient;
        } else {
            remainder += remainder;
        }

        if ((multiplier & (std::uint32_t{1} << bit)) != 0U) {
            if (remainder >= denominator - numerator) {
                remainder -= denominator - numerator;
                ++quotient;
            } else {
                remainder += numerator;
            }
        }
    }
    return quotient;
}

}  // namespace

ProgressStripDecision EvaluateProgressStrip(
    const ProgressStripInput& input) noexcept {
    ProgressStripDecision decision{};
    if (!input.fullscreen || input.toolbarVisible || !input.hasMedia ||
        input.availableWidth <= 0 || input.durationMs <= 0) {
        return decision;
    }

    const std::int64_t position = std::clamp(
        input.positionMs, std::int64_t{0}, input.durationMs);
    if (position <= 0) {
        return decision;
    }
    decision.visible = true;
    if (position >= input.durationMs) {
        decision.completedWidth = input.availableWidth;
        return decision;
    }

    const std::uint64_t scaled = MultiplyFractionByWidth(
        static_cast<std::uint64_t>(position),
        static_cast<std::uint64_t>(input.durationMs),
        static_cast<std::uint32_t>(input.availableWidth));
    decision.completedWidth = (std::max)(
        1,
        (std::min)(input.availableWidth, static_cast<int>(scaled)));
    return decision;
}

}  // namespace videoplayer
