// The Wavetable instrument's parameters, in the order its node reads them
// (phase_4.md §4.8).
#pragma once

#include <array>
#include <cstdint>

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

inline constexpr auto kWavetableParams = std::to_array<project::ParamDescriptor>({
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

static_assert(kWavetableParams.size() == static_cast<std::size_t>(WavetableParam::Count));

} // namespace adx::instruments
