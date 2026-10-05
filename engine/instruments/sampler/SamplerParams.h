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

/// Where a table built on the Sampler's carries `choke` and `fixedPitch`: right after
/// the Sampler's own parameters. The Sampler's table stops before them, so a Sampler
/// reads both as 0: it never chokes, and the key sets the pitch.
inline constexpr std::uint32_t kSamplerChokeIndex = static_cast<std::uint32_t>(SamplerParam::Count);
inline constexpr std::uint32_t kSamplerFixedPitchIndex = kSamplerChokeIndex + 1;

namespace detail {
/// The Sampler's table with other defaults and a `choke` row: the Slicer's and the
/// sample-pool channel's (phase_4.md §4.6; both are the Sampler under another name).
constexpr auto samplerVariant(float attack, float release, float oneShot, float choke,
                              float fixedPitch) {
    std::array<project::ParamDescriptor, kSamplerParams.size() + 2> table{};
    for (std::size_t i = 0; i < kSamplerParams.size(); ++i) {
        table[i] = kSamplerParams[i];
    }
    table[static_cast<std::size_t>(SamplerParam::EnvAttack)].defaultValue = attack;
    table[static_cast<std::size_t>(SamplerParam::EnvRelease)].defaultValue = release;
    table[static_cast<std::size_t>(SamplerParam::OneShot)].defaultValue = oneShot;
    // A new note fades out the voices already sounding: one pad at a time.
    table[kSamplerChokeIndex] = project::ParamDescriptor{.name = "choke",
                                                         .minimum = 0.0F,
                                                         .maximum = 1.0F,
                                                         .defaultValue = choke,
                                                         .unit = project::Unit::Boolean,
                                                         .scale = project::ScaleKind::Stepped,
                                                         .rate = project::RateClass::Block,
                                                         .curve = project::CurvePart::None};
    // 1: every key plays the sample at its recorded pitch, whatever the zone's root.
    table[kSamplerFixedPitchIndex] = project::ParamDescriptor{.name = "fixedPitch",
                                                              .minimum = 0.0F,
                                                              .maximum = 1.0F,
                                                              .defaultValue = fixedPitch,
                                                              .unit = project::Unit::Boolean,
                                                              .scale = project::ScaleKind::Stepped,
                                                              .rate = project::RateClass::Block,
                                                              .curve = project::CurvePart::None};
    return table;
}
} // namespace detail

/// The Slicer: slices play to their end and choke one another, as a chopped loop does.
inline constexpr auto kSlicerParams = detail::samplerVariant(0.0F, 0.005F, 1.0F, 1.0F, 0.0F);
/// The sample-pool channel: plain one-shots at their recorded pitch, which overlap.
inline constexpr auto kPoolParams = detail::samplerVariant(0.0F, 0.005F, 1.0F, 0.0F, 1.0F);

} // namespace adx::instruments
