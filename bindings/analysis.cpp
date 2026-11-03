// Phase 10's analyses for the app and `adx analyze` (phase_10.md §4.1, STATE.md P10-2):
// key, chords, tempo, timbre, melody and form, each as an AnalysisJob - progress,
// cancel, a result when done - and the key and chords also as plain calls.
//
// A job's inputs are copied into C++ under the GIL when it is made, and its result is
// turned into Python only in result(), so the worker never touches a Python object.
// Nothing here writes the project: the app applies a result as one undo group.
#include <pybind11/stl.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bindings/Bindings.h"
#include "engine/analysis/AnalysisJob.h"
#include "engine/analysis/AudioInput.h"
#include "engine/analysis/BpmDetector.h"
#include "engine/analysis/ChordDetector.h"
#include "engine/analysis/ChordSuggest.h"
#include "engine/analysis/Chroma.h"
#include "engine/analysis/KeyDetector.h"
#include "engine/analysis/MelodyExtractor.h"
#include "engine/analysis/TimbreAnalyzer.h"
#include "engine/analysis/TrackCleaner.h"
#include "engine/preset/Preset.h"

namespace py = pybind11;
namespace an = adx::analysis;

namespace {

/// Analyses run at this rate: the chroma's bins and the detectors' tests assume it.
constexpr std::uint32_t kAnalysisRate = 22050;

const char* modeName(an::Mode mode) {
    return mode == an::Mode::Major ? "major" : "minor";
}

/// What a job was asked for, copied out of Python.
struct Request {
    std::string kind;
    std::string path;
    /// Seconds-based notes (key, chords, timbre).
    std::vector<an::TimedChromaNote> notes;
    /// Tick-based notes (form).
    std::vector<an::CleanNote> cleanNotes;
    double frameSeconds{0.5};
    std::int64_t phraseTicks{0};
    std::int64_t grid{0};
    std::string name{"Analyzed"};
};

struct KeyResult {
    an::KeyEstimate estimate;
};

/// Everything any kind can produce; the kind says which parts mean something.
struct Outcome {
    std::optional<an::KeyEstimate> key;
    std::vector<an::ChordSegment> chords;
    double chordFrameSeconds{0.0};
    std::optional<double> bpm;
    std::optional<an::TimbreAnalysis> timbre;
    std::vector<an::DetectedNote> melody;
    std::optional<an::FormAnalysis> form;
    std::vector<an::CleanNote> rebuilt;
    std::string error;
};

std::vector<an::TimedChromaNote> timedNotesOf(const py::handle& list) {
    std::vector<an::TimedChromaNote> out;
    for (const py::handle item : list) {
        const auto t = item.cast<py::sequence>();
        out.push_back({.startSeconds = t[0].cast<double>(),
                       .seconds = t[1].cast<double>(),
                       .pitch = t[2].cast<std::uint8_t>(),
                       .velocity = t.size() > 3 ? t[3].cast<float>() / 127.0F : 1.0F});
    }
    return out;
}

std::vector<an::CleanNote> cleanNotesOf(const py::handle& list) {
    std::vector<an::CleanNote> out;
    for (const py::handle item : list) {
        const auto t = item.cast<py::sequence>();
        out.push_back({.start = t[0].cast<std::int64_t>(),
                       .length = t[1].cast<std::int64_t>(),
                       .pitch = t[2].cast<std::uint8_t>(),
                       .velocity = t.size() > 3 ? t[3].cast<std::uint8_t>() : std::uint8_t{100}});
    }
    return out;
}

Request requestOf(const std::string& kind, const py::dict& params) {
    Request r;
    r.kind = kind;
    if (params.contains("path")) {
        r.path = params["path"].cast<std::string>();
    }
    if (params.contains("notes")) {
        if (kind == "form") {
            r.cleanNotes = cleanNotesOf(params["notes"]);
        } else {
            r.notes = timedNotesOf(params["notes"]);
        }
    }
    if (params.contains("frame_seconds")) {
        r.frameSeconds = params["frame_seconds"].cast<double>();
    }
    if (params.contains("phrase_ticks")) {
        r.phraseTicks = params["phrase_ticks"].cast<std::int64_t>();
    }
    if (params.contains("grid")) {
        r.grid = params["grid"].cast<std::int64_t>();
    }
    if (params.contains("name")) {
        r.name = params["name"].cast<std::string>();
    }
    static const std::vector<std::string> kinds{"key", "chords", "bpm", "timbre", "melody", "form"};
    if (std::ranges::find(kinds, kind) == kinds.end()) {
        throw py::value_error("unknown analysis '" + kind + "'");
    }
    if ((kind == "bpm" || kind == "timbre" || kind == "melody") && r.path.empty()) {
        throw py::value_error("analysis '" + kind + "' needs a path");
    }
    if ((kind == "key" || kind == "chords") && r.path.empty() && r.notes.empty()) {
        throw py::value_error("analysis '" + kind + "' needs a path or notes");
    }
    if (kind == "timbre" && r.notes.empty()) {
        throw py::value_error("timbre analysis needs the notes that sound in the audio");
    }
    if (kind == "form" && (r.phraseTicks <= 0 || r.grid <= 0)) {
        throw py::value_error("form analysis needs phrase_ticks and grid");
    }
    if (kind == "melody" && !an::melodyExtractorAvailable()) {
        throw py::value_error("this build has no melody extractor (ADX_ENABLE_ONNX is off)");
    }
    return r;
}

bool loadAudio(const Request& r, std::uint32_t rate, an::MonoAudio& audio, Outcome& out) {
    if (!an::loadMono(r.path, rate, audio, out.error)) {
        return false;
    }
    return true;
}

an::Chromagram chromaOf(const Request& r, an::AnalysisControl& control, Outcome& out, bool& ok) {
    ok = true;
    if (!r.path.empty()) {
        an::MonoAudio audio;
        if (!loadAudio(r, kAnalysisRate, audio, out)) {
            ok = false;
            return {};
        }
        an::ChromaOptions options;
        options.progress = [&control](double f) { return control.proceed(f * 0.9); };
        return an::chromagram(audio.samples, audio.sampleRate, options);
    }
    return an::chromagramOfNotes(r.notes, r.frameSeconds);
}

/// The work of one job; runs on its thread.
void run(const Request& r, an::AnalysisControl& control, Outcome& out) {
    if (r.kind == "key" || r.kind == "chords") {
        bool ok = false;
        const an::Chromagram chroma = chromaOf(r, control, out, ok);
        if (!ok || control.cancelled()) {
            return;
        }
        out.key = an::estimateKey(chroma);
        if (r.kind == "chords") {
            out.chords = an::detectChords(chroma);
            out.chordFrameSeconds = static_cast<double>(chroma.hop) / chroma.sampleRate;
        }
        return;
    }
    if (r.kind == "bpm") {
        an::MonoAudio audio;
        if (loadAudio(r, 0, audio, out) && !control.cancelled()) {
            out.bpm = an::estimateBpm(audio.samples, audio.sampleRate);
        }
        return;
    }
    if (r.kind == "timbre") {
        an::MonoAudio audio;
        if (!loadAudio(r, 48000, audio, out) || control.cancelled()) {
            return;
        }
        std::vector<an::TimbreNote> notes;
        notes.reserve(r.notes.size());
        for (const an::TimedChromaNote& n : r.notes) {
            notes.push_back(
                {.startSeconds = n.startSeconds, .seconds = n.seconds, .pitch = n.pitch});
        }
        an::TimbreOptions options;
        options.name = r.name;
        options.progress = [&control](double f) { return control.proceed(f); };
        out.timbre = an::analyzeTimbre(audio.samples, audio.sampleRate, notes, options);
        if (!out.timbre && !control.cancelled()) {
            out.error = "no note was long enough, inside the audio and audible";
        }
        return;
    }
    if (r.kind == "melody") {
#if defined(ADX_ENABLE_ONNX)
        an::MonoAudio audio;
        if (!loadAudio(r, an::kMelodySampleRate, audio, out) || control.cancelled()) {
            return;
        }
        const auto notes = an::extractMelody(audio.samples, an::defaultMelodyModel(),
                                             [&control](double f) { return control.proceed(f); });
        if (notes) {
            out.melody = *notes;
        } else if (!control.cancelled()) {
            out.error = "melody extraction failed (is the model present?)";
        }
#endif
        return;
    }
    if (r.kind == "form") {
        an::FormAnalysis form;
        out.rebuilt = an::cleanNotes(r.cleanNotes, r.phraseTicks, r.grid, &form);
        out.form = std::move(form);
    }
}

py::dict keyOf(const an::KeyEstimate& e) {
    py::dict d;
    const an::Key best = e.best();
    d["name"] = an::keyName(best);
    d["tonic"] = best.tonic;
    d["mode"] = modeName(best.mode);
    d["confidence"] = e.confidence;
    py::list ranked;
    for (const an::KeyCandidate& c : e.ranked) {
        ranked.append(py::make_tuple(an::keyName(c.key), c.score));
    }
    d["ranked"] = ranked;
    return d;
}

py::list suggestionsOf(const an::Key& key, const std::optional<an::Chord>& previous) {
    py::list out;
    for (const an::ChordSuggestion& s : an::suggestChords(key, previous)) {
        py::dict d;
        d["chord"] = an::chordName(s.chord);
        d["roman"] = an::romanNumeral(key, s.chord);
        d["degree"] = s.degree;
        d["score"] = s.score;
        d["tones"] = an::chordTones(s.chord);
        out.append(d);
    }
    return out;
}

py::dict outcomeOf(const Request& r, const Outcome& o, bool cancelled) {
    py::dict d;
    d["kind"] = r.kind;
    d["cancelled"] = cancelled;
    d["ok"] = !cancelled && o.error.empty();
    d["error"] = o.error;
    if (cancelled || !o.error.empty()) {
        return d;
    }
    if (o.key) {
        d["key"] = keyOf(*o.key);
    }
    if (r.kind == "chords") {
        py::list segments;
        std::optional<an::Chord> last;
        for (const an::ChordSegment& s : o.chords) {
            py::dict seg;
            seg["chord"] = an::chordName(s.chord);
            seg["root"] = s.chord.root;
            seg["start"] = static_cast<double>(s.firstFrame) * o.chordFrameSeconds;
            seg["end"] = static_cast<double>(s.endFrame) * o.chordFrameSeconds;
            seg["confidence"] = s.confidence;
            seg["roman"] = o.key ? an::romanNumeral(o.key->best(), s.chord) : std::string{};
            segments.append(seg);
            if (s.chord.quality != an::ChordQuality::None) {
                last = s.chord;
            }
        }
        d["segments"] = segments;
        d["suggestions"] = o.key ? suggestionsOf(o.key->best(), last) : py::list{};
    }
    if (r.kind == "bpm") {
        d["bpm"] = o.bpm ? py::cast(*o.bpm) : py::none();
    }
    if (r.kind == "timbre" && o.timbre) {
        d["preset_text"] = adx::preset::writePreset(o.timbre->preset);
        d["harmonics"] = std::vector<float>(o.timbre->harmonics.begin(), o.timbre->harmonics.end());
        d["attack"] = o.timbre->attackSeconds;
        d["decay"] = o.timbre->decaySeconds;
        d["sustain"] = o.timbre->sustain;
        d["release"] = o.timbre->releaseSeconds;
        d["notes_measured"] = o.timbre->notesMeasured;
        py::list params;
        for (const adx::project::ParamValue& p : o.timbre->preset.params) {
            params.append(py::make_tuple(p.name, p.value));
        }
        d["params"] = params;
    }
    if (r.kind == "melody") {
        py::list notes;
        for (const an::DetectedNote& n : o.melody) {
            notes.append(py::make_tuple(n.startSeconds, n.seconds, n.pitch, n.velocity));
        }
        d["notes"] = notes;
    }
    if (r.kind == "form" && o.form) {
        d["form"] = o.form->observedForm;
        d["name"] = o.form->formName;
        d["blueprint"] = o.form->blueprint;
        d["target"] = o.form->target;
        d["used_blueprint"] = o.form->usedBlueprint;
        py::list markers;
        for (const an::FormMarker& m : an::formMarkers(*o.form)) {
            markers.append(py::make_tuple(m.tick, std::string(1, m.label)));
        }
        d["markers"] = markers;
        py::list notes;
        for (const an::CleanNote& n : o.rebuilt) {
            notes.append(py::make_tuple(n.start, n.length, n.pitch, n.velocity));
        }
        d["notes"] = notes;
    }
    return d;
}

/// The job and what it fills in, kept together so the worker's references stay valid.
class PyAnalysisJob {
public:
    PyAnalysisJob(const std::string& kind, const py::dict& params)
        : m_request(std::make_unique<Request>(requestOf(kind, params))),
          m_outcome(std::make_unique<Outcome>()),
          m_job(std::make_unique<an::AnalysisJob>(
              [request = m_request.get(), outcome = m_outcome.get()](an::AnalysisControl& control) {
                  run(*request, control, *outcome);
              })) {}

