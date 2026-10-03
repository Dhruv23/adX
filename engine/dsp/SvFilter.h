// The TPT state-variable filter (Zavalishin): zero-delay feedback, every mode from one
// structure.
//
// Iteration one's resonant filter and formant bank were this filter, inlined twice in
// AudioEngine.cpp (phase_4.md §4.3); here it is once. Coefficients are computed per
// control frame - they cost a tan - and the filter runs per sample. It stays stable
// for any cutoff below Nyquist and any Q, which dsp_filter_stability_sweep checks
// across the whole space rather than at a few points.
#pragma once

#include <cstdint>

namespace adx::dsp {

enum class SvfMode : std::uint8_t { LowPass, BandPass, HighPass, Notch, Peak, AllPass };

struct SvfCoefficients {
    float a1{1.0F};
    float a2{0.0F};
    float a3{0.0F};
    /// Damping, 1/Q.
    float k{2.0F};
};

/// Coefficients for a cutoff in Hz (clamped below Nyquist) and a Q (clamped to
/// [0.025, 1000]). Double precision inside; the tan is dsp::tanTurns.
[[nodiscard]] SvfCoefficients svfCoefficients(double cutoff, double q, double sampleRate) noexcept;

/// The same, from a damping k = 1/Q directly - iteration one's resonance mapping
/// k = 2 - 2 * resonance uses this.
[[nodiscard]] SvfCoefficients svfCoefficientsK(double cutoff, double k, double sampleRate) noexcept;

struct SvfOutputs {
    float low{0.0F};
    float band{0.0F};
    float high{0.0F};
};

class SvFilter {
public:
    void reset() noexcept {
        m_ic1 = 0.0F;
        m_ic2 = 0.0F;
    }

    /// One sample; every output at once.
    [[nodiscard]] SvfOutputs tick(float x, const SvfCoefficients& c) noexcept {
        const float v3 = x - m_ic2;
        const float v1 = (c.a1 * m_ic1) + (c.a2 * v3);
        const float v2 = m_ic2 + (c.a2 * m_ic1) + (c.a3 * v3);
        m_ic1 = (2.0F * v1) - m_ic1;
        m_ic2 = (2.0F * v2) - m_ic2;
        flushDenormals();
        return SvfOutputs{.low = v2, .band = v1, .high = x - (c.k * v1) - v2};
    }

    /// One sample of `mode`. BandPass is the constant-peak-gain form (k times the raw
    /// band output), so its peak is unity at any Q.
    [[nodiscard]] float process(float x, const SvfCoefficients& c, SvfMode mode) noexcept {
        const SvfOutputs y = tick(x, c);
        switch (mode) {
        case SvfMode::LowPass:
            return y.low;
        case SvfMode::BandPass:
            return c.k * y.band;
        case SvfMode::HighPass:
            return y.high;
        case SvfMode::Notch:
            return x - (c.k * y.band);
        case SvfMode::Peak:
            return y.low - y.high;
        case SvfMode::AllPass:
            return x - (2.0F * c.k * y.band);
        }
        return y.low;
    }

private:
    void flushDenormals() noexcept {
        // A decaying state that drops into the denormal range costs a hundred times a
        // normal multiply on x86. The audio thread sets FTZ/DAZ (rt/Denormal.h); this
        // is the belt to that brace for offline renders on a thread that has not.
        constexpr float kTiny = 1e-30F;
        if (m_ic1 < kTiny && m_ic1 > -kTiny) {
            m_ic1 = 0.0F;
        }
        if (m_ic2 < kTiny && m_ic2 > -kTiny) {
            m_ic2 = 0.0F;
        }
    }

    float m_ic1{0.0F};
    float m_ic2{0.0F};
};

} // namespace adx::dsp
