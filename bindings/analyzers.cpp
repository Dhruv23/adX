// The six analyzers (phase_10.md §4.4): an Analyzers hub on the engine's master tap, and
// the frame call that builds them with the meters - still one FFI call a frame, however
// many analyzer panels are open (visualizer_one_ffi_call).
//
//     hub = Analyzers(engine); hub.configure({"spectrum": True, "fft_size": 8192})
//     (ticks, rolling, strips, samples, meters, layers, image, readouts) =
//         engine.frame_with_analyzers(levels, meter_geometry_or_None, hub, dt)
//
// `layers` holds (address, vertex count, revision) per analyzer layer in kLayerNames'
// order, read before any other engine call as the meters' upload is; `image` is the
// spectrogram's (address, rows, columns, head, pushed); `readouts` the numbers the
// panels print (correlation, momentary, short-term, integrated).
#include <pybind11/stl.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "bindings/Bindings.h"
#include "bindings/EngineHandle.h"
#include "engine/geometry/AnalyzerHub.h"
#include "engine/geometry/MeterGeometry.h"

namespace py = pybind11;

using adx::bindings::EngineHandle;
using adx::geometry::AnalyzerHub;
using adx::geometry::AnalyzerSettings;
using adx::geometry::GeometryBuffer;

namespace {

constexpr std::array<const char*, 8> kLayerNames{
    "spectrum.fill", "spectrum.lines",   "scope",    "vector.trace", "vector.meter",
    "loudness.bars", "loudness.history", "particles"};

struct PyAnalyzers {
    std::unique_ptr<AnalyzerHub> hub;
    std::uint32_t masterInsert{1};
};

adx::dsp::WindowKind windowOf(const std::string& name) {
    if (name == "hann") {
        return adx::dsp::WindowKind::Hann;
    }
    if (name == "hamming") {
        return adx::dsp::WindowKind::Hamming;
    }
    if (name == "blackman-harris") {
        return adx::dsp::WindowKind::BlackmanHarris;
    }
    if (name == "rectangular") {
        return adx::dsp::WindowKind::Rectangular;
    }
    throw py::value_error("window is one of hann, hamming, blackman-harris, rectangular");
}

const char* windowName(adx::dsp::WindowKind kind) {
    switch (kind) {
    case adx::dsp::WindowKind::Hamming:
        return "hamming";
    case adx::dsp::WindowKind::BlackmanHarris:
        return "blackman-harris";
    case adx::dsp::WindowKind::Rectangular:
        return "rectangular";
    default:
        return "hann";
    }
}

template<class T> void take(const py::dict& d, const char* key, T& field) {
    if (d.contains(key)) {
        field = d[key].cast<T>();
    }
}

void configure(PyAnalyzers& self, const py::dict& d) {
    AnalyzerSettings s = self.hub->settings();
    take(d, "spectrum", s.spectrum);
    take(d, "spectrogram", s.spectrogram);
    take(d, "scope", s.scope);
    take(d, "vector", s.vector);
    take(d, "loudness", s.loudness);
    take(d, "particles", s.particles);
    take(d, "fft_size", s.fftSize);
    if (d.contains("window")) {
        s.window = windowOf(d["window"].cast<std::string>());
    }
    take(d, "spectrum_columns", s.spectrumStyle.columns);
    take(d, "floor_db", s.spectrumStyle.floorDb);
    take(d, "peak_hold", s.spectrumStyle.peakHold);
    take(d, "peak_fall", s.spectrumStyle.peakFallDbPerSecond);
    take(d, "spectrogram_rows", s.spectrogramRows);
    take(d, "spectrogram_columns", s.spectrogramColumns);
    take(d, "spectrogram_rate", s.spectrogramRate);
    take(d, "scope_frames", s.scopeStyle.windowFrames);
    take(d, "scope_trigger", s.scopeStyle.trigger);
    take(d, "scope_hold", s.scopeStyle.hold);
    take(d, "scope_gain", s.scopeStyle.gain);
    take(d, "vector_frames", s.vectorStyle.windowFrames);
    take(d, "vector_gain", s.vectorStyle.gain);
    take(d, "loudness_floor", s.loudnessStyle.floorLufs);
    take(d, "loudness_history", s.loudnessStyle.historyFrames);
    if (d.contains("reset_peaks") && d["reset_peaks"].cast<bool>()) {
        // Main thread, as the spectrum's geometry is.
        self.hub->spectrum().resetPeaks();
    }
    if (d.contains("master_insert")) {
        self.masterInsert = d["master_insert"].cast<std::uint32_t>();
    }
    self.hub->configure(s);
}

py::dict settingsOf(const PyAnalyzers& self) {
    const AnalyzerSettings s = self.hub->settings();
    py::dict d;
    d["spectrum"] = s.spectrum;
    d["spectrogram"] = s.spectrogram;
    d["scope"] = s.scope;
    d["vector"] = s.vector;
    d["loudness"] = s.loudness;
    d["particles"] = s.particles;
    d["fft_size"] = s.fftSize;
    d["window"] = windowName(s.window);
    d["peak_hold"] = s.spectrumStyle.peakHold;
    d["scope_frames"] = s.scopeStyle.windowFrames;
    d["scope_trigger"] = s.scopeStyle.trigger;
    d["scope_hold"] = s.scopeStyle.hold;
    d["spectrogram_rows"] = s.spectrogramRows;
    d["spectrogram_columns"] = s.spectrogramColumns;
    d["master_insert"] = self.masterInsert;
    return d;
}

py::tuple upload(GeometryBuffer& buffer) {
    return py::make_tuple(reinterpret_cast<std::uintptr_t>(buffer.data()), buffer.vertexCount(),
                          buffer.revision());
}

py::tuple frameWithAnalyzers(EngineHandle& self, adx::bindings::LevelArray& levels,
                             const py::object& meters, PyAnalyzers& analyzers, double dtSeconds) {
    const py::tuple base = adx::bindings::frameInto(self, levels);
    py::object meterUpload = py::none();
    if (!meters.is_none()) {
        auto& geometry = meters.cast<adx::geometry::MeterGeometry&>();
        self.meterScratch.clear();
        for (const auto& strip : self.levelScratch) {
            self.meterScratch.push_back(strip.frame);
        }
        geometry.build(self.meterScratch);
        GeometryBuffer& bars = geometry.buffer();
        meterUpload = upload(bars);
    }
    adx::rt::LevelFrame master;
    for (const auto& strip : self.levelScratch) {
        if (strip.insert.value == analyzers.masterInsert) {
            master = strip.frame;
        }
    }
    AnalyzerHub& hub = *analyzers.hub;
    hub.frame(master, dtSeconds);
    const py::tuple layers = py::make_tuple(
        upload(hub.spectrum().fill()), upload(hub.spectrum().lines()), upload(hub.scope().lines()),
        upload(hub.vector().trace()), upload(hub.vector().meter()), upload(hub.loudness().bars()),
        upload(hub.loudness().history()), upload(hub.particles().particles()));
    const auto& image = hub.spectrogram();
    const py::tuple spectrogram =
        py::make_tuple(reinterpret_cast<std::uintptr_t>(image.data()), image.rows(),
                       image.columns(), image.head(), image.pushed());
    const py::tuple readouts =
        py::make_tuple(hub.vector().correlation(), hub.loudness().momentary(),
                       hub.loudness().shortTerm(), hub.loudness().integrated());
    return py::make_tuple(base[0], base[1], base[2], base[3], meterUpload, layers, spectrogram,
                          readouts);
}

} // namespace

