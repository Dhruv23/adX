// The genre suites (phase_10.md §4.6): a style's template or a whole generated track, as
// .adx text the app opens or applies as one undo step.
#include <pybind11/stl.h>

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "bindings/Bindings.h"
#include "engine/analysis/KeyDetector.h"
#include "engine/format/adx/Writer.h"
#include "engine/generate/GenreSuite.h"
#include "engine/preset/PresetLibrary.h"

namespace py = pybind11;
namespace gen = adx::generate;

namespace {

/// One library per packs directory, scanned once.
const adx::preset::PresetLibrary& library(const std::string& packs) {
    static std::map<std::string, std::unique_ptr<adx::preset::PresetLibrary>> cache;
    auto& slot = cache[packs];
    if (!slot) {
        slot = std::make_unique<adx::preset::PresetLibrary>();
        adx::format::DiagnosticList diagnostics;
        slot->scan(packs, diagnostics);
    }
    return *slot;
}

std::optional<adx::analysis::Key> keyOf(const std::optional<std::string>& name) {
    if (!name || name->empty()) {
        return std::nullopt;
    }
    for (std::uint8_t tonic = 0; tonic < 12; ++tonic) {
        for (const auto mode : {adx::analysis::Mode::Major, adx::analysis::Mode::Minor}) {
            const adx::analysis::Key key{.tonic = tonic, .mode = mode};
            if (adx::analysis::keyName(key) == *name) {
                return key;
            }
        }
    }
    throw py::value_error("unknown key '" + *name + "' (spelled like 'A minor')");
}

py::dict trackOf(const gen::GeneratedTrack& track) {
    py::dict d;
    d["text"] = adx::format::write(track.project);
    d["bpm"] = track.bpm;
    d["key"] = adx::analysis::keyName(track.key);
    d["notes"] = track.notes;
    return d;
}

py::dict run(bool notes, const std::string& style, const std::string& packs, std::uint32_t seed,
             std::uint32_t bars, std::optional<double> bpm, const std::optional<std::string>& key) {
    gen::StyleOptions options;
    options.seed = seed;
    options.bars = bars;
    options.bpm = bpm;
    options.key = keyOf(key);
    try {
        return trackOf(notes ? gen::generateTrack(style, library(packs), options)
                             : gen::styleTemplate(style, library(packs), options));
    } catch (const std::invalid_argument& e) {
        throw py::value_error(e.what());
    }
}

} // namespace

void registerGenerateBindings(py::module_& m) {
    // GIL: trivial - a constant table
    m.def(
        "genre_styles",
        [] {
            py::list out;
            for (const gen::StyleInfo& s : gen::styles()) {
                py::dict d;
                d["id"] = s.id;
                d["name"] = s.name;
                d["description"] = s.description;
                d["pack"] = s.pack;
                out.append(d);
            }
            return out;
        },
        "The genre suites: id, name, description, pack.");
    // GIL: trivial - a few thousand notes, generated and written in milliseconds
    m.def(
        "generate_track",
        [](const std::string& style, const std::string& packs, std::uint32_t seed,
           std::uint32_t bars, std::optional<double> bpm, const std::optional<std::string>& key) {
            return run(true, style, packs, seed, bars, bpm, key);
        },
        py::arg("style"), py::arg("packs"), py::arg("seed") = 1, py::arg("bars") = 32,
        py::arg("bpm") = py::none(), py::arg("key") = py::none(),
        "A whole track in `style`, from the presets under `packs`: text, bpm, key, notes. The "
        "same seed is the same text.");
    // GIL: trivial - the same, without notes
    m.def(
        "style_template",
        [](const std::string& style, const std::string& packs, std::uint32_t seed,
           std::uint32_t bars, std::optional<double> bpm, const std::optional<std::string>& key) {
            return run(false, style, packs, seed, bars, bpm, key);
        },
        py::arg("style"), py::arg("packs"), py::arg("seed") = 1, py::arg("bars") = 32,
        py::arg("bpm") = py::none(), py::arg("key") = py::none(),
        "The style's channels, mixer, playlist skeleton and markers, without notes.");
}
