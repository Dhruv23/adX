// Live performance on the engine (phase_11.md §4.6-4.8): pads, their lights, tempo
// nudge and tap, and performance recording. Pad profiles parse and compile here; the
// map goes to the audio thread, the lights run on their own thread (PadFeedback).

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "bindings/EngineHandle.h"
#include "bindings/ProjectHandle.h"
#include "engine/geometry/SessionGeometry.h"
#include "engine/midi/DeviceNames.h"
#include "engine/midi/PadController.h"
#include "engine/session/ClipState.h"

namespace py = pybind11;
using adx::bindings::EngineHandle;
using adx::bindings::ProjectHandle;

namespace {

const char* lookName(adx::midi::PadLook look) {
    switch (look) {
    case adx::midi::PadLook::Empty:
        return "empty";
    case adx::midi::PadLook::Stopped:
        return "stopped";
    case adx::midi::PadLook::Queued:
        return "queued";
    case adx::midi::PadLook::Playing:
        return "playing";
    case adx::midi::PadLook::QueuedStop:
        return "queued-stop";
    case adx::midi::PadLook::Recording:
        return "recording";
    }
    return "none";
}

adx::midi::PadProfile parsedOrThrow(const std::string& text) {
    adx::midi::PadParse parse = adx::midi::parsePadProfile(text);
    if (!parse.ok) {
        throw py::value_error("not a pad profile: " +
                              (parse.warnings.empty() ? std::string{} : parse.warnings.front()));
    }
    return std::move(parse.profile);
}

py::dict profileDict(const adx::midi::PadProfile& p) {
    py::dict out;
    out["name"] = p.name;
    out["match_device"] = p.matchDevice;
    out["columns"] = p.columns;
    out["rows"] = p.rows;
    py::list notes;
    for (std::uint32_t r = 0; r < p.rows; ++r) {
        py::list row;
        for (std::uint32_t c = 0; c < p.columns; ++c) {
            row.append(p.noteAt(c, r));
        }
        notes.append(row);
    }
    out["notes"] = notes;
    out["channel"] = p.channel;
    out["velocity_sensitive"] = p.velocitySensitive;
    out["velocity_curve"] = p.velocityCurve;
    out["feedback"] = adx::midi::toString(p.feedback);
    out["feedback_channel"] = p.feedbackChannel;
    py::dict colors;
    for (std::size_t i = 0; i < adx::midi::kPadLooks; ++i) {
        const adx::midi::PadColor& color = p.colors.at(i);
        py::dict c;
        c["velocity"] = color.velocity;
        c["rgb"] = color.rgb;
        c["blink"] = color.blink;
        c["pulse"] = color.pulse;
        colors[lookName(static_cast<adx::midi::PadLook>(i))] = c;
    }
    out["colors"] = colors;
    const auto control = [](const adx::midi::PadControl& c) {
        py::dict d;
        d["cc"] = c.cc;
        d["numbers"] = c.numbers;
        return d;
    };
    out["scene_launch"] = control(p.sceneLaunch);
    out["stop_track"] = control(p.stopTrack);
    out["stop_all"] = control(p.stopAll);
    out["tap_tempo"] = control(p.tapTempo);
    out["unknown"] = p.unknown;
    return out;
}

adx::midi::PadLook lookOf(const EngineHandle& self, std::uint32_t column, std::uint32_t row) {
    if (column >= 64 || row >= 64 || (*self.padFilled)[(column * 64) + row].load() == 0) {
        return adx::midi::PadLook::Empty;
    }
    const adx::session::SessionEngine& session = self.engine->core().session();
    switch (session.state(column, row)) {
    case adx::session::ClipState::Queued:
    case adx::session::ClipState::Retriggered:
        return adx::midi::PadLook::Queued;
    case adx::session::ClipState::Playing:
        return session.recording() ? adx::midi::PadLook::Recording : adx::midi::PadLook::Playing;
    case adx::session::ClipState::QueuedStop:
        return adx::midi::PadLook::QueuedStop;
    case adx::session::ClipState::Stopped:
        break;
    }
    return adx::midi::PadLook::Stopped;
}

const char* kindName(adx::session::PerformanceEvent::Kind kind) {
    switch (kind) {
    case adx::session::PerformanceEvent::Kind::Pass:
        return "pass";
    case adx::session::PerformanceEvent::Kind::Stop:
        return "stop";
    case adx::session::PerformanceEvent::Kind::Rate:
        break;
    }
    return "rate";
}

std::size_t drain(EngineHandle& self) {
    std::size_t count = 0;
    adx::session::PerformanceEvent event;
    while (self.engine->core().session().takeLogged(event)) {
        self.performance.add(event);
        ++count;
    }
    return count;
}

} // namespace

