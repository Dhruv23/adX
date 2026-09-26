// The project handle, command submission, undo and redo, exposed to Python.
//
// FINAL_PLAN §2.2 Rule 2 is the design constraint here, and it rules out the obvious
// API: notes are **never** returned as a list of Python objects. A 10,000-note
// pattern would cost 10,000 allocations per redraw, and a convenience API built now
// would be depended on later. Phase 5 reads notes as a zero-copy numpy view; Phase
// 2 exposes counts and command submission, which is all the CLI and the tests need.
//
// Rule 3: every load, save, execute, undo and redo releases the GIL.

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "bindings/ProjectHandle.h"
#include "engine/format/adx/Document.h"
#include "engine/format/adx/Parser.h"
#include "engine/format/adx/Writer.h"
#include "engine/project/Validate.h"
#include "engine/project/commands/ChannelCommands.h"
#include "engine/project/commands/NoteCommands.h"
#include "engine/project/commands/ProjectCommands.h"

namespace py = pybind11;

namespace adx::bindings {

void loadInto(ProjectHandle& handle, std::string_view text, const std::string& baseDirectory,
              format::DiagnosticList& diagnostics) {
    // The spelling of the file on disk - its line endings and whether it had a BOM -
    // is read from the lossless document layer, which is the only layer that saw it.
    {
        format::DiagnosticList ignored;
        const format::Document document = format::Document::parse(text, ignored);
        handle.lineEnding = std::string(document.dominantTerminator());
        handle.bom = !document.bom().empty();
        handle.sourceVersion = format::isVersion2(document) ? project::kAdxVersion : 1;
    }
    format::ParseOptions options;
    options.baseDirectory = baseDirectory;
    format::load(text, handle.project, handle.stack, diagnostics, options);
    handle.stack.clear();
}

} // namespace adx::bindings

namespace {

using adx::bindings::ProjectHandle;

/// A command that has been described in Python but not yet built.
///
/// Built at execute() time rather than at construction, because building one needs
/// the project - `MoveNotes("Verse", "Lead", 120)` has to turn two names into two
/// ids, and only the handle it is executed against can do that.
struct PendingCommand {
    std::function<std::unique_ptr<adx::project::Command>(const adx::project::Project&)> build;
    std::string description;
    bool used{false};
};

[[nodiscard]] std::string readFile(const std::string& path) {
    const std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open " + path);
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("cannot write " + path);
    }
    stream << text;
}

[[nodiscard]] std::string serialise(const ProjectHandle& handle) {
    adx::format::WriteOptions options;
    options.lineEnding = handle.lineEnding;
    options.emitBom = handle.bom;
    return adx::format::write(handle.project, options);
}

[[nodiscard]] const adx::project::Channel& channelNamed(const adx::project::Project& project,
                                                        const std::string& name) {
    const adx::project::Channel* channel = project.findChannelByName(name);
    if (channel == nullptr) {
        throw std::invalid_argument("no channel named '" + name + "'");
    }
    return *channel;
}

[[nodiscard]] const adx::project::Pattern& patternNamed(const adx::project::Project& project,
                                                        const std::string& name) {
    const adx::project::Pattern* pattern = project.findPatternByName(name);
    if (pattern == nullptr) {
        throw std::invalid_argument("no pattern named '" + name + "'");
    }
    return *pattern;
}

[[nodiscard]] py::tuple loadResult(std::unique_ptr<ProjectHandle> handle,
                                   const adx::format::DiagnosticList& diagnostics) {
    return py::make_tuple(py::cast(std::move(handle)), toPython(diagnostics));
}

