// MIDI devices on the engine (phase_9.md §4.1): which inputs exist, opening them, and
// which channel live input plays. The engine owns the devices: an input's messages go
// straight from the device thread into the engine's ring, never through Python.

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "bindings/EngineHandle.h"
#include "engine/midi/MidiBackend.h"
#include "engine/record/MidiTake.h"

namespace py = pybind11;
using adx::bindings::EngineHandle;

namespace {

adx::midi::MidiDevices& devicesOf(EngineHandle& self) {
    if (!self.opened) {
        throw std::runtime_error("open MIDI inputs after the engine has started");
    }
    if (!self.midi) {
        self.midi = std::make_unique<adx::midi::MidiDevices>(*self.engine);
    }
    return *self.midi;
}

} // namespace

// NOLINTNEXTLINE(readability-function-size) - one .def per binding, nothing else.
void defineEngineMidi(py::class_<EngineHandle>& engine) {
    engine
        // GIL: released - asks the MIDI service for its ports
        .def(
            "midi_inputs",
            [](EngineHandle& self) {
                adx::midi::MidiDevices const& devices = devicesOf(self);
                const py::gil_scoped_release released;
                static_cast<void>(devices);
                return adx::midi::MidiDevices::inputs();
            },
            "The system's MIDI input ports, by name.")
        .def(
            "open_midi",
            [](EngineHandle& self, const std::string& name, bool sysex) {
                adx::midi::MidiDevices& devices = devicesOf(self);
                std::string error;
                bool ok = false;
                {
                    const py::gil_scoped_release released;
                    ok = devices.open(name, error, sysex);
                }
                if (!ok) {
                    throw std::runtime_error(error);
                }
            },
            py::arg("name"), py::arg("sysex") = false,
            "Open a MIDI input; its notes play the MIDI target channel. With sysex, its "
            "SysEx is kept for drain_sysex.")
        // GIL: trivial - copies the few messages queued since the last frame
        .def(
            "drain_sysex",
            [](EngineHandle& self) {
                py::list out;
                if (!self.midi) {
                    return out;
                }
                for (auto& [name, bytes] : self.midi->drainSysex()) {
                    out.append(
                        py::make_tuple(name, py::bytes(reinterpret_cast<const char*>(bytes.data()),
                                                       bytes.size())));
                }
                return out;
            },
            "Every whole SysEx message received since the last call: (input, bytes).")
        // GIL: trivial - atomic loads
        .def_property_readonly(
            "sysex_dropped",
            [](EngineHandle& self) { return self.midi ? self.midi->sysexDropped() : 0; })
        .def(
            "send_sysex",
            [](EngineHandle& self, const std::string& name, const std::vector<std::uint8_t>& bytes,
               int gapMillis) {
                adx::midi::MidiDevices& devices = devicesOf(self);
                std::string error;
                if (!devices.sendSysex(name, bytes, gapMillis, error)) {
                    throw std::runtime_error(error);
                }
            }, // GIL: trivial - queues one message for the sender thread
            py::arg("name"), py::arg("message"), py::arg("gap_ms") = 0,
            "Queue one SysEx message (F0..F7) for an open output, gap_ms after the last.")
        // GIL: trivial - a locked search
        .def(
            "midi_output_slot",
            [](EngineHandle& self, const std::string& name) {
                return self.midi ? self.midi->outputSlot(name) : -1;
            },
            py::arg("name"), "The slot a midiout channel's `port` names for an open output.")
        // GIL: trivial - one locked size()
        .def(
            "sysex_pending",
            [](EngineHandle& self, const std::string& name) {
                return self.midi ? self.midi->sysexPending(name) : std::size_t{0};
            },
            py::arg("name"))
        .def(
            "close_midi",
            [](EngineHandle& self, const std::string& name) {
                adx::midi::MidiDevices& devices = devicesOf(self);
                const py::gil_scoped_release released;
                devices.close(name);
            },
            py::arg("name"))
        // GIL: trivial - copies a few names
        .def_property_readonly("midi_opened",
                               [](EngineHandle& self) {
                                   return self.midi ? self.midi->opened()
                                                    : std::vector<std::string>{};
                               })
        // GIL: trivial - posts one message to the audio thread
        .def(
            "set_midi_target",
            [](EngineHandle& self, std::uint32_t channel) {
                self.engine->setMidiTarget(adx::core::ChannelId{channel});
            },
            py::arg("channel"),
            "The channel live MIDI input plays (0: none). Held notes release where they "
            "started.")
        // GIL: released - asks the MIDI service for its ports
        .def(
            "midi_outputs",
            [](EngineHandle& self) {
                static_cast<void>(devicesOf(self));
                const py::gil_scoped_release released;
                return adx::midi::MidiDevices::outputs();
            },
            "The system's MIDI output ports, by name.")
        .def(
            "open_midi_output",
            [](EngineHandle& self, const std::string& name) {
                adx::midi::MidiDevices& devices = devicesOf(self);
                std::string error;
                bool ok = false;
                {
                    const py::gil_scoped_release released;
                    ok = devices.openOutput(name, error);
                }
                if (!ok) {
                    throw std::runtime_error(error);
                }
            },
            py::arg("name"), "Open a MIDI output (P9-5): clock, time code and send_midi.")
        .def(
            "close_midi_output",
            [](EngineHandle& self, const std::string& name) {
                adx::midi::MidiDevices& devices = devicesOf(self);
                const py::gil_scoped_release released;
                devices.closeOutput(name);
            },
            py::arg("name"))
        // GIL: trivial - copies a few names
        .def_property_readonly("midi_outputs_opened",
                               [](EngineHandle& self) {
                                   return self.midi ? self.midi->openedOutputs()
                                                    : std::vector<std::string>{};
                               })
        .def(
            "send_midi",
            [](EngineHandle& self, const std::string& name,
               const std::vector<std::uint8_t>& bytes) {
                adx::midi::MidiDevices& devices = devicesOf(self);
                std::string error;
                bool ok = false;
                {
                    const py::gil_scoped_release released;
                    ok = devices.send(name, bytes, error);
                }
                if (!ok) {
                    throw std::runtime_error(error);
                }
            },
            py::arg("name"), py::arg("message"),
            "Send one message to an open output now (a controller's LEDs, a script).")
        // GIL: trivial - two atomic stores
        .def(
            "set_midi_sync",
            [](EngineHandle& self, const std::string& name, bool clock, bool mtc) {
                if (!devicesOf(self).setSync(name, clock, mtc)) {
                    throw std::runtime_error("not open: " + name);
                }
            },
            py::arg("name"), py::arg("clock") = true, py::arg("mtc") = false,
            "Whether an open output is sent MIDI clock and MIDI time code (30 fps).")
        // GIL: trivial - atomic loads
        .def_property_readonly("midi_sync_counts",
                               [](EngineHandle& self) {
                                   py::dict out;
                                   out["sent"] = self.midi ? self.midi->sent() : 0;
                                   out["lost"] = self.midi ? self.midi->lostOut() : 0;
                                   return out;
                               })
        // GIL: trivial - posts one message to the audio thread
        .def(
            "set_sync_follow",
            [](EngineHandle& self, const std::string& source) {
                adx::graph::SyncFollow follow = adx::graph::SyncFollow::None;
                if (source == "clock") {
                    follow = adx::graph::SyncFollow::Clock;
                } else if (source == "mtc") {
                    follow = adx::graph::SyncFollow::Mtc;
                } else if (source != "none") {
                    throw std::invalid_argument("follow 'none', 'clock' or 'mtc'");
                }
                self.engine->setSyncFollow(follow);
            },
            py::arg("source"),
            "The arrangement follows incoming MIDI clock ('clock'), time code ('mtc'), or "
            "neither ('none'), from any open input.")
        // GIL: trivial - one atomic store
        .def(
            "record_midi",
            [](EngineHandle& self, bool on) {
                self.engine->core().setRecordingMidi(on);
                if (on) {
                    self.take.clear();
                }
            },
            py::arg("on"),
            "Log what MIDI inputs play while the arrangement rolls: a MIDI take (P9-6).")
        // GIL: trivial - drains a ring into a vector
        .def(
            "drain_midi_take",
            [](EngineHandle& self) {
                adx::midi::TakeEvent event;
                std::size_t got = 0;
                while (self.engine->core().takeMidi(event)) {
                    self.take.push_back(event);
                    ++got;
                }
                return got;
            },
            "Move logged take messages off the audio thread; how many arrived. Call at UI "
            "rate while recording, so the log never fills.")
        .def_property_readonly("midi_take_size",
                               [](const EngineHandle& self) { return self.take.size(); })
        // GIL: trivial - a take is a few thousand messages, then one undo group
        .def(
            "commit_midi_take",
            [](EngineHandle& self, adx::bindings::ProjectHandle& project, std::uint32_t channel,
               std::int64_t origin, std::int64_t stop) {
                adx::midi::TakeEvent event;
                while (self.engine->core().takeMidi(event)) {
                    self.take.push_back(event);
                }
                std::string name =
                    adx::record::commitTake(self.take, origin, stop, adx::core::ChannelId{channel},
                                            project.project, project.stack);
                self.take.clear();
                return name;
            },
            py::arg("project"), py::arg("channel"), py::arg("origin"), py::arg("stop"),
            "The take as a pattern of `channel`'s notes from `origin`'s bar, on a new "
            "playlist track, one undo step; its name, or '' when nothing was played.");
}
