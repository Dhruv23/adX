// Clipper (phase_12.md §4.6, closes part of G5): the hardstyle chain's EQ -> clip -> EQ ->
// clip stage. Five transfer curves - soft (tanh), hard, sine-fold, foldback and an
// asymmetric "tube" - after `drive`, scaled to `ceiling`.
//
// Always 16x oversampled (two 4x stages), so the latency PDC plans for is one number
// (Oversampler.h's 40 samples plus the inner stage's 40 at 4x: 50). The plan's 1-16x
// `oversample` choice would have made the latency depend on a parameter, which PDC
// cannot follow; phase_12.md §11 records the change.
#pragma once

#include <array>

#include "engine/dsp/Oversampler.h"
#include "engine/effects/Effect.h"
#include "engine/effects/Params.h"

namespace adx::effects {

enum class ClipMode : std::uint8_t { Soft, Hard, SineFold, Foldback, AsymTube };
enum class ClipperParam : std::uint32_t { Mode, Drive, Ceiling, DcBlock, Count };

inline constexpr auto kClipperParams = std::to_array<project::ParamDescriptor>({
    choice("mode", 4.0F, 1.0F),
    perFrame("drive", 0.0F, 48.0F, 6.0F, project::Unit::Decibels),
    perFrame("ceiling", -24.0F, 0.0F, 0.0F, project::Unit::Decibels),
    param("dcBlock", 0.0F, 1.0F, 1.0F, project::Unit::Boolean, project::ScaleKind::Stepped),
});

/// Each mode's static curve, ceiling 1: what clipper_transfer holds the effect to.
[[nodiscard]] float clipShape(ClipMode mode, float x) noexcept;

/// The clipper's latency: the outer 4x stage's plus the inner one's, at the base rate.
inline constexpr std::uint32_t kClipperLatency =
    dsp::kOversampleLatency + (dsp::kOversampleLatency / 4);

class Clipper final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Clipper";
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
        return kClipperLatency;
    }

private:
    /// Outer and inner 4x stages, per channel.
    std::array<dsp::Oversampler4x, 2> m_outer{};
    std::array<dsp::Oversampler4x, 2> m_inner{};
    std::array<float, 2> m_dcIn{};
    std::array<float, 2> m_dcOut{};
    float m_dcPole{0.9995F};
};

} // namespace adx::effects
