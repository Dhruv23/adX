// PitchCorrect (phase_12.md §4.6, closes part of G5): hard-tune first.
//
// Detection is YIN (de Cheveigné & Kawahara 2002) on the mono sum, every 256 frames over
// a 1024-frame window, 70 Hz to 1 kHz: the difference function, its cumulative-mean
// normalisation, the first dip under 0.15, refined by a parabola. An unvoiced window
// keeps the last correction rather than chasing noise.
//
// The detected note is snapped to the nearest degree of `scale` in `key`; the
// correction - the ratio from where the voice is to where it should be - moves there in
// `retune` ms (0 is the hyperpop hard snap) and is applied by a period-synchronous
// delay-line shifter: two read heads half a grain apart, raised-cosine windowed, the
// grain two periods of the detected pitch. Two periods apart the heads read in phase on
// a periodic voice, which is what keeps the formants and avoids the comb a fixed grain
// would leave - the time-domain half of PSOLA, without its epoch marking.
//
//   humanize     cents: a note already this close to the scale is left alone
//   vibratoPass  0..1: the correction follows a smoothed pitch, so vibrato rides on top
//   formant      1 the period-synchronous grain, 0 a fixed 40 ms one (the robot)
//
// Natural, slow correction - a research project of its own (§9) - is not attempted;
// retune up to 400 ms is a slower slide to the same target.
#pragma once

#include <array>

#include "engine/effects/Effect.h"
#include "engine/effects/Params.h"
#include "engine/rt/OwnedArray.h"

namespace adx::effects {

enum class PitchCorrectParam : std::uint32_t {
    Key,
    Scale,
    Retune,
    Humanize,
    VibratoPass,
    Formant,
    Count,
};

inline constexpr std::uint8_t kPitchCorrectScales = 8;

inline constexpr auto kPitchCorrectParams = std::to_array<project::ParamDescriptor>({
    choice("key", 11.0F, 0.0F),
    // chromatic, major, minor, harmonic minor, major pentatonic, minor pentatonic,
    // blues, dorian.
    choice("scale", kPitchCorrectScales - 1, 0.0F),
    param("retune", 0.0F, 400.0F, 0.0F, project::Unit::Milliseconds),
    param("humanize", 0.0F, 50.0F, 0.0F, project::Unit::Cents),
    param("vibratoPass", 0.0F, 1.0F, 0.0F, project::Unit::Normalized),
    param("formant", 0.0F, 1.0F, 1.0F, project::Unit::Boolean, project::ScaleKind::Stepped),
});

/// The nearest note to `midi` (fractional) that `scale` in `key` allows.
[[nodiscard]] float snapToScale(float midi, int key, int scale) noexcept;

/// YIN's pitch of `window` (oldest first) at `rate`, Hz; 0 when it is not voiced.
[[nodiscard]] float yinPitch(std::span<const float> window, double rate,
                             std::span<float> scratch) noexcept;

inline constexpr std::size_t kYinWindow = 1024;
inline constexpr std::size_t kYinHop = 256;

class PitchCorrect final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "PitchCorrect";
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
    [[nodiscard]] bool runsWhileBypassed() const noexcept override {
        return true;
    }

private:
    void analyse(const EffectContext& context, std::uint32_t frame) noexcept;

    double m_rate{48000.0};
    /// The mono analysis ring, and YIN's working buffers.
    rt::OwnedArray<float> m_analysis;
    rt::OwnedArray<float> m_window;
    rt::OwnedArray<float> m_scratch;
    std::size_t m_analysisWrite{0};
    std::size_t m_sinceHop{0};
    /// The shifter's rings, per channel.
    std::array<rt::OwnedArray<float>, 2> m_ring;
    std::size_t m_ringMask{0};
    std::size_t m_write{0};
    double m_phase{0.0};
    /// The grain, frames: two detected periods.
    double m_grain{1920.0};
    /// The pitch YIN found, smoothed for vibratoPass, in MIDI notes.
    float m_smoothedNote{0.0F};
    bool m_haveNote{false};
    /// The correction, as log2 of the ratio: where it is, and where it is going.
    float m_logRatio{0.0F};
    float m_targetLogRatio{0.0F};
};

} // namespace adx::effects
