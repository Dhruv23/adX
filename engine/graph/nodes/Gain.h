// The gain and pan arithmetic every strip shares, defined once so a channel and an
// insert cannot disagree about what "pan 0.5" means.
//
// Linear balance, not constant power: centre is unity on both sides, and a hard pan
// silences the far side and leaves the near one untouched. It is the simplest law
// whose centre position is an exact no-op - which matters here more than loudness
// compensation does, because the golden-hash corpus and the routing tests compare
// against hand-computed values, and 0.7071... is not a value anybody hand-computes.
// Phase 4 owns what the mixer *sounds* like and may choose a pan law then; the
// corpus will say exactly which hashes that moves.
#pragma once

#include <cstddef>
#include <span>

namespace adx::graph {

struct PanGains {
    float left{1.0F};
    float right{1.0F};
};

[[nodiscard]] constexpr PanGains balance(float pan) noexcept {
    const float clamped = pan < -1.0F ? -1.0F : (pan > 1.0F ? 1.0F : pan);
    return PanGains{.left = clamped > 0.0F ? 1.0F - clamped : 1.0F,
                    .right = clamped < 0.0F ? 1.0F + clamped : 1.0F};
}

/// Scales a stereo pair in place by `gain`, then balances it by `pan`.
inline void applyGainPan(std::span<float> left, std::span<float> right, float gain,
                         float pan) noexcept {
    const PanGains sides = balance(pan);
    const float leftGain = gain * sides.left;
    const float rightGain = gain * sides.right;
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] *= leftGain;
        right[i] *= rightGain;
    }
}

} // namespace adx::graph
