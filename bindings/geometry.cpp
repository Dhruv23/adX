// Geometry across the boundary, zero-copy (phase_5.md §4.1, §4.4 - §4.6).
//
//     geometry = adx_engine.PianoRollGeometry()
//     geometry.build(project, viewport, "Verse", "Lead")      # GIL released
//     with geometry.notes.lease() as lease:
//         verts = np.asarray(lease)    # a view of the C++ storage: no copy
//         item.upload(lease.address, lease.vertex_count, lease.revision)
//
// The rule, enforced by GeometryLease and stated once: a numpy view never outlives its
// `with` block. Inside it, the buffer cannot be resized or patched (that throws); on
// leaving it, a revision that moved anyway raises, because something mutated the
// storage behind the view.
//
// Rule 2: one call builds every buffer, one call per hit test, and a rubber band
// returns one array. Rule 3: every call that walks notes or samples releases the GIL.

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "bindings/PendingCommand.h"
#include "bindings/ProjectHandle.h"
#include "engine/format/audio/SampleBuffer.h"
#include "engine/geometry/ClipPeakCache.h"
#include "engine/geometry/ColorPalette.h"
#include "engine/geometry/GeometryBuffer.h"
#include "engine/geometry/HitTest.h"
#include "engine/geometry/PianoRollGeometry.h"
#include "engine/geometry/WaveformGeometry.h"

namespace py = pybind11;

using adx::bindings::channelNamed;
using adx::bindings::patternNamed;
using adx::bindings::ProjectHandle;
using namespace adx::geometry;

