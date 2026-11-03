// Phase 8's offline work, exposed to Python: the export matrix and its job, MIDI files,
// collect-and-save, freeze, and the audio analysis that seeds warp markers.
//
// Everything here that touches audio runs with the GIL released (Rule 3), and returns
// paths and numbers rather than samples (Rule 2).

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include "bindings/Bindings.h"
#include "bindings/ProjectHandle.h"
#include "engine/analysis/BpmDetector.h"
#include "engine/analysis/OnsetDetector.h"
#include "engine/clip/Freeze.h"
#include "engine/format/audio/AudioFileLoader.h"
#include "engine/format/midi/MidiExporter.h"
#include "engine/format/midi/MidiImporter.h"
#include "engine/render/Bundle.h"
#include "engine/render/ExportJob.h"

namespace py = pybind11;
namespace render = adx::render;

namespace {

template<class T> T get(const py::dict& d, const char* key, T fallback) {
    return d.contains(key) && !d[key].is_none() ? d[key].cast<T>() : std::move(fallback);
}

/// The request as Python spells it: a dict of plain values, every key optional.
render::ExportRequest requestOf(const py::dict& d) {
    render::ExportRequest r;
    r.output = get<std::string>(d, "output", "");
    if (r.output.empty()) {
        throw py::value_error("an export needs an 'output' path");
    }
    const auto format = get<std::string>(d, "format", "wav24");
    if (!render::fileFormatFromString(format, r.encode.format)) {
        throw py::value_error("unknown format '" + format + "'");
    }
    r.encode.sampleRate = get<std::uint32_t>(d, "sample_rate", 0);
    r.encode.mp3Bitrate = get<std::uint32_t>(d, "mp3_bitrate", 320);
    r.encode.flacLevel = get<std::uint32_t>(d, "flac_level", 5);
    r.encode.flacBits = get<std::uint32_t>(d, "flac_bits", 24);
    r.encode.oggQuality = get<float>(d, "ogg_quality", 0.6F);
    r.renderRate = get<std::uint32_t>(d, "render_rate", 48000);
    r.blockFrames = get<std::uint32_t>(d, "block", 256);

    const auto range = get<std::string>(d, "range", "project");
    if (range == "loop") {
        r.range = render::RangeKind::Loop;
    } else if (range == "markers") {
        r.range = render::RangeKind::Markers;
    } else if (range == "selection") {
        r.range = render::RangeKind::Selection;
    } else if (range != "project") {
        throw py::value_error("range is project, loop, markers or selection");
    }
    r.from = adx::core::Ticks{get<std::int64_t>(d, "from", 0)};
    r.to = adx::core::Ticks{get<std::int64_t>(d, "to", 0)};
    r.fromMarker = get<std::string>(d, "from_marker", "");
    r.toMarker = get<std::string>(d, "to_marker", "");
    r.splitAtMarkers = get<bool>(d, "split_at_markers", false);

    r.master = get<bool>(d, "master", true);
    const auto stems = get<std::string>(d, "stems", "");
    if (!stems.empty()) {
        render::StemMode mode{};
        if (!render::stemModeFromString(stems, mode)) {
            throw py::value_error("stems is channels, inserts or patterns");
        }
        r.stems = mode;
    }
    r.tailSeconds = get<double>(d, "tail", 0.0);
    r.tailUntilSilence = get<bool>(d, "tail_until_silence", false);
    r.silenceDb = get<float>(d, "silence_db", -80.0F);
    r.maxTailSeconds = get<double>(d, "max_tail", 30.0);

    const auto dither = get<std::string>(d, "dither", "tpdf");
    r.dither = render::DitherKind::Tpdf;
    if (dither == "off") {
        r.dither = render::DitherKind::Off;
    } else if (dither == "shaped") {
        r.dither = render::DitherKind::Shaped;
    }
    const auto normalize = get<std::string>(d, "normalize", "off");
    r.normalize = render::NormalizeKind::Off;
    if (normalize == "peak") {
        r.normalize = render::NormalizeKind::Peak;
    } else if (normalize == "lufs") {
        r.normalize = render::NormalizeKind::Lufs;
    }
    r.normalizeTarget = get<float>(d, "normalize_target",
                                   r.normalize == render::NormalizeKind::Lufs ? -14.0F : -1.0F);
    r.writeMidi = get<bool>(d, "midi", false);
    return r;
}

py::dict resultOf(const render::ExportResult& result) {
    py::list files;
    for (const render::ExportedFile& file : result.files) {
        py::dict f;
        f["path"] = file.path.string();
        f["label"] = file.label;
        f["frames"] = file.frames;
        f["sample_rate"] = file.sampleRate;
        f["peak"] = file.stats.peak;
        f["rms"] = file.stats.rms;
        f["hash"] = file.stats.hash.hex();
        files.append(f);
    }
    py::dict out;
    out["ok"] = result.ok;
    out["cancelled"] = result.cancelled;
    out["error"] = result.error;
    out["files"] = files;
    out["gain_db"] = result.gainDb;
    out["loudness"] = result.loudness;
    return out;
}

/// A decoded file, mono-summed: what the analysis takes.
std::vector<float> monoOf(const std::string& path, std::uint32_t& rate) {
    adx::format::SampleBuffer buffer;
    std::string error;
    if (!adx::format::decodeFile(path, buffer, error)) {
        throw std::runtime_error(error);
    }
    rate = buffer.sampleRate;
    std::vector<float> mono(buffer.frames);
    for (std::size_t i = 0; i < mono.size(); ++i) {
        mono[i] = 0.5F * (buffer.leftAudio()[i] + buffer.rightAudio()[i]);
    }
    return mono;
}

void registerFreezeAndAnalysis(py::module_& m);

} // namespace

