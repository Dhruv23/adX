// adx-thread: main
#include <algorithm>
#include <chrono>
#include <exception>
#include <span>

#include <RtMidi.h>

#include "engine/midi/DeviceNames.h"
#include "engine/midi/MidiBackend.h"
#include "engine/midi/MidiInput.h"
#include "engine/midi/MidiOutput.h"
#include "engine/midi/sysex/SysexAssembler.h"
#include "engine/midi/sysex/SysexQueue.h"
#include "engine/render/RenderEngine.h"

namespace adx::midi {

namespace {

std::int64_t nowNanos() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// RtMidi's callback, on RtMidi's thread: the device thread.
void onMessage(double /*delta*/, std::vector<unsigned char>* message, void* user) {
    if (message == nullptr || user == nullptr) {
        return;
    }
    static_cast<MidiInput*>(user)->onMessage(
        nowNanos(), std::span<const std::uint8_t>(message->data(), message->size()));
}

/// RtMidi reports errors by throwing; this layer reports them as text.
template<class Body> bool guarded(std::string& error, Body&& body) {
    try {
        body();
        return true;
    } catch (const RtMidiError& failure) {
        error = failure.getMessage();
    } catch (const std::exception& failure) {
        error = failure.what();
    }
    return false;
}

} // namespace

struct MidiDevices::Open {
    std::string name;
    std::size_t slot{0};
    std::unique_ptr<MidiInput> input;
    std::unique_ptr<RtMidiIn> port;
    /// Present when the input keeps SysEx: filled on the device thread, drained here.
    std::unique_ptr<SysexQueue> sysex;
    std::unique_ptr<SysexAssembler> assembler;
};

struct MidiDevices::OpenOut {
    std::string name;
    std::size_t slot{0};
    std::unique_ptr<MidiOutput> output;
    std::unique_ptr<RtMidiOut> port;
    /// Sender thread only: popped, not yet due.
    std::vector<OutMessage> waiting;
    /// Under m_outLock: SysEx to send, in order, each with the gap to leave after it.
    std::deque<std::pair<std::vector<std::uint8_t>, int>> sysex;
    std::int64_t sysexNext{0};
};

MidiDevices::MidiDevices(render::RenderEngine& engine) : m_engine(engine) {}

MidiDevices::~MidiDevices() {
    m_stop.store(true, std::memory_order_release);
    if (m_sender.joinable()) {
        m_sender.join();
    }
    for (const auto& out : m_outs) {
        std::string ignored;
        static_cast<void>(guarded(ignored, [&] { out->port->closePort(); }));
        m_engine.core().setMidiOutput(out->slot, nullptr);
    }
    for (const auto& open : m_open) {
        std::string ignored;
        static_cast<void>(guarded(ignored, [&] { open->port->closePort(); }));
        m_engine.core().setMidiInput(open->slot, nullptr);
    }
}

std::vector<std::string> MidiDevices::inputs() {
    std::vector<std::string> names;
    std::string ignored;
    static_cast<void>(guarded(ignored, [&] {
        RtMidiIn probe;
        const unsigned int count = probe.getPortCount();
        for (unsigned int i = 0; i < count; ++i) {
            names.push_back(probe.getPortName(i));
        }
    }));
    return names;
}

bool MidiDevices::open(const std::string& name, std::string& error, bool sysex) {
    if (std::ranges::any_of(m_open, [&](const auto& open) { return open->name == name; })) {
        error = "already open: " + name;
        return false;
    }
    std::size_t slot = 0;
    while (slot < graph::EngineCore::kMaxMidiInputs &&
           std::ranges::any_of(m_open, [&](const auto& open) { return open->slot == slot; })) {
        ++slot;
    }
    if (slot == graph::EngineCore::kMaxMidiInputs) {
        error = "every MIDI input slot is in use";
        return false;
    }
    auto open = std::make_unique<Open>();
    open->name = name;
    open->slot = slot;
    open->input = std::make_unique<MidiInput>(
        m_engine.core().midiClock(), m_engine.options().sampleRate, m_engine.options().blockFrames,
        static_cast<std::uint8_t>(slot));
    if (sysex) {
        open->sysex = std::make_unique<SysexQueue>();
        open->assembler = std::make_unique<SysexAssembler>(*open->sysex);
        open->input->attachSysex(open->assembler.get());
    }
    const bool ok = guarded(error, [&] {
        open->port = std::make_unique<RtMidiIn>();
        unsigned int index = open->port->getPortCount();
        for (unsigned int i = 0; i < open->port->getPortCount(); ++i) {
            if (open->port->getPortName(i) == name) {
                index = i;
                break;
            }
        }
        if (index == open->port->getPortCount()) {
            throw RtMidiError("no MIDI input named " + name, RtMidiError::INVALID_DEVICE);
        }
        // SysEx, timing and active sensing are ignored at the source; clock is let
        // through for following (MidiClock.h).
        open->port->ignoreTypes(!sysex, false, true);
        open->port->setCallback(&onMessage, open->input.get());
        open->port->openPort(index, "adX");
    });
    if (!ok) {
        return false;
    }
    m_engine.core().setMidiInput(slot, open->input.get());
    setDeviceName(slot, name);
    m_open.push_back(std::move(open));
    return true;
}

void MidiDevices::close(const std::string& name) {
    const auto found =
        std::ranges::find_if(m_open, [&](const auto& open) { return open->name == name; });
    if (found == m_open.end()) {
        return;
    }
    std::string ignored;
    static_cast<void>(guarded(ignored, [&] { (*found)->port->closePort(); }));
    (*found)->input->attachSysex(nullptr); // the port is closed: no callback is running
    m_engine.core().setMidiInput((*found)->slot, nullptr);
    clearDeviceName((*found)->slot);
    m_retired.push_back(std::move((*found)->input));
    m_open.erase(found);
}

std::vector<std::string> MidiDevices::opened() const {
    std::vector<std::string> names;
    names.reserve(m_open.size());
    for (const auto& open : m_open) {
        names.push_back(open->name);
    }
    return names;
}

std::vector<std::pair<std::string, std::vector<std::uint8_t>>> MidiDevices::drainSysex() {
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> out;
    SysexMessage message;
    for (const auto& open : m_open) {
        if (!open->sysex) {
            continue;
        }
        while (open->sysex->pop(message)) {
            const auto view = message.view();
            out.emplace_back(open->name, std::vector<std::uint8_t>(view.begin(), view.end()));
        }
    }
    return out;
}

std::uint64_t MidiDevices::sysexDropped() const {
    std::uint64_t total = 0;
    for (const auto& open : m_open) {
        if (open->sysex) {
            const SysexStats& stats = open->assembler->stats();
            total +=
                open->sysex->droppedCount() + stats.oversize + stats.interrupted + stats.timedOut;
        }
    }
    return total;
}

std::uint64_t MidiDevices::lost() const {
    std::uint64_t total = 0;
    for (const auto& open : m_open) {
        total += open->input->lostCount();
    }
    return total;
}

std::vector<std::string> MidiDevices::outputs() {
    std::vector<std::string> names;
    std::string ignored;
    static_cast<void>(guarded(ignored, [&] {
        RtMidiOut probe;
        const unsigned int count = probe.getPortCount();
        for (unsigned int i = 0; i < count; ++i) {
            names.push_back(probe.getPortName(i));
        }
    }));
    return names;
}

bool MidiDevices::openOutput(const std::string& name, std::string& error) {
    const std::scoped_lock lock(m_outLock);
    if (std::ranges::any_of(m_outs, [&](const auto& out) { return out->name == name; })) {
        error = "already open: " + name;
        return false;
    }
    std::size_t slot = 0;
    while (slot < graph::EngineCore::kMaxMidiOutputs &&
           std::ranges::any_of(m_outs, [&](const auto& out) { return out->slot == slot; })) {
        ++slot;
    }
    if (slot == graph::EngineCore::kMaxMidiOutputs) {
        error = "every MIDI output slot is in use";
        return false;
    }
    auto out = std::make_unique<OpenOut>();
    out->name = name;
    out->slot = slot;
    out->output = std::make_unique<MidiOutput>();
    out->waiting.reserve(MidiOutput::kRingCapacity);
    const bool ok = guarded(error, [&] {
        out->port = std::make_unique<RtMidiOut>();
        unsigned int index = out->port->getPortCount();
        for (unsigned int i = 0; i < out->port->getPortCount(); ++i) {
            if (out->port->getPortName(i) == name) {
                index = i;
                break;
            }
        }
        if (index == out->port->getPortCount()) {
            throw RtMidiError("no MIDI output named " + name, RtMidiError::INVALID_DEVICE);
        }
        out->port->openPort(index, "adX");
    });
    if (!ok) {
        return false;
    }
    m_latencyNanos.store(static_cast<std::int64_t>(m_engine.audio().info().outputLatencyMs * 1e6),
                         std::memory_order_release);
    m_engine.core().setMidiOutput(slot, out->output.get());
    m_outs.push_back(std::move(out));
    if (!m_sender.joinable()) {
        m_sender = std::thread([this] { sendLoop(); });
    }
    return true;
}

void MidiDevices::closeOutput(const std::string& name) {
    const std::scoped_lock lock(m_outLock);
    const auto found =
        std::ranges::find_if(m_outs, [&](const auto& out) { return out->name == name; });
    if (found == m_outs.end()) {
        return;
    }
    std::string ignored;
    static_cast<void>(guarded(ignored, [&] { (*found)->port->closePort(); }));
    m_engine.core().setMidiOutput((*found)->slot, nullptr);
    m_retiredOut.push_back(std::move((*found)->output));
    m_outs.erase(found);
}

std::vector<std::string> MidiDevices::openedOutputs() const {
    const std::scoped_lock lock(m_outLock);
    std::vector<std::string> names;
    names.reserve(m_outs.size());
    for (const auto& out : m_outs) {
        names.push_back(out->name);
    }
    return names;
}

bool MidiDevices::send(const std::string& name, std::span<const std::uint8_t> bytes,
                       std::string& error) {
    if (bytes.empty() || (bytes[0] & 0x80U) == 0) {
        error = "a MIDI message starts with a status byte";
        return false;
    }
    const std::scoped_lock lock(m_outLock);
    const auto found =
        std::ranges::find_if(m_outs, [&](const auto& out) { return out->name == name; });
    if (found == m_outs.end()) {
        error = "not open: " + name;
        return false;
    }
    return guarded(error, [&] { (*found)->port->sendMessage(bytes.data(), bytes.size()); });
}

bool MidiDevices::sendSysex(const std::string& name, std::span<const std::uint8_t> bytes,
                            int gapMillis, std::string& error) {
    if (bytes.size() < 2 || bytes.front() != 0xF0 || bytes.back() != 0xF7) {
        error = "a SysEx message runs from F0 to F7";
        return false;
    }
    const std::scoped_lock lock(m_outLock);
    const auto found =
        std::ranges::find_if(m_outs, [&](const auto& out) { return out->name == name; });
    if (found == m_outs.end()) {
        error = "not open: " + name;
        return false;
    }
    (*found)->sysex.emplace_back(std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
                                 std::max(0, gapMillis));
    return true;
}

int MidiDevices::outputSlot(const std::string& name) const {
    const std::scoped_lock lock(m_outLock);
    const auto found =
        std::ranges::find_if(m_outs, [&](const auto& out) { return out->name == name; });
    return found == m_outs.end() ? -1 : static_cast<int>((*found)->slot);
}

std::size_t MidiDevices::sysexPending(const std::string& name) const {
    const std::scoped_lock lock(m_outLock);
    const auto found =
        std::ranges::find_if(m_outs, [&](const auto& out) { return out->name == name; });
    return found == m_outs.end() ? 0 : (*found)->sysex.size();
}

bool MidiDevices::setSync(const std::string& name, bool clock, bool mtc) {
    const std::scoped_lock lock(m_outLock);
    const auto found =
        std::ranges::find_if(m_outs, [&](const auto& out) { return out->name == name; });
    if (found == m_outs.end()) {
        return false;
    }
    (*found)->output->sync().clock.store(clock, std::memory_order_release);
    (*found)->output->sync().mtc.store(mtc, std::memory_order_release);
    return true;
}

std::uint64_t MidiDevices::lostOut() const {
    const std::scoped_lock lock(m_outLock);
    std::uint64_t total = 0;
    for (const auto& out : m_outs) {
        total += out->output->lostCount();
    }
    return total;
}

void MidiDevices::sendLoop() {
    const double rate = m_engine.options().sampleRate;
    while (!m_stop.load(std::memory_order_acquire)) {
        {
            const std::scoped_lock lock(m_outLock);
            std::uint64_t frame = 0;
            std::int64_t wall = 0;
            const bool clocked = m_engine.core().midiClock().read(frame, wall);
            // A message's block is heard one output latency after it started.
            const std::int64_t latency = m_latencyNanos.load(std::memory_order_acquire);
            const std::int64_t now = nowNanos();
            for (const auto& out : m_outs) {
                OutMessage message;
                while (out->waiting.size() < out->waiting.capacity() && out->output->pop(message)) {
                    out->waiting.push_back(message);
                }
                std::size_t kept = 0;
                for (const OutMessage& due : out->waiting) {
                    const auto since =
                        static_cast<double>(static_cast<std::int64_t>(due.streamFrame) -
                                            static_cast<std::int64_t>(frame));
                    const std::int64_t at =
                        clocked ? wall + static_cast<std::int64_t>(since * 1e9 / rate) + latency
                                : now;
                    if (at > now) {
                        out->waiting[kept++] = due;
                        continue;
                    }
                    std::string ignored;
                    static_cast<void>(guarded(
                        ignored, [&] { out->port->sendMessage(due.bytes.data(), due.size); }));
                    m_sent.fetch_add(1, std::memory_order_acq_rel);
                }
                out->waiting.resize(kept);
                if (!out->sysex.empty() && now >= out->sysexNext) {
                    const auto& [bytes, gap] = out->sysex.front();
                    std::string ignored;
                    static_cast<void>(guarded(
                        ignored, [&] { out->port->sendMessage(bytes.data(), bytes.size()); }));
                    out->sysexNext = now + (static_cast<std::int64_t>(gap) * 1'000'000);
                    out->sysex.pop_front();
                }
            }
        }
        // A millisecond: MIDI clock at 300 bpm is a pulse every 8 ms.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

} // namespace adx::midi
