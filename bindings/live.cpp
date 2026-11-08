// The session on Project and Engine (phase_11.md): the [SESSION] model read and written
// as plain dicts, and launching, stopping and firing scenes on the engine. The cells'
// states are read straight from the audio thread's atomics, at UI rate.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "bindings/EngineHandle.h"
#include "bindings/ProjectHandle.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/SessionCommands.h"
#include "engine/session/ClipState.h"

namespace py = pybind11;
using adx::bindings::EngineHandle;
using adx::bindings::ProjectHandle;

namespace {

py::dict sessionInfo(const ProjectHandle& handle) {
    const adx::project::Project& project = handle.project;
    const adx::project::SessionModel& model = project.session;
    py::dict out;
    out["quantize"] = adx::session::toString(model.quantize);
    py::list scenes;
    for (const adx::project::SessionScene& scene : model.scenes) {
        py::dict s;
        s["name"] = scene.name;
        s["tempo"] = scene.tempo ? py::object(py::float_(*scene.tempo)) : py::object(py::none());
        scenes.append(s);
    }
    out["scenes"] = scenes;
    py::list tracks;
    for (const adx::project::SessionTrack& track : model.tracks) {
        py::dict t;
        const adx::project::Channel* channel = project.find(track.channel);
        t["channel"] = channel != nullptr ? channel->name : std::string{};
        t["channel_id"] = track.channel.value;
        t["linked"] = track.linked.value;
        t["stop_on_empty"] = track.stopOnEmptyScene;
        py::list slots;
        for (const std::optional<adx::project::ClipSlot>& slot : track.slots) {
            const adx::project::Pattern* pattern =
                slot.has_value() ? project.find(slot.value().pattern) : nullptr;
            if (!slot.has_value() || pattern == nullptr) {
                slots.append(py::none());
                continue;
            }
            const adx::project::ClipSlot& cell = slot.value();
            py::dict c;
            c["pattern"] = pattern->name;
            c["length"] = cell.length.value;
            c["loop"] = cell.loop;
            c["quantize"] = adx::session::toString(cell.quantize);
            c["follow"] = adx::session::toString(cell.follow);
            c["repeats"] = cell.followRepeats;
            c["legato"] = cell.legato;
            c["gain"] = cell.gain;
            slots.append(c);
        }
        t["slots"] = slots;
        tracks.append(t);
    }
    out["tracks"] = tracks;
    return out;
}

adx::project::SessionModel modelOf(const ProjectHandle& handle, const py::dict& info) {
    const adx::project::Project& project = handle.project;
    adx::project::SessionModel model;
    if (info.contains("quantize")) {
        const auto text = info["quantize"].cast<std::string>();
        if (!adx::session::parseQuantize(text.c_str(), model.quantize) ||
            model.quantize == adx::session::LaunchQuantize::Global) {
            throw py::value_error("unknown launch quantize '" + text + "'");
        }
    }
    if (info.contains("scenes")) {
        for (const py::handle item : info["scenes"].cast<py::list>()) {
            const auto s = item.cast<py::dict>();
            adx::project::SessionScene scene;
            scene.name = s.contains("name") ? s["name"].cast<std::string>() : std::string{};
            if (s.contains("tempo") && !s["tempo"].is_none()) {
                scene.tempo = s["tempo"].cast<double>();
            }
            model.scenes.push_back(std::move(scene));
        }
    }
    if (info.contains("tracks")) {
        for (const py::handle item : info["tracks"].cast<py::list>()) {
            const auto t = item.cast<py::dict>();
            const auto name = t["channel"].cast<std::string>();
            const adx::project::Channel* channel = project.findChannelByName(name);
            if (channel == nullptr) {
                throw py::value_error("no channel named '" + name + "'");
            }
            adx::project::SessionTrack track;
            track.channel = channel->id;
            if (t.contains("linked")) {
                track.linked = adx::core::PlaylistTrackId{t["linked"].cast<std::uint32_t>()};
            }
            if (t.contains("stop_on_empty")) {
                track.stopOnEmptyScene = t["stop_on_empty"].cast<bool>();
            }
            if (t.contains("slots")) {
                for (const py::handle cell : t["slots"].cast<py::list>()) {
                    if (cell.is_none()) {
                        track.slots.emplace_back();
                        continue;
                    }
                    const auto c = cell.cast<py::dict>();
                    const auto patternName = c["pattern"].cast<std::string>();
                    const adx::project::Pattern* pattern = project.findPatternByName(patternName);
                    if (pattern == nullptr) {
                        throw py::value_error("no pattern named '" + patternName + "'");
                    }
                    adx::project::ClipSlot slot;
                    slot.pattern = pattern->id;
                    if (c.contains("length")) {
                        slot.length = adx::core::Ticks{c["length"].cast<std::int64_t>()};
                    }
                    if (c.contains("loop")) {
                        slot.loop = c["loop"].cast<bool>();
                    }
                    if (c.contains("quantize")) {
                        const auto q = c["quantize"].cast<std::string>();
                        if (!adx::session::parseQuantize(q.c_str(), slot.quantize)) {
                            throw py::value_error("unknown launch quantize '" + q + "'");
                        }
                    }
                    if (c.contains("follow")) {
                        const auto f = c["follow"].cast<std::string>();
                        if (!adx::session::parseFollow(f.c_str(), slot.follow)) {
                            throw py::value_error("unknown follow action '" + f + "'");
                        }
                    }
                    if (c.contains("repeats")) {
                        slot.followRepeats =
                            static_cast<std::uint8_t>(std::clamp(c["repeats"].cast<int>(), 1, 255));
                    }
                    if (c.contains("legato")) {
                        slot.legato = c["legato"].cast<bool>();
                    }
                    if (c.contains("gain")) {
                        slot.gain = std::clamp(c["gain"].cast<float>(), 0.0F, 4.0F);
                    }
                    track.slots.emplace_back(slot);
                }
            }
            if (track.slots.size() < model.scenes.size()) {
                track.slots.resize(model.scenes.size());
            }
            model.tracks.push_back(std::move(track));
        }
    }
    return model;
}

std::uint32_t checked(std::int64_t value, const char* what) {
    if (value < 0 || value >= std::int64_t{0xFFFFFFFF}) {
        throw py::value_error(std::string("bad ") + what);
    }
    return static_cast<std::uint32_t>(value);
}

} // namespace

