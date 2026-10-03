// Oscillators: a phase accumulator with two ways of turning phase into a waveform.
//
//   band-limited  reads the mipmapped WaveTable for the octave the fundamental is in.
//                 Nothing above Nyquist exists to alias; this is what meets -60 dBc
//                 at a 10 kHz fundamental (phase_4.md §4.1). The VA's oscillators.
//   polyBLEP      iteration one's oscillator, ported from AudioEngine.cpp: a naive
//                 waveform with a 2-sample polynomial patch at each discontinuity.
//                 Cheap and sufficient at low and middle pitches; its aliasing at
//                 high pitches is measured, not hidden, by dsp_polyblep_aliasing.
//                 Kept for the Additive port's unison stage, which v1 built on it.
//
// Phase is in turns, [0, 1). The increment is frequency / sample rate.
#pragma once

#include <cstdint>

#include "engine/dsp/WaveTable.h"

namespace adx::dsp {

enum class OscShape : std::uint8_t { Sine, Saw, Square, Triangle, Pulse };

/// The polyBLEP residual for a discontinuity at phase 0: Valimaki's 2-sample patch,
/// verbatim from the archive.
[[nodiscard]] inline float polyBlep(float t, float dt) noexcept {
    if (dt <= 0.0F) {
        return 0.0F;
    }
    if (t < dt) {
        t /= dt;
        return t + t - (t * t) - 1.0F;
    }
    if (t > 1.0F - dt) {
        t = (t - 1.0F) / dt;
        return (t * t) + t + t + 1.0F;
    }
    return 0.0F;
}

class Oscillator {
public:
    /// Main thread: makes sure the shared tables exist before the audio thread reads
    /// them.
    static void prepareTables();

    void reset(float phase = 0.0F) noexcept {
        m_phase = phase - static_cast<float>(static_cast<int>(phase));
        m_integrator = 0.0F;
    }

    [[nodiscard]] float phase() const noexcept {
        return m_phase;
    }

    /// The band-limited sample at the current phase, then advances by `increment`.
    /// `pulseWidth` in (0, 1) is used by Pulse only.
    [[nodiscard]] float nextBandLimited(OscShape shape, float increment,
                                        float pulseWidth = 0.5F) noexcept;

    /// The archive's polyBLEP sample, then advances. Triangle is the archive's
    /// leaky-integrated band-limited square, with its integrator state.
    [[nodiscard]] float nextPolyBlep(OscShape shape, float increment,
                                     float pulseWidth = 0.5F) noexcept;

private:
    void advance(float increment) noexcept {
        m_phase += increment;
        if (m_phase >= 1.0F) {
            m_phase -= 1.0F;
        } else if (m_phase < 0.0F) {
            m_phase += 1.0F;
        }
    }

    float m_phase{0.0F};
    float m_integrator{0.0F};
};

} // namespace adx::dsp
