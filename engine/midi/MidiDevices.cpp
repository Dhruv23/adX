// adx-thread: main
#include <algorithm>
#include <chrono>
#include <exception>
#include <span>

#include <RtMidi.h>

#include "engine/midi/DeviceNames.h"
#include "engine/midi/MidiBackend.h"
#include "engine/midi/MidiInput.h"
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
};

MidiDevices::MidiDevices(render::RenderEngine& engine) : m_engine(engine) {}

MidiDevices::~MidiDevices() {
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

bool MidiDevices::open(const std::string& name, std::string& error) {
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
        open->port->ignoreTypes(true, false, true);
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

std::uint64_t MidiDevices::lost() const {
    std::uint64_t total = 0;
    for (const auto& open : m_open) {
        total += open->input->lostCount();
    }
    return total;
}

} // namespace adx::midi
