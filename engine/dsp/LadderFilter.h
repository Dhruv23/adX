// A 4-pole zero-delay-feedback ladder with input saturation.
//
// Four TPT one-pole lowpasses in series with global negative feedback, solved for the
// feedback sample in closed form each sample (Zavalishin, The Art of VA Filter Design,
// ch. 5). `resonance` in [0, 1] maps to a feedback gain of 0..4; at 4 the ladder
// self-oscillates. `drive` scales the signal into a tanh at the feedback summing
// point - the place a transistor ladder actually clips - and back out, so it adds
// harmonics without changing the level much. At drive 0 the saturation is skipped and
// the filter is exactly linear, which is what the response test measures.
#pragma once

#include <array>

#include "engine/dsp/Math.h"

namespace adx::dsp {

struct LadderCoefficients {
    /// The one-pole stage gain g / (1 + g).
    float stage{0.0F};
    /// Feedback, 0..4.
    float k{0.0F};
};

[[nodiscard]] LadderCoefficients ladderCoefficients(double cutoff, double resonance,
                                                    double sampleRate) noexcept;

class LadderFilter {
public:
    void reset() noexcept {
        m_state.fill(0.0F);
    }

    [[nodiscard]] float process(float x, const LadderCoefficients& c, float drive) noexcept {
        const float g = c.stage;
        const float oneMinus = 1.0F - g;
        // y4 = G^4 u + S, where S is what the stages' states contribute.
        const float g2 = g * g;
        const float s = (g2 * g * oneMinus * m_state[0]) + (g2 * oneMinus * m_state[1]) +
                        (g * oneMinus * m_state[2]) + (oneMinus * m_state[3]);
        const float g4 = g2 * g2;
        const float y4 = ((g4 * x) + s) / (1.0F + (c.k * g4));
        float u = x - (c.k * y4);
        if (drive > 0.0F) {
            const float gain = 1.0F + drive;
            u = tanhF(u * gain) / gain;
        }
        float in = u;
        for (float& z : m_state) {
            const float v = (in - z) * g;
            const float y = v + z;
            z = y + v;
            in = y;
        }
        constexpr float kTiny = 1e-30F;
        for (float& z : m_state) {
            if (z < kTiny && z > -kTiny) {
                z = 0.0F;
            }
        }
        return in;
    }

private:
    std::array<float, 4> m_state{};
};

} // namespace adx::dsp