    [[nodiscard]] float progress() const noexcept {
        return m_job->progress();
    }
    [[nodiscard]] bool done() const noexcept {
        return m_job->done();
    }
    void cancel() noexcept {
        m_job->cancel();
    }
    [[nodiscard]] const std::string& kind() const noexcept {
        return m_request->kind;
    }
    /// Blocks until done; call without the GIL.
    void wait() {
        m_job->wait();
    }
    py::dict result() {
        {
            const py::gil_scoped_release released;
            m_job->wait();
        }
        if (!m_job->error().empty() && m_outcome->error.empty()) {
            m_outcome->error = m_job->error();
        }
        return outcomeOf(*m_request, *m_outcome, m_job->cancelled());
    }

private:
    std::unique_ptr<Request> m_request;
    std::unique_ptr<Outcome> m_outcome;
    std::unique_ptr<an::AnalysisJob> m_job;
};

an::Key keyFromName(const std::string& name) {
    for (std::uint8_t tonic = 0; tonic < 12; ++tonic) {
        for (const an::Mode mode : {an::Mode::Major, an::Mode::Minor}) {
            const an::Key key{.tonic = tonic, .mode = mode};
            if (an::keyName(key) == name) {
                return key;
            }
        }
    }
    throw py::value_error("unknown key '" + name + "' (spelled like 'C major', 'F# minor')");
}

std::optional<an::Chord> chordFromName(const std::string& name) {
    if (name.empty()) {
        return std::nullopt;
    }
    for (std::uint8_t root = 0; root < 12; ++root) {
        for (int q = 0; q <= static_cast<int>(an::ChordQuality::Minor7); ++q) {
            const an::Chord chord{.root = root, .quality = static_cast<an::ChordQuality>(q)};
            if (an::chordName(chord) == name) {
                return chord;
            }
        }
    }
    throw py::value_error("unknown chord '" + name + "'");
}

} // namespace

