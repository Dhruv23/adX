// adx-thread: realtime
//
// MIDI learn on the audio thread (phase_9.md §3 step 2, §5 midi_learn_*): a controller
// message matched against the snapshot's compiled bindings and turned into a parameter
// value, and the last control message seen, for the main thread's learn mode to pick
// up.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

#include "engine/midi/MidiEvent.h"
#include "engine/project/MidiControl.h"
#include "engine/project/ParamRef.h"

namespace adx::midi {

/// A controller message, normalised: which control, and where it is, 0..1.
struct ControlMessage {
    std::uint8_t device{0};
    std::uint8_t channel{0};
    project::MidiControlKind kind{project::MidiControlKind::ControlChange};
    std::uint8_t number{0};
    float value{0.0F};
    /// A note binding's press (note-on with velocity); false for its release.
    bool pressed{false};
};

/// A binding as the snapshot carries it: trivially copyable, the device resolved to an
/// index (kAnyDevice for any).
struct CompiledBinding {
    std::uint8_t device;
    std::uint8_t channel;
    project::MidiControlKind kind;
    std::uint8_t number;
    project::ParamRef target;
    float minimum;
    float maximum;
    bool toggle;
};

/// What `event` is as a control, or false for anything learn ignores (clock, SysEx).
/// Notes are controls too, so a pad can be learned; the router still plays them.
[[nodiscard]] bool controlOf(const MidiEvent& event, ControlMessage& out) noexcept;

[[nodiscard]] bool matches(const CompiledBinding& binding, const ControlMessage& message) noexcept;

/// The parameter value `message` asks `binding` for. A toggle flips `toggled` on each
/// press and returns false for a release (nothing to write).
[[nodiscard]] bool valueFor(const CompiledBinding& binding, const ControlMessage& message,
                            bool& toggled, float& out) noexcept;

/// The last control message the engine saw, for learn mode. Written by the audio
/// thread, read by any thread; a sequence number says whether it is new.
class LastControl {
public:
    void publish(const ControlMessage& message) noexcept;
    /// True when a message newer than `sequence` is there; `sequence` is updated.
    [[nodiscard]] bool read(std::uint64_t& sequence, ControlMessage& out) const noexcept;

private:
    std::atomic<std::uint64_t> m_packed{0};
    std::atomic<std::uint64_t> m_sequence{0};
};

/// A value learn wrote, for the main thread to put in the project (so the knob moves,
/// and the value is saved).
struct LearnedValue {
    project::ParamRef target;
    float value;
};

} // namespace adx::midi
