// A parallel bank of band-pass filters tuned to a vowel, morphing between two.
//
// Two vowel tables:
//
//   Classic3  iteration one's formant stage, verbatim (AudioEngine.cpp's
//             GetVowelFormants): three bands at the textbook F1-F3 averages, a fixed
//             Q of 8, the raw band output weighted 1, 0.6, 0.35. The Additive port uses
//             it so that suffocation.adx's vocal sounds like it did.
//   Tenor5    five bands with per-band bandwidth and level - the usual tenor table -
//             for the FormantFilter effect, where intelligibility matters more than
//             continuity with iteration one.
//
// Vowels are indexed a, e, i, o, u (0..4), the v1 shim's FORMANT mapping. Morph
// interpolates centre frequency (and, for Tenor5, bandwidth and level) linearly.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/dsp/SvFilter.h"

namespace adx::dsp {

enum class VowelSet : std::uint8_t { Classic3, Tenor5 };

inline constexpr std::size_t kMaxFormants = 5;
inline constexpr std::size_t kVowelCount = 5;

struct Formant {
    float frequency{0.0F};
    float bandwidth{0.0F};
    /// Linear weight of the band in the sum.
    float gain{0.0F};
};

struct VowelSpec {
    std::array<Formant, kMaxFormants> bands{};
    std::size_t count{0};
};

[[nodiscard]] const VowelSpec& vowelSpec(VowelSet set, std::size_t vowel) noexcept;

class FormantBank {
public:
    void reset() noexcept {
        for (SvFilter& filter : m_filters) {
            filter.reset();
        }
    }

    /// Control rate. `vowelA`, `vowelB` index a e i o u; `morph` in [0, 1].
    void configure(VowelSet set, std::size_t vowelA, std::size_t vowelB, float morph,
                   double sampleRate, float shiftRatio = 1.0F) noexcept;

    [[nodiscard]] float process(float x) noexcept {
        float sum = 0.0F;
        for (std::size_t b = 0; b < m_count; ++b) {
            const SvfOutputs y = m_filters[b].tick(x, m_coefficients[b]);
            // Classic3 sums the raw band output, whose peak gain is Q, as iteration one
            // did; Tenor5 the unity-peak form.
            const float band = m_raw ? y.band : m_coefficients[b].k * y.band;
            sum += band * m_gains[b];
        }
        return sum;
    }

private:
    std::array<SvFilter, kMaxFormants> m_filters{};
    std::array<SvfCoefficients, kMaxFormants> m_coefficients{};
    std::array<float, kMaxFormants> m_gains{};
    std::size_t m_count{0};
    bool m_raw{false};
};

} // namespace adx::dsp
