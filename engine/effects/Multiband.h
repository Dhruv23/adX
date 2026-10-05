// MultibandComp and TransientShaper (FINAL_PLAN §5.4, phase_4.md Tranche C).
//
// MultibandComp splits the signal into 3-6 bands with Linkwitz-Riley crossovers (two
// cascaded Butterworth sections, so each low/high pair sums to an allpass) and
// compresses each band on its own, stereo-linked. Splitting a band off the top of the
// remaining signal leaves the lower bands without the later crossovers' phase, so each
// lower band also runs through those crossovers' allpasses: with every band at ratio 1
// and 0 dB the output's magnitude is flat (multiband_flat_when_neutral). No lookahead:
// latency 0.
//
// TransientShaper follows the level twice, fast and slow. Where the fast envelope is
// above the slow one the signal is attacking, where it is below it is sustaining; the
// difference in dB, scaled by `attack` or `sustain`, is the gain - up to +-12 dB.
#pragma once

#include <array>
#include <cstdint>

#include "engine/dsp/RbjFilter.h"
#include "engine/effects/Effect.h"
#include "engine/effects/Params.h"

namespace adx::effects {

inline constexpr std::size_t kMaxBands = 6;

enum class MultibandParam : std::uint32_t {
    Bands,
    Crossover1,
    Attack = Crossover1 + kMaxBands - 1,
    Release,
    /// Then, per band b (0-based): threshold, ratio, gain at Band0 + 3 b.
    Band0,
    Count = Band0 + (3 * kMaxBands),
};

[[nodiscard]] constexpr std::uint32_t bandParam(std::size_t band, std::uint32_t field) noexcept {
    return static_cast<std::uint32_t>(MultibandParam::Band0) +
           (3 * static_cast<std::uint32_t>(band)) + field;
}

inline constexpr auto kMultibandParams = std::to_array<project::ParamDescriptor>({
    param("bands", 3.0F, 6.0F, 4.0F, project::Unit::Count, project::ScaleKind::Stepped),
    param("crossover1", 20.0F, 20000.0F, 120.0F, project::Unit::Hertz,
          project::ScaleKind::Logarithmic),
    param("crossover2", 20.0F, 20000.0F, 600.0F, project::Unit::Hertz,
          project::ScaleKind::Logarithmic),
    param("crossover3", 20.0F, 20000.0F, 2500.0F, project::Unit::Hertz,
          project::ScaleKind::Logarithmic),
    param("crossover4", 20.0F, 20000.0F, 6000.0F, project::Unit::Hertz,
          project::ScaleKind::Logarithmic),
    param("crossover5", 20.0F, 20000.0F, 12000.0F, project::Unit::Hertz,
          project::ScaleKind::Logarithmic),
    param("attack", 0.1F, 200.0F, 10.0F, project::Unit::Milliseconds,
          project::ScaleKind::Logarithmic),
    param("release", 5.0F, 2000.0F, 150.0F, project::Unit::Milliseconds,
          project::ScaleKind::Logarithmic),
    perFrame("band1.threshold", -60.0F, 0.0F, -18.0F, project::Unit::Decibels),
    param("band1.ratio", 1.0F, 20.0F, 2.0F, project::Unit::Ratio, project::ScaleKind::Logarithmic),
    perFrame("band1.gain", -24.0F, 24.0F, 0.0F, project::Unit::Decibels),
    perFrame("band2.threshold", -60.0F, 0.0F, -18.0F, project::Unit::Decibels),
    param("band2.ratio", 1.0F, 20.0F, 2.0F, project::Unit::Ratio, project::ScaleKind::Logarithmic),
    perFrame("band2.gain", -24.0F, 24.0F, 0.0F, project::Unit::Decibels),
    perFrame("band3.threshold", -60.0F, 0.0F, -18.0F, project::Unit::Decibels),
    param("band3.ratio", 1.0F, 20.0F, 2.0F, project::Unit::Ratio, project::ScaleKind::Logarithmic),
    perFrame("band3.gain", -24.0F, 24.0F, 0.0F, project::Unit::Decibels),
    perFrame("band4.threshold", -60.0F, 0.0F, -18.0F, project::Unit::Decibels),
    param("band4.ratio", 1.0F, 20.0F, 2.0F, project::Unit::Ratio, project::ScaleKind::Logarithmic),
    perFrame("band4.gain", -24.0F, 24.0F, 0.0F, project::Unit::Decibels),
    perFrame("band5.threshold", -60.0F, 0.0F, -18.0F, project::Unit::Decibels),
    param("band5.ratio", 1.0F, 20.0F, 2.0F, project::Unit::Ratio, project::ScaleKind::Logarithmic),
    perFrame("band5.gain", -24.0F, 24.0F, 0.0F, project::Unit::Decibels),
    perFrame("band6.threshold", -60.0F, 0.0F, -18.0F, project::Unit::Decibels),
    param("band6.ratio", 1.0F, 20.0F, 2.0F, project::Unit::Ratio, project::ScaleKind::Logarithmic),
    perFrame("band6.gain", -24.0F, 24.0F, 0.0F, project::Unit::Decibels),
});
static_assert(kMultibandParams.size() == static_cast<std::size_t>(MultibandParam::Count));

enum class TransientParam : std::uint32_t { Attack, Sustain, Output, Count };
inline constexpr auto kTransientParams = std::to_array<project::ParamDescriptor>({
    perFrame("attack", -1.0F, 1.0F, 0.5F, project::Unit::Normalized),
    perFrame("sustain", -1.0F, 1.0F, 0.0F, project::Unit::Normalized),
    perFrame("output", -24.0F, 12.0F, 0.0F, project::Unit::Decibels),
});

/// A Linkwitz-Riley 4th-order low/high pair at one frequency, one channel.
struct Crossover {
    std::array<dsp::Biquad, 2> low{};
    std::array<dsp::Biquad, 2> high{};
};

class MultibandComp final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "MultibandComp";
    }
    [[nodiscard]] MixLaw mixLaw() const noexcept override {
        return MixLaw::Linear;
    }

