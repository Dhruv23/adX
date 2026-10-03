// Biquads from Robert Bristow-Johnson's Audio EQ Cookbook.
//
// The EQs' filter: shelves and peaks, plus the plain responses and an allpass. The
// archived EQ's three bands (_archive/src-cpp/src/AudioEffects.cpp) are this with a
// fixed shelf slope of 1; Eq3 ports them through here unchanged. Transposed direct
// form II, the form with the least coefficient sensitivity in float.
#pragma once

#include <cstdint>

namespace adx::dsp {

enum class BiquadKind : std::uint8_t {
    LowPass,
    HighPass,
    BandPass,
    Notch,
    AllPass,
    Peaking,
    LowShelf,
    HighShelf,
};

struct BiquadCoefficients {
    float b0{1.0F};
    float b1{0.0F};
    float b2{0.0F};
    float a1{0.0F};
    float a2{0.0F};
};

/// `q` is the band's Q for the pass, stop, all-pass and peaking kinds; for the shelves
/// it is the shelf slope S (1 is the steepest without overshoot). `gainDb` is used by
/// Peaking and the shelves. `frequency` is clamped below Nyquist.
[[nodiscard]] BiquadCoefficients biquadCoefficients(BiquadKind kind, double frequency, double q,
                                                    double gainDb, double sampleRate) noexcept;

/// |H(e^jw)| of a coefficient set at `frequency`, in dB. What the response tests and
/// the EQ display read.
[[nodiscard]] double biquadMagnitudeDb(const BiquadCoefficients& c, double frequency,
                                       double sampleRate) noexcept;

class Biquad {
public:
    void reset() noexcept {
        m_z1 = 0.0F;
        m_z2 = 0.0F;
    }

    [[nodiscard]] float process(float x, const BiquadCoefficients& c) noexcept {
        const float y = (c.b0 * x) + m_z1;
        m_z1 = (c.b1 * x) - (c.a1 * y) + m_z2;
        m_z2 = (c.b2 * x) - (c.a2 * y);
        constexpr float kTiny = 1e-30F;
        if (m_z1 < kTiny && m_z1 > -kTiny) {
            m_z1 = 0.0F;
        }
        if (m_z2 < kTiny && m_z2 > -kTiny) {
            m_z2 = 0.0F;
        }
        return y;
    }

private:
    float m_z1{0.0F};
    float m_z2{0.0F};
};

} // namespace adx::dsp