// NOLINTBEGIN(bugprone-exception-escape) - a builder's contract is to throw: an
// unknown channel or pattern name is std::invalid_argument, which pybind11 turns
// into the ValueError the caller sees. Nothing here is below the audio callback.
void registerCommands(py::module_& m) {
    py::module_ commands = m.def_submodule(
        "commands", "Command constructors. Build one, then pass it to Project.execute().");

    py::class_<PendingCommand>(commands, "Command")
        .def("__repr__", [](const PendingCommand& command) {
            return "<adx_engine.commands.Command " + command.description + ">";
        });

    commands.def(
        "MoveNotes",
        [](const std::string& pattern, const std::string& channel, std::int64_t deltaTicks,
           int deltaPitch) {
            return PendingCommand{
                .build =
                    [=](const adx::project::Project& project) {
                        const auto& p = patternNamed(project, pattern);
                        const auto& c = channelNamed(project, channel);
                        std::vector<adx::core::NoteId> ids;
                        if (const adx::project::NoteClip* clip = p.clipFor(c.id)) {
                            for (const adx::project::Note& note : clip->notes) {
                                ids.push_back(note.id);
                            }
                        }
                        return std::unique_ptr<adx::project::Command>(
                            std::make_unique<adx::project::MoveNotes>(p.id, c.id, std::move(ids),
                                                                      adx::core::Ticks{deltaTicks},
                                                                      deltaPitch));
                    },
                .description = "MoveNotes",
            };
        },
        py::arg("pattern"), py::arg("channel"), py::arg("delta_ticks"), py::arg("delta_pitch") = 0,
        "Move every note one channel plays in one pattern.");

    commands.def(
        "SetChannelVolume",
        [](const std::string& channel, double volume) {
            return PendingCommand{
                .build =
                    [=](const adx::project::Project& project) {
                        return std::unique_ptr<adx::project::Command>(
                            std::make_unique<adx::project::SetChannelValue>(
                                channelNamed(project, channel).id,
                                adx::project::ChannelField::Volume, volume));
                    },
                .description = "SetChannelVolume",
            };
        },
        py::arg("channel"), py::arg("volume"));

    commands.def(
        "RenameChannel",
        [](const std::string& channel, const std::string& name) {
            return PendingCommand{
                .build =
                    [=](const adx::project::Project& project) {
                        return std::unique_ptr<adx::project::Command>(
                            std::make_unique<adx::project::RenameChannel>(
                                channelNamed(project, channel).id, name));
                    },
                .description = "RenameChannel",
            };
        },
        py::arg("channel"), py::arg("name"));

    commands.def(
        "SetTempo",
        [](double bpm, std::int64_t atTicks, bool ramp) {
            return PendingCommand{
                .build =
                    [=](const adx::project::Project&) {
                        return std::unique_ptr<adx::project::Command>(
                            std::make_unique<adx::project::SetTempoEvent>(adx::core::Ticks{atTicks},
                                                                          bpm, ramp));
                    },
                .description = "SetTempo",
            };
        },
        py::arg("bpm"), py::arg("at_ticks") = 0, py::arg("ramp") = false);

    commands.def(
        "AddMarker",
        [](std::int64_t atTicks, const std::string& name) {
            return PendingCommand{
                .build =
                    [=](const adx::project::Project&) {
                        return std::unique_ptr<adx::project::Command>(
                            std::make_unique<adx::project::AddMarker>(adx::core::Ticks{atTicks},
                                                                      name));
                    },
                .description = "AddMarker",
            };
        },
        py::arg("at_ticks"), py::arg("name"));
}
// NOLINTEND(bugprone-exception-escape)

} // namespace

namespace {
void defineProjectIo(py::class_<ProjectHandle>& project);
void defineProjectEditing(py::class_<ProjectHandle>& project);
} // namespace

void registerProjectBindings(py::module_& m) {
    m.attr("PPQ") = adx::core::kPpq;

    registerCommands(m);

    py::class_<ProjectHandle> project(m, "Project",
                                      "One project and the undo history that edits it.");
    defineProjectIo(project);
    defineProjectEditing(project);
}