namespace adx::bindings {

void defineProjectLive(py::class_<ProjectHandle>& project) {
    project
        // GIL: trivial - a grid of a few dozen cells
        .def("session_info", &sessionInfo,
             "The session: quantize, scenes [{name, tempo}], tracks [{channel, linked, "
             "stop_on_empty, slots: [None | {pattern, length, loop, quantize, follow, repeats, "
             "legato, gain}]}].")
        // GIL: trivial - one command
        .def(
            "set_session",
            [](ProjectHandle& handle, const py::dict& info) {
                handle.stack.execute(std::make_unique<project::SetSession>(modelOf(handle, info)),
                                     handle.project);
            },
            py::arg("info"), "Replace the session (session_info's shape), one undo step.");
}

void defineEngineLive(py::class_<EngineHandle>& engine) {
    engine
        // GIL: trivial - each posts one message to the audio thread
        .def(
            "launch_clip",
            [](EngineHandle& self, std::int64_t track, std::int64_t row, float gain) {
                self.engine->launchClip(checked(track, "track"), checked(row, "row"),
                                        std::clamp(gain, 0.0F, 4.0F));
            },
            py::arg("track"), py::arg("row"), py::arg("gain") = 0.0F,
            "Launch a cell's clip on its quantize boundary; `gain` (a pad's) times the cell's.")
        // GIL: trivial - posts a message or reads an atomic
        .def(
            "stop_clip",
            [](EngineHandle& self, std::int64_t track) {
                self.engine->stopClip(checked(track, "track"));
            },
            py::arg("track"), "Stop a track's clip on its quantize boundary.")
        // GIL: trivial - posts a message or reads an atomic
        .def(
            "fire_scene",
            [](EngineHandle& self, std::int64_t row, float gain) {
                self.engine->fireScene(checked(row, "row"), std::clamp(gain, 0.0F, 4.0F));
            },
            py::arg("row"), py::arg("gain") = 0.0F,
            "Launch every track's cell in a scene row (stopping empty tracks).")
        // GIL: trivial - posts a message or reads an atomic
        .def(
            "stop_all_clips", [](EngineHandle& self) { self.engine->stopAllClips(); },
            "Stop every clip on its boundary.")
        // GIL: trivial - atomic loads
        .def(
            "clip_state",
            [](const EngineHandle& self, std::uint32_t track, std::uint32_t row) {
                return std::string(
                    session::toString(self.engine->core().session().state(track, row)));
            },
            py::arg("track"), py::arg("row"),
            "A cell's state: stopped, queued, playing, queued-stop or retriggered.")
        // GIL: trivial - posts a message or reads an atomic
        .def(
            "clip_progress",
            [](const EngineHandle& self, std::uint32_t track) {
                return self.engine->core().session().progress(track);
            },
            py::arg("track"), "Where a track's playing clip is in its loop, 0..1; -1 for none.")
        // GIL: trivial - posts a message or reads an atomic
        .def_property_readonly("session_counts", [](const EngineHandle& self) {
            py::dict out;
            out["active"] = self.engine->core().session().active();
            out["refused"] = self.engine->core().session().refused();
            return out;
        });
}

} // namespace adx::bindings
