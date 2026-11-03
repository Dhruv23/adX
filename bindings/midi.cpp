// MIDI devices on the engine (phase_9.md §4.1): which inputs exist, opening them, and
// which channel live input plays. The engine owns the devices: an input's messages go
// straight from the device thread into the engine's ring, never through Python.

#include <memory>
#include <stdexcept>
#include <string>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "bindings/EngineHandle.h"
#include "engine/midi/MidiBackend.h"

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
            [](EngineHandle& self, const std::string& name) {
                adx::midi::MidiDevices& devices = devicesOf(self);
                std::string error;
                bool ok = false;
                {
                    const py::gil_scoped_release released;
                    ok = devices.open(name, error);
                }
                if (!ok) {
                    throw std::runtime_error(error);
                }
            },
            py::arg("name"), "Open a MIDI input; its notes play the MIDI target channel.")
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
            "started.");
}
