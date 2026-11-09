// adx-thread: realtime
//
// The MIDI outputs a channel can play into (phase_11.5.md §4.6, the FM-1 as an external
// instrument). EngineCore publishes each open output's ring here by slot, and the stream
// frame its current block starts at; a `midiout` channel node reads them on the audio
// thread and queues its notes the way the clock sender does, stamped with the frame they
// belong to, so MidiDevices' sender thread sends each when it is heard.
//
// One table for the process: there is one live engine with devices. A second engine (a
// test's, an offline render) publishes no outputs, so its midiout channels are silent.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace adx::midi {

class MidiOutput;

inline constexpr std::size_t kLiveOutputSlots = 8;

struct LiveOutputs {
    std::array<std::atomic<MidiOutput*>, kLiveOutputSlots> outputs{};
    /// The stream frame of the block being rendered (EngineCore::processStep).
    std::atomic<std::uint64_t> blockFrame{0};
};

[[nodiscard]] LiveOutputs& liveOutputs() noexcept;

} // namespace adx::midi
