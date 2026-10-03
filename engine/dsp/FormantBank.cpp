#include "engine/dsp/FormantBank.h"

#include <algorithm>

#include "engine/dsp/Math.h"

namespace adx::dsp {
namespace {

// Iteration one's three formants (AudioEngine.cpp GetVowelFormants), reindexed a e i o
// u: Ah, Eh, Ee, Oh, Oo. Bandwidth carries the fixed Q of 8 as f / 8.
constexpr float kClassicQ = 8.0F;

constexpr VowelSpec classic(float f1, float f2, float f3) {
    return VowelSpec{.bands = {Formant{.frequency = f1, .bandwidth = f1 / kClassicQ, .gain = 1.0F},
                               Formant{.frequency = f2, .bandwidth = f2 / kClassicQ, .gain = 0.6F},
                               Formant{.frequency = f3, .bandwidth = f3 / kClassicQ, .gain = 0.35F},
                               Formant{}, Formant{}},
                     .count = 3};
}

constexpr std::array<VowelSpec, kVowelCount> kClassic{
    classic(700.0F, 1220.0F, 2600.0F), // a (Ah)
    classic(530.0F, 1840.0F, 2480.0F), // e (Eh)
    classic(270.0F, 2290.0F, 3010.0F), // i (Ee)
    classic(570.0F, 840.0F, 2410.0F),  // o (Oh)
    classic(300.0F, 870.0F, 2240.0F),  // u (Oo)
};

constexpr Formant band(float frequency, float bandwidth, float gainDb) {
    // dB to linear without a library call: these are constants, and the table is
    // compile-time. 10^(dB/20) for the handful of levels used.
    float gain = 1.0F;
    if (gainDb <= -30.0F) {
        gain = 0.0316F;
    } else if (gainDb <= -26.0F) {
        gain = 0.0501F;
    } else if (gainDb <= -22.0F) {
        gain = 0.0794F;
    } else if (gainDb <= -20.0F) {
        gain = 0.1F;
    } else if (gainDb <= -18.0F) {
        gain = 0.1259F;
    } else if (gainDb <= -17.0F) {
        gain = 0.1413F;
    } else if (gainDb <= -15.0F) {
        gain = 0.1778F;
    } else if (gainDb <= -14.0F) {
        gain = 0.1995F;
    } else if (gainDb <= -12.0F) {
        gain = 0.2512F;
    } else if (gainDb <= -10.0F) {
        gain = 0.3162F;
    } else if (gainDb <= -8.0F) {
        gain = 0.3981F;
    } else if (gainDb <= -7.0F) {
        gain = 0.4467F;
    } else if (gainDb <= -6.0F) {
        gain = 0.5012F;
    }
    return Formant{.frequency = frequency, .bandwidth = bandwidth, .gain = gain};
}

// The tenor formant table (Csound manual, Appendix "Formant values").
constexpr std::array<VowelSpec, kVowelCount> kTenor{
    VowelSpec{.bands = {band(650, 80, 0), band(1080, 90, -6), band(2650, 120, -7),
                        band(2900, 130, -8), band(3250, 140, -22)},
              .count = 5},
    VowelSpec{.bands = {band(400, 70, 0), band(1700, 80, -14), band(2600, 100, -12),
                        band(3200, 120, -14), band(3580, 120, -20)},
              .count = 5},
    VowelSpec{.bands = {band(290, 40, 0), band(1870, 90, -15), band(2800, 100, -18),
                        band(3250, 120, -20), band(3540, 120, -30)},
              .count = 5},
    VowelSpec{.bands = {band(400, 40, 0), band(800, 80, -10), band(2600, 100, -12),
                        band(2800, 120, -12), band(3000, 120, -26)},
              .count = 5},
    VowelSpec{.bands = {band(350, 40, 0), band(600, 60, -20), band(2700, 100, -17),
                        band(2900, 120, -14), band(3300, 120, -26)},
              .count = 5},
};

} // namespace

const VowelSpec& vowelSpec(VowelSet set, std::size_t vowel) noexcept {
    const std::size_t index = std::min(vowel, kVowelCount - 1);
    return set == VowelSet::Classic3 ? kClassic[index] : kTenor[index];
}

void FormantBank::configure(VowelSet set, std::size_t vowelA, std::size_t vowelB, float morph,
                            double sampleRate, float shiftRatio) noexcept {
    const VowelSpec& a = vowelSpec(set, vowelA);
    const VowelSpec& b = vowelSpec(set, vowelB);
    const float t = std::clamp(morph, 0.0F, 1.0F);
    m_count = a.count;
    m_raw = set == VowelSet::Classic3;
    for (std::size_t i = 0; i < m_count; ++i) {
        const float frequency =
            (a.bands[i].frequency + ((b.bands[i].frequency - a.bands[i].frequency) * t)) *
            shiftRatio;
        const float bandwidth =
            a.bands[i].bandwidth + ((b.bands[i].bandwidth - a.bands[i].bandwidth) * t);
        m_gains[i] = a.bands[i].gain + ((b.bands[i].gain - a.bands[i].gain) * t);
        // Classic3 keeps iteration one's fixed Q whatever the frequency; Tenor5's Q is
        // the band's centre over its bandwidth.
        const double q = m_raw ? static_cast<double>(kClassicQ)
                               : static_cast<double>(frequency) / std::max(1.0F, bandwidth);
        m_coefficients[i] = svfCoefficients(
            std::min(static_cast<double>(frequency), sampleRate * 0.49), q, sampleRate);
    }
}

} // namespace adx::dsp