protected:
    void prepareEffect(const graph::PrepareInfo& /*info*/) override {}
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;

private:
    void refresh(const EffectContext& context, std::uint32_t frame) noexcept;

    static constexpr std::size_t kCrossovers = kMaxBands - 1;
    /// Per channel: the crossovers, and every band's compensating allpasses.
    std::array<std::array<Crossover, kCrossovers>, 2> m_split{};
    std::array<std::array<std::array<dsp::Biquad, kCrossovers>, kMaxBands>, 2> m_allpass{};
    std::array<dsp::BiquadCoefficients, kCrossovers> m_lowCoefficients{};
    std::array<dsp::BiquadCoefficients, kCrossovers> m_highCoefficients{};
    std::array<dsp::BiquadCoefficients, kCrossovers> m_allpassCoefficients{};
    /// Each band's smoothed gain reduction, dB (<= 0).
    std::array<float, kMaxBands> m_reduction{};
    std::size_t m_bands{4};
    float m_attack{0.0F};
    float m_release{0.0F};
    std::array<float, kMaxBands> m_ratio{};
};

class TransientShaper final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "TransientShaper";
    }
    [[nodiscard]] MixLaw mixLaw() const noexcept override {
        return MixLaw::Linear;
    }

protected:
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override {
        m_fast = 0.0F;
        m_slow = 0.0F;
    }
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;

private:
    float m_fast{0.0F};
    float m_slow{0.0F};
    // One-pole coefficients: attack and release of each follower.
    float m_fastAttack{0.0F};
    float m_fastRelease{0.0F};
    float m_slowAttack{0.0F};
    float m_slowRelease{0.0F};
};

} // namespace adx::effects
