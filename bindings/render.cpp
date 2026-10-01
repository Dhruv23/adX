// Offline render, exposed to Python.
//
// One call renders a whole project through the same engine a live stream uses and
// returns a dict of statistics - never the samples as a list, which would be
// O(frames) Python objects (FINAL_PLAN §2.2 Rule 2). The file on disk is the output.
// The render itself runs with the GIL released (Rule 3).

#include <stdexcept>
#include <string>

#include <pybind11/pybind11.h>

#include "bindings/Bindings.h"
#include "bindings/ProjectHandle.h"
#include "engine/render/OfflineRender.h"

namespace py = pybind11;

namespace {

py::dict toDict(const adx::render::RenderStats& stats) {
    py::dict out;
    out["frames"] = stats.frames;
    out["sample_rate"] = stats.sampleRate;
    out["block_frames"] = stats.blockFrames;
    out["channels"] = stats.channels;
    out["callbacks"] = stats.callbacks;
    out["rt_violations"] = stats.violations;
    out["peak"] = stats.peak;
    out["rms"] = stats.rms;
    out["latency_samples"] = stats.latencySamples;
    out["hash"] = stats.hash.hex();
    out["arena_high_water"] = stats.arenaHighWater;
    out["wall_seconds"] = stats.wallSeconds;
    return out;
}

} // namespace

void registerRenderBindings(py::module_& m) {
    m.def(
        "render_offline",
        [](adx::bindings::ProjectHandle& project, const std::string& path, std::int64_t start,
           std::int64_t end, std::uint32_t blockFrames, std::uint32_t sampleRate,
           double tailSeconds) {
            adx::render::OfflineRenderOptions options;
            options.start = adx::core::Ticks{start};
            options.end = adx::core::Ticks{end};
            options.blockFrames = blockFrames;
            options.sampleRate = sampleRate;
            options.tailSeconds = tailSeconds;
            adx::render::RenderStats stats;
            bool written = true;
            {
                const py::gil_scoped_release released;
                const std::vector<float> samples =
                    adx::render::renderOffline(project.project, options, stats);
                if (stats.error.empty() && !path.empty()) {
                    written = adx::render::writeWavFloat32(path, samples, options.channels,
                                                           options.sampleRate);
                }
            }
            if (!stats.error.empty()) {
                throw std::runtime_error(stats.error);
            }
            if (!written) {
                throw std::runtime_error("could not write " + path);
            }
            return toDict(stats);
        },
        py::arg("project"), py::arg("path") = std::string{}, py::arg("start") = 0,
        py::arg("end") = 0, py::arg("block_frames") = 256, py::arg("sample_rate") = 48000,
        py::arg("tail_seconds") = 0.0,
        "Render `project` offline through the realtime engine's own code path and, if "
        "`path` is given, write a 32-bit float WAV. `end` of 0 means the end of the "
        "arrangement. Returns render statistics, including the golden-corpus hash.");
}
