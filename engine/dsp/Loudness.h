// Loudness per ITU-R BS.1770-4 / EBU R 128: momentary, short-term and gated integrated
// loudness of a stereo signal, in LUFS (phase_4.md §4.10).
//
//   K-weighting   the standard's two biquads - a high shelf (the head's acoustic
//                 effect) then the RLB high-pass - per channel, coefficients derived
//                 for the actual sample rate rather than the spec's 48 kHz table.
//   blocks        mean square summed over channels (both weighted 1.0), in 100 ms
//                 steps; momentary is the last 400 ms, short-term the last 3 s.
//   integrated    every 400 ms block (75 % overlap, so one per 100 ms) above the
//                 -70 LUFS absolute gate goes into a histogram of 0.01 LU bins that
//                 keeps each bin's energy; the relative gate (-10 LU below the
//                 ungated mean) is applied when the value is read. Bounded memory and
//                 bounded work, whatever the length of the programme - the audio
//                 thread computes it, so it may not grow with time.
//
// Verified against EBU TECH 3341's compliance signals 1-5 (meter_lufs_matches_reference).
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "engine/rt/OwnedArray.h"

namespace adx::dsp {

/// What silence reads as: below any gate, finite so it survives a float round trip.
inline constexpr float kSilentLufs = -200.0F;

struct KBiquad {
    double b0{1.0};
    double b1{0.0};
    double b2{0.0};
    double a1{0.0};
    double a2{0.0};
    double z1{0.0};
    double z2{0.0};

    [[nodiscard]] double process(double x) noexcept {
        const double y = (b0 * x) + z1;
        z1 = (b1 * x) - (a1 * y) + z2;
        z2 = (b2 * x) - (a2 * y);
        return y;
    }
};

class LoudnessMeter {
public:
    /// Main thread. Allocates the histogram.
    void prepare(std::uint32_t sampleRate);
    void reset() noexcept;

    /// Audio thread.
    void process(std::span<const float> left, std::span<const float> right) noexcept;

    [[nodiscard]] float momentary() const noexcept {
        return m_momentary;
    }
    [[nodiscard]] float shortTerm() const noexcept {
        return m_shortTerm;
    }
    /// The gated integrated loudness of everything since the last reset. Computed from
    /// the histogram on each call: O(bins).
    [[nodiscard]] float integrated() const noexcept;

    /// The K-weighting filter's coefficients at `sampleRate`, for its own test.
    static void kWeighting(std::uint32_t sampleRate, KBiquad& shelf, KBiquad& highPass) noexcept;

private:
    void finishStep() noexcept;

    static constexpr std::size_t kSteps = 30; // 3 s of 100 ms steps
    static constexpr float kHistogramFloor = -70.0F;
    static constexpr float kHistogramCeiling = 10.0F;
    static constexpr float kBinWidth = 0.01F;

    std::array<KBiquad, 2> m_shelf{};
    std::array<KBiquad, 2> m_highPass{};
    std::uint32_t m_stepLength{4800};
    std::uint32_t m_inStep{0};
    double m_stepEnergy{0.0};
    /// Mean square of each of the last kSteps 100 ms steps.
    std::array<double, kSteps> m_steps{};
    std::size_t m_stepCursor{0};
    std::size_t m_stepsSeen{0};
    float m_momentary{kSilentLufs};
    float m_shortTerm{kSilentLufs};
    /// Per bin: how many gated blocks, and their summed mean square.
    rt::OwnedArray<std::uint32_t> m_binCount;
    rt::OwnedArray<double> m_binEnergy;
};

} // namespace adx::dsp