namespace {

void defineProjectIo(py::class_<ProjectHandle>& project) {
    project.def(py::init<>())
        .def_static(
            "load",
            [](const std::string& path) {
                const std::string text = readFile(path);
                auto handle = std::make_unique<ProjectHandle>();
                adx::format::DiagnosticList diagnostics;
                {
                    const py::gil_scoped_release released;
                    const std::string base =
                        std::filesystem::path(path).parent_path().generic_string();
                    adx::bindings::loadInto(*handle, text, base, diagnostics);
                }
                return loadResult(std::move(handle), diagnostics);
            },
            py::arg("path"), "Load a .adx file. Returns (Project, diagnostics).")
        .def_static(
            "loads",
            [](const std::string& text, const std::string& baseDirectory) {
                auto handle = std::make_unique<ProjectHandle>();
                adx::format::DiagnosticList diagnostics;
                {
                    const py::gil_scoped_release released;
                    adx::bindings::loadInto(*handle, text, baseDirectory, diagnostics);
                }
                return loadResult(std::move(handle), diagnostics);
            },
            py::arg("text"), py::arg("base_dir") = "",
            "Parse .adx text. Returns (Project, diagnostics).")
        .def(
            "save",
            [](const ProjectHandle& handle, const std::string& path) {
                std::string text;
                {
                    const py::gil_scoped_release released;
                    text = serialise(handle);
                    writeFile(path, text);
                }
            },
            py::arg("path"), "Write the canonical form to `path`.")
        .def(
            "dumps",
            [](const ProjectHandle& handle) {
                const py::gil_scoped_release released;
                return serialise(handle);
            },
            "The canonical text, exactly what save() would write.")
        .def_property_readonly(
            "channels",
            [](const ProjectHandle& handle) {
                std::vector<std::string> names;
                names.reserve(handle.project.channels.size());
                for (const auto& channel : handle.project.channels) {
                    names.push_back(channel.name);
                }
                return names;
            },
            "Channel names, in id order.")
        .def_property_readonly(
            "patterns",
            [](const ProjectHandle& handle) {
                std::vector<std::string> names;
                names.reserve(handle.project.patterns.size());
                for (const auto& pattern : handle.project.patterns) {
                    names.push_back(pattern.name);
                }
                return names;
            },
            "Pattern names, in id order.")
        .def(
            "note_count",
            [](const ProjectHandle& handle, const std::string& pattern) {
                std::size_t total = 0;
                for (const auto& clip : patternNamed(handle.project, pattern).noteClips) {
                    total += clip.notes.size();
                }
                return total;
            },
            py::arg("pattern"), "How many notes a pattern holds - a count, never a list.")
        .def(
            "info",
            [](const ProjectHandle& handle) {
                const adx::project::Project& project = handle.project;
                std::size_t notes = 0;
                for (const auto& pattern : project.patterns) {
                    for (const auto& clip : pattern.noteClips) {
                        notes += clip.notes.size();
                    }
                }
                double minBpm = adx::core::kMaxBpm;
                double maxBpm = adx::core::kMinBpm;
                for (const auto& event : project.tempo.tempoEvents()) {
                    minBpm = std::min(minBpm, event.bpm);
                    maxBpm = std::max(maxBpm, event.bpm);
                }
                const adx::core::Ticks length = project.contentLength();
                py::dict out;
                out["title"] = project.meta.title;
                out["source_version"] = handle.sourceVersion;
                out["channels"] = project.channels.size();
                out["patterns"] = project.patterns.size();
                out["notes"] = notes;
                out["playlist_tracks"] = project.playlist.tracks.size();
                out["inserts"] = project.mixer.inserts.size();
                out["markers"] = project.markers.size();
                out["length_ticks"] = length.value;
                out["length_seconds"] = project.tempo.secondsAt(length);
                out["bpm_min"] = minBpm;
                out["bpm_max"] = maxBpm;
                return out;
            },
            "Counts, duration and tempo range - what `adx info` prints.");
}

void defineProjectEditing(py::class_<ProjectHandle>& project) {
    project
        .def(
            "execute",
            [](ProjectHandle& handle, PendingCommand& pending) {
                if (pending.used) {
                    throw std::invalid_argument("this command has already been executed");
                }
                std::unique_ptr<adx::project::Command> command = pending.build(handle.project);
                pending.used = true;
                const py::gil_scoped_release released;
                handle.stack.execute(std::move(command), handle.project);
            },
            py::arg("command"), "Apply a command. It becomes one undo step.")
        .def(
            "undo",
            [](ProjectHandle& handle) {
                const py::gil_scoped_release released;
                return handle.stack.undo(handle.project);
            },
            "Undo one step. Returns False when there was nothing to undo.")
        .def(
            "redo",
            [](ProjectHandle& handle) {
                const py::gil_scoped_release released;
                return handle.stack.redo(handle.project);
            },
            "Redo one step. Returns False when there was nothing to redo.")
        .def_property_readonly(
            "revision", [](const ProjectHandle& handle) { return handle.stack.revision(); },
            "Bumped on every mutation. The change token a UI polls.")
        .def(
            "history",
            [](const ProjectHandle& handle) {
                std::vector<std::string> labels;
                labels.reserve(handle.stack.history().size());
                for (const auto& entry : handle.stack.history()) {
                    labels.push_back(entry.label);
                }
                return labels;
            },
            "Undo history, oldest first.")
        .def(
            "validate",
            [](const ProjectHandle& handle, bool checkSampleFiles) {
                adx::format::DiagnosticList diagnostics;
                adx::project::ValidationOptions options;
                options.checkSampleFiles = checkSampleFiles;
                (void)adx::project::validate(handle.project, diagnostics, options);
                return toPython(diagnostics);
            },
            py::arg("check_sample_files") = false, "Every broken invariant, as diagnostics.")
        .def_property_readonly(
            "source_version", [](const ProjectHandle& handle) { return handle.sourceVersion; },
            "The format version the file was read as. 1 means it was migrated from v1.");
}

} // namespace
