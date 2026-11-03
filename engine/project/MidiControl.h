// What kind of MIDI control a learned binding listens to. Its own header, free of
// <string> and <vector>, because the audio thread's learn table names it too.
#pragma once

#include <cstdint>

namespace adx::project {

enum class MidiControlKind : std::uint8_t {
    /// A continuous controller: number is the CC, 0..127.
    ControlChange,
    /// A key: number is the note. Velocity is the value, or a toggle.
    Note,
    PitchBend,
    ChannelPressure,
};

/// Any MIDI channel.
inline constexpr std::uint8_t kAnyMidiChannel = 0xFF;

} // namespace adx::project
