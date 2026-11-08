// The engine and its transport, exposed to Python (phase_3.md §4.12).
//
//     engine = adx_engine.Engine(null_backend=True)
//     engine.set_project(project)
//     engine.start()
//     t = engine.transport
//     t.play(); t.seek_ticks(n); t.set_loop(start, end, True)
//     t.position_ticks()        # an atomic the audio thread stores once per block
//
// Every call here is O(1) or O(project) and driven by a user action, which is what
// FINAL_PLAN §2.2 Rule 2 allows. Nothing here is on the audio thread and nothing the
// audio thread runs can reach Python (Rule 1). Building a snapshot is real work, so
// set_project and commit release the GIL (Rule 3).

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "bindings/EngineHandle.h"
#include "bindings/ProjectHandle.h"
#include "engine/audio/NullBackend.h"
#include "engine/audio/RtAudioBackend.h"
#include "engine/project/ParamRegistry.h"

namespace py = pybind11;

using adx::bindings::EngineHandle;
using adx::bindings::ProjectHandle;
using adx::bindings::TransportHandle;

namespace {

void throwOnError(const adx::audio::Error& error) {
    if (error.code != adx::audio::Error::Code::None) {
        throw std::runtime_error(std::string(error.message.view()));
    }
}

void throwOnCommit(const adx::render::CommitResult& result) {
    if (!result.error.empty()) {
        throw std::runtime_error(result.error);
    }
}

adx::render::RenderEngine& engineOf(TransportHandle& transport) {
    if (transport.engine == nullptr || !transport.engine->engine) {
        throw std::runtime_error("the engine behind this transport is gone");
    }
    return *transport.engine->engine;
}

void defineTransport(py::module_& m) {
    py::class_<TransportHandle>(m, "Transport", "The arrangement's playback position.")
        // GIL: trivial - posts one message to the audio thread
        .def("play", [](TransportHandle& t) { engineOf(t).play(); })
        // GIL: trivial - posts one message to the audio thread
        .def("stop", [](TransportHandle& t) { engineOf(t).stopPlayback(); })
        // GIL: trivial - posts one message to the audio thread
        .def(
            "seek_ticks",
            [](TransportHandle& t, std::int64_t ticks) {
                engineOf(t).seek(adx::core::Ticks{ticks});
            },
            py::arg("ticks"))
        // GIL: trivial - posts one message to the audio thread
        .def(
            "set_loop",
            [](TransportHandle& t, std::int64_t start, std::int64_t end, bool enabled) {
                engineOf(t).setLoop(adx::transport::LoopRegion{.start = adx::core::Ticks{start},
                                                               .end = adx::core::Ticks{end},
                                                               .enabled = enabled});
            },
            py::arg("start"), py::arg("end"), py::arg("enabled") = true)
        // GIL: trivial - reads an atomic
        .def(
            "position_ticks", [](TransportHandle& t) { return engineOf(t).positionTicks().value; },
            "Where the arrangement is, as of the audio thread's last block. O(1); poll it.")
        // GIL: trivial - reads an atomic
        .def("position_samples", [](TransportHandle& t) { return engineOf(t).positionSamples(); })
        // GIL: trivial - reads an atomic
        .def("state", [](TransportHandle& t) {
            return std::string(adx::transport::toString(engineOf(t).state()));
        });
}

/// The test-facing half: rendering an offline engine on demand, and whether a clip still
/// plays its stand-in (phase_8.md §11, 15).
void defineEngineOffline(py::class_<EngineHandle>& engine) {
    engine
        .def(
            "render_offline",
            [](EngineHandle& self, std::uint64_t frames) {
                if (self.offline == nullptr) {
                    throw std::runtime_error("render_offline needs an engine made with loopback=");
                }
                {
                    const py::gil_scoped_release released;
                    static_cast<void>(self.offline->renderFrames(frames));
                }
                self.engine->pump();
            },
            py::arg("frames"),
            "An offline engine only: render `frames` now, on this thread, as a device clock "
            "would have over that long.")
        // GIL: trivial - reads a vector's size
        .def_property_readonly(
            "audio_pending", [](const EngineHandle& self) { return self.engine->audioPending(); },
            "An audio clip still plays a stand-in until its buffer is ready; commit() "
            "swaps it in.");
}

/// An engine on a device, the null backend, or - with `loopback` - a test's offline backend
/// whose outputs come back to its inputs that many frames later.
std::unique_ptr<EngineHandle>
makeEngine(std::uint32_t sampleRate, std::uint32_t blockFrames, std::uint32_t outputChannels,
           bool nullBackend, std::uint32_t inputChannels, std::optional<std::uint32_t> inputDevice,
           std::optional<std::uint32_t> loopback, bool asio, bool exclusive) {
    auto handle = std::make_unique<EngineHandle>();
    std::unique_ptr<adx::audio::AudioBackend> backend;
    if (loopback) {
        // A test's engine: rendered on demand, its outputs wired back to its
        // inputs `loopback` frames later (OfflineBackend::setLoopback).
        auto offline = std::make_unique<adx::audio::OfflineBackend>();
        offline->setLoopback(*loopback);
        handle->offline = offline.get();
        backend = std::move(offline);
    } else if (nullBackend) {
        backend = std::make_unique<adx::audio::NullBackend>();
    } else if (exclusive) {
        auto device = std::make_unique<adx::audio::WasapiExclusiveBackend>();
        handle->exclusive = device.get();
        backend = std::move(device);
    } else {
        backend = std::make_unique<adx::audio::RtAudioBackend>(
            asio ? adx::audio::AudioApi::Asio : adx::audio::AudioApi::Default);
    }
    handle->engine = std::make_unique<adx::render::RenderEngine>(
        std::move(backend),
        adx::render::EngineOptions{.sampleRate = sampleRate,
                                   .blockFrames = blockFrames,
                                   .outputChannels = outputChannels,
                                   .inputDeviceId =
                                       inputChannels == 0
                                           ? adx::audio::kNoDevice
                                           : inputDevice.value_or(adx::audio::kDefaultDeviceId),
                                   .inputChannels = inputChannels,
                                   .offline = handle->offline != nullptr});
    return handle;
}

// NOLINTNEXTLINE(readability-function-size) - the Engine class's bindings, one .def each.
void defineEngine(py::module_& m) {
    py::class_<EngineHandle> engine(
        m, "Engine", "The render engine: a stream, a transport, and the project it plays.");
    engine
        // GIL: trivial - allocates the engine; nothing is opened until start(), which releases
        .def(py::init(&makeEngine), py::arg("sample_rate") = 48000, py::arg("block_frames") = 256,
             py::arg("output_channels") = 2, py::arg("null_backend") = false,
             py::arg("input_channels") = 0, py::arg("input_device") = py::none(),
             py::arg("loopback") = py::none(), py::arg("asio") = false,
             py::arg("exclusive") = false)
        .def(
            "start",
            [](EngineHandle& self) {
                if (self.running) {
                    return;
                }
                adx::audio::Error error;
                {
                    const py::gil_scoped_release released;
                    if (!self.opened) {
                        error = self.engine->open();
                    }
                    if (error.code == adx::audio::Error::Code::None) {
                        self.opened = true;
                        error = self.engine->start();
                    }
                }
                throwOnError(error);
                self.running = true;
            },
            "Open and start the stream. Messages sent before this apply on the first block.")
        // GIL: trivial - posts one message to the audio thread
        .def(
            "stop",
            [](EngineHandle& self) {
                const py::gil_scoped_release released;
                self.engine->stop();
                self.running = false;
            },
            "Stop the stream. The transport keeps its position.")
        .def(
            "set_project",
            [](EngineHandle& self, ProjectHandle& project) {
                adx::render::CommitResult result;
                {
                    const py::gil_scoped_release released;
                    result = self.engine->setProject(project.project, project.stack.revision());
                }
                throwOnCommit(result);
            },
            py::arg("project"), "Build a snapshot of `project` and hand it to the audio thread.")
        .def(
            "commit",
            [](EngineHandle& self, ProjectHandle& project) {
                adx::render::CommitResult result;
                {
                    const py::gil_scoped_release released;
                    result = self.engine->commit(project.project, project.stack);
                }
                throwOnCommit(result);
                return result.rebuilt;
            },
            py::arg("project"),
            "Bring the audio thread up to the project's latest edit, rebuilding only what "
            "changed. Returns whether a snapshot was built.")
        // GIL: trivial - one value onto the queue and one command, no rebuild
        .def(
            "set_param",
            [](EngineHandle& self, ProjectHandle& project, const std::string& path, float value) {
                const adx::project::ParamResolution resolved =
                    adx::project::ParamRegistry::resolve(path, project.project);
                if (!resolved.ok()) {
                    throw std::invalid_argument("no such parameter: " + path);
                }
                return self.engine->setParam(project.project, project.stack, resolved.ref, value);
            },
            py::arg("project"), py::arg("path"), py::arg("value"),
            "A knob turn: the edit goes into the project and its undo history, and the value "
            "straight to the audio thread, with no rebuild.")
        // GIL: trivial - a copy of a few dozen floats per strip
        .def(
            "levels",
            [](EngineHandle& self) {
                // One call per UI frame for every strip (P3-7, phase_5.md §4.9): an
                // N x 9 float32 array, one row per insert with a meter -
                // insert id, peak L/R, RMS L/R (linear), momentary, short-term and
                // integrated loudness (LUFS; -200 is silence), true peak (linear, the
                // master's only). GIL: trivial - a copy of a few dozen floats per strip.
                std::vector<adx::render::RenderEngine::StripLevel> strips;
                self.engine->levels(strips);
                py::array_t<float> out({static_cast<py::ssize_t>(strips.size()), py::ssize_t{9}});
                auto rows = out.mutable_unchecked<2>();
                for (std::size_t i = 0; i < strips.size(); ++i) {
                    const adx::rt::LevelFrame& f = strips[i].frame;
                    const auto r = static_cast<py::ssize_t>(i);
                    rows(r, 0) = static_cast<float>(strips[i].insert.value);
                    rows(r, 1) = f.peakLeft;
                    rows(r, 2) = f.peakRight;
                    rows(r, 3) = f.rmsLeft;
                    rows(r, 4) = f.rmsRight;
                    rows(r, 5) = f.momentary;
                    rows(r, 6) = f.shortTerm;
                    rows(r, 7) = f.integrated;
                    rows(r, 8) = f.truePeak;
                }
                return out;
            },
            "Every strip's latest meter reading, in one call: an N x 9 array of insert id, "
            "peak L/R, RMS L/R, momentary/short-term/integrated LUFS, and true peak.")
        // GIL: trivial - reclaims retired snapshots; bounded and short
        .def(
            "pump", [](EngineHandle& self) { self.engine->pump(); },
            "Housekeeping for a UI timer: reclaim retired snapshots, resend anything the queue "
            "refused.")
        .def_property_readonly(
            "transport", [](EngineHandle& self) { return TransportHandle{.engine = &self}; },
            py::keep_alive<0, 1>())
        .def_property_readonly("latency_samples",
                               [](EngineHandle& self) { return self.engine->latencySamples(); })
        // GIL: trivial - two atomics; the read asks the device clock once
        .def_property_readonly("exclusive",
                               [](const EngineHandle& self) { return self.exclusive != nullptr; })
        // GIL: trivial - sets two atomics
        .def(
            "arm_output_onset",
            [](EngineHandle& self, float threshold) {
                if (self.exclusive == nullptr) {
                    throw std::runtime_error("only an exclusive-mode engine has the onset probe");
                }
                self.exclusive->armOnset(threshold);
            },
            py::arg("threshold"),
            "Latency measurement: note the next output frame louder than `threshold`.")
        // GIL: trivial - reads one atomic
        .def(
            "output_onset_time",
            [](const EngineHandle& self) -> std::int64_t {
                return self.exclusive != nullptr ? self.exclusive->onsetPlayTime() : 0;
            },
            "When the armed onset plays by the device clock, in QPC 100 ns units; 0 until "
            "it is heard.")
        .def_property_readonly(
            "stream_block_frames",
            [](EngineHandle& self) { return self.engine->audio().info().blockFrames; })
        .def_property_readonly(
            "stream_output_latency_ms",
            [](EngineHandle& self) { return self.engine->audio().info().outputLatencyMs; })
        .def_property_readonly(
            "stream_xruns",
            [](EngineHandle& self) { return self.engine->audio().info().xrunCount; })
        .def_property_readonly("output_format",
                               [](const EngineHandle& self) {
                                   return self.exclusive != nullptr
                                              ? std::string(self.exclusive->formatName())
                                              : std::string{};
                               })
        .def_property_readonly("snapshots_built",
                               [](EngineHandle& self) { return self.engine->snapshotsBuilt(); })
        .def_property_readonly("sample_rate",
                               [](EngineHandle& self) { return self.engine->options().sampleRate; })
        .def_property_readonly(
            "worst_callback_ms",
            [](EngineHandle& self) {
                return static_cast<double>(self.engine->audio().callbacks().worstCallbackNs()) /
                       1e6;
            },
            "The slowest audio callback since the stream started, in milliseconds.");
    defineEngineFrame(engine);
    defineEngineOffline(engine);
    defineEngineMidi(engine);
    defineEnginePlugins(engine);
    defineEngineCapture(engine);
    adx::bindings::defineEngineExtras(engine);
    adx::bindings::defineEngineLive(engine);
    adx::bindings::defineEnginePads(engine);
}

} // namespace

void registerTransportBindings(py::module_& m) {
    defineTransport(m);
    defineEngine(m);
}
