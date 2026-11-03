// Hosted plugins and MIDI learn (phase_9.md §4, §3 step 2): scanning and the cache,
// the live instances' health and editors, what flows from them back into the project,
// and the project's MIDI section.
//
// Everything that talks to a sandbox child releases the GIL: a child is another
// process, and one that hangs must not freeze Python with it.

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "bindings/EngineHandle.h"
#include "bindings/ProjectHandle.h"
#include "engine/midi/DeviceNames.h"
#include "engine/plugin/PluginCore.h"
#include "engine/plugin/PluginHost.h"
#include "engine/plugin/ProjectSync.h"
#include "engine/project/ParamRegistry.h"
#include "engine/project/commands/MidiCommands.h"

namespace py = pybind11;
using adx::bindings::EngineHandle;
using adx::bindings::ProjectHandle;
namespace plugin = adx::plugin;
namespace project = adx::project;

namespace {

plugin::PluginFormat formatOf(const std::string& name) {
    plugin::PluginFormat format = plugin::PluginFormat::Clap;
    if (!plugin::formatFromString(name, format)) {
        throw std::invalid_argument("a plugin format is 'clap' or 'vst3', not '" + name + "'");
    }
    return format;
}

py::dict descriptorDict(const plugin::PluginDescriptor& d, bool withParams) {
    py::dict out;
    out["format"] = plugin::toString(d.id.format);
    out["uid"] = d.id.uid;
    out["name"] = d.id.name;
    out["path"] = d.id.path;
    out["version"] = d.id.version;
    out["vendor"] = d.vendor;
    out["instrument"] = d.instrument;
    out["editor"] = d.hasEditor;
    out["latency"] = d.latency;
    out["param_count"] = d.params.size();
    if (withParams) {
        py::list params;
        for (const plugin::ParamInfo& p : d.params) {
            py::dict param;
            param["id"] = p.id;
            param["key"] = p.key;
            param["name"] = p.name;
            param["min"] = p.minimum;
            param["max"] = p.maximum;
            param["default"] = p.defaultValue;
            param["steps"] = p.steps;
            param["automatable"] = p.automatable;
            param["hidden"] = p.hidden;
            params.append(std::move(param));
        }
        out["params"] = std::move(params);
    }
    return out;
}

py::dict scanDict(const plugin::ScanResult& r) {
    py::dict out;
    out["path"] = r.path;
    out["format"] = plugin::toString(r.format);
    out["ok"] = r.ok;
    out["error"] = r.error;
    out["plugins"] = r.plugins.size();
    return out;
}

const char* noticeKind(plugin::bridge::Notice::Kind kind) {
    using Kind = plugin::bridge::Notice::Kind;
    switch (kind) {
    case Kind::Crashed:
        return "crashed";
    case Kind::Hung:
        return "hung";
    case Kind::Restarted:
        return "restarted";
    case Kind::LateBypassed:
        return "late";
    case Kind::NanMuted:
        return "nan";
    case Kind::LatencyChanged:
        return "latency";
    case Kind::StateRejected:
        return "state";
    case Kind::LatencyMismatch:
        return "latency-mismatch";
    }
    return "unknown";
}

const char* stateName(plugin::bridge::LinkState state) {
    using State = plugin::bridge::LinkState;
    switch (state) {
    case State::Idle:
        return "idle";
    case State::Running:
        return "running";
    case State::Crashed:
        return "crashed";
    case State::LateBypassed:
        return "late";
    case State::Muted:
        return "muted";
    }
    return "unknown";
}

std::shared_ptr<plugin::bridge::RemoteInstance> instanceOrThrow(std::uint32_t id) {
    auto instance = plugin::PluginHost::instance().find(id);
    if (!instance) {
        throw std::invalid_argument("no live plugin instance " + std::to_string(id));
    }
    return instance;
}

void definePluginHost(py::module_& m) {
    // GIL: trivial - copies a path
    m.def("plugin_host_path", [] { return plugin::PluginHost::instance().executable(); });
    // GIL: trivial - sets a path
    m.def(
        "set_plugin_host",
        [](const std::string& path) { plugin::PluginHost::instance().setExecutable(path); },
        py::arg("path"), "Where adx_plugin_host.exe is (beside the extension by default).");
    // GIL: trivial - one file-exists check
    m.def("plugin_host_available", [] { return plugin::PluginHost::instance().available(); });
    m.def(
        "plugin_load_cache",
        [](const std::string& path) {
            std::string error;
            bool ok = false;
            {
                const py::gil_scoped_release released;
                ok = plugin::PluginHost::instance().loadCache(path, error);
            }
            return std::make_pair(ok, error);
        },
        py::arg("path") = std::string{},
        "Load the scan cache (the default location when empty). Returns (ok, error).");
    m.def(
        "plugin_scan",
        [](const std::vector<std::string>& directories, bool force) {
            const py::gil_scoped_release released;
            return plugin::PluginHost::instance().scan(directories, force);
        },
        py::arg("directories") = std::vector<std::string>{}, py::arg("force") = false,
        "Scan plugin folders out of process into the cache; returns how many files were "
        "scanned. With no folders, the standard CLAP and VST3 ones.");
    m.def(
        "plugin_rescan",
        [](const std::string& format, const std::string& path) {
            plugin::ScanResult result;
            {
                const py::gil_scoped_release released;
                result = plugin::PluginHost::instance().rescan(
                    plugin::Candidate{.format = formatOf(format), .path = path});
            }
            return scanDict(result);
        },
        py::arg("format"), py::arg("path"), "Scan one file now, blacklisted or not.");
    m.def(
        "plugin_forget",
        [](const std::string& path) {
            const py::gil_scoped_release released;
            const bool removed = plugin::PluginHost::instance().cache().remove(path);
            std::string ignored;
            static_cast<void>(plugin::PluginHost::instance().saveCache(ignored));
            return removed;
        },
        py::arg("path"), "Drop a file from the cache (and so from the blacklist).");
    // GIL: trivial - copies the cache's entries
    m.def("plugin_list", [] {
        py::list out;
        for (const plugin::PluginDescriptor* d : plugin::PluginHost::instance().cache().plugins()) {
            out.append(descriptorDict(*d, false));
        }
        return out;
    });
    // GIL: trivial - copies the cache's entries
    m.def("plugin_scan_results", [] {
        py::list out;
        for (const plugin::ScanResult& r : plugin::PluginHost::instance().cache().entries()) {
            out.append(scanDict(r));
        }
        return out;
    });
    m.def(
        "plugin_describe",
        [](const std::string& format, const std::string& uid,
           const std::string& path) -> py::object {
            const plugin::PluginDescriptor* found = nullptr;
            {
                const py::gil_scoped_release released;
                found = plugin::PluginHost::instance().describe(
                    plugin::PluginId{.format = formatOf(format),
                                     .uid = uid,
                                     .path = path,
                                     .name = {},
                                     .version = {}});
            }
            return found == nullptr ? py::object(py::none())
                                    : py::object(descriptorDict(*found, true));
        },
        py::arg("format"), py::arg("uid"), py::arg("path") = std::string{});
}

/// The live instances: their notices, health, parameters and editors.
void definePluginInstances(py::module_& m) {
    // GIL: trivial - drains a short queue
    m.def("plugin_notices", [] {
        py::list out;
        for (const auto& n : plugin::PluginHost::instance().takeNotices()) {
            py::dict notice;
            notice["kind"] = noticeKind(n.kind);
            notice["instance"] = n.instance;
            notice["plugin"] = n.plugin;
            notice["message"] = n.message;
            out.append(std::move(notice));
        }
        return out;
    });
    m.def(
        "plugin_restart",
        [](std::uint32_t instance) {
            std::string error;
            const py::gil_scoped_release released;
            return plugin::PluginHost::instance().restart(instance, error);
        },
        py::arg("instance") = 0,
        "Restart crashed plugins (or the child holding `instance`) with their last state.");
    // GIL: trivial - reads atomics of each live instance
    m.def("plugin_instances", [] {
        py::list out;
        for (const auto& instance : plugin::PluginHost::instance().instances()) {
            py::dict row;
            row["id"] = instance->id();
            row["name"] = instance->plugin().name;
            row["uid"] = instance->plugin().uid;
            row["format"] = plugin::toString(instance->plugin().format);
            row["state"] = stateName(instance->health().state.load());
            row["blocks"] = instance->health().blocks.load();
            row["late"] = instance->health().lateBlocks.load();
            row["nan"] = instance->health().nanBlocks.load();
            row["latency"] = instance->latency();
            row["isolated"] = instance->isolated();
            out.append(std::move(row));
        }
        return out;
    });
    m.def(
        "plugin_param_values",
        [](std::uint32_t id) {
            const auto instance = instanceOrThrow(id);
            std::vector<std::pair<std::uint32_t, double>> values;
            const py::gil_scoped_release released;
            static_cast<void>(instance->paramValues(values));
            return values;
        },
        py::arg("instance"), "(param id, value) for every parameter of a live instance.");
    m.def(
        "plugin_param_text",
        [](std::uint32_t id, std::uint32_t param, double value) -> py::object {
            const auto instance = instanceOrThrow(id);
            std::string text;
            bool ok = false;
            {
                const py::gil_scoped_release released;
                ok = instance->paramText(param, value, text);
            }
            return ok ? py::object(py::str(text)) : py::object(py::none());
        },
        py::arg("instance"), py::arg("param"), py::arg("value"));
    m.def(
        "plugin_open_editor",
        [](std::uint32_t id) -> py::object {
            const auto instance = instanceOrThrow(id);
            std::uint64_t window = 0;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            bool ok = false;
            {
                const py::gil_scoped_release released;
                ok = instance->openEditor(window, width, height);
            }
            return ok ? py::object(py::make_tuple(window, width, height)) : py::object(py::none());
        },
        py::arg("instance"),
        "Open the plugin's own editor in the sandbox: (native window, width, height) to "
        "embed, or None when it has none (use the generic editor).");
    m.def(
        "plugin_close_editor",
        [](std::uint32_t id) {
            const auto instance = instanceOrThrow(id);
            const py::gil_scoped_release released;
            instance->closeEditor();
        },
        py::arg("instance"));
    // GIL: trivial - one atomic exchange
    m.def(
        "plugin_reenable", [](std::uint32_t id) { instanceOrThrow(id)->reenable(); },
        py::arg("instance"), "Play a plugin bypassed for lateness, or muted for NaN, again.");
    // GIL: trivial - one flag
    m.def(
        "plugin_set_isolated",
        [](std::uint32_t id, bool isolated) { instanceOrThrow(id)->setIsolated(isolated); },
        py::arg("instance"), py::arg("isolated"),
        "Give the plugin a sandbox process of its own from its next restart.");
    m.def(
        "plugin_check_latency",
        [](std::uint32_t id) {
            const py::gil_scoped_release released;
            return plugin::PluginHost::instance().checkLatency(id);
        },
        py::arg("instance"),
        "Measure a plugin's latency with an impulse (only while it is not playing); a "
        "mismatch with its report becomes a notice. Returns the measurement, or -1.");
    // GIL: trivial - reads a few environment variables
    m.def("plugin_search_paths", [] { return plugin::defaultSearchPaths(); });
}

} // namespace