void registerExportBindings(py::module_& m) {
    m.def(
        "export_project",
        [](adx::bindings::ProjectHandle& project, const py::dict& request) {
            const render::ExportRequest r = requestOf(request);
            render::ExportResult result;
            {
                const py::gil_scoped_release released;
                result = render::runExport(project.project, r);
            }
            return resultOf(result);
        },
        py::arg("project"), py::arg("request"),
        "Export `project` as `request` describes (phase_8.md §3.5) and wait for it. Keys: "
        "output, format (wav16 wav24 wav32f mp3 flac ogg), sample_rate, mp3_bitrate, "
        "flac_level, flac_bits, ogg_quality, render_rate, block, range (project loop markers "
        "selection), from, to, from_marker, to_marker, split_at_markers, master, stems "
        "(channels inserts patterns), tail, tail_until_silence, silence_db, max_tail, dither "
        "(tpdf shaped off), normalize (off peak lufs), normalize_target, midi.");

    py::class_<render::ExportJob>(m, "ExportJob",
                                  "An export on a thread of its own, with its own copy of the "
                                  "project: progress, cancel, and the result when done.")
        // GIL: trivial - copies the project and starts the thread
        .def(py::init([](adx::bindings::ProjectHandle& project, const py::dict& request) {
                 return std::make_unique<render::ExportJob>(project.project, requestOf(request));
             }),
             py::arg("project"), py::arg("request"))
        .def_property_readonly("progress", &render::ExportJob::progress)
        .def_property_readonly("done", &render::ExportJob::done)
        // GIL: trivial - sets an atomic flag
        .def("cancel", &render::ExportJob::cancel)
        .def(
            "result",
            [](render::ExportJob& job) {
                const render::ExportResult* result = nullptr;
                {
                    const py::gil_scoped_release released;
                    result = &job.wait();
                }
                return resultOf(*result);
            },
            "Wait for the job and return what export_project would have.");

    // GIL: trivial - a MIDI file parses in well under a millisecond; then one command
    m.def(
        "import_midi",
        [](adx::bindings::ProjectHandle& project, const std::string& path) {
            std::string error;
            const std::optional<adx::format::midi::MidiSong> song =
                adx::format::midi::readMidiFile(path, error);
            if (!song) {
                throw std::runtime_error(error);
            }
            const std::string stem = std::filesystem::path(path).stem().string();
            py::dict out;
            out["pattern"] = adx::format::midi::importSong(*song, project.project, project.stack,
                                                           stem.empty() ? "MIDI" : stem);
            out["tracks"] = song->tracks.size();
            out["division"] = song->division;
            return out;
        },
        py::arg("project"), py::arg("path"),
        "Import a Standard MIDI File into `project` as one undo step: a channel per track, "
        "one pattern of their notes placed at the start, its tempo, meter and markers.");
    m.def(
        "export_midi",
        [](adx::bindings::ProjectHandle& project, const std::string& path) {
            std::string error;
            bool ok = false;
            {
                const py::gil_scoped_release released;
                ok = adx::format::midi::exportMidiFile(project.project, path, error);
            }
            if (!ok) {
                throw std::runtime_error(error);
            }
        },
        py::arg("project"), py::arg("path"),
        "Write the arrangement as a type-1 Standard MIDI File, a track per channel.");

    m.def(
        "bundle_project",
        [](const std::string& path, const std::string& destination, bool zip,
           const std::vector<std::string>& library) {
            render::BundleOptions options{
                .project = path, .destination = destination, .zip = zip, .library = {}};
            for (const std::string& folder : library) {
                options.library.emplace_back(folder);
            }
            render::BundleResult result;
            {
                const py::gil_scoped_release released;
                result = render::bundleProject(options);
            }
            py::dict out;
            out["ok"] = result.ok;
            out["error"] = result.error;
            out["project"] = result.projectOut.string();
            out["zip"] = result.zipOut.string();
            out["copied"] = result.filesCopied;
            out["missing"] = result.missing;
            out["rewritten"] = result.rewritten;
            return out;
        },
        py::arg("path"), py::arg("destination"), py::arg("zip") = false,
        py::arg("library") = std::vector<std::string>{},
        "Collect-and-save the project at `path` into `destination` (phase_8.md §3.7).");
    registerFreezeAndAnalysis(m);
}

