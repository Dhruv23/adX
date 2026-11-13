// The Wavetable instrument's parameters, in the order its node reads them
// (phase_4.md §4.8).
#pragma once

#include <array>
#include <cstdint>

#include "engine/instruments/mod/ModLayer.h"
#include "engine/project/ParamDescriptor.h"

namespace adx::instruments {

enum class WavetableParam : std::uint32_t {
    Table,
    Position,
    Morph,
    UnisonVoices,
    UnisonDetune,
    UnisonSpread,
    FilterCutoff,
    FilterResonance,
    EnvAttack,
    EnvDecay,
    EnvSustain,
    EnvRelease,
    LfoRate,
    LfoDepth,
    Glide,
    Level,
    Count,
};

/// The built-in tables, then the channel's own (its first zone's file).
enum class WavetableBank : std::uint8_t { Classic, Pulse, Harmonic, Formant, User };
inline constexpr std::uint8_t kWavetableBankCount = 5;
inline constexpr std::uint32_t kWavetableMaxUnison = 8;

namespace detail {
constexpr project::ParamDescriptor wt(std::string_view name, float lo, float hi, float def,
                                      project::Unit unit,
                                      project::ScaleKind scale = project::ScaleKind::Linear,
                                      project::RateClass rate = project::RateClass::Block) {
    return project::ParamDescriptor{.name = name,
                                    .minimum = lo,
                                    .maximum = hi,
                                    .defaultValue = def,
                                    .unit = unit,
                                    .scale = scale,
                                    .rate = rate,
                                    .curve = project::CurvePart::None};
}
} // namespace detail

inline constexpr auto kWavetableOwnParams = std::to_array<project::ParamDescriptor>({
    detail::wt("table", 0, kWavetableBankCount - 1, 0, project::Unit::Count,
               project::ScaleKind::Stepped),
    // Through the table's frames, 0 the first and 1 the last. Per frame: a sweep is smooth.
    detail::wt("position", 0, 1, 0, project::Unit::Normalized, project::ScaleKind::Linear,
               project::RateClass::Sample),
    // 0 crossfades adjacent frames; 1 reads the spectrally interpolated table.
    detail::wt("morph", 0, 1, 0, project::Unit::Count, project::ScaleKind::Stepped),
    detail::wt("unison.voices", 1, kWavetableMaxUnison, 1, project::Unit::Count,
               project::ScaleKind::Stepped),
    detail::wt("unison.detune", 0, 100, 12, project::Unit::Cents),
    detail::wt("unison.spread", 0, 1, 0.7F, project::Unit::Normalized),
    detail::wt("filter.cutoff", 20, 20000, 20000, project::Unit::Hertz,
               project::ScaleKind::Logarithmic),
    detail::wt("filter.resonance", 0, 1, 0, project::Unit::Normalized),
    detail::wt("env.attack", 0, 10, 0.005F, project::Unit::Seconds,
               project::ScaleKind::Logarithmic),
    detail::wt("env.decay", 0, 10, 0.3F, project::Unit::Seconds, project::ScaleKind::Logarithmic),
    detail::wt("env.sustain", 0, 1, 0.8F, project::Unit::Normalized),
    detail::wt("env.release", 0, 10, 0.25F, project::Unit::Seconds,
               project::ScaleKind::Logarithmic),
    // An LFO on position: the classic wavetable movement.
    detail::wt("lfo.rate", 0.01F, 40, 0.5F, project::Unit::Hertz, project::ScaleKind::Logarithmic),
    detail::wt("lfo.depth", 0, 1, 0, project::Unit::Normalized),
    detail::wt("glide", 0, 5, 0, project::Unit::Seconds, project::ScaleKind::Logarithmic),
    detail::wt("level", -48, 12, -6, project::Unit::Decibels, project::ScaleKind::Linear,
               project::RateClass::Sample),
});

static_assert(kWavetableOwnParams.size() == static_cast<std::size_t>(WavetableParam::Count));

inline constexpr ModLayout kWavetableMod =
    ModLayout::appendedTo(static_cast<std::uint32_t>(WavetableParam::Count));

/// The modern shape (phase_12.md §4.3): a second oscillator, a sub, noise, the VA's
/// filter types with an envelope, and warp. After the modulation block, so nothing
/// before it moved; every default is off, so a patch without them renders as before.
enum class WavetableExtra : std::uint32_t {
    Osc2Table,
    Osc2Position,
    Osc2Semi,
    Osc2Fine,
    Osc2Level,
    SubLevel,
    NoiseLevel,
    FilterType,
    FilterEnvAmount,
    FilterEnvAttack,
    FilterEnvDecay,
    FilterEnvSustain,
    FilterEnvRelease,
    WarpMode,
    WarpAmount,
    Count,
};

/// filter.type: low-pass first, so the default is the filter the wavetable always had.
enum class WavetableFilter : std::uint8_t { LowPass, Ladder, BandPass, HighPass, Notch };
inline constexpr std::uint8_t kWavetableFilterCount = 5;

/// warp.mode: how the read phase is bent before the table is read.
enum class WavetableWarp : std::uint8_t {
    None,
    Sync,
    BendPlus,
    BendMinus,
    Pwm,
    Asym,
    FmFromOsc2,
    Mirror,
    Quantise,
};
inline constexpr std::uint8_t kWavetableWarpCount = 9;

inline constexpr auto kWavetableExtraParams = std::to_array<project::ParamDescriptor>({
    detail::wt("osc2.table", 0, kWavetableBankCount - 1, 0, project::Unit::Count,
               project::ScaleKind::Stepped),
    detail::wt("osc2.position", 0, 1, 0, project::Unit::Normalized),
    detail::wt("osc2.semi", -24, 24, 0, project::Unit::Semitones, project::ScaleKind::Stepped),
    detail::wt("osc2.fine", -100, 100, 0, project::Unit::Cents),
    // 0 is off: osc2 is not read at all.
    detail::wt("osc2.level", 0, 1, 0, project::Unit::Normalized),
    // A sine an octave below the note.
    detail::wt("sub.level", 0, 1, 0, project::Unit::Normalized),
    detail::wt("noise.level", 0, 1, 0, project::Unit::Normalized),
    detail::wt("filter.type", 0, kWavetableFilterCount - 1, 0, project::Unit::Count,
               project::ScaleKind::Stepped),
    // Octaves at full envelope, as the VA's.
    detail::wt("filter.envAmount", -8, 8, 0, project::Unit::Count),
    detail::wt("filterEnv.attack", 0, 10, 0.005F, project::Unit::Seconds,
               project::ScaleKind::Logarithmic),
    detail::wt("filterEnv.decay", 0, 10, 0.3F, project::Unit::Seconds,
               project::ScaleKind::Logarithmic),
    detail::wt("filterEnv.sustain", 0, 1, 0.3F, project::Unit::Normalized),
    detail::wt("filterEnv.release", 0, 10, 0.3F, project::Unit::Seconds,
               project::ScaleKind::Logarithmic),
    detail::wt("warp.mode", 0, kWavetableWarpCount - 1, 0, project::Unit::Count,
               project::ScaleKind::Stepped),
    detail::wt("warp.amount", 0, 1, 0, project::Unit::Normalized),
});
static_assert(kWavetableExtraParams.size() == static_cast<std::size_t>(WavetableExtra::Count));

/// Where the extra block starts: after the own parameters and the modulation layer.
inline constexpr std::uint32_t kWavetableExtraBase =
    static_cast<std::uint32_t>(WavetableParam::Count) + kModParamCount;

/// The table: the wavetable's own parameters, the modulation layer's, then the extra.
inline constexpr auto kWavetableParams = [] {
    constexpr auto withMod = withModParams(kWavetableOwnParams);
    std::array<project::ParamDescriptor, withMod.size() + kWavetableExtraParams.size()> table{};
    for (std::size_t i = 0; i < withMod.size(); ++i) {
        table[i] = withMod[i];
    }
    for (std::size_t i = 0; i < kWavetableExtraParams.size(); ++i) {
        table[withMod.size() + i] = kWavetableExtraParams[i];
    }
    return table;
}();
static_assert(kWavetableParams.size() == kWavetableExtraBase + kWavetableExtraParams.size());

} // namespace adx::instruments