void defineEnginePlugins(py::class_<EngineHandle>& engine) {
    engine
        // GIL: trivial - a node lookup
        .def(
            "plugin_instance_of_slot",
            [](EngineHandle& self, std::uint32_t slot) {
                const plugin::PluginCore* core =
                    plugin::pluginOfSlot(*self.engine, adx::core::SlotId{slot});
                return core != nullptr ? core->instanceId() : 0U;
            },
            py::arg("slot"), "The live plugin instance in a slot, or 0.")
        // GIL: trivial - a node lookup
        .def(
            "plugin_instance_of_channel",
            [](EngineHandle& self, std::uint32_t channel) {
                const plugin::PluginCore* core =
                    plugin::pluginOfChannel(*self.engine, adx::core::ChannelId{channel});
                return core != nullptr ? core->instanceId() : 0U;
            },
            py::arg("channel"), "The live plugin instrument of a channel, or 0.")
        .def(
            "sync_plugin_states",
            [](EngineHandle& self, ProjectHandle& project) {
                const py::gil_scoped_release released;
                return plugin::syncPluginStates(*self.engine, project.project, project.stack);
            },
            py::arg("project"),
            "Store every live plugin's state in the project (before a save or an export).")
        .def(
            "apply_plugin_echoes",
            [](EngineHandle& self, ProjectHandle& project) {
                const py::gil_scoped_release released;
                return plugin::applyPluginEchoes(*self.engine, project.project, project.stack);
            },
            py::arg("project"),
            "Put parameter changes made in plugins' own editors into the project.")
        .def(
            "apply_learned",
            [](EngineHandle& self, ProjectHandle& project) {
                const py::gil_scoped_release released;
                return self.engine->applyLearned(project.project, project.stack);
            },
            py::arg("project"), "Put values learned MIDI controls wrote into the project.")
        // GIL: trivial - reads two atomics
        .def(
            "midi_last_control",
            [](EngineHandle& self) -> py::object {
                adx::midi::ControlMessage control;
                if (!self.engine->core().lastControl().read(self.learnSequence, control)) {
                    return py::none();
                }
                py::dict out;
                out["kind"] = project::toString(control.kind);
                out["number"] = control.number;
                out["channel"] = control.channel + 1;
                out["device"] = adx::midi::deviceName(control.device);
                out["value"] = control.value;
                out["pressed"] = control.pressed;
                return out;
            },
            "The newest controller message since the last call, or None: what learn binds.")
        .def(
            "refresh",
            [](EngineHandle& self, ProjectHandle& project) {
                const py::gil_scoped_release released;
                return self.engine->setProject(project.project, project.stack.revision()).rebuilt;
            },
            py::arg("project"),
            "Rebuild the whole graph: after a plugin's latency changed, so delay "
            "compensation follows it.");
}

