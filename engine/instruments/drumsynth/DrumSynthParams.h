// The DrumSynth's parameters, in the order its node reads them (phase_4.md §4.7).
//
// One table for five models: each parameter means the same kind of thing in every
// model (`tone` is brightness, `decay` is how long it rings), so a kit is one channel
// and one knob set rather than five instruments.
#pragma once

#include <array>
#include <cstdint>

#include "engine/project/ParamDescriptor.h"

namespace adx::instruments {

enum class DrumParam : std::uint32_t {
    Model,
    Tune,
    KeyTrack,
    Decay,
    Tone,
    Click,
    Noise,
    PitchDrop,
    PitchTime,
    Drive,
    Sub,
    Level,
    Count,
};

/// 0..4 one model for every key; Kit maps keys to models as General MIDI does.
enum class DrumModel : std::uint8_t { Kick, Snare, Hat, Clap, Tom, Kit };
inline constexpr std::uint8_t kDrumModelCount = 6;

namespace detail {
constexpr project::ParamDescriptor drum(std::string_view name, float lo, float hi, float def,
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

/// Read when a hit starts - a drum machine latches its knobs at the trigger - except
/// `level`, which is per frame.
inline constexpr auto kDrumSynthParams = std::to_array<project::ParamDescriptor>({
    detail::drum("model", 0, kDrumModelCount - 1, 0, project::Unit::Count,
                 project::ScaleKind::Stepped),
    detail::drum("tune", -24, 24, 0, project::Unit::Semitones),
    // How far the drum's pitch follows the key: 0 every key is the same drum.
    detail::drum("keyTrack", 0, 1, 0, project::Unit::Normalized),
    // Time to fall 60 dB.
    detail::drum("decay", 0.01F, 4, 0.4F, project::Unit::Seconds, project::ScaleKind::Logarithmic),
    detail::drum("tone", 0, 1, 0.5F, project::Unit::Normalized),
    detail::drum("click", 0, 1, 0.5F, project::Unit::Normalized),
    detail::drum("noise", 0, 1, 0.5F, project::Unit::Normalized),
    // The kick's and tom's pitch sweep: semitones above the resting pitch at the hit,
    // decaying with `pitchTime` as its time constant.
    detail::drum("pitchDrop", 0, 48, 24, project::Unit::Semitones),
    detail::drum("pitchTime", 0.001F, 0.5F, 0.04F, project::Unit::Seconds,
                 project::ScaleKind::Logarithmic),
    // 0 clean; above it, a normalised tanh with this much gain: the hardstyle stage.
    detail::drum("drive", 0, 30, 0, project::Unit::Ratio),
    // A sine at the resting pitch under the swept body: sub reinforcement.
    detail::drum("sub", 0, 1, 0, project::Unit::Normalized),
    detail::drum("level", -48, 12, -6, project::Unit::Decibels, project::ScaleKind::Linear,
                 project::RateClass::Sample),
});

static_assert(kDrumSynthParams.size() == static_cast<std::size_t>(DrumParam::Count));

} // namespace adx::instruments
