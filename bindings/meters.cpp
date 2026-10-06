// The UI frame: one FFI call per 60 Hz tick (phase_5.md §4.8, §4.9).
//
//     levels = np.zeros((64, 9), np.float32)          # allocated once, reused
//     ticks, rolling, strips, samples = engine.frame(levels)
//
// The playhead position, whether the transport is rolling and every strip's meter
// reading cross in a single call, written into a buffer Python owns and reuses - so the
// per-frame cost is one call and no allocation on either side once the scratch vector
// has grown (P4-1). The `perf_geometry_budgets` gate counts the calls: exactly one per
// simulated meter frame.

#include <algorithm>
#include <cstdint>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include "bindings/Bindings.h"
#include "bindings/EngineHandle.h"
#include "engine/transport/PlayState.h"

namespace py = pybind11;

using adx::bindings::EngineHandle;

namespace {

constexpr py::ssize_t kColumns = 9;

py::tuple frame(EngineHandle& self,
                py::array_t<float, py::array::c_style | py::array::forcecast>& out) {
    if (out.ndim() != 2 || out.shape(1) != kColumns) {
        throw py::value_error("frame() wants an (N, 9) float32 array");
    }
    // The housekeeping a UI timer owes the engine - reclaiming retired snapshots,
    // resending anything the queue refused - rides on the same call.
    self.engine->pump();
    self.engine->levels(self.levelScratch);
    auto rows = out.mutable_unchecked<2>();
    const auto count = static_cast<py::ssize_t>(self.levelScratch.size());
    const py::ssize_t written = std::min(count, rows.shape(0));
    for (py::ssize_t r = 0; r < written; ++r) {
        const auto& strip = self.levelScratch[static_cast<std::size_t>(r)];
        const adx::rt::LevelFrame& f = strip.frame;
        rows(r, 0) = static_cast<float>(strip.insert.value);
        rows(r, 1) = f.peakLeft;
        rows(r, 2) = f.peakRight;
        rows(r, 3) = f.rmsLeft;
        rows(r, 4) = f.rmsRight;
        rows(r, 5) = f.momentary;
        rows(r, 6) = f.shortTerm;
        rows(r, 7) = f.integrated;
        rows(r, 8) = f.truePeak;
    }
    const bool rolling = adx::transport::isRolling(self.engine->state());
    return py::make_tuple(self.engine->positionTicks().value, rolling, count,
                          self.engine->positionSamples());
}

} // namespace

void defineEngineFrame(py::class_<EngineHandle>& engine) {
    engine.def("frame", &frame, py::arg("levels"), // GIL: trivial - a few floats per strip
               "Everything one UI frame reads and does, in one call: pumps the engine, fills "
               "`levels` (N x 9, as levels() "
               "lays it out) and returns (position_ticks, rolling, strip_count, position_samples). "
               "A strip_count "
               "above N means the array was too small; grow it for the next frame.");
}
