// adx-thread: realtime
//
// One MIDI message as the engine carries it (phase_9.md §4.1): three bytes and when it
// happened, on the audio clock. Trivially copyable, because rings move it by value.
#pragma once

#include <cstdint>

namespace adx::midi {

/// Status nibbles, high four bits of the status byte.
enum class MidiKind : std::uint8_t {
    NoteOff = 0x80,
    NoteOn = 0x90,
    PolyPressure = 0xA0,
    ControlChange = 0xB0,
    ProgramChange = 0xC0,
    ChannelPressure = 0xD0,
    PitchBend = 0xE0,
    System = 0xF0,
};

struct MidiEvent {
    /// Stream frames since the stream started: the same clock as StreamTime::streamFrames.
    std::uint64_t timestampSamples{0};
    std::uint8_t status{0};
    std::uint8_t data1{0};
    std::uint8_t data2{0};
    /// Which open input it came from.
    std::uint8_t device{0};

    [[nodiscard]] MidiKind kind() const noexcept {
        return status >= 0xF0 ? MidiKind::System : static_cast<MidiKind>(status & 0xF0U);
    }
    /// 0..15. Meaningless for a system message.
    [[nodiscard]] std::uint8_t channel() const noexcept {
        return status & 0x0FU;
    }
    /// A note-on with velocity 0 is a note-off, as the MIDI spec says.
    [[nodiscard]] bool isNoteOn() const noexcept {
        return kind() == MidiKind::NoteOn && data2 > 0;
    }
    [[nodiscard]] bool isNoteOff() const noexcept {
        return kind() == MidiKind::NoteOff || (kind() == MidiKind::NoteOn && data2 == 0);
    }

    [[nodiscard]] friend bool operator==(const MidiEvent&, const MidiEvent&) noexcept = default;
};

/// One message of a MIDI take (STATE.md P9-6): what an input played while the
/// arrangement rolled with MIDI recording on, at the arrangement tick it played at.
struct TakeEvent {
    std::int64_t tick{0};
    std::uint8_t status{0};
    std::uint8_t data1{0};
    std::uint8_t data2{0};
    std::uint8_t device{0};
};

} // namespace adx::midi
