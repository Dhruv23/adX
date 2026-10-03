// The Sampler's parameters, in the order its node reads them (phase_4.md §4.5).
//
// The key/velocity map is not here: zones are structure, carried on the InstrumentSpec
// and baked into the node when it is built (SampleZone.h). These are what a knob turns.
#pragma once

#include <array>
#include <cstdint>

#include "engine/project/ParamDescriptor.h"

namespace adx::instruments {

enum class SamplerParam : std::uint32_t {
    EnvAttack,
    EnvDecay,
    EnvSustain,
    EnvRelease,
    Gain,
    Tune,
    Velocity,
    OneShot,
    Interpolation,
    Count,
};

// NOLINTBEGIN(modernize-use-designated-initializers) - a table reads as a table.
inline constexpr auto kSamplerParams = std::to_array<project::ParamDescriptor>({
    {"env.attack", 0.0F, 10.0F, 0.001F, project::Unit::Seconds, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"env.decay", 0.0F, 10.0F, 0.0F, project::Unit::Seconds, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"env.sustain", 0.0F, 1.0F, 1.0F, project::Unit::Normalized, project::ScaleKind::Linear,
     project::RateClass::Block, project::CurvePart::None},
    {"env.release", 0.0F, 10.0F, 0.05F, project::Unit::Seconds, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"gain", -48.0F, 12.0F, 0.0F, project::Unit::Decibels, project::ScaleKind::Linear,
     project::RateClass::Sample, project::CurvePart::None},
    // Cents, added to every zone's own tune.
    {"tune", -2400.0F, 2400.0F, 0.0F, project::Unit::Cents, project::ScaleKind::Linear,
     project::RateClass::Block, project::CurvePart::None},
    // How much velocity scales the level: 0 ignores it, 1 is linear in velocity.
    {"velocity", 0.0F, 1.0F, 1.0F, project::Unit::Normalized, project::ScaleKind::Linear,
     project::RateClass::Block, project::CurvePart::None},
    // A one-shot plays its sample to the end whatever the note length: drums.
    {"oneShot", 0.0F, 1.0F, 0.0F, project::Unit::Boolean, project::ScaleKind::Stepped,
     project::RateClass::Block, project::CurvePart::None},
    // 0 linear, 1 the 8-point windowed sinc. Only used when the pitch ratio is not 1.
    {"interpolation", 0.0F, 1.0F, 1.0F, project::Unit::Count, project::ScaleKind::Stepped,
     project::RateClass::Block, project::CurvePart::None},
});
// NOLINTEND(modernize-use-designated-initializers)

static_assert(kSamplerParams.size() == static_cast<std::size_t>(SamplerParam::Count));

} // namespace adx::instruments
