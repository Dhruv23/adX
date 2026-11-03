// What an agent hears a running engine with (phase_10.5.md §3 step 1, §4.5): the xrun
// count, a capture of the master bus to a WAV, and the callback stall that plants an
// overload for the tests.
#include <pybind11/numpy.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "bindings/Bindings.h"
#include "bindings/EngineHandle.h"
#include "engine/audio/CaptureTap.h"
#include "engine/dsp/Loudness.h"

namespace py = pybind11;

using adx::bindings::EngineHandle;

void defineEngineCapture(py::class_<EngineHandle>& engine) {
    // GIL: trivial - reads an atomic
    engine.def(
        "xruns", [](EngineHandle& self) { return self.engine->audio().info().xrunCount; },
        "Underruns since the stream opened, as the backend counts them: read before and after "
        "a scenario, any increase is a glitch the listener would hear.");
    // GIL: trivial - reads an atomic
    engine.def(
        "late_callbacks",
        [](EngineHandle& self) { return self.engine->audio().callbacks().overDeadlineCount(); },
        "Callbacks that took longer than their block's real-time duration.");
    // GIL: trivial - copies at most 1024 entries of a ring
    engine.def(
        "callback_timings",
        [](EngineHandle& self, std::uint64_t since) {
            const auto& ring = self.engine->audio().callbacks().timingTap();
            const std::uint64_t written = ring.framesWritten();
            const auto wanted = static_cast<std::size_t>(written > since ? written - since : 0);
            std::vector<adx::audio::CallbackTiming> got(std::min(wanted, ring.capacity()));
            std::uint64_t lost = 0;
            const std::uint64_t first = written - got.size();
            const std::size_t read =
                got.empty() ? 0 : ring.readFrom(first, got.data(), got.size(), lost);
            py::array_t<std::uint32_t> durations(static_cast<py::ssize_t>(read));
            auto view = durations.mutable_unchecked<1>();
            for (std::size_t i = 0; i < read; ++i) {
                view(static_cast<py::ssize_t>(i)) = got[i].durationNs;
            }
            // Skipped (more than the ring holds since `since`) and overtaken both count.
            return py::make_tuple(durations, first + lost + read,
                                  (first - std::min(since, first)) + lost);
        },
        py::arg("since"),
        "Callback durations in ns from position `since` on: (durations, next position, lost). "
        "The ring holds 1024; poll faster than that many callbacks, or `lost` counts the gap.");
    // GIL: trivial - starts a thread
    engine.def(
        "capture_start",
        [](EngineHandle& self) {
            if (!self.capture) {
                self.capture =
                    std::make_unique<adx::audio::CaptureTap>(self.engine->audio().scopeTap());
            }
            self.capture->start();
        },
        "Start capturing the master bus at the next frame the audio thread writes.");
    // GIL: trivial - reads an atomic
    engine.def(
        "capture_frames",
        [](EngineHandle& self) { return self.capture ? self.capture->frames() : 0; },
        "Frames captured so far.");
    engine.def(
        "capture_stop",
        [](EngineHandle& self, const std::optional<std::string>& path) {
            if (!self.capture || !self.capture->running()) {
                throw py::value_error("no capture is running");
            }
            adx::audio::Capture capture;
            std::string error;
            bool written = true;
            {
                const py::gil_scoped_release released;
                capture = self.capture->stop();
                if (path) {
                    written = adx::audio::writeCaptureWav(
                        capture, self.engine->options().sampleRate, *path, error);
                }
            }
            if (!written) {
                throw py::value_error("cannot write the capture: " + error);
            }
            py::dict d;
            d["frames"] = capture.frames;
            d["lost"] = capture.lost;
            d["start_frame"] = capture.startFrame;
            d["sample_rate"] = self.engine->options().sampleRate;
            d["path"] = path ? py::cast(*path) : py::none();
            if (!path) {
                d["samples"] = py::array_t<float>(static_cast<py::ssize_t>(capture.samples.size()),
                                                  capture.samples.data());
            }
            return d;
        },
        py::arg("path") = py::none(),
        "Stop capturing. Writes a 32-bit float WAV to `path` when given, else returns the samples "
        "(interleaved). Always frames, lost (0 for a whole capture), start_frame, sample_rate.");
    // GIL: trivial - stores an atomic
    engine.def(
        "set_callback_stall_us",
        [](EngineHandle& self, std::uint32_t microseconds) {
            self.engine->audio().callbacks().setStallMicroseconds(microseconds);
        },
        py::arg("microseconds"),
        "Fault injection for tests: every callback spins this long after its work (0 is off).");
}

void registerCaptureBindings(py::module_& m) {
    m.def(
        "measure_loudness",
        [](const py::array_t<float, py::array::c_style | py::array::forcecast>& audio,
           std::uint32_t rate) {
            if (audio.ndim() != 2 || audio.shape(1) != 2) {
                throw py::value_error("measure_loudness wants an (N, 2) float32 array");
            }
            const auto frames = static_cast<std::size_t>(audio.shape(0));
            std::vector<float> left(frames);
            std::vector<float> right(frames);
            const auto view = audio.unchecked<2>();
            for (std::size_t i = 0; i < frames; ++i) {
                left[i] = view(static_cast<py::ssize_t>(i), 0);
                right[i] = view(static_cast<py::ssize_t>(i), 1);
            }
            float integrated = -200.0F;
            float momentary = -200.0F;
            float shortTerm = -200.0F;
            {
                const py::gil_scoped_release released;
                adx::dsp::LoudnessMeter meter;
                meter.prepare(rate);
                constexpr std::size_t kBlock = 480;
                for (std::size_t at = 0; at < frames; at += kBlock) {
                    const std::size_t n = std::min(kBlock, frames - at);
                    meter.process(std::span<const float>(left).subspan(at, n),
                                  std::span<const float>(right).subspan(at, n));
                    momentary = std::max(momentary, meter.momentary());
                    shortTerm = std::max(shortTerm, meter.shortTerm());
                }
                integrated = meter.integrated();
            }
            py::dict d;
            d["integrated"] = integrated;
            d["max_momentary"] = momentary;
            d["max_short_term"] = shortTerm;
            return d;
        },
        py::arg("audio"), py::arg("rate"),
        "BS.1770-4 loudness of stereo audio, by the engine's own meter (Phase 4, EBU TECH 3341): "
        "integrated, max_momentary, max_short_term in LUFS.");
}
