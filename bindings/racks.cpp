// Racks, macros, SFZ import and the library store from Python (phase_12.md §4.1, §4.2,
// §4.7): what the browser, the rack panel and tools/packs drive.
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "bindings/Bindings.h"
#include "bindings/ProjectHandle.h"
#include "engine/format/audio/LibraryPaths.h"
#include "engine/format/sfz/SfzParser.h"
#include "engine/preset/PresetLibrary.h"
#include "engine/preset/Rack.h"
#include "engine/preset/SfzImport.h"
#include "engine/project/ParamRegistry.h"
#include "engine/project/Rack.h"
#include "engine/project/commands/RackCommands.h"

namespace py = pybind11;

namespace adx::bindings {
namespace {

adx::project::Rack loadRackOrThrow(const std::string& path, format::DiagnosticList& diagnostics) {
    adx::project::Rack rack;
    if (!preset::loadRack(path, rack, diagnostics)) {
        throw std::runtime_error("cannot read rack " + path);
    }
    return rack;
}

py::dict rackDict(const adx::project::Rack& rack, const std::string& path) {
    py::dict out;
    out["name"] = rack.name;
    out["pack"] = rack.pack;
    out["category"] = rack.category;
    out["tags"] = rack.tags;
    out["tempo_hint"] = rack.tempoHint;
    out["root_hint"] = rack.rootHint;
    out["fallback"] = rack.fallback;
    out["path"] = path;
    out["kind"] = "rack";
    py::list layers;
    bool needsLibrary = false;
    for (const adx::project::RackLayer& layer : rack.layers) {
        py::dict each;
        each["name"] = layer.name;
        each["type"] = layer.preset.empty() ? layer.type : "preset:" + layer.preset;
        each["zones"] = layer.zones.size();
        layers.append(each);
        for (const adx::project::RackZone& zone : layer.zones) {
            needsLibrary = needsLibrary || (format::isLibraryPath(zone.path) &&
                                            format::resolveLibraryPath(zone.path).empty());
        }
    }
    out["layers"] = layers;
    out["needs_library"] = needsLibrary;
    out["chain"] = rack.chain.size();
    out["sends"] = rack.sends.size();
    py::list macros;
    for (const adx::project::RackMacro& macro : rack.macros) {
        macros.append(macro.name);
    }
    out["macros"] = macros;
    return out;
}

py::list macroList(const adx::project::Project& p) {
    py::list out;
    for (const adx::project::Macro& macro : p.macros) {
        py::dict each;
        each["id"] = macro.id.value;
        each["name"] = macro.name;
        each["value"] = macro.value;
        py::list targets;
        for (const adx::project::MacroTarget& target : macro.targets) {
            py::dict t;
            const std::string path = adx::project::ParamRegistry::pathOf(target.target, p);
            t["path"] = path.empty() ? target.path : path;
            t["resolved"] = target.target.valid();
            t["min"] = target.minimum;
            t["max"] = target.maximum;
            targets.append(t);
        }
        each["targets"] = targets;
        out.append(each);
    }
    return out;
}

} // namespace

// NOLINTBEGIN(bugprone-exception-escape) - pybind11 turns the exceptions into Python's.
void defineRacks(py::class_<ProjectHandle>& handleClass) {
    handleClass
        .def(
            "insert_rack",
            [](ProjectHandle& handle, const std::string& path,
               const std::vector<std::string>& packs) {
                format::DiagnosticList diagnostics;
                adx::project::Rack rack = loadRackOrThrow(path, diagnostics);
                py::dict out;
                std::vector<std::uint32_t> channels;
                std::vector<std::string> unresolved;
                std::uint32_t insert = 0;
                std::string name;
                {
                    const py::gil_scoped_release released;
                    preset::PresetLibrary library;
                    for (const std::string& directory : packs) {
                        if (std::filesystem::is_directory(directory)) {
                            static_cast<void>(library.scan(directory, diagnostics));
                        }
                    }
                    preset::resolveRackPresets(rack, library, diagnostics);
                    preset::anchorRackPaths(rack, std::filesystem::path(path).parent_path());
                    auto command = std::make_unique<adx::project::InsertRack>(std::move(rack));
                    const adx::project::InsertRack* raw = command.get();
                    handle.stack.execute(std::move(command), handle.project);
                    insert = raw->insert().value;
                    name = raw->instanceName();
                    for (const adx::core::ChannelId channel : raw->channels()) {
                        channels.push_back(channel.value);
                    }
                    unresolved = raw->unresolved();
                }
                out["insert"] = insert;
                out["name"] = name;
                out["channels"] = channels;
                out["unresolved"] = unresolved;
                out["diagnostics"] = toPython(diagnostics);
                return out;
            },
            py::arg("path"), py::arg("packs") = std::vector<std::string>{},
            "Expand an .adxrack into channels, inserts, sends and macros: one undo step. "
            "`packs` are the preset folders its PRESET= layers are looked up in. Returns "
            "the rack insert's id, the name it took, its channels, any macro targets that "
            "named nothing, and the file's diagnostics.")
        .def(
            "save_rack",
            [](const ProjectHandle& handle, std::uint32_t insert, const std::string& path,
               const std::string& name, const std::string& pack, const std::string& category,
               const std::vector<std::string>& tags) {
                adx::project::Rack rack =
                    adx::project::rackFrom(handle.project, adx::core::InsertId{insert});
                if (rack.name.empty() && rack.layers.empty()) {
                    throw std::invalid_argument("no insert " + std::to_string(insert));
                }
                if (!name.empty()) {
                    rack.name = name;
                }
                rack.pack = pack;
                rack.category = category;
                rack.tags = tags;
                if (!preset::saveRack(rack, path)) {
                    throw std::runtime_error("cannot write " + path);
                }
                return rackDict(rack, path);
            }, // GIL: trivial - a small text file
            py::arg("insert"), py::arg("path"), py::arg("name") = "", py::arg("pack") = "",
            py::arg("category") = "", py::arg("tags") = std::vector<std::string>{},
            "Save an insert, the channels that feed it, its sends and its macros as an "
            ".adxrack: the inverse of insert_rack.")
        // GIL: trivial - copies a few small structs
        .def(
            "macros", [](const ProjectHandle& handle) { return macroList(handle.project); },
            "The project's macros: id, name, value and each target's path and range.")
        .def(
            "set_macro",
            [](ProjectHandle& handle, std::uint32_t macro, float value) {
                if (handle.project.find(adx::core::MacroId{macro}) == nullptr) {
                    throw std::invalid_argument("no macro " + std::to_string(macro));
                }
                const py::gil_scoped_release released;
                handle.stack.execute(
                    std::make_unique<adx::project::SetMacros>(adx::project::macrosWithValue(
                        handle.project, adx::core::MacroId{macro}, value)),
                    handle.project);
            },
            py::arg("macro"), py::arg("value"),
            "Turn a macro's knob (0..1). Consecutive turns are one undo step.");
}
// NOLINTEND(bugprone-exception-escape)

void registerRacks(py::module_& m) {
    m.def(
        "scan_racks",
        [](const std::vector<std::string>& directories) {
            std::vector<std::pair<adx::project::Rack, std::string>> racks;
            {
                const py::gil_scoped_release released;
                for (const std::string& directory : directories) {
                    std::error_code error;
                    if (!std::filesystem::is_directory(directory, error)) {
                        continue;
                    }
                    for (const auto& entry :
                         std::filesystem::recursive_directory_iterator(directory, error)) {
                        if (entry.path().extension() != preset::kRackExtension) {
                            continue;
                        }
                        format::DiagnosticList diagnostics;
                        adx::project::Rack rack;
                        if (preset::loadRack(entry.path(), rack, diagnostics)) {
                            racks.emplace_back(std::move(rack), entry.path().generic_string());
                        }
                    }
                }
            }
            py::list out;
            for (const auto& [rack, path] : racks) {
                out.append(rackDict(rack, path));
            }
            return out;
        },
        py::arg("directories"),
        "Every .adxrack under the given folders: name, pack, category, tags, layers, "
        "macro names, hints, and whether a sound library it needs is missing.");
    m.def(
        "read_rack",
        [](const std::string& path) {
            format::DiagnosticList diagnostics;
            const adx::project::Rack rack = loadRackOrThrow(path, diagnostics);
            py::dict out = rackDict(rack, path);
            out["diagnostics"] = toPython(diagnostics);
            out["text"] = preset::writeRack(rack);
            return out;
        }, // GIL: trivial - one small text file
        py::arg("path"), "One .adxrack: scan_racks' fields, its diagnostics, its canonical text.");
    m.def(
        "import_sfz",
        [](const std::string& path, const std::string& name, const std::string& pack,
           const std::string& prefix) {
            format::sfz::Import import;
            preset::Preset preset;
            std::size_t missing = 0;
            {
                const py::gil_scoped_release released;
                if (!format::sfz::loadSfz(path, import)) {
                    throw std::runtime_error("cannot read " + path);
                }
                const std::filesystem::path directory = std::filesystem::path(path).parent_path();
                for (const format::sfz::Region& region : import.regions) {
                    std::error_code error;
                    missing +=
                        std::filesystem::is_regular_file(directory / region.sample, error) ? 0 : 1;
                }
                preset = preset::presetFromSfz(import,
                                               {.name = name, .pack = pack, .pathPrefix = prefix});
            }
            py::dict out;
            out["preset"] = preset::writePreset(preset);
            out["regions"] = import.regions.size();
            out["missing_samples"] = missing;
            out["unknown"] = import.count(format::sfz::NoteKind::Unknown);
            out["ignored"] = import.count(format::sfz::NoteKind::Ignored);
            out["approximated"] = import.count(format::sfz::NoteKind::Approximated);
            out["report"] = format::sfz::reportOf(import);
            return out;
        },
        py::arg("path"), py::arg("name"), py::arg("pack") = "", py::arg("prefix") = "",
        "An SFZ file as a sampler preset's text, with the import report. `prefix` goes "
        "before every sample path (lib://<library>/<folder>/).");
    // GIL: trivial - environment lookups
    m.def(
        "library_store", [] { return format::libraryStore().generic_string(); },
        "Where sound libraries are installed.");
    // GIL: trivial - one directory lookup, remembered
    m.def(
        "resolve_library_path",
        [](const std::string& path) { return format::resolveLibraryPath(path).generic_string(); },
        py::arg("path"), "A lib:// path as a file; empty when the library is not installed.");
    m.def(
        "forget_library_folders",
        [] { format::forgetLibraryFolders(); }, // GIL: trivial - clears a small map
        "Look for installed libraries again (after a fetch).");
}

} // namespace adx::bindings
