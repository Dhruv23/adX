// Filter and Resonator (phase_12.md §4.6, closes part of G5).
//
// Filter: a standalone resonant filter for sweeps and filtered chords - the ladder, the
// SVF's low/band/high/notch, a positive and a negative comb, and a two-peak formant -
// whose cutoff an LFO (free or tempo-synced) and an envelope follower move, in octaves.
// Modulation is read on the control grid, as every block-rate parameter is, so the
// output does not depend on the block size.
//
// Resonator: up to four tuned feedback combs, each a note from C4, damped in the loop:
// metallic tones, comb formants for screeches, tuned noise.
#pragma once

#include <array>

#include "engine/dsp/LadderFilter.h"
#include "engine/dsp/SvFilter.h"
#include "engine/effects/Effect.h"
#include "engine/effects/Params.h"
#include "engine/rt/OwnedArray.h"

namespace adx::effects {

enum class FilterType : std::uint8_t {
    Ladder,
    LowPass,
    BandPass,
    HighPass,
    Notch,
    CombPlus,
    CombMinus,
    Formant
};

enum class FilterParam : std::uint32_t {
    Type,
    Cutoff,
    Resonance,
    Drive,
    LfoRate,
    LfoSync,
    LfoShape,
    LfoDepth,
    FollowAmount,
    FollowAttack,
    FollowRelease,
    KeyInput,
    Count
};

inline constexpr auto kFilterParams = std::to_array<project::ParamDescriptor>({
    choice("type", 7.0F, 1.0F),
    param("cutoff", 20.0F, 20000.0F, 1000.0F, project::Unit::Hertz,
          project::ScaleKind::Logarithmic),
    param("resonance", 0.0F, 1.0F, 0.2F, project::Unit::Normalized),
    perFrame("drive", 0.0F, 24.0F, 0.0F, project::Unit::Decibels),
    param("lfo.rate", 0.01F, 20.0F, 1.0F, project::Unit::Hertz, project::ScaleKind::Logarithmic),
    // 0 free; 1.. the Delay's divisions (1/32 .. 1 bar), one cycle each.
    choice("lfo.sync", 9.0F, 0.0F),
    // sine, triangle, saw down, square
    choice("lfo.shape", 3.0F, 0.0F),
    param("lfo.depth", 0.0F, 4.0F, 0.0F, project::Unit::Normalized),
    param("follow.amount", -4.0F, 4.0F, 0.0F, project::Unit::Normalized),
    param("follow.attack", 0.1F, 200.0F, 5.0F, project::Unit::Milliseconds,
          project::ScaleKind::Logarithmic),
    param("follow.release", 1.0F, 2000.0F, 150.0F, project::Unit::Milliseconds,
          project::ScaleKind::Logarithmic),
    // 1: the follower hears the sidechain, not the input.
    param("keyInput", 0.0F, 1.0F, 0.0F, project::Unit::Boolean, project::ScaleKind::Stepped),
});

class Filter final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Filter";
    }

protected:
    [[nodiscard]] bool runsWhileBypassed() const noexcept override {
        return true;
    }
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;
    [[nodiscard]] bool usesSidechain() const noexcept override {
        return true;
    }

private:
    [[nodiscard]] static float lfo(float phase, int shape) noexcept;
    void retune(const EffectContext& context, float octaves) noexcept;

    std::array<dsp::LadderFilter, 2> m_ladder{};
    std::array<dsp::SvFilter, 2> m_svf{};
    std::array<dsp::SvFilter, 2> m_formant{};
    dsp::LadderCoefficients m_ladderCoefficients;
    dsp::SvfCoefficients m_svfCoefficients;
    dsp::SvfCoefficients m_formantCoefficients;
    std::array<rt::OwnedArray<float>, 2> m_comb;
    std::size_t m_combIndex{0};
    float m_combDelay{48.0F};
    double m_lfoPhase{0.0};
    float m_follow{0.0F};
    bool m_tuned{false};
    double m_rate{48000.0};
};

enum class ResonatorParam : std::uint32_t {
    Voices,
    Note1,
    Note2,
    Note3,
    Note4,
    Feedback,
    Damping,
    Count
};

inline constexpr auto kResonatorParams = std::to_array<project::ParamDescriptor>({
    param("voices", 1.0F, 4.0F, 1.0F, project::Unit::Count, project::ScaleKind::Stepped),
    param("note1", -24.0F, 24.0F, 0.0F, project::Unit::Semitones),
    param("note2", -24.0F, 24.0F, 7.0F, project::Unit::Semitones),
    param("note3", -24.0F, 24.0F, 12.0F, project::Unit::Semitones),
    param("note4", -24.0F, 24.0F, 16.0F, project::Unit::Semitones),
    param("feedback", 0.0F, 0.995F, 0.9F, project::Unit::Normalized),
    param("damping", 0.0F, 1.0F, 0.3F, project::Unit::Normalized),
});

class Resonator final : public Effect {
public:
    static constexpr std::size_t kVoices = 4;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Resonator";
    }

protected:
    [[nodiscard]] bool runsWhileBypassed() const noexcept override {
        return true;
    }
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;

private:
    /// One comb per voice per channel, each with its loop's low-pass state.
    std::array<std::array<rt::OwnedArray<float>, kVoices>, 2> m_lines;
    std::array<std::array<float, kVoices>, 2> m_damp{};
    std::size_t m_index{0};
    double m_rate{48000.0};
};

} // namespace adx::effects