namespace {

/// What an empty buffer's view points at: numpy wants a non-null pointer even for
/// zero elements.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
float emptyStorage = 0.0F;

py::buffer_info viewOf(const GeometryBuffer& buffer) {
    float* data = buffer.vertexCount() == 0 ? &emptyStorage : const_cast<float*>(buffer.data());
    const auto fpv = static_cast<py::ssize_t>(buffer.floatsPerVertex());
    return py::buffer_info(
        data, sizeof(float), py::format_descriptor<float>::format(), 2,
        {static_cast<py::ssize_t>(buffer.vertexCount()), fpv},
        {static_cast<py::ssize_t>(sizeof(float)) * fpv, static_cast<py::ssize_t>(sizeof(float))},
        /*readonly=*/true);
}

/// The Python `with` block around a view.
struct LeaseHandle {
    GeometryBuffer* buffer{nullptr};
    std::unique_ptr<GeometryLease> lease;
};

LeaseHandle& enterLease(LeaseHandle& self) {
    if (self.lease) {
        throw std::runtime_error("this lease is already active");
    }
    self.lease = std::make_unique<GeometryLease>(*self.buffer);
    return self;
}

void exitLease(LeaseHandle& self) {
    if (!self.lease) {
        return;
    }
    const bool unchanged = self.lease->release();
    self.lease.reset();
    if (!unchanged) {
        throw std::runtime_error("geometry changed during its lease: a view of it was stale");
    }
}

const GeometryBuffer& activeBuffer(const LeaseHandle& self) {
    if (!self.lease) {
        throw std::runtime_error(
            "a geometry view exists only inside its `with` block (phase_5.md 4.1)");
    }
    return *self.buffer;
}

std::vector<adx::core::NoteId>
idsOf(const py::array_t<std::uint32_t, py::array::c_style | py::array::forcecast>& ids) {
    const auto view = ids.unchecked<1>();
    std::vector<adx::core::NoteId> out;
    out.reserve(static_cast<std::size_t>(view.shape(0)));
    for (py::ssize_t i = 0; i < view.shape(0); ++i) {
        out.push_back(adx::core::NoteId{view(i)});
    }
    std::ranges::sort(out);
    return out;
}

using IdArray = py::array_t<std::uint32_t, py::array::c_style | py::array::forcecast>;

void defineBuffers(py::module_& m) {
    py::class_<GeometryBuffer>(m, "GeometryBuffer", py::buffer_protocol(),
                               "C++-owned vertices: (vertex_count, floats_per_vertex) float32.")
        .def_buffer([](GeometryBuffer& b) { return viewOf(b); })
        .def_property_readonly("revision", &GeometryBuffer::revision)
        .def_property_readonly("vertex_count", &GeometryBuffer::vertexCount)
        .def_property_readonly("floats_per_vertex", &GeometryBuffer::floatsPerVertex)
        .def(
            "lease", [](GeometryBuffer& b) { return LeaseHandle{.buffer = &b, .lease = nullptr}; },
            py::keep_alive<0, 1>(), // GIL: trivial
            "A context manager pinning this buffer; np.asarray() it inside the block.");

    py::class_<LeaseHandle>(m, "GeometryLease", py::buffer_protocol())
        .def_buffer([](LeaseHandle& self) { return viewOf(activeBuffer(self)); })
        .def("__enter__", &enterLease, py::return_value_policy::reference) // GIL: trivial
        .def("__exit__",
             [](LeaseHandle& self, const py::object&, const py::object&, const py::object&) {
                 exitLease(self);
                 return false;
             }) // GIL: trivial
        .def_property_readonly("active",
                               [](const LeaseHandle& self) { return self.lease != nullptr; })
        .def_property_readonly(
            "address",
            [](const LeaseHandle& self) {
                return reinterpret_cast<std::uintptr_t>(activeBuffer(self).data());
            },
            "The storage's address, for the scene-graph item's one-memcpy upload. Valid only "
            "inside the block.")
        .def_property_readonly(
            "vertex_count",
            [](const LeaseHandle& self) { return activeBuffer(self).vertexCount(); })
        .def_property_readonly(
            "revision", [](const LeaseHandle& self) { return activeBuffer(self).revision(); });
}

void defineViewport(py::module_& m) {
    py::class_<Viewport>(m, "Viewport", "The view rectangle in tick/pitch space (Viewport.h).")
        // GIL: trivial - Viewport::make is a few float operations
        .def(py::init([](double tickStart, float pitchTop, float pixelsPerTick,
                         float pixelsPerSemitone, float widthPx, float heightPx) {
                 return Viewport::make(tickStart, pitchTop, pixelsPerTick, pixelsPerSemitone,
                                       widthPx, heightPx);
             }),
             py::arg("tick_start"), py::arg("pitch_top"), py::arg("pixels_per_tick"),
             py::arg("pixels_per_semitone"), py::arg("width"), py::arg("height"))
        .def_readonly("tick_start", &Viewport::tickStart)
        .def_readonly("tick_end", &Viewport::tickEnd)
        .def_readonly("pitch_low", &Viewport::pitchLow)
        .def_readonly("pitch_high", &Viewport::pitchHigh)
        .def_readonly("pixels_per_tick", &Viewport::pixelsPerTick)
        .def_readonly("pixels_per_semitone", &Viewport::pixelsPerSemitone)
        .def_property_readonly("density", [](const Viewport& v) { return densityLod(v); });
}

void definePianoRoll(py::module_& m) {
    py::class_<PianoRollGeometry>(m, "PianoRollGeometry")
        .def(py::init<>())
        .def(
            "build",
            [](PianoRollGeometry& self, const ProjectHandle& handle, const Viewport& viewport,
               const std::string& pattern, const std::string& channel, const IdArray& selected,
               int lane, int scaleRoot, int scaleMask, std::int64_t gridDivision, bool ghosts) {
                BuildOptions options;
                options.pattern = patternNamed(handle.project, pattern).id;
                options.channel = channelNamed(handle.project, channel).id;
                const std::vector<adx::core::NoteId> ids = idsOf(selected);
                options.selected = ids;
                options.ghosts = ghosts;
                options.lane = static_cast<LaneKind>(std::clamp(lane, 0, 5));
                options.scale =
                    ScaleHighlight{.root = static_cast<std::uint8_t>(scaleRoot % 12),
                                   .mask = static_cast<std::uint16_t>(scaleMask & 0xFFF)};
                options.gridDivision = gridDivision;
                options.dataRevision = handle.stack.revision();
                const py::gil_scoped_release released;
                self.build(handle.project, viewport, options);
            },
            py::arg("project"), py::arg("viewport"), py::arg("pattern"), py::arg("channel"),
            py::arg("selected") = IdArray(0), py::arg("lane") = 0, py::arg("scale_root") = 0,
            py::arg("scale_mask") = 0, py::arg("grid_division") = adx::core::kPpq / 4,
            py::arg("ghosts") = true, "Fill every buffer for one channel's notes in one pattern.")
        .def(
            "update_selection",
            [](PianoRollGeometry& self, const IdArray& selected) {
                const std::vector<adx::core::NoteId> ids = idsOf(selected);
                const py::gil_scoped_release released;
                self.updateSelection(ids);
            },
            py::arg("selected"), "Recolour for a new selection, in place. Nothing is rebuilt.")
        .def_property_readonly("rows", &PianoRollGeometry::rows,
                               py::return_value_policy::reference_internal)
        .def_property_readonly("grid", &PianoRollGeometry::grid,
                               py::return_value_policy::reference_internal)
        .def_property_readonly("ghosts", &PianoRollGeometry::ghosts,
                               py::return_value_policy::reference_internal)
        .def_property_readonly("notes", &PianoRollGeometry::notes,
                               py::return_value_policy::reference_internal)
        .def_property_readonly("lanes", &PianoRollGeometry::lanes,
                               py::return_value_policy::reference_internal)
        .def_property_readonly("curves", &PianoRollGeometry::curves,
                               py::return_value_policy::reference_internal)
        .def_property_readonly("density", &PianoRollGeometry::density)
        .def_property_readonly(
            "visible_count",
            [](const PianoRollGeometry& self) { return self.visibleNotes().size(); })
        .def(
            "visible_ids",
            [](const PianoRollGeometry& self) {
                const auto ids = self.visibleNotes();
                py::array_t<std::uint32_t> out(static_cast<py::ssize_t>(ids.size()));
                std::uint32_t* w = out.mutable_data();
                for (std::size_t i = 0; i < ids.size(); ++i) {
                    w[i] = ids[i].value;
                }
                return out;
            }, // GIL: trivial - a copy of the visible ids, bounded by the screen
            "The notes the last build drew, in vertex order: one array.");
}

void defineHitTests(py::module_& m) {
    m.def(
        "hit_test_note",
        [](const ProjectHandle& handle, const std::string& pattern, const std::string& channel,
           const Viewport& viewport, float x, float y) {
            const auto p = patternNamed(handle.project, pattern).id;
            const auto c = channelNamed(handle.project, channel).id;
            NoteHit hit;
            {
                const py::gil_scoped_release released;
                hit = hitTestNote(handle.project, p, c, viewport, x, y);
            }
            return py::make_tuple(hit.note.value, static_cast<int>(hit.part));
        },
        py::arg("project"), py::arg("pattern"), py::arg("channel"), py::arg("viewport"),
        py::arg("x"), py::arg("y"),
        "(note id or 0, part): part is 0 none, 1 body, 2 left edge, 3 right edge.");

    m.def(
        "hit_test_rect",
        [](const ProjectHandle& handle, const std::string& pattern, const std::string& channel,
           const Viewport& viewport, float x0, float y0, float x1, float y1) {
            const auto p = patternNamed(handle.project, pattern).id;
            const auto c = channelNamed(handle.project, channel).id;
            const adx::project::NoteClip* clip = handle.project.find(p)->clipFor(c);
            std::vector<adx::core::NoteId> ids(clip == nullptr ? 0 : clip->notes.size());
            std::size_t count = 0;
            {
                const py::gil_scoped_release released;
                count = hitTestRect(handle.project, p, c, viewport,
                                    Rect{.x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1}, ids);
            }
            py::array_t<std::uint32_t> out(static_cast<py::ssize_t>(count));
            std::uint32_t* w = out.mutable_data();
            for (std::size_t i = 0; i < count; ++i) {
                w[i] = ids[i].value;
            }
            return out;
        },
        py::arg("project"), py::arg("pattern"), py::arg("channel"), py::arg("viewport"),
        py::arg("x0"), py::arg("y0"), py::arg("x1"), py::arg("y1"),
        "Every note a pixel rectangle touches, as one uint32 array.");

    m.def(
        "hit_test_lane",
        [](const ProjectHandle& handle, const std::string& pattern, const std::string& channel,
           const Viewport& viewport, float laneHeight, float x, float y) {
            const auto p = patternNamed(handle.project, pattern).id;
            const auto c = channelNamed(handle.project, channel).id;
            LaneHit hit;
            {
                const py::gil_scoped_release released;
                hit = hitTestLane(handle.project, p, c, viewport, laneHeight, x, y);
            }
            return py::make_tuple(hit.note.value, hit.value);
        },
        py::arg("project"), py::arg("pattern"), py::arg("channel"), py::arg("viewport"),
        py::arg("lane_height"), py::arg("x"), py::arg("y"), "(note id or 0, value 0..1).");
}

void defineWaveforms(py::module_& m) {
    py::class_<ClipPeaks, std::shared_ptr<ClipPeaks>>(m, "ClipPeaks")
        .def_property_readonly("complete", &ClipPeaks::complete)
        .def_property_readonly("failed", &ClipPeaks::failed)
        .def_property_readonly("error", &ClipPeaks::error)
        .def_property_readonly("frames", &ClipPeaks::frames)
        .def_property_readonly("sample_rate", &ClipPeaks::sampleRate)
        .def_property_readonly("progress", &ClipPeaks::progress)
        .def("tier_ready", &ClipPeaks::tierReady, py::arg("tier")); // GIL: trivial

    py::class_<ClipPeakCache>(m, "ClipPeakCache", "Peaks computed on a worker; never blocks.")
        .def(py::init<>())
        .def(
            "request_file",
            [](ClipPeakCache& self, const std::string& path, float gain) {
                return std::const_pointer_cast<ClipPeaks>(
                    self.request(PeakKey{.path = path, .gain = gain}));
            }, // GIL: trivial - queues the decode for the worker and returns
            py::arg("path"), py::arg("gain") = 1.0F)
        .def(
            "request_array",
            [](ClipPeakCache& self, const std::string& name,
               const py::array_t<float, py::array::c_style | py::array::forcecast>& samples,
               std::uint32_t sampleRate, float gain) {
                auto buffer = std::make_shared<adx::format::SampleBuffer>();
                const auto frames = static_cast<std::uint64_t>(samples.shape(0));
                const bool stereo = samples.ndim() == 2 && samples.shape(1) >= 2;
                const float* data = samples.data();
                const auto stride =
                    static_cast<std::uint64_t>(samples.ndim() == 2 ? samples.shape(1) : 1);
                {
                    const py::gil_scoped_release released;
                    buffer->sampleRate = sampleRate;
                    buffer->sourceChannels = stereo ? 2 : 1;
                    buffer->frames = frames;
                    const std::size_t padded = frames + (2 * adx::format::kSampleGuardFrames);
                    buffer->left.assign(padded, 0.0F);
                    if (stereo) {
                        buffer->right.assign(padded, 0.0F);
                    }
                    for (std::uint64_t f = 0; f < frames; ++f) {
                        buffer->left[adx::format::kSampleGuardFrames + f] = data[f * stride];
                        if (stereo) {
                            buffer->right[adx::format::kSampleGuardFrames + f] =
                                data[(f * stride) + 1];
                        }
                    }
                }
                return std::const_pointer_cast<ClipPeaks>(
                    self.request(PeakKey{.path = name, .gain = gain}, std::move(buffer)));
            },
            py::arg("name"), py::arg("samples"), py::arg("sample_rate"), py::arg("gain") = 1.0F,
            "Peaks over samples already in memory: (frames,) or (frames, 2) float32.")
        .def("wait_idle",
             [](ClipPeakCache& self) {
                 const py::gil_scoped_release released;
                 self.waitIdle();
             })
        .def_property_readonly("size", &ClipPeakCache::size);

    py::class_<WaveformGeometry>(m, "WaveformGeometry")
        .def(py::init<>())
        .def(
            "build",
            [](WaveformGeometry& self, const ClipPeaks& peaks, double frameStart, double frameEnd,
               std::uint32_t columns) {
                const py::gil_scoped_release released;
                self.build(
                    peaks,
                    WaveView{.frameStart = frameStart, .frameEnd = frameEnd, .columns = columns});
            },
            py::arg("peaks"), py::arg("frame_start"), py::arg("frame_end"), py::arg("columns"),
            "Two vertices per column: x in seconds, y in amplitude. O(columns).")
        .def_property_readonly(
            "strip",
            static_cast<const GeometryBuffer& (WaveformGeometry::*)() const noexcept>(
                &WaveformGeometry::strip),
            py::return_value_policy::reference_internal)
        .def_property_readonly("tier", &WaveformGeometry::tier);

    m.def("ideal_tier", &idealTier, py::arg("samples_per_column")); // GIL: trivial
}

} // namespace

void registerGeometryBindings(py::module_& m) {
    std::vector<std::string> roles(kColorRoleNames.begin(), kColorRoleNames.end());
    m.attr("COLOR_ROLES") = roles;
    m.attr("FLOATS_PER_VERTEX") = kFloatsPerVertex;
    m.attr("DENSITY_PIXELS_PER_SIXTEENTH") = kDensityPixelsPerSixteenth;
    m.attr("LANE_BAR_PIXELS") = kLaneBarPixels;
    defineBuffers(m);
    defineViewport(m);
    definePianoRoll(m);
    defineHitTests(m);
    defineWaveforms(m);
}
