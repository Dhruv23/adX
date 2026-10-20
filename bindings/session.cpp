// The Project methods a Phase 6 gesture needs beyond one command: grouping several
// commands into one undo step, jumping through history, and the drops that are a pool
// entry plus a placement, or a preset plus its samples (phase_6.md §4.6, §4.8; P4-2).
//
// Each runs its commands through the project's own stack inside one group, so whatever it
// does is exactly one history entry, undone byte for byte like any other.

#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "bindings/PendingCommand.h"
#include "bindings/ProjectHandle.h"
#include "engine/preset/Preset.h"
#include "engine/preset/PresetLibrary.h"
#include "engine/project/TypeCatalog.h"
#include "engine/project/commands/ArrangeCommands.h"
#include "engine/project/commands/ChannelCommands.h"
#include "engine/project/commands/MixerCommands.h"
#include "engine/project/commands/PlaylistCommands.h"
#include "engine/project/commands/ProjectCommands.h"

namespace py = pybind11;
namespace project = adx::project;
namespace core = adx::core;

using adx::bindings::ProjectHandle;

namespace {

/// Runs `body` inside one group labelled `label`: one undo step, whatever it executes.
template<class Body> void grouped(ProjectHandle& handle, std::string_view label, Body&& body) {
    handle.stack.beginGroup(label);
    try {
        body();
    } catch (...) {
        handle.stack.endGroup();
        throw;
    }
    handle.stack.endGroup();
}

template<class T> T* run(ProjectHandle& handle, std::unique_ptr<T> command) {
    T* raw = command.get();
    handle.stack.execute(std::move(command), handle.project);
    return raw;
}

/// A path as the pool should record it: relative to the project when it is inside the
/// project's folder, absolute otherwise.
std::string poolPath(const project::Project& p, const std::filesystem::path& file) {
    if (file.is_relative() || p.resources.baseDirectory.empty()) {
        return file.generic_string();
    }
    const std::filesystem::path relative =
        file.lexically_relative(std::filesystem::path(p.resources.baseDirectory));
    if (relative.empty() || relative.begin()->string() == "..") {
        return file.generic_string();
    }
    return relative.generic_string();
}

adx::preset::Preset loadOrThrow(const std::string& path, adx::format::DiagnosticList& diagnostics) {
    adx::preset::Preset preset;
    if (!adx::preset::loadPreset(path, preset, diagnostics)) {
        throw std::runtime_error("cannot read preset " + path);
    }
    return preset;
}

py::dict presetDict(const adx::preset::Preset& preset) {
    py::dict out;
    out["name"] = preset.name;
    out["type"] = preset.type;
    out["pack"] = preset.pack;
    out["tags"] = preset.tags;
    out["path"] = preset.source.generic_string();
    out["kind"] = preset.kind() == project::TypeKind::Effect ? "effect" : "instrument";
    out["zones"] = preset.zones.size();
    out["params"] = preset.params.size();
    return out;
}

/// Applies an instrument preset to a channel: its samples into the pool, then the whole
/// instrument in one command. Called inside a group.
void applyInstrumentPreset(ProjectHandle& handle, core::ChannelId channel,
                           const adx::preset::Preset& preset) {
    if (preset.kind() != project::TypeKind::Instrument) {
        throw std::invalid_argument("'" + preset.name + "' is an effect preset");
    }
    if (handle.project.find(channel) == nullptr) {
        throw std::invalid_argument("no channel " + std::to_string(channel.value));
    }
    project::InstrumentSpec spec = adx::preset::instrumentOf(preset);
    spec.zones.clear();
    const std::filesystem::path base = preset.source.parent_path();
    for (const adx::preset::PresetZone& zone : preset.zones) {
        std::filesystem::path file(zone.path);
        if (file.is_relative()) {
            file = base / file;
        }
        auto* sample = run(handle, std::make_unique<project::AddSample>(
                                       poolPath(handle.project, file.lexically_normal())));
        project::SampleZone placed = zone.zone;
        placed.sample = sample->created();
        spec.zones.push_back(placed);
    }
    run(handle, std::make_unique<project::SetInstrument>(channel, std::move(spec)));
}

std::vector<project::SlotParam> slotParamsOf(const adx::preset::Preset& preset) {
    std::vector<project::SlotParam> params;
    params.reserve(preset.params.size());
    for (const project::ParamValue& value : preset.params) {
        params.push_back(project::SlotParam{.name = value.name, .value = value.value});
    }
    return params;
}

} // namespace

