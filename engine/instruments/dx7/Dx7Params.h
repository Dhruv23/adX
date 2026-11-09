// The dx7 instrument's parameters (phase_11.5.md §4.3): every byte of the voice, then
// the DX7 function parameters, the FM-1 part, and adX's own.
//
// Parameter i < 156 is voice byte i, so a `.syx` voice and a dx7 channel convert into
// each other with no loss by construction. A value is the DX7's displayed number;
// three bytes are offset from it (algorithm 1..32 is byte 0..31, detune -7..+7 is
// 0..14, transpose -24..+24 semitones is 0..48). The ten name bytes are parameters too
// (character codes), so nothing in the voice is out of reach of the project.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "engine/instruments/dx7/Dx7Voice.h"
#include "engine/project/ParamDescriptor.h"

namespace adx::dx7 {

/// After the 156 voice bytes (155 is operator on/off as a 6-bit mask).
enum class Dx7Param : std::uint32_t {
    Mono = kVoiceBytes,
    BendRange,
    BendStep,
    PortaMode,
    PortaGliss,
    PortaTime,
    WheelRange,
    WheelAssign,
    FootRange,
    FootAssign,
    BreathRange,
    BreathAssign,
    AftertouchRange,
    AftertouchAssign,
    PartLevel,
    PartPan,
    PartTuning,
    Fm1Rate,
    Polyphony,
    Gain,
    /// Live controllers, as parameters so automation, MIDI learn and the FM-1 part CCs
    /// all reach them: 0..127, bend -8192..8191, sustain 0/1.
    CtlWheel,
    CtlBreath,
    CtlFoot,
    CtlAftertouch,
    CtlBend,
    CtlSustain,
    Count,
};

inline constexpr std::uint32_t kDx7ParamCount = static_cast<std::uint32_t>(Dx7Param::Count);
inline constexpr std::uint32_t kDx7FunctionFirst = static_cast<std::uint32_t>(Dx7Param::Mono);

[[nodiscard]] constexpr std::uint32_t paramIndex(Dx7Param p) noexcept {
    return static_cast<std::uint32_t>(p);
}

/// One descriptor per parameter, in index order.
[[nodiscard]] std::span<const project::ParamDescriptor> dx7Params() noexcept;

/// What a voice byte is offset by from its parameter value (byte = value + offset).
[[nodiscard]] int byteOffset(std::size_t byte) noexcept;

/// Parameter values -> voice bytes (clamped to each byte's range). `params` is the
/// instrument's slice (index 0 is voice byte 0).
void voiceFromParams(std::span<const float> params, Dx7Voice& out) noexcept;
/// Voice bytes -> parameter values; the parameters after the voice are left alone.
void paramsFromVoice(const Dx7Voice& voice, std::span<float> params) noexcept;

} // namespace adx::dx7