namespace {

void registerFreezeAndAnalysis(py::module_& m) {
    m.def(
        "freeze_channel",
        [](adx::bindings::ProjectHandle& project, std::uint32_t channel) {
            adx::clip::FreezeResult result;
            {
                const py::gil_scoped_release released;
                result = adx::clip::freezeChannel(project.project, project.stack,
                                                  adx::core::ChannelId{channel});
            }
            if (!result.ok) {
                throw std::runtime_error(result.error);
            }
            py::dict out;
            out["file"] = result.file.string();
            out["frames"] = result.frames;
            return out;
        },
        py::arg("project"), py::arg("channel"),
        "Render a channel to freeze/<name>-<hash>.wav beside the project and play that in its "
        "place (phase_8.md §3.4). One undo step.");
    // GIL: trivial - one command putting the saved channel back
    m.def(
        "unfreeze_channel",
        [](adx::bindings::ProjectHandle& project, std::uint32_t channel) {
            adx::clip::unfreezeChannel(project.project, project.stack,
                                       adx::core::ChannelId{channel});
        },
        py::arg("project"), py::arg("channel"));

    m.def(
        "estimate_bpm",
        [](const std::string& path) -> std::optional<double> {
            const py::gil_scoped_release released;
            std::uint32_t rate = 0;
            const std::vector<float> mono = monoOf(path, rate);
            return adx::analysis::estimateBpm(mono, rate);
        },
        py::arg("path"), "The tempo of an audio file, or None when it has no steady pulse.");
    m.def(
        "detect_onsets",
        [](const std::string& path) {
            std::vector<double> onsets;
            {
                const py::gil_scoped_release released;
                std::uint32_t rate = 0;
                const std::vector<float> mono = monoOf(path, rate);
                onsets = adx::analysis::detectOnsets(mono, rate);
            }
            return py::array_t<double>(static_cast<py::ssize_t>(onsets.size()), onsets.data());
        },
        py::arg("path"), "Onset times, in seconds, of an audio file (spectral flux).");
}

} // namespace