namespace adx::bindings {

// NOLINTBEGIN(bugprone-exception-escape) - every body here may throw std::invalid_argument
// or std::runtime_error, which pybind11 turns into the exception the caller sees.
namespace {

/// The reads the Phase 6 panels make that are not plain project data: the routing
/// matrix, sampler zones, and duplication.
void defineSessionQueries(py::class_<ProjectHandle>& project) {
    project
        .def(
            "routing_matrix",
            [](const ProjectHandle& handle) {
                const project::Mixer& mixer = handle.project.mixer;
                std::vector<std::uint32_t> ids;
                ids.reserve(mixer.inserts.size());
                for (const project::Insert& insert : mixer.inserts) {
                    ids.push_back(insert.id.value);
                }
                std::ranges::sort(ids);
                const std::size_t n = ids.size();
                const auto index = [&ids](core::InsertId id) {
                    const auto at = std::ranges::lower_bound(ids, id.value);
                    return at != ids.end() && *at == id.value
                               ? static_cast<std::size_t>(at - ids.begin())
                               : ids.size();
                };
                // Every edge the graph builder makes between inserts: routes, sends and
                // sidechain keys - the same edges its cycle check sees.
                std::vector<std::uint8_t> state(n * n, 0);
                std::vector<std::vector<std::size_t>> edges(n);
                const auto edge = [&](core::InsertId from, core::InsertId to, std::uint8_t kind) {
                    const std::size_t a = index(from);
                    const std::size_t b = index(to);
                    if (a < n && b < n) {
                        edges[a].push_back(b);
                        state[(a * n) + b] = std::max(state[(a * n) + b], kind);
                    }
                };
                for (const project::Route& route : mixer.routes) {
                    edge(route.from, route.to, 1);
                }
                for (const project::Insert& insert : mixer.inserts) {
                    for (const project::Send& send : insert.sends) {
                        edge(insert.id, send.target, 2);
                    }
                    for (const project::Slot& slot : insert.slots) {
                        if (slot.sidechain.valid()) {
                            edge(slot.sidechain, insert.id, 3);
                        }
                    }
                }
                // A cell that is not already an edge closes a cycle when its target reaches
                // its source: disabled before any command is issued (phase_6.md 4.3).
                std::vector<std::uint8_t> reach(n * n, 0);
                for (std::size_t start = 0; start < n; ++start) {
                    std::vector<std::size_t> stack{start};
                    while (!stack.empty()) {
                        const std::size_t at = stack.back();
                        stack.pop_back();
                        for (const std::size_t next : edges[at]) {
                            if (reach[(start * n) + next] == 0) {
                                reach[(start * n) + next] = 1;
                                stack.push_back(next);
                            }
                        }
                    }
                }
                for (std::size_t a = 0; a < n; ++a) {
                    for (std::size_t b = 0; b < n; ++b) {
                        if (a == b) {
                            state[(a * n) + b] = 5;
                        } else if (state[(a * n) + b] == 0 && reach[(b * n) + a] != 0) {
                            state[(a * n) + b] = 4;
                        }
                    }
                }
                py::array_t<std::uint8_t> out(
                    {static_cast<py::ssize_t>(n), static_cast<py::ssize_t>(n)});
                std::ranges::copy(state, out.mutable_data());
                return py::make_tuple(ids, out);
            }, // GIL: trivial - O(inserts^2) for a few hundred inserts, once per edit
            "(insert ids ascending, n x n uint8): 0 none, 1 route, 2 send, 3 sidechain key, 4 "
            "would close a cycle, 5 self. Row is the source, column the target.")
        .def(
            "add_zone",
            [](ProjectHandle& handle, std::uint32_t channel, const std::string& path, int keyLow,
               int keyHigh, int root) {
                const project::Channel* target = handle.project.find(core::ChannelId{channel});
                if (target == nullptr) {
                    throw std::invalid_argument("no channel " + std::to_string(channel));
                }
                const py::gil_scoped_release released;
                grouped(handle, "Add zone", [&] {
                    auto* sample = run(handle, std::make_unique<project::AddSample>(poolPath(
                                                   handle.project, std::filesystem::path(path))));
                    std::vector<project::SampleZone> zones =
                        handle.project.find(core::ChannelId{channel})->instrument.zones;
                    project::SampleZone zone;
                    zone.sample = sample->created();
                    zone.keyLow = static_cast<std::uint8_t>(std::clamp(keyLow, 0, 127));
                    zone.keyHigh = static_cast<std::uint8_t>(std::clamp(keyHigh, keyLow, 127));
                    zone.rootKey = static_cast<std::uint8_t>(std::clamp(root, 0, 127));
                    zones.push_back(zone);
                    run(handle, std::make_unique<project::SetChannelZones>(core::ChannelId{channel},
                                                                           std::move(zones)));
                });
            },
            py::arg("channel"), py::arg("path"), py::arg("key_low") = 0, py::arg("key_high") = 127,
            py::arg("root") = 60, "A sample dropped on the zone editor: one more zone, one step.")
        .def(
            "zones",
            [](const ProjectHandle& handle, std::uint32_t channel) {
                const project::Channel* target = handle.project.find(core::ChannelId{channel});
                py::list out;
                if (target == nullptr) {
                    return out;
                }
                for (const project::SampleZone& zone : target->instrument.zones) {
                    const project::SampleRef* sample = handle.project.resources.find(zone.sample);
                    py::dict d;
                    d["sample"] = sample != nullptr ? sample->path : std::string{};
                    d["sample_id"] = zone.sample.value;
                    d["loop"] = static_cast<int>(zone.loop);
                    d["key_low"] = zone.keyLow;
                    d["key_high"] = zone.keyHigh;
                    d["root"] = zone.rootKey;
                    d["vel_low"] = zone.velocityLow;
                    d["vel_high"] = zone.velocityHigh;
                    d["start"] = zone.start;
                    d["end"] = zone.end;
                    d["loop_start"] = zone.loopStart;
                    d["loop_end"] = zone.loopEnd;
                    out.append(d);
                }
                return out;
            }, // GIL: trivial - one dict per zone
            py::arg("channel"), "A sampler channel's zones, for the zone editor.");
}

/// Copies of channels, inserts and slots, and a slot's effect or impulse swapped.
void defineDuplication(py::class_<ProjectHandle>& project) {
    project
        .def(
            "duplicate_channel",
            [](ProjectHandle& handle, std::uint32_t channel) {
                const project::Channel* source = handle.project.find(core::ChannelId{channel});
                if (source == nullptr) {
                    throw std::invalid_argument("no channel " + std::to_string(channel));
                }
                project::Channel copy = *source;
                copy.id = {};
                copy.soloed = false;
                std::string name = source->name + "_2";
                for (int n = 3; handle.project.findChannelByName(name) != nullptr; ++n) {
                    name = source->name + "_" + std::to_string(n);
                }
                copy.name = name;
                const py::gil_scoped_release released;
                return run(handle, std::make_unique<project::AddChannel>(std::move(copy)))
                    ->created()
                    .value;
            },
            py::arg("channel"), "A copy of a channel - instrument, settings, zones - one step.")
        .def(
            "duplicate_insert",
            [](ProjectHandle& handle, std::uint32_t insert) {
                const project::Insert* source = handle.project.mixer.find(core::InsertId{insert});
                if (source == nullptr) {
                    throw std::invalid_argument("no insert " + std::to_string(insert));
                }
                project::Insert copy = *source;
                copy.id = {};
                copy.name = source->name + " copy";
                copy.soloed = false;
                // Fresh ids for its slots and sends: a copied id would be adopted, and two
                // slots answering to one path is exactly what ids exist to prevent.
                for (project::Slot& slot : copy.slots) {
                    slot.id = {};
                }
                for (project::Send& send : copy.sends) {
                    send.id = {};
                }
                const py::gil_scoped_release released;
                core::InsertId created;
                grouped(handle, "Duplicate insert", [&] {
                    created = run(handle, std::make_unique<project::AddInsert>(std::move(copy)))
                                  ->created();
                    // It goes where the original went.
                    std::vector<core::InsertId> targets;
                    for (const project::Route& route : handle.project.mixer.routes) {
                        if (route.from == core::InsertId{insert}) {
                            targets.push_back(route.to);
                        }
                    }
                    for (const core::InsertId target : targets) {
                        run(handle, std::make_unique<project::AddRoute>(created, target));
                    }
                });
                return created.value;
            },
            py::arg("insert"), "A copy of an insert with its effects, sends and routes.")
        .def(
            "duplicate_slot",
            [](ProjectHandle& handle, std::uint32_t slot) {
                const core::InsertId owner = handle.project.mixer.ownerOfSlot(core::SlotId{slot});
                const project::Slot* source = handle.project.mixer.findSlot(core::SlotId{slot});
                if (source == nullptr || !owner.valid()) {
                    throw std::invalid_argument("no slot " + std::to_string(slot));
                }
                project::Slot copy = *source;
                copy.id = {};
                const py::gil_scoped_release released;
                return run(handle, std::make_unique<project::AddSlot>(owner, std::move(copy)))
                    ->created()
                    .value;
            },
            py::arg("slot"), "A copy of an effect slot, appended to its insert.")
        .def(
            "set_slot_impulse",
            [](ProjectHandle& handle, std::uint32_t slot, const std::string& path) {
                if (handle.project.mixer.findSlot(core::SlotId{slot}) == nullptr) {
                    throw std::invalid_argument("no slot " + std::to_string(slot));
                }
                const py::gil_scoped_release released;
                grouped(handle, "Set impulse response", [&] {
                    core::SampleId impulse;
                    if (!path.empty()) {
                        impulse = run(handle, std::make_unique<project::AddSample>(poolPath(
                                                  handle.project, std::filesystem::path(path))))
                                      ->created();
                    }
                    run(handle,
                        std::make_unique<project::SetSlotImpulse>(core::SlotId{slot}, impulse));
                });
            },
            py::arg("slot"), py::arg("path"),
            "A Convolution slot's impulse response file (empty: the synthetic room), one step.")
        .def(
            "set_slot_effect",
            [](ProjectHandle& handle, std::uint32_t slot, const std::string& type) {
                if (project::findEffectType(type) == nullptr) {
                    throw std::invalid_argument("no effect type " + type);
                }
                const py::gil_scoped_release released;
                run(handle, std::make_unique<project::SetSlotEffect>(
                                core::SlotId{slot}, type, std::vector<project::SlotParam>{}, 1.0F));
            },
            py::arg("slot"), py::arg("type"),
            "Replace a slot's effect with a fresh one of `type`, keeping its place.");
}

} // namespace

void defineSession(py::class_<ProjectHandle>& project) {
    project
        // GIL: trivial - opens a transaction
        .def(
            "begin_group",
            [](ProjectHandle& handle, const std::string& label) { handle.stack.beginGroup(label); },
            py::arg("label"), "Everything executed until end_group() is one undo step.")
        // GIL: trivial - closes a transaction
        .def("end_group", [](ProjectHandle& handle) { handle.stack.endGroup(); })
        // GIL: trivial - sets a flag
        .def(
            "seal_coalescing", [](ProjectHandle& handle) { handle.stack.sealCoalescing(); },
            "The next command starts a new history entry (a new gesture began).")
        .def_property_readonly("undo_depth",
                               [](const ProjectHandle& handle) { return handle.stack.undoDepth(); })
        .def(
            "undo_to",
            [](ProjectHandle& handle, std::size_t depth) {
                const py::gil_scoped_release released;
                bool moved = false;
                while (handle.stack.undoDepth() > depth && handle.stack.undo(handle.project)) {
                    moved = true;
                }
                while (handle.stack.undoDepth() < depth && handle.stack.redo(handle.project)) {
                    moved = true;
                }
                return moved;
            },
            py::arg("depth"),
            "Undo or redo until `depth` steps are done: the history panel's jump (§4.8).")
        .def(
            "add_audio_item",
            [](ProjectHandle& handle, std::uint32_t track, const std::string& path,
               std::int64_t start, std::int64_t length) {
                const py::gil_scoped_release released;
                core::ItemId created;
                grouped(handle, "Add audio clip", [&] {
                    auto* sample = run(handle, std::make_unique<project::AddSample>(poolPath(
                                                   handle.project, std::filesystem::path(path))));
                    project::PlaylistItem item;
                    item.start = core::Ticks{std::max<std::int64_t>(0, start)};
                    item.length = core::Ticks{std::max<std::int64_t>(0, length)};
                    item.content = project::AudioClipRef{.sample = sample->created()};
                    created = run(handle, std::make_unique<project::AddPlaylistItem>(
                                              core::PlaylistTrackId{track}, item))
                                  ->created();
                });
                return created.value;
            },
            py::arg("track"), py::arg("path"), py::arg("start"), py::arg("length") = 0,
            "A sample dropped on the playlist: pool entry and placement, one undo step. "
            "Returns the item id.")
        .def(
            "add_sampler_channel",
            [](ProjectHandle& handle, const std::string& name, const std::string& path,
               std::uint32_t output) {
                const py::gil_scoped_release released;
                core::ChannelId created;
                grouped(handle, "Add sampler channel", [&] {
                    auto* sample = run(handle, std::make_unique<project::AddSample>(poolPath(
                                                   handle.project, std::filesystem::path(path))));
                    project::Channel prototype;
                    prototype.name = name;
                    prototype.instrument.type = "sampler";
                    prototype.output =
                        output != 0 ? core::InsertId{output} : handle.project.mixer.master;
                    project::SampleZone zone;
                    zone.sample = sample->created();
                    prototype.instrument.zones.push_back(zone);
                    created =
                        run(handle, std::make_unique<project::AddChannel>(std::move(prototype)))
                            ->created();
                });
                return created.value;
            },
            py::arg("name"), py::arg("path"), py::arg("output") = 0,
            "A sample dropped on the channel rack: a sampler channel playing it across the "
            "keyboard, one undo step. Returns the channel id.")
        .def(
            "apply_preset",
            [](ProjectHandle& handle, std::uint32_t channel, const std::string& path) {
                adx::format::DiagnosticList diagnostics;
                const adx::preset::Preset preset = loadOrThrow(path, diagnostics);
                {
                    const py::gil_scoped_release released;
                    grouped(handle, "Apply preset " + preset.name, [&] {
                        applyInstrumentPreset(handle, core::ChannelId{channel}, preset);
                    });
                }
                return toPython(diagnostics);
            },
            py::arg("channel"), py::arg("path"),
            "An instrument preset onto a channel, its samples included (P4-2): one undo "
            "step. Returns the preset's diagnostics.")
        .def(
            "apply_effect_preset",
            [](ProjectHandle& handle, std::uint32_t insert, std::uint32_t slot,
               const std::string& path) {
                adx::format::DiagnosticList diagnostics;
                const adx::preset::Preset preset = loadOrThrow(path, diagnostics);
                if (preset.kind() != project::TypeKind::Effect) {
                    throw std::invalid_argument("'" + preset.name + "' is not an effect preset");
                }
                core::SlotId target{slot};
                {
                    const py::gil_scoped_release released;
                    grouped(handle, "Apply preset " + preset.name, [&] {
                        if (!target.valid() || handle.project.mixer.findSlot(target) == nullptr) {
                            project::Slot prototype;
                            prototype.type = preset.type;
                            target = run(handle, std::make_unique<project::AddSlot>(
                                                     core::InsertId{insert}, std::move(prototype)))
                                         ->created();
                        }
                        run(handle, std::make_unique<project::SetSlotEffect>(
                                        target, preset.type, slotParamsOf(preset),
                                        static_cast<float>(preset.mix.value_or(1.0))));
                    });
                }
                return py::make_tuple(target.value, toPython(diagnostics));
            },
            py::arg("insert"), py::arg("slot"), py::arg("path"),
            "An effect preset onto a slot (or, with slot 0, a new slot on `insert`): one undo "
            "step. Returns (slot id, diagnostics).");
    defineSessionQueries(project);
    defineDuplication(project);
}
// NOLINTEND(bugprone-exception-escape)

void registerPresets(py::module_& m) {
    m.def(
        "scan_presets",
        [](const std::vector<std::string>& directories) {
            adx::preset::PresetLibrary library;
            adx::format::DiagnosticList diagnostics;
            {
                const py::gil_scoped_release released;
                for (const std::string& directory : directories) {
                    if (std::filesystem::is_directory(directory)) {
                        static_cast<void>(library.scan(directory, diagnostics));
                    }
                }
            }
            py::list out;
            for (const adx::preset::Preset& preset : library.all()) {
                out.append(presetDict(preset));
            }
            return out;
        },
        py::arg("directories"),
        "Every .adxpreset under the given folders: name, type, pack, tags, path, kind.");
    m.def(
        "read_preset",
        [](const std::string& path) {
            adx::format::DiagnosticList diagnostics;
            adx::preset::Preset preset;
            {
                const py::gil_scoped_release released;
                if (!adx::preset::loadPreset(path, preset, diagnostics)) {
                    throw std::runtime_error("cannot read preset " + path);
                }
            }
            return presetDict(preset);
        },
        py::arg("path"));
}

} // namespace adx::bindings
