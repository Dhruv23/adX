// Phase 6's geometry and engine calls (phase_6.md §2): the playlist, step grid,
// automation, meter and EQ-curve builders, and the engine's audition, metronome, EQ
// spectrum and one-call meter frame.
//
// Every builder follows Phase 5's contract (bindings/geometry.cpp): one call builds every
// buffer with the GIL released, a buffer is read through a lease, and anything per item or
// per point crosses as one numpy array.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "bindings/EngineHandle.h"
#include "bindings/ProjectHandle.h"
#include "engine/effects/GrossBeat.h"
#include "engine/effects/ParametricEq.h"
#include "engine/geometry/AutomationGeometry.h"
#include "engine/geometry/ClipPeakCache.h"
#include "engine/geometry/EqCurveGeometry.h"
#include "engine/geometry/MeterGeometry.h"
#include "engine/geometry/PlaylistGeometry.h"
#include "engine/geometry/StepGridGeometry.h"
#include "engine/instruments/fm/FmInstrument.h"
#include "engine/instruments/wavetable/WavetableInstrument.h"
#include "engine/transport/PlayState.h"

namespace py = pybind11;
namespace core = adx::core;
using namespace adx::geometry;
using adx::bindings::EngineHandle;
using adx::bindings::ProjectHandle;

