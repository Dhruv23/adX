// One-pole parameter smoothing.
//
// Every audible parameter a person can turn is smoothed with one of these, at about
// 10 ms (phase_4.md §4.1). Un-smoothed parameter changes are zipper noise, and
// iteration one had it on every knob. Automation does not need it - ramps arrive
// already smooth, per frame (§4.0) - so a node smooths the value it is handed and the
// smoother simply tracks a ramp with a few samples of lag.
//
// The first value a smoother sees is taken as-is rather than glided to from zero, so
// a node starting at gain 0.8 starts at 0.8.
#pragma once

#include <cstdint>

#include "engine/dsp/Math.h"

namespace adx::dsp {

inline constexpr float kSmoothingSeconds = 0.010F;

class Smoother {
public:
    /// Main thread. The time constant is the time to cover 1 - 1/e of a step.
    void prepare(std::uint32_t sampleRate, float seconds = kSmoothingSeconds) noexcept {
        const double samples = static_cast<double>(seconds) * static_cast<double>(sampleRate);
        m_coefficient = samples <= 1.0 ? 1.0F : static_cast<float>(1.0 - exp(-1.0 / samples));
    }

    /// Jumps to `value`: no glide.
    void snap(float value) noexcept {
        m_value = value;
        m_primed = true;
    }

    /// One sample toward `target`.
    [[nodiscard]] float next(float target) noexcept {
        if (!m_primed) {
            snap(target);
            return m_value;
        }
        m_value += (target - m_value) * m_coefficient;
        return m_value;
    }

    [[nodiscard]] float value() const noexcept {
        return m_value;
    }

    /// Forgets the value, so the next one is taken as-is.
    void reset() noexcept {
        m_primed = false;
    }

private:
    float m_coefficient{1.0F};
    float m_value{0.0F};
    bool m_primed{false};
};

} // namespace adx::dsp
