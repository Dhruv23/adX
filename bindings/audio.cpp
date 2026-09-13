// Device enumeration and stream control, exposed to Python.
//
// Everything here is O(1)-ish and driven by user interaction, so returning real
// Python objects is legal - FINAL_PLAN §2.2 Rule 2 prohibits crossing this boundary
// O(notes) or O(frames), not O(devices). The hot surfaces that Rule 2 is actually
// about arrive in Phase 5 as zero-copy buffers.
//
// Rule 3 applies to every call that does real work: opening a stream talks to the
// driver and can block for tens of milliseconds, so the GIL is released around it.
// Python must never block the Qt event loop waiting on C++.

#include <memory>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "engine/audio/AudioThread.h"
#include "engine/audio/NullBackend.h"
#include "engine/audio/RtAudioBackend.h"
#include "engine/rt/AllocGuard.h"
#include "engine/rt/Violation.h"

namespace py = pybind11;

namespace {

/// The process-wide stream.
///
/// A function-local static rather than a namespace-scope object: it must not be
/// constructed during static initialisation, because constructing it installs the
/// realtime guards, and it must be destroyed deterministically at close_stream()
/// rather than at an unspecified point during interpreter teardown.
std::unique_ptr<adx::audio::AudioThread>& streamSlot() {
    static std::unique_ptr<adx::audio::AudioThread> stream;
    return stream;
}

py::dict toDict(const adx::audio::DeviceInfo& device) {
    py::dict out;
    out["id"] = device.id;
    out["name"] = std::string(device.name.view());
    out["api"] = std::string(device.apiName.view());
    out["max_output_channels"] = device.maxOutputChannels;
    out["max_input_channels"] = device.maxInputChannels;
    out["preferred_sample_rate"] = device.preferredSampleRate;
    out["is_default_output"] = device.isDefaultOutput;
    out["is_default_input"] = device.isDefaultInput;
    std::vector<std::uint32_t> rates;
    rates.reserve(device.sampleRates.size());
    for (const std::uint32_t rate : device.sampleRates.view()) {
        rates.push_back(rate);
    }
    out["sample_rates"] = rates;
    return out;
}

py::dict toDict(const adx::audio::StreamInfo& info) {
    py::dict out;
    out["is_open"] = info.isOpen;
    out["is_running"] = info.isRunning;
    out["sample_rate"] = info.sampleRate;
    out["block_frames"] = info.blockFrames;
    out["output_channels"] = info.outputChannels;
    out["input_channels"] = info.inputChannels;
    out["output_latency_ms"] = info.outputLatencyMs;
    out["input_latency_ms"] = info.inputLatencyMs;
    out["round_trip_latency_ms"] = info.roundTripLatencyMs;
    out["callback_count"] = info.callbackCount;
    out["xrun_count"] = info.xrunCount;
    out["backend"] = std::string(info.backendName.view());
    return out;
}

void throwOnError(const adx::audio::Error& error) {
    if (error.code == adx::audio::Error::Code::None) {
        return;
    }
    // Translated at the boundary, never thrown across it from RT code
    // (phase_0.md §4.2): below this layer the failure is a value.
    throw std::runtime_error(std::string(error.message.view()));
}

} // namespace

void registerAudioBindings(py::module_& m) {
    m.def(
        "enumerate_devices",
        []() {
            std::vector<py::dict> out;
            {
                // Probing the driver is real work and can take a while on a machine
                // with a lot of hardware.
                const py::gil_scoped_release released;
                adx::audio::RtAudioBackend backend;
                const auto devices = backend.enumerate();
                const py::gil_scoped_acquire acquired;
                out.reserve(devices.size());
                for (const auto& device : devices) {
                    out.push_back(toDict(device));
                }
            }
            return out;
        },
        "Every audio device the system exposes, as a list of dicts.");

    m.def(
        "open_stream",
        [](std::uint32_t sampleRate, std::uint32_t blockFrames, std::uint32_t outputChannels,
           bool useNullBackend) {
            auto& stream = streamSlot();
            if (stream) {
                throw std::runtime_error("a stream is already open");
            }

            adx::audio::StreamConfig config;
            config.sampleRate = sampleRate;
            config.blockFrames = blockFrames;
            config.outputChannels = outputChannels;

            std::unique_ptr<adx::audio::AudioBackend> backend;
            if (useNullBackend) {
                backend = std::make_unique<adx::audio::NullBackend>();
            } else {
                backend = std::make_unique<adx::audio::RtAudioBackend>();
            }
            stream = std::make_unique<adx::audio::AudioThread>(std::move(backend));

            const py::gil_scoped_release released;
            adx::audio::Error error = stream->open(config);
            if (error.code == adx::audio::Error::Code::None) {
                error = stream->start();
            }
            if (error.code != adx::audio::Error::Code::None) {
                stream.reset();
            }
            const py::gil_scoped_acquire acquired;
            throwOnError(error);
        },
        py::arg("sample_rate") = 48000, py::arg("block_frames") = 256,
        py::arg("output_channels") = 2, py::arg("null_backend") = false,
        "Open and start the audio stream. null_backend=True runs a clock with no "
        "device, which is what CI uses.");

    m.def(
        "close_stream",
        []() {
            auto& stream = streamSlot();
            if (!stream) {
                return;
            }
            const py::gil_scoped_release released;
            // Stopping joins the audio thread, which must not happen while holding the
            // GIL: the callback never touches Python (Rule 1), but the join itself can
            // take a buffer's worth of time and the UI has no business freezing for it.
            stream->stop();
            stream.reset();
        },
        "Stop and release the audio stream. Safe to call when nothing is open.");

    m.def(
        "stream_info",
        []() {
            auto& stream = streamSlot();
            if (!stream) {
                return toDict(adx::audio::StreamInfo{});
            }
            return toDict(stream->info());
        },
        "Sample rate, block size, latency readout and xrun count for the open stream.");

    m.def(
        "rt_violation_count", []() { return adx::rt::ViolationLog::instance().count(); },
        "Realtime violations recorded since the last reset. Zero, or something is "
        "wrong on the audio thread.");

    m.def(
        "reset_rt_violations", []() { adx::rt::ViolationLog::instance().reset(); },
        "Clear the realtime violation log. Main thread, between runs.");

    m.def(
        "rt_guard_enabled", []() { return adx::rt::allocGuardCompiledIn(); },
        "Whether the allocator hook is compiled into this build. False in Release, "
        "where a zero violation count proves nothing.");
}