namespace {

using IdArray = py::array_t<std::uint32_t, py::array::c_style | py::array::forcecast>;
using DoubleArray = py::array_t<double, py::array::c_style | py::array::forcecast>;
using FloatArray = py::array_t<float, py::array::c_style | py::array::forcecast>;
using ByteArray = py::array_t<std::uint8_t, py::array::c_style | py::array::forcecast>;

template<class Id> std::vector<Id> idsOf(const IdArray& ids, bool sort) {
    const auto view = ids.unchecked<1>();
    std::vector<Id> out;
    out.reserve(static_cast<std::size_t>(view.shape(0)));
    for (py::ssize_t i = 0; i < view.shape(0); ++i) {
        out.push_back(Id{view(i)});
    }
    if (sort) {
        std::ranges::sort(out, {}, [](Id id) { return id.value; });
    }
    return out;
}

/// Breakpoints from an (n, 2..8) array (tick, value, curve kind, tension, c1x...), as
/// automation_points() returns them.
std::vector<LanePoint> lanePointsOf(const DoubleArray& points) {
    std::vector<LanePoint> out;
    if (points.ndim() != 2 || points.shape(1) < 2) {
        return out;
    }
    const auto rows = points.unchecked<2>();
    const py::ssize_t columns = points.shape(1);
    out.reserve(static_cast<std::size_t>(rows.shape(0)));
    for (py::ssize_t i = 0; i < rows.shape(0); ++i) {
        LanePoint point{.tick = static_cast<std::int64_t>(rows(i, 0)),
                        .value = static_cast<float>(rows(i, 1)),
                        .curve = {}};
        if (columns > 2) {
            point.curve.kind = static_cast<core::CurveKind>(std::clamp(
                static_cast<int>(rows(i, 2)), 0, static_cast<int>(core::kCurveKindCount) - 1));
        }
        if (columns > 3) {
            point.curve.tension = static_cast<float>(rows(i, 3));
        }
        if (columns > 7) {
            point.curve.c1x = static_cast<float>(rows(i, 4));
            point.curve.c1y = static_cast<float>(rows(i, 5));
            point.curve.c2x = static_cast<float>(rows(i, 6));
            point.curve.c2y = static_cast<float>(rows(i, 7));
        }
        out.push_back(point);
    }
    return out;
}

PlaylistViewport playlistView(double tickStart, double tickEnd, float rowStart, float rowEnd,
                              float pixelsPerTick, float pixelsPerRow) {
    return PlaylistViewport{.tickStart = tickStart,
                            .tickEnd = tickEnd,
                            .rowStart = rowStart,
                            .rowEnd = rowEnd,
                            .pixelsPerTick = std::max(pixelsPerTick, 1e-9F),
                            .pixelsPerRow = std::max(pixelsPerRow, 1.0F)};
}

void definePlaylist(py::module_& m) {
    py::class_<PlaylistGeometry>(m, "PlaylistGeometry")
        .def(py::init<>())
        .def(
            "build",
            [](PlaylistGeometry& self, const ProjectHandle& handle, double tickStart,
               double tickEnd, float rowStart, float rowEnd, float pixelsPerTick,
               float pixelsPerRow, const IdArray& selected, const py::object& peaks) {
                const std::vector<core::ItemId> ids = idsOf<core::ItemId>(selected, true);
                ClipPeakCache* cache = peaks.is_none() ? nullptr : peaks.cast<ClipPeakCache*>();
                const py::gil_scoped_release released;
                self.build(
                    handle.project,
                    playlistView(tickStart, tickEnd, rowStart, rowEnd, pixelsPerTick, pixelsPerRow),
                    PlaylistBuildOptions{.selected = ids, .peaks = cache});
            },
            py::arg("project"), py::arg("tick_start"), py::arg("tick_end"), py::arg("row_start"),
            py::arg("row_end"), py::arg("pixels_per_tick"), py::arg("pixels_per_row"),
            py::arg("selected") = IdArray(0), py::arg("peaks") = py::none(),
            "Fill every layer for the visible range: O(visible clips), GIL released.")
        .def_property_readonly("lanes", &PlaylistGeometry::lanes,
                               py::return_value_policy::reference_internal)
        .def_property_readonly("grid", &PlaylistGeometry::grid,
                               py::return_value_policy::reference_internal)
        .def_property_readonly("clips", &PlaylistGeometry::clips,
                               py::return_value_policy::reference_internal)
        .def_property_readonly("previews", &PlaylistGeometry::previews,
                               py::return_value_policy::reference_internal)
        .def_property_readonly("curves", &PlaylistGeometry::curves,
                               py::return_value_policy::reference_internal)
        .def_property_readonly("pending_peaks", &PlaylistGeometry::pendingPeaks)
        .def_property_readonly("detailed", &PlaylistGeometry::detailed)
        .def(
            "rows",
            [](const PlaylistGeometry& self) {
                const auto rows = self.rows();
                py::array_t<double> out({static_cast<py::ssize_t>(rows.size()), py::ssize_t{3}});
                auto w = out.mutable_unchecked<2>();
                for (std::size_t i = 0; i < rows.size(); ++i) {
                    const auto r = static_cast<py::ssize_t>(i);
                    w(r, 0) = rows[i].track.value;
                    w(r, 1) = rows[i].top;
                    w(r, 2) = rows[i].height;
                }
                return out;
            }, // GIL: trivial - one row per track
            "(n, 3) float64: track id, top row, height in rows - the last build's layout.")
        .def(
            "visible",
            [](const PlaylistGeometry& self) {
                const auto clips = self.visible();
                py::array_t<double> out({static_cast<py::ssize_t>(clips.size()), py::ssize_t{8}});
                auto w = out.mutable_unchecked<2>();
                for (std::size_t i = 0; i < clips.size(); ++i) {
                    const VisibleClip& c = clips[i];
                    const auto r = static_cast<py::ssize_t>(i);
                    w(r, 0) = c.item.value;
                    w(r, 1) = c.track.value;
                    w(r, 2) = static_cast<double>(c.start);
                    w(r, 3) = static_cast<double>(c.end);
                    w(r, 4) = c.top;
                    w(r, 5) = c.height;
                    w(r, 6) = static_cast<double>(c.kind);
                    w(r, 7) = c.ref;
                }
                return out;
            }, // GIL: trivial - bounded by the clips on screen
            "(n, 8) float64 per drawn clip: item, track, start, end, top, height, kind (0 "
            "pattern, 1 audio, 2 automation), ref. For labels.")
        .def(
            "hit_test",
            [](const PlaylistGeometry& self, double tickStart, float rowStart, float pixelsPerTick,
               float pixelsPerRow, float x, float y) {
                const ClipHit hit =
                    self.hitTest(playlistView(tickStart, tickStart, rowStart, rowStart,
                                              pixelsPerTick, pixelsPerRow),
                                 x, y);
                return py::make_tuple(hit.track.value, hit.item.value, static_cast<int>(hit.part));
            }, // GIL: trivial - bounded by the clips on screen
            py::arg("tick_start"), py::arg("row_start"), py::arg("pixels_per_tick"),
            py::arg("pixels_per_row"), py::arg("x"), py::arg("y"),
            "(track, item or 0, part): part 0 none, 1 body, 2 left edge, 3 right edge, 4 header.");
}

void defineStepGrid(py::module_& m) {
    py::class_<StepGridGeometry>(m, "StepGridGeometry")
        .def(py::init<>())
        .def(
            "build",
            [](StepGridGeometry& self, const ProjectHandle& handle, std::uint32_t pattern,
               const IdArray& channels, std::int64_t stepTicks, std::uint32_t steps,
               std::uint32_t groupSize) {
                const std::vector<core::ChannelId> rows = idsOf<core::ChannelId>(channels, false);
                const py::gil_scoped_release released;
                self.build(handle.project, StepGridSpec{.pattern = core::PatternId{pattern},
                                                        .channels = rows,
                                                        .stepTicks = stepTicks,
                                                        .steps = steps,
                                                        .groupSize = groupSize});
            },
            py::arg("project"), py::arg("pattern"), py::arg("channels"), py::arg("step_ticks"),
            py::arg("steps"), py::arg("group_size") = 4,
            "Every cell of a channels x steps grid in one C++ build: no per-cell widget.")
        .def_property_readonly("cells", &StepGridGeometry::cells,
                               py::return_value_policy::reference_internal)
        .def_property_readonly("velocity", &StepGridGeometry::velocity,
                               py::return_value_policy::reference_internal)
        .def_property_readonly("lit", &StepGridGeometry::lit);

    m.def(
        "steps_of",
        [](const ProjectHandle& handle, std::uint32_t pattern, std::uint32_t channel,
           std::int64_t stepTicks, std::uint32_t steps) {
            std::vector<StepCell> cells;
            {
                const py::gil_scoped_release released;
                stepsOf(handle.project, core::PatternId{pattern}, core::ChannelId{channel},
                        stepTicks, steps, cells);
            }
            py::array_t<std::uint32_t> out(
                {static_cast<py::ssize_t>(cells.size()), py::ssize_t{2}});
            auto w = out.mutable_unchecked<2>();
            for (std::size_t i = 0; i < cells.size(); ++i) {
                w(static_cast<py::ssize_t>(i), 0) = cells[i].note.value;
                w(static_cast<py::ssize_t>(i), 1) = cells[i].velocity;
            }
            return out;
        },
        py::arg("project"), py::arg("pattern"), py::arg("channel"), py::arg("step_ticks"),
        py::arg("steps"),
        "One channel's steps as an (n, 2) uint32 array: the note on each step (0: none) and "
        "its velocity. One array per rack click.");
}

void defineAutomation(py::module_& m) {
    py::class_<AutomationGeometry>(m, "AutomationGeometry")
        .def(py::init<>())
        .def(
            "build",
            [](AutomationGeometry& self, const DoubleArray& points, float minimum, float maximum,
               bool logarithmic, double tickStart, double tickEnd, float pixelsPerTick,
               float heightPx, std::uint32_t segments, const ByteArray& selected, double tickOffset,
               float yTop, float yScale) {
                const std::vector<LanePoint> lane = lanePointsOf(points);
                const std::vector<std::uint8_t> flags(selected.data(),
                                                      selected.data() + selected.size());
                const py::gil_scoped_release released;
                self.build(
                    lane,
                    LaneRange{.minimum = minimum, .maximum = maximum, .logarithmic = logarithmic},
                    LaneView{.tickStart = tickStart,
                             .tickEnd = tickEnd,
                             .pixelsPerTick = pixelsPerTick,
                             .heightPx = heightPx,
                             .segments = segments,
                             .tickOffset = tickOffset,
                             .yTop = yTop,
                             .yScale = yScale},
                    flags);
            },
            py::arg("points"), py::arg("minimum"), py::arg("maximum"), py::arg("logarithmic"),
            py::arg("tick_start"), py::arg("tick_end"), py::arg("pixels_per_tick"),
            py::arg("height_px"), py::arg("segments") = 0, py::arg("selected") = ByteArray(0),
            py::arg("tick_offset") = 0.0, py::arg("y_top") = 0.0F, py::arg("y_scale") = 1.0F,
            "Tessellate a lane with core::Curve::evaluate - the engine's evaluator. The offset "
            "and y band place an item-relative envelope on its clip.")
        .def_property_readonly("line", &AutomationGeometry::line,
                               py::return_value_policy::reference_internal)
        .def_property_readonly("points", &AutomationGeometry::points,
                               py::return_value_policy::reference_internal);

    m.def(
        "hit_test_lane_point",
        [](const DoubleArray& points, float minimum, float maximum, bool logarithmic,
           double tickStart, float pixelsPerTick, float heightPx, float x, float y) {
            const std::vector<LanePoint> lane = lanePointsOf(points);
            return hitTestLanePoint(
                lane, LaneRange{.minimum = minimum, .maximum = maximum, .logarithmic = logarithmic},
                LaneView{.tickStart = tickStart,
                         .tickEnd = tickStart,
                         .pixelsPerTick = pixelsPerTick,
                         .heightPx = heightPx},
                x, y);
        }, // GIL: trivial - one lane's points
        py::arg("points"), py::arg("minimum"), py::arg("maximum"), py::arg("logarithmic"),
        py::arg("tick_start"), py::arg("pixels_per_tick"), py::arg("height_px"), py::arg("x"),
        py::arg("y"), "The index of the point under (x, y), or -1.");
    m.def(
        "lane_normalise",
        [](float value, float minimum, float maximum, bool logarithmic) {
            return LaneRange{.minimum = minimum, .maximum = maximum, .logarithmic = logarithmic}
                .normalise(value);
        }, // GIL: trivial - one value
        py::arg("value"), py::arg("minimum"), py::arg("maximum"), py::arg("logarithmic"));
    m.def(
        "lane_positions",
        [](const DoubleArray& values, float minimum, float maximum, bool logarithmic) {
            const LaneRange range{
                .minimum = minimum, .maximum = maximum, .logarithmic = logarithmic};
            py::array_t<double> out(values.size());
            const double* in = values.data();
            double* w = out.mutable_data();
            for (py::ssize_t i = 0; i < values.size(); ++i) {
                w[i] = range.normalise(static_cast<float>(in[i]));
            }
            return out;
        }, // GIL: trivial - one lane's values, one array
        py::arg("values"), py::arg("minimum"), py::arg("maximum"), py::arg("logarithmic"),
        "Every value's position in its range, 0..1, as one array.");
    m.def(
        "lane_denormalise",
        [](float position, float minimum, float maximum, bool logarithmic) {
            return LaneRange{.minimum = minimum, .maximum = maximum, .logarithmic = logarithmic}
                .denormalise(position);
        }, // GIL: trivial - one value
        py::arg("position"), py::arg("minimum"), py::arg("maximum"), py::arg("logarithmic"));
}

std::vector<adx::rt::LevelFrame> framesOf(const FloatArray& levels, std::size_t count) {
    std::vector<adx::rt::LevelFrame> frames;
    if (levels.ndim() != 2 || levels.shape(1) < 9) {
        return frames;
    }
    const auto rows = levels.unchecked<2>();
    count = std::min(count, static_cast<std::size_t>(rows.shape(0)));
    frames.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto r = static_cast<py::ssize_t>(i);
        frames.push_back(adx::rt::LevelFrame{.peakLeft = rows(r, 1),
                                             .peakRight = rows(r, 2),
                                             .rmsLeft = rows(r, 3),
                                             .rmsRight = rows(r, 4),
                                             .momentary = rows(r, 5),
                                             .shortTerm = rows(r, 6),
                                             .integrated = rows(r, 7),
                                             .truePeak = rows(r, 8)});
    }
    return frames;
}

