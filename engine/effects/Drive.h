// Saturation and Overdrive: waveshapers run at four times the rate (dsp/Oversampler.h),
// so the harmonics they make fold back as little aliasing as a 4x stage allows. Both
// declare the oversampler's 40 samples of latency, and PDC compensates it.
//
//   Saturation  colour, not distortion: tube (an asymmetric tanh - even harmonics -
//               with the static offset removed and a DC blocker after), tape (an
//               algebraic sigmoid, softer at the knee than tanh) or transformer (a cubic
//               soft clip). Drive in, output level out.
//   Overdrive   the pedal (phase_4.md §4.9): input gain, a pre-emphasis high-pass
//               (`tightness`, which keeps the low end from flubbing the clipper), the
//               shaper - soft, tube or hard - then a low-pass `tone` and the level.
//               The two filters are linear, so they run at the base rate either side of
//               the oversampler; that is the same as running them inside it. Hard mode
//               clips through first-order antiderivative anti-aliasing (ADAA) on top of
//               the oversampling, because a bare clamp's 1/n harmonics outrun 4x. A mode
//               change crossfades the two shapers over 5 ms rather than switching.
#pragma once

#include <array>
#include <cstdint>

#include "engine/dsp/Oversampler.h"
#include "engine/dsp/SvFilter.h"
#include "engine/effects/Effect.h"
#include "engine/effects/Params.h"

namespace adx::effects {

enum class SaturationModel : std::uint8_t { Tube, Tape, Transformer };
enum class OverdriveMode : std::uint8_t { Soft, Tube, Hard };

enum class SaturationParam : std::uint32_t { Model, Drive, Output, Count };
inline constexpr auto kSaturationParams = std::to_array<project::ParamDescriptor>({
    choice("model", 2.0F, 0.0F),
    perFrame("drive", 0.0F, 36.0F, 6.0F, project::Unit::Decibels),
    perFrame("output", -24.0F, 12.0F, 0.0F, project::Unit::Decibels),
});

enum class OverdriveParam : std::uint32_t { Drive, Tone, Tightness, Mode, Level, Count };
inline constexpr auto kOverdriveParams = std::to_array<project::ParamDescriptor>({
    perFrame("drive", 0.0F, 48.0F, 18.0F, project::Unit::Decibels),
    param("tone", 500.0F, 12000.0F, 4000.0F, project::Unit::Hertz, project::ScaleKind::Logarithmic),
    param("tightness", 20.0F, 1000.0F, 120.0F, project::Unit::Hertz,
          project::ScaleKind::Logarithmic),
    choice("mode", 2.0F, 0.0F),
    perFrame("level", -36.0F, 12.0F, -12.0F, project::Unit::Decibels),
});

/// The shapers, exposed for tests.
[[nodiscard]] float saturate(SaturationModel model, float x) noexcept;
[[nodiscard]] float overdriveShape(OverdriveMode mode, float x) noexcept;

/// First-order ADAA hard clip at the oversampled rate: the difference quotient of the
/// clamp's antiderivative between successive inputs.
class AdaaClip {
public:
    void reset() noexcept {
        m_previous = 0.0F;
    }
    [[nodiscard]] float process(float x) noexcept;

private:
    float m_previous{0.0F};
};

class Saturation final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Saturation";
    }
    [[nodiscard]] MixLaw mixLaw() const noexcept override {
        return MixLaw::Linear;
    }

protected:
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;
    [[nodiscard]] std::uint32_t effectLatency() const noexcept override {
        return dsp::kOversampleLatency;
    }

private:
    std::array<dsp::Oversampler4x, 2> m_oversampler{};
    /// One-pole DC blocker state, per channel.
    std::array<float, 2> m_dcIn{};
    std::array<float, 2> m_dcOut{};
    float m_dcPole{0.9995F};
    SaturationModel m_model{SaturationModel::Tube};
};

class Overdrive final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Overdrive";
    }
    [[nodiscard]] MixLaw mixLaw() const noexcept override {
        return MixLaw::Linear;
    }

protected:
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;
    [[nodiscard]] std::uint32_t effectLatency() const noexcept override {
        return dsp::kOversampleLatency;
    }

private:
    std::array<dsp::Oversampler4x, 2> m_oversampler{};
    std::array<dsp::SvFilter, 2> m_tight{};
    std::array<dsp::SvFilter, 2> m_tone{};
    std::array<AdaaClip, 2> m_clip{};
    dsp::SvfCoefficients m_tightCoefficients{};
    dsp::SvfCoefficients m_toneCoefficients{};
    OverdriveMode m_mode{OverdriveMode::Soft};
    OverdriveMode m_from{OverdriveMode::Soft};
    /// 1 when settled on m_mode; climbs from 0 over 5 ms after a change.
    float m_blend{1.0F};
    float m_blendStep{1.0F};
};

} // namespace adx::effects
