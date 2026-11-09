// The M-Vave FM-1's effects chain as one native effect (phase_11.5.md §4.7): filter
// (LP/HP/BP), distortion, chorus, phaser, delay, reverb (room/hall/plate), in the
// device's order. Its 24 parameters are the device's effect CCs, by the same names and
// with the same 0..127 range (fm1_v16.adxdevice), so "Match in adX" copies the FM-1's
// state into it value for value.
//
// Built from the existing native blocks - the state-variable filter, and the Distortion,
// Chorus, Phaser, Delay and Reverb effects as inner stages, each with its own mix - not
// new algorithms. M-VAVE's FX code is not public, so matching the device is a listening
// judgement (the final review), not a bit-exactness claim.
#pragma once

#include <array>
#include <cstdint>

#include "engine/dsp/SvFilter.h"
#include "engine/effects/Delay.h"
#include "engine/effects/Effect.h"
#include "engine/effects/Modulation.h"
#include "engine/effects/Ported.h"

namespace adx::effects {

enum class Fm1Param : std::uint32_t {
    FilterType,
    FilterCutoff,
    FilterReso,
    DistDrive,
    DistTone,
    DistMix,
    ChorusRate,
    ChorusDepth,
    ChorusMix,
    PhaserRate,
    PhaserDepth,
    PhaserFeedback,
    PhaserMix,
    DelayTime,
    DelayFeedback,
    DelayTone,
    DelaySync,
    DelayMix,
    ReverbType,
    ReverbSize,
    ReverbDamping,
    ReverbPredelay,
    ReverbWidth,
    ReverbMix,
    Count,
};

[[nodiscard]] constexpr project::ParamDescriptor fm1Param(std::string_view name,
                                                          float defaultValue) noexcept {
    return {name,
            0.0F,
            127.0F,
            defaultValue,
            project::Unit::Count,
            project::ScaleKind::Stepped,
            project::RateClass::Block,
            project::CurvePart::None};
}

/// The defaults are the chain's "nothing happens" setting: the filter open, every mix 0.
inline constexpr auto kFm1EffectsParams = std::to_array<project::ParamDescriptor>({
    fm1Param("filter_type", 0.0F),     fm1Param("filter_cutoff", 127.0F),
    fm1Param("filter_reso", 0.0F),     fm1Param("dist_drive", 32.0F),
    fm1Param("dist_tone", 127.0F),     fm1Param("dist_mix", 0.0F),
    fm1Param("chorus_rate", 32.0F),    fm1Param("chorus_depth", 64.0F),
    fm1Param("chorus_mix", 0.0F),      fm1Param("phaser_rate", 32.0F),
    fm1Param("phaser_depth", 64.0F),   fm1Param("phaser_feedback", 64.0F),
    fm1Param("phaser_mix", 0.0F),      fm1Param("delay_time", 48.0F),
    fm1Param("delay_feedback", 48.0F), fm1Param("delay_tone", 100.0F),
    fm1Param("delay_sync", 0.0F),      fm1Param("delay_mix", 0.0F),
    fm1Param("reverb_type", 0.0F),     fm1Param("reverb_size", 64.0F),
    fm1Param("reverb_damping", 64.0F), fm1Param("reverb_predelay", 0.0F),
    fm1Param("reverb_width", 127.0F),  fm1Param("reverb_mix", 0.0F),
});

class Fm1Effects final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Fm1Effects";
    }

protected:
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;
    [[nodiscard]] bool runsWhileBypassed() const noexcept override {
        return true;
    }

private:
    /// One inner effect's wet output over `left`/`right` in place, mixed by `mix`.
    template<class Stage, std::size_t N>
    void stage(Stage& effect, const std::array<float, N>& params, float mix, std::span<float> left,
               std::span<float> right, const EffectContext& context) noexcept;

    Distortion m_dist;
    Chorus m_chorus;
    Phaser m_phaser;
    Delay m_delay;
    Reverb m_reverb;
    std::array<dsp::SvFilter, 2> m_filter{};
    std::array<dsp::SvFilter, 2> m_distTone{};
    rt::OwnedArray<float> m_stageLeft;
    rt::OwnedArray<float> m_stageRight;
    rt::OwnedArray<float> m_predelay;
    std::size_t m_predelayIndex{0};
    double m_rate{48000.0};
};

} // namespace adx::effects