void defineMeters(py::module_& m) {
    py::class_<MeterGeometry>(m, "MeterGeometry")
        .def(py::init<>())
        .def(
            "build",
            [](MeterGeometry& self, const FloatArray& levels, std::size_t count) {
                const std::vector<adx::rt::LevelFrame> frames = framesOf(levels, count);
                self.build(frames);
            }, // GIL: trivial - a few floats per strip
            py::arg("levels"), py::arg("count"),
            "Build from an (N, 9) levels array; Engine.frame() does this itself.")
        .def("reset_clips", &MeterGeometry::resetClips)            // GIL: trivial
        .def("clipped", &MeterGeometry::clipped, py::arg("index")) // GIL: trivial
        .def_property_readonly("bars", &MeterGeometry::bars,
                               py::return_value_policy::reference_internal);
    m.def("meter_position", &meterPosition, py::arg("amplitude")); // GIL: trivial
}

void defineEqCurve(py::module_& m) {
    py::class_<EqCurveGeometry>(m, "EqCurveGeometry")
        .def(py::init<>())
        .def(
            "build",
            [](EqCurveGeometry& self, const std::string& type, const FloatArray& params,
               double sampleRate, float rangeDb, std::uint32_t points) {
                const std::vector<EqBand> bands = eqBandsOf(
                    type,
                    std::span<const float>{params.data(), static_cast<std::size_t>(params.size())});
                const py::gil_scoped_release released;
                self.build(bands, sampleRate, rangeDb, points);
            },
            py::arg("type"), py::arg("params"), py::arg("sample_rate") = 48000.0,
            py::arg("range_db") = 24.0F, py::arg("points") = 256,
            "The response of `type` ('ParametricEq' or 'EQ') with `params` in descriptor "
            "order.")
        .def_property_readonly("curve", &EqCurveGeometry::curve,
                               py::return_value_policy::reference_internal)
        .def_property_readonly("handles", &EqCurveGeometry::handles,
                               py::return_value_policy::reference_internal)
        .def_property_readonly("spectrum", &EqCurveGeometry::spectrum,
                               py::return_value_policy::reference_internal)
        .def(
            "response",
            [](const EqCurveGeometry& self) {
                const auto response = self.responseDb();
                return py::array_t<double>(static_cast<py::ssize_t>(response.size()),
                                           response.data());
            }, // GIL: trivial - one value per curve point
            "The summed response in dB at each point of the last build.");
    m.def("eq_axis_position", &eqAxisPosition, py::arg("frequency")); // GIL: trivial
    m.def(
        "fm_algorithm",
        [](int number) {
            const adx::instruments::FmAlgorithm& algorithm = adx::instruments::fmAlgorithm(number);
            std::vector<int> modulators(algorithm.modulators.begin(), algorithm.modulators.end());
            return py::make_tuple(modulators, algorithm.carriers, algorithm.feedbackFrom,
                                  algorithm.feedbackTo);
        }, // GIL: trivial - one table row
        py::arg("number"),
        "(modulator bitmask per operator, carrier bitmask, feedback from, feedback to): the "
        "routing the FM instrument runs for algorithm 1..32.");
    m.def(
        "wavetable_frames",
        [](int bank, int samplesPerFrame) {
            const int clamped = std::clamp(bank, 0, 3);
            const adx::dsp::WaveTable& table =
                adx::instruments::builtinWavetable(
                    static_cast<adx::instruments::WavetableBank>(clamped))
                    .linear;
            const auto frames = static_cast<py::ssize_t>(table.frames());
            const auto width = static_cast<py::ssize_t>(std::max(samplesPerFrame, 8));
            py::array_t<float> out({frames, width});
            auto w = out.mutable_unchecked<2>();
            for (py::ssize_t f = 0; f < frames; ++f) {
                for (py::ssize_t i = 0; i < width; ++i) {
                    w(f, i) = table.read(static_cast<std::size_t>(f), 0,
                                         static_cast<float>(i) / static_cast<float>(width));
                }
            }
            return out;
        }, // GIL: trivial - a few thousand reads of a built table, once per editor open
        py::arg("bank"), py::arg("samples_per_frame") = 128,
        "A built-in wavetable's frames, (frames, samples) float32, for the 3D table view.");
    m.def(
        "grossbeat_curve",
        [](int pattern, int points) {
            py::array_t<double> out(static_cast<py::ssize_t>(std::max(points, 2)));
            auto w = out.mutable_unchecked<1>();
            for (py::ssize_t i = 0; i < w.shape(0); ++i) {
                const double p = static_cast<double>(i) / static_cast<double>(w.shape(0) - 1);
                w(i) = adx::effects::grossBeatMap(
                    static_cast<adx::effects::GrossBeatPattern>(std::clamp(
                        pattern, 0, static_cast<int>(adx::effects::kGrossBeatPatterns) - 1)),
                    p);
            }
            return out;
        }, // GIL: trivial - a few hundred evaluations of the engine's own map
        py::arg("pattern"), py::arg("points") = 128,
        "GrossBeat's time map f(p) for a pattern, at evenly spaced p in 0..1.");
    m.def("eq_axis_frequency", &eqAxisFrequency, py::arg("x")); // GIL: trivial
}