namespace adx::bindings {

// NOLINTNEXTLINE(readability-function-size): one binding table
void defineEnginePads(py::class_<EngineHandle>& engine) {
    engine
        // GIL: trivial - each posts one message to the audio thread
        .def(
            "set_session_rate",
            [](EngineHandle& self, double rate) {
                if (!(rate > 0.0)) {
                    throw py::value_error("rate must be positive");
                }
                self.engine->setSessionRate(rate);
            },
            py::arg("rate"), "The master rate every clip follows: a nudge, or a set tempo.")
        // GIL: trivial - posts a message
        .def(
            "tap_tempo", [](EngineHandle& self, double seconds) { self.engine->tapTempo(seconds); },
            py::arg("seconds") = -1.0,
            "A tap of tap tempo at `seconds` on a monotonic clock (default: the audio clock).")
        // GIL: trivial - atomic loads
        .def_property_readonly(
            "session_rate",
            [](const EngineHandle& self) { return self.engine->core().session().rate(); })
        // GIL: trivial - an atomic load
        .def_property_readonly(
            "tapped_bpm",
            [](const EngineHandle& self) { return self.engine->core().session().tappedBpm(); })
        // GIL: trivial - posts a message
        .def(
            "record_performance",
            [](EngineHandle& self, bool on) {
                if (on) {
                    drain(self);
                    self.performance.clear();
                }
                self.engine->recordPerformance(on);
            },
            py::arg("on"), "Arm (clearing what was logged) or disarm the performance log.")
        // GIL: trivial - an atomic load
        .def_property_readonly(
            "performance_recording",
            [](const EngineHandle& self) { return self.engine->core().session().recording(); })
        // GIL: trivial - drains a ring of small structs
        .def(
            "drain_performance", [](EngineHandle& self) { return drain(self); },
            "Moves what the audio thread logged into the recorder; returns how many events.")
        // GIL: trivial - a short list
        .def_property_readonly("performance_events",
                               [](const EngineHandle& self) {
                                   py::list out;
                                   for (const session::PerformanceEvent& e :
                                        self.performance.events()) {
                                       py::dict d;
                                       d["kind"] = kindName(e.kind);
                                       d["track"] = e.track;
                                       d["row"] = e.row;
                                       d["tick"] = e.tick;
                                       d["rate"] = e.rate;
                                       out.append(d);
                                   }
                                   return out;
                               })
        // GIL: trivial - a few commands
        .def(
            "commit_performance",
            [](EngineHandle& self, ProjectHandle& project, std::int64_t end) {
                drain(self);
                const record::PerformanceSummary summary =
                    self.performance.commit(project.project, project.stack, end);
                self.performance.clear();
                py::dict out;
                out["items"] = summary.items;
                out["tempo_events"] = summary.tempoEvents;
                out["patterns_made"] = summary.patternsMade;
                out["tracks_made"] = summary.tracksMade;
                return out;
            },
            py::arg("project"), py::arg("end_tick") = -1,
            "Writes the logged performance into the project's arrangement as one undo step.")
        // GIL: trivial - one parse and one copy
        .def(
            "set_pad_profile",
            [](EngineHandle& self, const std::string& input, const std::string& text) {
                const midi::PadParse parse = midi::parsePadProfile(text);
                if (!parse.ok) {
                    throw py::value_error("not a pad profile");
                }
                const std::uint8_t slot = midi::deviceSlot(input);
                if (slot >= graph::EngineCore::kMaxMidiInputs) {
                    throw py::value_error("no open MIDI input called '" + input + "'");
                }
                const midi::PadMap map = midi::compilePadMap(parse.profile);
                self.engine->setPadMap(slot, &map);
                return parse.warnings;
            },
            py::arg("input"), py::arg("profile"),
            "The open input `input` becomes a pad controller speaking `profile` (its text). "
            "Returns the profile's warnings.")
        // GIL: trivial - an atomic store
        .def(
            "clear_pad_profile",
            [](EngineHandle& self, const std::string& input) {
                const std::uint8_t slot = midi::deviceSlot(input);
                if (slot < graph::EngineCore::kMaxMidiInputs) {
                    self.engine->setPadMap(slot, nullptr);
                }
            },
            py::arg("input"))
        // GIL: trivial - an atomic load
        .def_property_readonly(
            "pad_presses",
            [](const EngineHandle& self) { return self.engine->core().padPresses(); })
        // GIL: trivial - atomic stores
        .def(
            "set_pad_cells",
            [](EngineHandle& self, const std::vector<std::pair<int, int>>& cells) {
                // Built aside, then stored cell by cell: clearing first would let the
                // lights' thread catch a filled cell as empty for a tick.
                std::array<std::uint8_t, 4096> filled{};
                for (const auto& [track, row] : cells) {
                    if (track >= 0 && track < 64 && row >= 0 && row < 64) {
                        filled.at((static_cast<std::size_t>(track) * 64) +
                                  static_cast<std::size_t>(row)) = 1;
                    }
                }
                for (std::size_t i = 0; i < filled.size(); ++i) {
                    (*self.padFilled).at(i).store(filled.at(i));
                }
            },
            py::arg("cells"), "Which (track, row) cells hold a clip: the rest light as empty.")
        // GIL: released - joins the old lights' thread
        .def(
            "start_pad_feedback",
            [](EngineHandle& self, const std::string& output, const std::string& text, double hz) {
                midi::PadProfile profile = parsedOrThrow(text);
                if (!self.midi) {
                    throw std::runtime_error("open the MIDI output first");
                }
                py::gil_scoped_release const release;
                self.feedback.reset();
                midi::MidiDevices* devices = self.midi.get();
                const EngineHandle* handle = &self;
                self.feedback = std::make_unique<midi::PadFeedback>(
                    std::move(profile),
                    [handle](std::uint32_t c, std::uint32_t r) { return lookOf(*handle, c, r); },
                    // NOLINTNEXTLINE(bugprone-exception-escape): a failed send is a false
                    [devices, output](std::span<const std::uint8_t> bytes) {
                        std::string error;
                        return devices->send(output, bytes, error);
                    },
                    hz);
            },
            py::arg("output"), py::arg("profile"), py::arg("hz") = 30.0,
            "Lights the controller on `output` (open) from the clip states, at `hz`.")
        // GIL: released - joins the lights' thread
        .def("stop_pad_feedback",
             [](EngineHandle& self) {
                 py::gil_scoped_release const release;
                 self.feedback.reset();
             })
        // GIL: trivial - atomic loads
        .def_property_readonly("pad_feedback",
                               [](const EngineHandle& self) {
                                   py::dict out;
                                   out["running"] = self.feedback != nullptr;
                                   out["ticks"] = self.feedback ? self.feedback->ticks() : 0;
                                   out["sent"] = self.feedback ? self.feedback->sent() : 0;
                                   out["own_thread"] =
                                       self.feedback != nullptr &&
                                       self.feedback->thread() != std::this_thread::get_id();
                                   return out;
                               })
        // GIL: trivial - an atomic load
        .def(
            "pad_shown",
            [](const EngineHandle& self, std::uint32_t column, std::uint32_t row) {
                return self.feedback ? std::string(lookName(self.feedback->shown(column, row)))
                                     : std::string("none");
            },
            py::arg("column"), py::arg("row"), "What a pad's light last showed.");
}

void registerPads(py::module_& m) {
    py::class_<geometry::SessionGeometry>(m, "SessionGeometry")
        .def(py::init<>())
        .def(
            "build",
            [](geometry::SessionGeometry& self, const EngineHandle& engine, std::uint32_t tracks,
               std::uint32_t rows, const py::array_t<std::uint8_t, py::array::c_style>& filled,
               bool recording) {
                const std::span<const std::uint8_t> cells{filled.data(),
                                                          static_cast<std::size_t>(filled.size())};
                const py::gil_scoped_release released;
                self.build(
                    engine.engine->core().session(),
                    geometry::SessionGridSpec{
                        .tracks = tracks, .rows = rows, .filled = cells, .recording = recording});
            },
            py::arg("engine"), py::arg("tracks"), py::arg("rows"), py::arg("filled"),
            py::arg("recording") = false,
            "Every cell, ring and mark of the grid from the clip states: one call a frame.")
        .def_property_readonly("cells", &geometry::SessionGeometry::cells,
                               py::return_value_policy::reference_internal)
        .def_property_readonly("marks", &geometry::SessionGeometry::marks,
                               py::return_value_policy::reference_internal)
        .def_property_readonly("playing", &geometry::SessionGeometry::playing)
        .def_property_readonly("queued", &geometry::SessionGeometry::queued);

    // GIL: trivial - a profile is a few dozen lines
    m.def(
        "parse_pad_profile",
        [](const std::string& text) {
            const midi::PadParse parse = midi::parsePadProfile(text);
            py::dict out;
            out["ok"] = parse.ok;
            out["warnings"] = parse.warnings;
            out["profile"] = profileDict(parse.profile);
            out["text"] = midi::writePadProfile(parse.profile);
            return out;
        },
        py::arg("text"),
        "A pad profile: {ok, warnings, profile, text} - `text` is it written back.");
    // GIL: trivial - one parse
    m.def(
        "pad_profile_matches",
        [](const std::string& text, const std::string& port) {
            return midi::profileMatches(parsedOrThrow(text), port);
        },
        py::arg("profile"), py::arg("port"));
    // GIL: trivial - one parse and a few bytes
    m.def(
        "pad_message",
        [](const std::string& text, int note, const std::string& look, bool lit) {
            const midi::PadProfile profile = parsedOrThrow(text);
            for (std::size_t i = 0; i < midi::kPadLooks; ++i) {
                if (look == lookName(static_cast<midi::PadLook>(i))) {
                    const auto bytes = midi::PadFeedback::message(
                        profile, static_cast<std::uint8_t>(note & 0x7F), profile.colors.at(i), lit);
                    return py::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                }
            }
            throw py::value_error("unknown look '" + look + "'");
        },
        py::arg("profile"), py::arg("note"), py::arg("look"), py::arg("lit") = true,
        "The message that lights pad `note` as `look` (empty, stopped, queued, ...).");

    py::class_<midi::PadLearn>(m, "PadLearn",
                               "Learn mode: press each pad in order, then the scene buttons, "
                               "stop all and tap tempo (skip() past any the device lacks).")
        // GIL: trivial - learn mode is a counter and a few vectors
        .def(py::init<std::string, std::uint32_t, std::uint32_t>(), py::arg("name"),
             py::arg("columns"), py::arg("rows"))
        .def_property_readonly("prompt", &midi::PadLearn::prompt)
        .def_property_readonly("done", &midi::PadLearn::done)
        // GIL: trivial - one step
        .def(
            "press",
            [](midi::PadLearn& self, const std::string& kind, int channel, int number, int value) {
                midi::ControlMessage message;
                message.channel = static_cast<std::uint8_t>(channel & 0x0F);
                message.number = static_cast<std::uint8_t>(number & 0x7F);
                if (kind == "note") {
                    message.kind = project::MidiControlKind::Note;
                    message.pressed = value > 0;
                    message.value = static_cast<float>(value) / 127.0F;
                } else if (kind == "cc") {
                    message.kind = project::MidiControlKind::ControlChange;
                    message.value = static_cast<float>(value) / 127.0F;
                } else {
                    return false;
                }
                return self.press(message);
            },
            py::arg("kind"), py::arg("channel"), py::arg("number"), py::arg("value"),
            "A message (kind 'note' or 'cc', channel 0-15): true when it was the next step.")
        // GIL: trivial - one step
        .def("skip", &midi::PadLearn::skip)
        // GIL: trivial - a few lines of text
        .def("result",
             [](const midi::PadLearn& self) { return midi::writePadProfile(self.result()); });
}

} // namespace adx::bindings
