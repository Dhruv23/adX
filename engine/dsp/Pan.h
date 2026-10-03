// Pan laws.
//
// The mixer's own pan stays the linear balance of engine/graph/nodes/Gain.h: centre
// is an exact no-op, which the routing tests and the corpus rely on, and it is what
// iteration one's mixer did. These are the laws an instrument or effect chooses for
// spreading voices across the field - unison detune, sampler zones, granular spray -
// where equal loudness across the arc is the point.
#pragma once

#include <cstdint>

#include "engine/dsp/Math.h"

namespace adx::dsp {

enum class PanLaw : std::uint8_t {
    /// Balance: centre is unity on both sides; a hard pan silences the far side.
    Linear,
    /// Constant power, -3 dB at centre: L = cos, R = sin of the pan angle.
    ConstantPower,
    /// -4.5 dB at centre: the geometric mean of the -3 and -6 dB laws.
    Minus4_5,
    /// Constant voltage, -6 dB at centre: L + R = 1.
    Minus6,
};

struct StereoGains {
    float left{1.0F};
    float right{1.0F};
};

/// `pan` in [-1, 1]; clamped.
[[nodiscard]] inline StereoGains panGains(float pan, PanLaw law) noexcept {
    const float p = pan < -1.0F ? -1.0F : (pan > 1.0F ? 1.0F : pan);
    const float right = (p + 1.0F) * 0.5F; // 0 .. 1
    switch (law) {
    case PanLaw::Linear:
        return StereoGains{.left = p > 0.0F ? 1.0F - p : 1.0F, .right = p < 0.0F ? 1.0F + p : 1.0F};
    case PanLaw::ConstantPower:
        return StereoGains{.left = cosTurnsF(right * 0.25F), .right = sinTurnsF(right * 0.25F)};
    case PanLaw::Minus4_5: {
        const float cpLeft = cosTurnsF(right * 0.25F);
        const float cpRight = sinTurnsF(right * 0.25F);
        return StereoGains{.left = std::sqrt(cpLeft * (1.0F - right)),
                           .right = std::sqrt(cpRight * right)};
    }
    case PanLaw::Minus6:
        return StereoGains{.left = 1.0F - right, .right = right};
    }
    return {};
}

} // namespace adx::dsp
