// The VA instrument's parameters, in the order its node reads them (phase_4.md §4.4).
#pragma once

#include <array>
#include <cstdint>

#include "engine/project/ParamDescriptor.h"

namespace adx::instruments {

enum class VaParam : std::uint32_t {
    Osc1Wave,
    Osc1Octave,
    Osc1Semi,
    Osc1Fine,
    Osc1Level,
    Osc1PulseWidth,
    Osc2Wave,
    Osc2Octave,
    Osc2Semi,
    Osc2Fine,
    Osc2Level,
    Osc2PulseWidth,
    SubLevel,
    NoiseLevel,
    UnisonVoices,
    UnisonDetune,
    UnisonSpread,
    FilterType,
    FilterCutoff,
    FilterResonance,
    FilterDrive,
    FilterKeyTrack,
    FilterEnvAmount,
    FilterVelocity,
    EnvAttack,
    EnvDecay,
    EnvSustain,
    EnvRelease,
    FilterEnvAttack,
    FilterEnvDecay,
    FilterEnvSustain,
    FilterEnvRelease,
    Lfo1Shape,
    Lfo1Rate,
    Lfo1Retrig,
    Lfo1Target,
    Lfo1Depth,
    Lfo2Shape,
    Lfo2Rate,
    Lfo2Retrig,
    Lfo2Target,
    Lfo2Depth,
    Glide,
    Level,
    Count,
};

/// What an LFO modulates. Depth 1 means: pitch +-12 semitones, cutoff +-4 octaves,
/// amplitude a full tremolo, pan hard left to right, pulse width +-0.45.
enum class VaLfoTarget : std::uint8_t { None, Pitch, Cutoff, Amplitude, Pan, PulseWidth };
inline constexpr std::uint8_t kVaLfoTargetCount = 6;

/// 0 the ladder (24 dB lowpass), then the state-variable modes.
enum class VaFilterType : std::uint8_t { Ladder, LowPass, BandPass, HighPass, Notch };
inline constexpr std::uint8_t kVaFilterTypeCount = 5;

inline constexpr std::uint32_t kVaMaxUnison = 8;

namespace detail {
using project::CurvePart;
using project::ParamDescriptor;
using project::RateClass;
using project::ScaleKind;
using project::Unit;

constexpr ParamDescriptor stepped(std::string_view name, float lo, float hi, float def) {
    return ParamDescriptor{.name = name,
                           .minimum = lo,
                           .maximum = hi,
                           .defaultValue = def,
                           .unit = Unit::Count,
                           .scale = ScaleKind::Stepped,
                           .rate = RateClass::Block,
                           .curve = CurvePart::None};
}
constexpr ParamDescriptor value(std::string_view name, float lo, float hi, float def, Unit unit,
                                ScaleKind scale = ScaleKind::Linear,
                                RateClass rate = RateClass::Block) {
    return ParamDescriptor{.name = name,
                           .minimum = lo,
                           .maximum = hi,
                           .defaultValue = def,
                           .unit = unit,
                           .scale = scale,
                           .rate = rate,
                           .curve = CurvePart::None};
}
} // namespace detail

/// Waves: 0 sine, 1 saw, 2 square, 3 triangle, 4 pulse (dsp::OscShape order).
inline constexpr auto kVaParams = std::to_array<project::ParamDescriptor>({
    detail::stepped("osc1.wave", 0, 4, 1),
    detail::stepped("osc1.octave", -3, 3, 0),
    detail::stepped("osc1.semi", -12, 12, 0),
    detail::value("osc1.fine", -100, 100, 0, project::Unit::Cents),
    detail::value("osc1.level", 0, 1, 1, project::Unit::Normalized),
    detail::value("osc1.pulseWidth", 0.05F, 0.95F, 0.5F, project::Unit::Normalized),
    detail::stepped("osc2.wave", 0, 4, 1),
    detail::stepped("osc2.octave", -3, 3, 0),
    detail::stepped("osc2.semi", -12, 12, 0),
    detail::value("osc2.fine", -100, 100, 7, project::Unit::Cents),
    detail::value("osc2.level", 0, 1, 0, project::Unit::Normalized),
    detail::value("osc2.pulseWidth", 0.05F, 0.95F, 0.5F, project::Unit::Normalized),
    detail::value("sub.level", 0, 1, 0, project::Unit::Normalized),
    detail::value("noise.level", 0, 1, 0, project::Unit::Normalized),
    detail::stepped("unison.voices", 1, kVaMaxUnison, 1),
    detail::value("unison.detune", 0, 100, 12, project::Unit::Cents),
    detail::value("unison.spread", 0, 1, 0.7F, project::Unit::Normalized),
    detail::stepped("filter.type", 0, kVaFilterTypeCount - 1, 0),
    detail::value("filter.cutoff", 20, 20000, 20000, project::Unit::Hertz,
                  project::ScaleKind::Logarithmic),
    detail::value("filter.resonance", 0, 1, 0, project::Unit::Normalized),
    detail::value("filter.drive", 0, 4, 0, project::Unit::Normalized),
    detail::value("filter.keyTrack", 0, 1, 0, project::Unit::Normalized),
    detail::value("filter.envAmount", -8, 8, 0, project::Unit::Count),
    detail::value("filter.velocity", 0, 4, 0, project::Unit::Count),
    detail::value("env.attack", 0, 10, 0.005F, project::Unit::Seconds,
                  project::ScaleKind::Logarithmic),
    detail::value("env.decay", 0, 10, 0.2F, project::Unit::Seconds,
                  project::ScaleKind::Logarithmic),
    detail::value("env.sustain", 0, 1, 0.8F, project::Unit::Normalized),
    detail::value("env.release", 0, 10, 0.2F, project::Unit::Seconds,
                  project::ScaleKind::Logarithmic),
    detail::value("filterEnv.attack", 0, 10, 0.005F, project::Unit::Seconds,
                  project::ScaleKind::Logarithmic),
    detail::value("filterEnv.decay", 0, 10, 0.3F, project::Unit::Seconds,
                  project::ScaleKind::Logarithmic),
    detail::value("filterEnv.sustain", 0, 1, 0.3F, project::Unit::Normalized),
    detail::value("filterEnv.release", 0, 10, 0.3F, project::Unit::Seconds,
                  project::ScaleKind::Logarithmic),
    detail::stepped("lfo1.shape", 0, 5, 0),
    detail::value("lfo1.rate", 0.01F, 40, 5, project::Unit::Hertz, project::ScaleKind::Logarithmic),
    detail::stepped("lfo1.retrig", 0, 1, 1),
    detail::stepped("lfo1.target", 0, kVaLfoTargetCount - 1, 0),
    detail::value("lfo1.depth", 0, 1, 0, project::Unit::Normalized),
    detail::stepped("lfo2.shape", 0, 5, 0),
    detail::value("lfo2.rate", 0.01F, 40, 0.5F, project::Unit::Hertz,
                  project::ScaleKind::Logarithmic),
    detail::stepped("lfo2.retrig", 0, 1, 0),
    detail::stepped("lfo2.target", 0, kVaLfoTargetCount - 1, 0),
    detail::value("lfo2.depth", 0, 1, 0, project::Unit::Normalized),
    detail::value("glide", 0, 5, 0, project::Unit::Seconds, project::ScaleKind::Logarithmic),
    detail::value("level", -48, 12, -6, project::Unit::Decibels, project::ScaleKind::Linear,
                  project::RateClass::Sample),
});

static_assert(kVaParams.size() == static_cast<std::size_t>(VaParam::Count));

} // namespace adx::instruments
