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
#include <stdexcept>
#include <string>

#include <pybind11/pybind11.h>

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
        .def("play", [](TransportHandle& t) { engineOf(t).play(); })
        .def("stop", [](TransportHandle& t) { engineOf(t).stopPlayback(); })
        .def(
            "seek_ticks",
            [](TransportHandle& t, std::int64_t ticks) {
                engineOf(t).seek(adx::core::Ticks{ticks});
            },
            py::arg("ticks"))
        .def(
            "set_loop",
            [](TransportHandle& t, std::int64_t start, std::int64_t end, bool enabled) {
                engineOf(t).setLoop(adx::transport::LoopRegion{.start = adx::core::Ticks{start},
                                                               .end = adx::core::Ticks{end},
                                                               .enabled = enabled});
            },
            py::arg("start"), py::arg("end"), py::arg("enabled") = true)
        .def(
            "position_ticks", [](TransportHandle& t) { return engineOf(t).positionTicks().value; },
            "Where the arrangement is, as of the audio thread's last block. O(1); poll it.")
        .def("position_samples", [](TransportHandle& t) { return engineOf(t).positionSamples(); })
        .def("state", [](TransportHandle& t) {
            return std::string(adx::transport::toString(engineOf(t).state()));
        });
}

void defineEngine(py::module_& m) {
    py::class_<EngineHandle>(m, "Engine",
                             "The render engine: a stream, a transport, and the project it plays.")
        .def(py::init([](std::uint32_t sampleRate, std::uint32_t blockFrames,
                         std::uint32_t outputChannels, bool nullBackend) {
                 auto handle = std::make_unique<EngineHandle>();
                 std::unique_ptr<adx::audio::AudioBackend> backend;
                 if (nullBackend) {
                     backend = std::make_unique<adx::audio::NullBackend>();
                 } else {
                     backend = std::make_unique<adx::audio::RtAudioBackend>();
                 }
                 handle->engine = std::make_unique<adx::render::RenderEngine>(
                     std::move(backend),
                     adx::render::EngineOptions{.sampleRate = sampleRate,
                                                .blockFrames = blockFrames,
                                                .outputChannels = outputChannels});
                 return handle;
             }),
             py::arg("sample_rate") = 48000, py::arg("block_frames") = 256,
             py::arg("output_channels") = 2, py::arg("null_backend") = false)
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
        .def(
            "pump", [](EngineHandle& self) { self.engine->pump(); },
            "Housekeeping for a UI timer: reclaim retired snapshots, resend anything the queue "
            "refused.")
        .def_property_readonly(
            "transport", [](EngineHandle& self) { return TransportHandle{.engine = &self}; },
            py::keep_alive<0, 1>())
        .def_property_readonly("latency_samples",
                               [](EngineHandle& self) { return self.engine->latencySamples(); })
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
}

} // namespace

void registerTransportBindings(py::module_& m) {
    defineTransport(m);
    defineEngine(m);
}
