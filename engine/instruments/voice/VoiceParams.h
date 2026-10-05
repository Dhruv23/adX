// The Voice instrument's parameters (phase_4.md §4.13).
//
// Two kinds. Post-render (`level`, `velocity`): applied as the cached render plays,
// automate freely, nothing re-renders. Baked (the rest): they change the synthesis, so
// they live in the cached render and a change re-renders the notes it touches. Baked
// parameters are read from the channel when the node is built; automation of a baked
// parameter is not evaluated per WORLD frame yet (phase_4.md §11, P4-8).
#pragma once

#include <array>
#include <cstdint>

#include "engine/project/ParamDescriptor.h"

namespace adx::instruments {

enum class VoiceParam : std::uint32_t {
    Level,
    Velocity,
    Gender,
    Breathiness,
    Tuning,
    PeakCompression,
    VibratoDepth,
    VibratoRate,
    VibratoDelay,
    Count,
};

namespace detail {
constexpr project::ParamDescriptor voice(std::string_view name, float lo, float hi, float def,
                                         project::Unit unit,
                                         project::RateClass rate = project::RateClass::Baked) {
    return project::ParamDescriptor{.name = name,
                                    .minimum = lo,
                                    .maximum = hi,
                                    .defaultValue = def,
                                    .unit = unit,
                                    .scale = project::ScaleKind::Linear,
                                    .rate = rate,
                                    .curve = project::CurvePart::None};
}
} // namespace detail

inline constexpr auto kVoiceParams = std::to_array<project::ParamDescriptor>({
    detail::voice("level", -48, 12, 0, project::Unit::Decibels, project::RateClass::Sample),
    detail::voice("velocity", 0, 1, 0.5F, project::Unit::Normalized, project::RateClass::Block),
    // UTAU's g: positive deepens (formants down), negative brightens.
    detail::voice("gender", -100, 100, 0, project::Unit::Normalized),
    // UTAU's B: 50 neutral, 0 clean, 100 breathy.
    detail::voice("breathiness", 0, 100, 50, project::Unit::Normalized),
    // UTAU's t, in cents.
    detail::voice("tuning", -100, 100, 0, project::Unit::Cents),
    // UTAU's P: how far each note's peak is normalised.
    detail::voice("peakCompression", 0, 100, 86, project::Unit::Percent),
    detail::voice("vibrato.depth", 0, 200, 0, project::Unit::Cents),
    detail::voice("vibrato.rate", 0.1F, 12, 5.5F, project::Unit::Hertz),
    // Seconds into a note before the vibrato starts.
    detail::voice("vibrato.delay", 0, 2, 0.25F, project::Unit::Seconds),
});

static_assert(kVoiceParams.size() == static_cast<std::size_t>(VoiceParam::Count));

} // namespace adx::instruments
