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

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace adx::render {
class RenderEngine;
}

namespace adx::midi {

class MidiInput;
class MidiOutput;

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

    // --- outputs (STATE.md P9-5) ---

    /// The system's MIDI output ports, by name, in port order.
    [[nodiscard]] static std::vector<std::string> outputs();
    /// Opens the output named `name`; open()'s failure cases.
    bool openOutput(const std::string& name, std::string& error);
    void closeOutput(const std::string& name);
    [[nodiscard]] std::vector<std::string> openedOutputs() const;
    /// Sends `bytes` (one complete message) to an open output now, not on the audio
    /// clock: a controller's LEDs, a script. False with `error` set otherwise.
    bool send(const std::string& name, std::span<const std::uint8_t> bytes, std::string& error);
    /// Whether an open output is sent MIDI clock and time code. False for one not open.
    bool setSync(const std::string& name, bool clock, bool mtc);
    /// Clock and time-code messages sent, and lost to a full ring, across outputs.
    [[nodiscard]] std::uint64_t sent() const noexcept {
        return m_sent.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t lostOut() const;

private:
    struct Open;
    struct OpenOut;
    /// The sender thread: each queued message goes out when its frame is heard.
    void sendLoop();
    render::RenderEngine& m_engine;
    std::vector<std::unique_ptr<Open>> m_open;
    /// Inputs closed while the stream may still be reading them: kept until destruction,
    /// which is after the stream has stopped (EngineCore::setMidiInput's contract).
    std::vector<std::unique_ptr<MidiInput>> m_retired;

    /// Guards m_outs against the sender thread; never taken on the audio thread.
    mutable std::mutex m_outLock;
    std::vector<std::unique_ptr<OpenOut>> m_outs;
    std::vector<std::unique_ptr<MidiOutput>> m_retiredOut;
    std::atomic<bool> m_stop{false};
    std::atomic<std::uint64_t> m_sent{0};
    /// The stream's output latency, read when an output opens (main thread).
    std::atomic<std::int64_t> m_latencyNanos{0};
    std::thread m_sender;
};

} // namespace adx::midi