void defineMidiSettings(py::class_<ProjectHandle>& projectClass) {
    projectClass
        // GIL: trivial - copies a short list
        .def("midi_bindings",
             [](ProjectHandle& self) {
                 py::list out;
                 for (const project::MidiBinding& b : self.project.midi.bindings) {
                     py::dict row;
                     row["kind"] = project::toString(b.kind);
                     row["number"] = b.number;
                     std::string const path =
                         b.target.valid() ? project::ParamRegistry::pathOf(b.target, self.project)
                                          : std::string{};
                     row["path"] = path.empty() ? b.targetPath : path;
                     row["channel"] = b.channel == project::kAnyMidiChannel ? 0 : b.channel + 1;
                     row["device"] = b.device;
                     row["min"] = b.minimum;
                     row["max"] = b.maximum;
                     row["toggle"] = b.toggle;
                     row["valid"] = b.target.valid();
                     out.append(std::move(row));
                 }
                 return out;
             })
        // GIL: trivial - resolves one path and runs one command
        .def(
            "add_midi_binding",
            [](ProjectHandle& self, const std::string& kind, std::uint8_t number,
               const std::string& path, std::uint8_t channel, const std::string& device,
               float minimum, float maximum, bool toggle) {
                project::MidiBinding binding;
                if (!project::midiControlKindFromString(kind, binding.kind)) {
                    throw std::invalid_argument("a control kind is cc, note, bend or pressure");
                }
                const project::ParamResolution resolved =
                    project::ParamRegistry::resolve(path, self.project);
                if (!resolved.ok()) {
                    throw std::invalid_argument("no such parameter: " + path);
                }
                binding.number = number;
                binding.targetPath = path;
                binding.target = resolved.ref;
                binding.channel = channel == 0 ? project::kAnyMidiChannel
                                               : static_cast<std::uint8_t>(channel - 1);
                binding.device = device;
                binding.minimum = minimum;
                binding.maximum = maximum;
                binding.toggle = toggle;
                self.stack.execute(std::make_unique<project::AddMidiBinding>(std::move(binding)),
                                   self.project);
            },
            py::arg("kind"), py::arg("number"), py::arg("path"), py::arg("channel") = 0,
            py::arg("device") = std::string{}, py::arg("minimum") = 0.0F, py::arg("maximum") = 1.0F,
            py::arg("toggle") = false,
            "Bind a MIDI control to a parameter path (channel 0 = any). Replaces a binding "
            "of the same control.")
        // GIL: trivial - one command
        .def(
            "remove_midi_binding",
            [](ProjectHandle& self, std::size_t index) {
                self.stack.execute(std::make_unique<project::RemoveMidiBinding>(index),
                                   self.project);
            },
            py::arg("index"))
        // GIL: trivial - copies a short list
        .def("midi_routes",
             [](ProjectHandle& self) {
                 py::list out;
                 for (const project::MidiRouteSetting& r : self.project.midi.routes) {
                     py::dict row;
                     row["channel"] = r.target.value;
                     row["device"] = r.device;
                     row["channels"] = r.channels;
                     row["key_low"] = r.keyLow;
                     row["key_high"] = r.keyHigh;
                     row["velocity_low"] = r.velocityLow;
                     row["velocity_high"] = r.velocityHigh;
                     row["transpose"] = r.transpose;
                     row["curve"] = r.curve;
                     out.append(std::move(row));
                 }
                 return out;
             })
        // GIL: trivial - one command over a short list
        .def(
            "set_midi_routes",
            [](ProjectHandle& self, const std::vector<py::dict>& rows) {
                std::vector<project::MidiRouteSetting> routes;
                for (const py::dict& row : rows) {
                    project::MidiRouteSetting r;
                    r.target = adx::core::ChannelId{row["channel"].cast<std::uint32_t>()};
                    if (row.contains("device")) {
                        r.device = row["device"].cast<std::string>();
                    }
                    if (row.contains("channels")) {
                        r.channels = row["channels"].cast<std::uint16_t>();
                    }
                    if (row.contains("key_low")) {
                        r.keyLow = row["key_low"].cast<std::uint8_t>();
                    }
                    if (row.contains("key_high")) {
                        r.keyHigh = row["key_high"].cast<std::uint8_t>();
                    }
                    if (row.contains("velocity_low")) {
                        r.velocityLow = row["velocity_low"].cast<std::uint8_t>();
                    }
                    if (row.contains("velocity_high")) {
                        r.velocityHigh = row["velocity_high"].cast<std::uint8_t>();
                    }
                    if (row.contains("transpose")) {
                        r.transpose = row["transpose"].cast<std::int8_t>();
                    }
                    if (row.contains("curve")) {
                        r.curve = row["curve"].cast<float>();
                    }
                    routes.push_back(std::move(r));
                }
                self.stack.execute(std::make_unique<project::SetMidiRoutes>(std::move(routes)),
                                   self.project);
            },
            py::arg("routes"),
            "Replace the project's MIDI routes: dicts with 'channel' (an id) and optional "
            "device, channels (a 16-bit mask), key_low/high, velocity_low/high, transpose, "
            "curve. None at all: every input plays the selected channel.");
}

void registerPluginBindings(py::module_& m) {
    definePluginHost(m);
    definePluginInstances(m);
}