void registerAnalyzerBindings(py::module_& m) {
    py::class_<PyAnalyzers>(
        m, "Analyzers",
        "The six analyzers on an engine's master tap, their FFT on a worker thread "
        "(phase_10.md 4.4). Built by Engine.frame_with_analyzers.")
        // GIL: trivial - starts the worker thread
        .def(py::init([](EngineHandle& engine) {
                 if (!engine.engine) {
                     throw py::value_error("the engine is not open");
                 }
                 auto out = std::make_unique<PyAnalyzers>();
                 out->hub = std::make_unique<AnalyzerHub>(engine.engine->audio().scopeTap(),
                                                          engine.engine->options().sampleRate);
                 return out;
             }),
             py::arg("engine"), py::keep_alive<1, 2>())
        // GIL: trivial - copies settings under a mutex
        .def("configure", &configure, py::arg("settings"),
             "Keys: spectrum spectrogram scope vector loudness particles (open or not), fft_size, "
             "window (hann hamming blackman-harris rectangular), spectrum_columns, floor_db, "
             "peak_hold, peak_fall, spectrogram_rows, spectrogram_columns, spectrogram_rate, "
             "scope_frames, scope_trigger, scope_hold, scope_gain, vector_frames, vector_gain, "
             "loudness_floor, loudness_history, master_insert, reset_peaks.")
        // GIL: trivial - copies settings under a mutex
        .def("settings", &settingsOf)
        .def_property_readonly("transforms",
                               [](const PyAnalyzers& a) { return a.hub->transforms(); })
        .def_property_readonly("frames_lost",
                               [](const PyAnalyzers& a) { return a.hub->framesLost(); })
        .def_property_readonly_static("layers", [](const py::object&) {
            py::tuple out(kLayerNames.size());
            for (std::size_t i = 0; i < kLayerNames.size(); ++i) {
                out[i] = kLayerNames.at(i);
            }
            return out;
        });

    m.attr("Engine").cast<py::class_<EngineHandle>>().def(
        "frame_with_analyzers", &frameWithAnalyzers, py::arg("levels"), py::arg("meters"),
        py::arg("analyzers"), py::arg("dt"),
        // GIL: trivial - O(strips) plus each open analyzer's O(columns) build
        "frame() with the meters (or None) and every open analyzer built in the same call. Returns "
        "(position_ticks, rolling, strip_count, position_samples, meter_upload or None, analyzer "
        "layer uploads in Analyzers.layers order, spectrogram (address, rows, columns, head, "
        "pushed), readouts (correlation, momentary, short-term, integrated LUFS)).");
}
