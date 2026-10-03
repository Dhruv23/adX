// An 8-band fully parametric EQ, with a spectrum tap for the UI.
//
// Each band is off, a peak, a shelf, a cut (12 dB/oct high- or low-pass), a notch or
// a band-pass, from RbjFilter.h. Coefficients are recomputed on the control grid, and
// only when a band's parameters actually moved, so a static EQ costs eight biquads per
// sample and nothing else.
//
// The spectrum tap (phase_4.md §9: "the spectrum tap on ParametricEq, ready for the
// Phase 6 EQ display") is the post-EQ signal, mono, written in 64-sample frames into an
// OverwriteRing the UI reads at its own rate - never waited on.
#pragma once

#include <array>
#include <cstdint>

#include "engine/dsp/RbjFilter.h"
#include "engine/effects/Effect.h"
#include "engine/rt/OverwriteRing.h"

namespace adx::effects {

inline constexpr std::uint32_t kEqBands = 8;
inline constexpr std::uint32_t kEqBandParams = 4;

/// band.type values.
enum class EqBandType : std::uint8_t {
    Off,
    Peak,
    LowShelf,
    HighShelf,
    LowCut,
    HighCut,
    Notch,
    BandPass
};

/// Parameter index of band `band` (0-based), field `field` (0 type, 1 freq, 2 gain, 3 q).
[[nodiscard]] constexpr std::uint32_t eqParam(std::uint32_t band, std::uint32_t field) noexcept {
    return (band * kEqBandParams) + field;
}
inline constexpr std::uint32_t kEqOutputParam = kEqBands * kEqBandParams;

// NOLINTBEGIN(modernize-use-designated-initializers)
inline constexpr auto kParametricEqParams = std::to_array<project::ParamDescriptor>({
    {"band1.type", 0.0F, 7.0F, 0.0F, project::Unit::Count, project::ScaleKind::Stepped,
     project::RateClass::Block, project::CurvePart::None},
    {"band1.freq", 20.0F, 20000.0F, 60.0F, project::Unit::Hertz, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"band1.gain", -24.0F, 24.0F, 0.0F, project::Unit::Decibels, project::ScaleKind::Linear,
     project::RateClass::Block, project::CurvePart::None},
    {"band1.q", 0.1F, 30.0F, 0.707F, project::Unit::Ratio, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"band2.type", 0.0F, 7.0F, 0.0F, project::Unit::Count, project::ScaleKind::Stepped,
     project::RateClass::Block, project::CurvePart::None},
    {"band2.freq", 20.0F, 20000.0F, 150.0F, project::Unit::Hertz, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"band2.gain", -24.0F, 24.0F, 0.0F, project::Unit::Decibels, project::ScaleKind::Linear,
     project::RateClass::Block, project::CurvePart::None},
    {"band2.q", 0.1F, 30.0F, 0.707F, project::Unit::Ratio, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"band3.type", 0.0F, 7.0F, 0.0F, project::Unit::Count, project::ScaleKind::Stepped,
     project::RateClass::Block, project::CurvePart::None},
    {"band3.freq", 20.0F, 20000.0F, 400.0F, project::Unit::Hertz, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"band3.gain", -24.0F, 24.0F, 0.0F, project::Unit::Decibels, project::ScaleKind::Linear,
     project::RateClass::Block, project::CurvePart::None},
    {"band3.q", 0.1F, 30.0F, 0.707F, project::Unit::Ratio, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"band4.type", 0.0F, 7.0F, 0.0F, project::Unit::Count, project::ScaleKind::Stepped,
     project::RateClass::Block, project::CurvePart::None},
    {"band4.freq", 20.0F, 20000.0F, 1000.0F, project::Unit::Hertz, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"band4.gain", -24.0F, 24.0F, 0.0F, project::Unit::Decibels, project::ScaleKind::Linear,
     project::RateClass::Block, project::CurvePart::None},
    {"band4.q", 0.1F, 30.0F, 0.707F, project::Unit::Ratio, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"band5.type", 0.0F, 7.0F, 0.0F, project::Unit::Count, project::ScaleKind::Stepped,
     project::RateClass::Block, project::CurvePart::None},
    {"band5.freq", 20.0F, 20000.0F, 2500.0F, project::Unit::Hertz, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"band5.gain", -24.0F, 24.0F, 0.0F, project::Unit::Decibels, project::ScaleKind::Linear,
     project::RateClass::Block, project::CurvePart::None},
    {"band5.q", 0.1F, 30.0F, 0.707F, project::Unit::Ratio, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"band6.type", 0.0F, 7.0F, 0.0F, project::Unit::Count, project::ScaleKind::Stepped,
     project::RateClass::Block, project::CurvePart::None},
    {"band6.freq", 20.0F, 20000.0F, 5000.0F, project::Unit::Hertz, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"band6.gain", -24.0F, 24.0F, 0.0F, project::Unit::Decibels, project::ScaleKind::Linear,
     project::RateClass::Block, project::CurvePart::None},
    {"band6.q", 0.1F, 30.0F, 0.707F, project::Unit::Ratio, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"band7.type", 0.0F, 7.0F, 0.0F, project::Unit::Count, project::ScaleKind::Stepped,
     project::RateClass::Block, project::CurvePart::None},
    {"band7.freq", 20.0F, 20000.0F, 10000.0F, project::Unit::Hertz, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"band7.gain", -24.0F, 24.0F, 0.0F, project::Unit::Decibels, project::ScaleKind::Linear,
     project::RateClass::Block, project::CurvePart::None},
    {"band7.q", 0.1F, 30.0F, 0.707F, project::Unit::Ratio, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"band8.type", 0.0F, 7.0F, 0.0F, project::Unit::Count, project::ScaleKind::Stepped,
     project::RateClass::Block, project::CurvePart::None},
    {"band8.freq", 20.0F, 20000.0F, 16000.0F, project::Unit::Hertz, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"band8.gain", -24.0F, 24.0F, 0.0F, project::Unit::Decibels, project::ScaleKind::Linear,
     project::RateClass::Block, project::CurvePart::None},
    {"band8.q", 0.1F, 30.0F, 0.707F, project::Unit::Ratio, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"output", -24.0F, 24.0F, 0.0F, project::Unit::Decibels, project::ScaleKind::Linear,
     project::RateClass::Sample, project::CurvePart::None},
});
// NOLINTEND(modernize-use-designated-initializers)

/// One frame of the spectrum tap.
using SpectrumFrame = std::array<float, 64>;
using SpectrumTap = rt::OverwriteRing<SpectrumFrame, 64>;

class ParametricEq final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "ParametricEq";
    }

    /// The UI's view of the output: the last 4096 samples, in 64-sample frames.
    [[nodiscard]] const SpectrumTap& spectrumTap() const noexcept {
        return m_tap;
    }

protected:
    void prepareEffect(const graph::PrepareInfo& /*info*/) override {}
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;

private:
    struct Band {
        dsp::Biquad left;
        dsp::Biquad right;
        dsp::BiquadCoefficients coefficients;
        std::array<float, kEqBandParams> last{-1.0F, -1.0F, -1.0F, -1.0F};
        bool active{false};
    };
    std::array<Band, kEqBands> m_bands{};
    SpectrumTap m_tap;
    SpectrumFrame m_frame{};
    std::size_t m_frameFill{0};
};

} // namespace adx::effects
