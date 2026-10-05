// The Granular instrument's parameters, in the order its node reads them
// (phase_4.md §4.8).
#pragma once

#include <array>
#include <cstdint>

#include "engine/project/ParamDescriptor.h"

namespace adx::instruments {

enum class GranularParam : std::uint32_t {
    Source,
    Position,
    Scan,
    Spray,
    Size,
    Density,
    PitchJitter,
    Window,
    Spread,
    EnvAttack,
    EnvDecay,
    EnvSustain,
    EnvRelease,
    Level,
    Count,
};

/// What a grain reads: the channel's sample (its first matching zone), or a built-in
/// tone at the note's pitch - which is also what a sample source falls back to when
/// the channel has no zone, so the instrument is never silent by surprise.
enum class GrainSource : std::uint8_t { Sample, Sine, Saw };
inline constexpr std::uint8_t kGrainSourceCount = 3;

/// The grain envelope: Hann, Tukey (flat top, quarter tapers), triangle.
enum class GrainWindow : std::uint8_t { Hann, Tukey, Triangle };
inline constexpr std::uint8_t kGrainWindowCount = 3;

/// Grains one voice can have sounding at once. The pool is preallocated per voice -
/// polyphony x this - so a burst of density never allocates; a grain due while the
/// pool is full is skipped, not queued.
inline constexpr std::uint32_t kGrainsPerVoice = 32;

namespace detail {
constexpr project::ParamDescriptor grain(std::string_view name, float lo, float hi, float def,
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

inline constexpr auto kGranularParams = std::to_array<project::ParamDescriptor>({
    detail::grain("source", 0, kGrainSourceCount - 1, 0, project::Unit::Count,
                  project::ScaleKind::Stepped),
    // Where in the sample grains start, as a fraction of its length.
    detail::grain("position", 0, 1, 0.25F, project::Unit::Normalized),
    // How fast that point moves while the note is held: sample lengths per second.
    detail::grain("scan", -1, 1, 0, project::Unit::Normalized),
    // Random offset added to each grain's start, seconds.
    detail::grain("spray", 0, 1, 0.02F, project::Unit::Seconds),
    detail::grain("size", 5, 500, 80, project::Unit::Milliseconds, project::ScaleKind::Logarithmic),
    // Grains started per second.
    detail::grain("density", 1, 200, 30, project::Unit::Hertz, project::ScaleKind::Logarithmic),
    // Random pitch offset per grain, +- this many cents.
    detail::grain("pitchJitter", 0, 1200, 0, project::Unit::Cents),
    detail::grain("window", 0, kGrainWindowCount - 1, 0, project::Unit::Count,
                  project::ScaleKind::Stepped),
    // How far grains scatter across the stereo field.
    detail::grain("spread", 0, 1, 0.5F, project::Unit::Normalized),
    detail::grain("env.attack", 0, 10, 0.01F, project::Unit::Seconds,
                  project::ScaleKind::Logarithmic),
    detail::grain("env.decay", 0, 10, 0.2F, project::Unit::Seconds,
                  project::ScaleKind::Logarithmic),
    detail::grain("env.sustain", 0, 1, 1, project::Unit::Normalized),
    detail::grain("env.release", 0, 10, 0.3F, project::Unit::Seconds,
                  project::ScaleKind::Logarithmic),
    detail::grain("level", -48, 12, -6, project::Unit::Decibels, project::ScaleKind::Linear,
                  project::RateClass::Sample),
});

static_assert(kGranularParams.size() == static_cast<std::size_t>(GranularParam::Count));

} // namespace adx::instruments
