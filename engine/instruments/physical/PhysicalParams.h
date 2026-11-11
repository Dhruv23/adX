// The `physical` instrument's parameters (phase_12.md §4.4), then the modulation
// layer's (§4.3).
#pragma once

#include <array>
#include <cstdint>

#include "engine/instruments/mod/ModLayer.h"
#include "engine/project/ParamDescriptor.h"

namespace adx::instruments {

/// 0 pluck (extended Karplus-Strong), 1 bow (waveguide and friction), 2 modal
/// (resonator bank), 3 tube (bore and jet or reed).
enum class PhysicalModel : std::uint8_t { Pluck, Bow, Modal, Tube };
inline constexpr std::uint8_t kPhysicalModels = 4;

/// none, guitar, violin, cello, steel-pan skirt: a few fixed body resonances.
inline constexpr std::uint8_t kPhysicalBodies = 5;
/// flute (jet), clarinet (cylinder and reed), saxophone (cone and reed).
inline constexpr std::uint8_t kPhysicalTubes = 3;

enum class PhysicalParam : std::uint32_t {
    Model,
    StringDecay,
    StringDamping,
    StringBrightness,
    StringDispersion,
    ExciterPosition,
    ExciterHardness,
    Mute,
    BodyType,
    BodyMix,
    BowPressure,
    BowSpeed,
    BowPosition,
    ModesTable,
    ModesDecay,
    ModesDecayTilt,
    TubeKind,
    BreathPressure,
    BreathNoise,
    ReedStiffness,
    JetRatio,
    EnvAttack,
    EnvDecay,
    EnvSustain,
    EnvRelease,
    Glide,
    Level,
    Count,
};

namespace detail {
constexpr project::ParamDescriptor phys(std::string_view name, float lo, float hi, float def,
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
constexpr project::ParamDescriptor physChoice(std::string_view name, float hi, float def) {
    return phys(name, 0, hi, def, project::Unit::Count, project::ScaleKind::Stepped);
}
} // namespace detail

inline constexpr auto kPhysicalOwnParams = std::to_array<project::ParamDescriptor>({
    detail::physChoice("model", kPhysicalModels - 1, 0),
    // Seconds for the fundamental to fall 60 dB at C4; shorter up the keyboard.
    detail::phys("string.decay", 0.05F, 30.0F, 3.0F, project::Unit::Seconds,
                 project::ScaleKind::Logarithmic),
    // Loop low-pass: 0 keeps every harmonic as long as the fundamental.
    detail::phys("string.damping", 0, 1, 0.35F, project::Unit::Normalized),
    // The excitation's low-pass: a soft finger to a hard pick.
    detail::phys("string.brightness", 0, 1, 0.7F, project::Unit::Normalized),
    // Allpass stiffness: the stretched partials of a piano or a steel string.
    detail::phys("string.dispersion", 0, 1, 0, project::Unit::Normalized),
    // Where along the string it is struck, as a fraction: 0.5 is the middle.
    detail::phys("exciter.position", 0.02F, 0.5F, 0.13F, project::Unit::Normalized),
    detail::phys("exciter.hardness", 0, 1, 0.5F, project::Unit::Normalized),
    // Palm mute: shorter, darker, and a thunk.
    detail::phys("mute", 0, 1, 0, project::Unit::Normalized),
    detail::physChoice("body.type", kPhysicalBodies - 1, 0),
    detail::phys("body.mix", 0, 1, 0.5F, project::Unit::Normalized),
    detail::phys("bow.pressure", 0, 1, 0.5F, project::Unit::Normalized),
    detail::phys("bow.speed", 0, 1, 0.6F, project::Unit::Normalized),
    detail::phys("bow.position", 0.02F, 0.5F, 0.127F, project::Unit::Normalized),
    // ModeTables.h: 0 a single mode, 1.. the shipped tables.
    detail::physChoice("modes.table", 15, 2),
    detail::phys("modes.decay", 0.05F, 30.0F, 1.5F, project::Unit::Seconds,
                 project::ScaleKind::Logarithmic),
    // How much faster the upper modes die: 0 all alike.
    detail::phys("modes.decayTilt", 0, 4, 1, project::Unit::Normalized),
    detail::physChoice("tube.kind", kPhysicalTubes - 1, 1),
    detail::phys("breath.pressure", 0, 1, 0.7F, project::Unit::Normalized),
    detail::phys("breath.noise", 0, 1, 0.1F, project::Unit::Normalized),
    detail::phys("reed.stiffness", 0, 1, 0.5F, project::Unit::Normalized),
    detail::phys("jet.ratio", 0.05F, 0.6F, 0.32F, project::Unit::Normalized),
    detail::phys("env.attack", 0, 10, 0.0F, project::Unit::Seconds,
                 project::ScaleKind::Logarithmic),
    detail::phys("env.decay", 0, 10, 0.2F, project::Unit::Seconds, project::ScaleKind::Logarithmic),
    detail::phys("env.sustain", 0, 1, 1.0F, project::Unit::Normalized),
    detail::phys("env.release", 0, 10, 0.3F, project::Unit::Seconds,
                 project::ScaleKind::Logarithmic),
    detail::phys("glide", 0, 5, 0, project::Unit::Seconds, project::ScaleKind::Logarithmic),
    detail::phys("level", -48, 12, -6, project::Unit::Decibels, project::ScaleKind::Linear,
                 project::RateClass::Sample),
});
static_assert(kPhysicalOwnParams.size() == static_cast<std::size_t>(PhysicalParam::Count));

inline constexpr auto kPhysicalParams = withModParams(kPhysicalOwnParams);
inline constexpr ModLayout kPhysicalMod =
    ModLayout::appendedTo(static_cast<std::uint32_t>(PhysicalParam::Count));

} // namespace adx::instruments