void registerAnalysisBindings(py::module_& m) {
    py::class_<PyAnalysisJob>(m, "AnalysisJob",
                              "One analysis on a thread of its own (phase_10.md §4.1): progress, "
                              "cancel, and the result as a dict when done. Kinds: key, chords "
                              "(path, or notes as (start_s, seconds, pitch[, velocity]) with "
                              "frame_seconds), bpm (path), timbre (path, notes, name), melody "
                              "(path; ONNX builds only), form (notes as (start, length, pitch[, "
                              "velocity]) in ticks, phrase_ticks, grid).")
        // GIL: trivial - copies the request and starts the thread
        .def(py::init<const std::string&, const py::dict&>(), py::arg("kind"), py::arg("params"))
        .def_property_readonly("progress", &PyAnalysisJob::progress)
        .def_property_readonly("done", &PyAnalysisJob::done)
        .def_property_readonly("kind", &PyAnalysisJob::kind)
        // GIL: trivial - sets an atomic flag
        .def("cancel", &PyAnalysisJob::cancel)
        .def("result", &PyAnalysisJob::result,
             "Wait for the job; a dict with ok, cancelled, error and the kind's fields.");

    m.def(
        "analyze",
        [](const std::string& kind, const py::dict& params) {
            PyAnalysisJob job(kind, params);
            {
                const py::gil_scoped_release released;
                job.wait();
            }
            return job.result();
        },
        py::arg("kind"), py::arg("params"),
        "AnalysisJob(kind, params).result(): the same, waited for.");

    // GIL: trivial - a constant
    m.def("melody_extractor_available", &an::melodyExtractorAvailable,
          "True when this build has the ONNX melody extractor (ADX_ENABLE_ONNX).");

    // GIL: trivial - a table lookup over at most a dozen chords
    m.def(
        "suggest_chords",
        [](const std::string& key, const std::string& previous) {
            return suggestionsOf(keyFromName(key), chordFromName(previous));
        },
        py::arg("key"), py::arg("previous") = "",
        "Ranked next-chord suggestions in `key` after `previous` (a chord name, or none).");

    // GIL: trivial - seven pitch classes
    m.def(
        "scale_of",
        [](const std::string& key) {
            const auto scale = an::scaleOf(keyFromName(key));
            return std::vector<std::uint8_t>(scale.begin(), scale.end());
        },
        py::arg("key"), "The key's seven pitch classes, tonic first (natural minor for minor).");
}
