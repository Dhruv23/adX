// adx-thread: main
//
// The MIDI devices (phase_9.md §4.1): which inputs exist, opening and closing them, and
// handing each open one to the engine as a MidiInput. The device library (RtMidi) is an
// implementation detail of MidiDevices.cpp, as RtAudio is of RtAudioBackend.cpp.
//
// An input's messages arrive on the library's own thread, which is the device thread
// MidiInput::onMessage is written for: it timestamps them on the audio clock and pushes
// them into the input's ring, and the audio thread does the rest.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace adx::render {
class RenderEngine;
}

namespace adx::midi {

class MidiInput;

class MidiDevices {
public:
    /// The engine must outlive this, and must have been opened (its block size and rate
    /// are what inputs schedule against).
    explicit MidiDevices(render::RenderEngine& engine);
    ~MidiDevices();
    MidiDevices(const MidiDevices&) = delete;
    MidiDevices& operator=(const MidiDevices&) = delete;
    MidiDevices(MidiDevices&&) = delete;
    MidiDevices& operator=(MidiDevices&&) = delete;

    /// The system's MIDI input ports, by name, in port order. Empty when the library
    /// could not start (no MIDI service at all).
    [[nodiscard]] static std::vector<std::string> inputs();

    /// Opens the input named `name`. False with `error` set when there is no such port,
    /// it is already open, every engine slot is taken, or the driver refused.
    bool open(const std::string& name, std::string& error);
    /// Closes it; a no-op for one that is not open.
    void close(const std::string& name);
    /// The names of the open inputs.
    [[nodiscard]] std::vector<std::string> opened() const;
    /// Messages lost across every input because a ring was full.
    [[nodiscard]] std::uint64_t lost() const;

private:
    struct Open;
    render::RenderEngine& m_engine;
    std::vector<std::unique_ptr<Open>> m_open;
    /// Inputs closed while the stream may still be reading them: kept until destruction,
    /// which is after the stream has stopped (EngineCore::setMidiInput's contract).
    std::vector<std::unique_ptr<MidiInput>> m_retired;
};

} // namespace adx::midi