/// The latest ParametricEq tap of `slot` into `geometry`'s spectrum. False when the slot
/// is not a ParametricEq the engine is running.
bool eqSpectrum(EngineHandle& self, std::uint32_t slot, EqCurveGeometry& geometry) {
    const std::shared_ptr<const adx::graph::Node> node =
        self.engine->builder().nodes().findSlot(core::SlotId{slot});
    const auto* eq = dynamic_cast<const adx::effects::ParametricEq*>(node.get());
    if (eq == nullptr) {
        return false;
    }
    std::array<adx::effects::SpectrumFrame, 64> frames{};
    const std::size_t read = eq->spectrumTap().readLatest(frames.data(), frames.size());
    std::vector<float> samples;
    samples.reserve(read * 64);
    // readLatest gives newest first; the FFT wants time order.
    for (std::size_t i = read; i-- > 0;) {
        samples.insert(samples.end(), frames.at(i).begin(), frames.at(i).end());
    }
    geometry.buildSpectrum(samples, self.engine->options().sampleRate);
    return true;
}

} // namespace

namespace adx::bindings {

void registerPanelGeometry(py::module_& m) {
    m.attr("CLIP_DETAIL_PIXELS") = kClipDetailPixels;
    m.attr("METER_FLOOR_DB") = kMeterFloorDb;
    definePlaylist(m);
    defineStepGrid(m);
    defineAutomation(m);
    defineMeters(m);
    defineEqCurve(m);
}

void defineEngineExtras(py::class_<EngineHandle>& engine) {
    engine
        .def(
            "audition",
            [](EngineHandle& self, std::uint32_t channel, int pitch, int velocity, double seconds) {
                self.engine->audition(
                    core::ChannelId{channel}, static_cast<std::uint8_t>(std::clamp(pitch, 0, 127)),
                    static_cast<std::uint8_t>(std::clamp(velocity, 1, 127)), seconds);
            }, // GIL: trivial - posts one message to the audio thread
            py::arg("channel"), py::arg("pitch"), py::arg("velocity") = 100,
            py::arg("seconds") = 0.3,
            "Play one note through a channel's instrument now, outside the arrangement (P5-1).")
        .def(
            "stop_audition", [](EngineHandle& self) { self.engine->stopAudition(); },
            "Release every audition note.") // GIL: trivial - posts one message
        .def(
            "set_metronome",
            [](EngineHandle& self, bool enabled, std::int64_t beatTicks,
               std::uint32_t beatsPerBar) {
                self.engine->setMetronome(enabled, core::Ticks{beatTicks}, beatsPerBar);
            }, // GIL: trivial - posts one message to the audio thread
            py::arg("enabled"), py::arg("beat_ticks") = core::kPpq, py::arg("beats_per_bar") = 4,
            "The click, realtime only (P5-3).")
        .def(
            "gain_reduction",
            [](EngineHandle& self, std::uint32_t slot) {
                const std::shared_ptr<const adx::graph::Node> node =
                    self.engine->builder().nodes().findSlot(core::SlotId{slot});
                const auto* effect = dynamic_cast<const adx::effects::Effect*>(node.get());
                return effect == nullptr ? 0.0F : effect->gainReductionDb();
            }, // GIL: trivial - one atomic load
            py::arg("slot"),
            "A running dynamics slot's gain reduction over its last block, dB (<= 0).")
        .def(
            "eq_spectrum",
            [](EngineHandle& self, std::uint32_t slot, EqCurveGeometry& geometry) {
                return eqSpectrum(self, slot, geometry);
            }, // GIL: trivial - 4096 samples through one FFT, at the editor's frame rate
            py::arg("slot"), py::arg("geometry"),
            "Fill `geometry`'s spectrum from a running ParametricEq's tap. False if none.");
}

/// Engine.frame with the mixer's meters: the same one call, which also builds every
/// strip's meter vertices and returns where they are.
py::tuple frameWithMeters(EngineHandle& self,
                          py::array_t<float, py::array::c_style | py::array::forcecast>& levels,
                          MeterGeometry& meters) {
    const py::tuple base = frameInto(self, levels);
    self.meterScratch.clear();
    for (const auto& strip : self.levelScratch) {
        self.meterScratch.push_back(strip.frame);
    }
    meters.build(self.meterScratch);
    const GeometryBuffer& bars = meters.bars();
    return py::make_tuple(base[0], base[1], base[2], base[3],
                          reinterpret_cast<std::uintptr_t>(bars.data()), bars.vertexCount(),
                          bars.revision());
}

} // namespace adx::bindings
