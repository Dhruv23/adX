// Notes across the boundary (phase_5.md §3, §4.7). See bindings/Notes.h.
//
// Rule 3: reading a clip, resolving lyric aliases and building edits are O(notes), so
// each releases the GIL around its loop.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Notes.h"
#include "bindings/PendingCommand.h"
#include "engine/instruments/voice/VoiceSetup.h"
#include "engine/project/TypeCatalog.h"
#include "engine/project/commands/EditNotes.h"
#include "engine/project/commands/NoteCommands.h"

namespace py = pybind11;

using adx::bindings::channelNamed;
using adx::bindings::NoteRecord;
using adx::bindings::patternNamed;
using adx::bindings::PendingCommand;
using adx::bindings::ProjectHandle;

namespace {

/// The structured dtype of a NoteRecord row, built from offsetof rather than with
/// PYBIND11_NUMPY_DTYPE - whose macro machinery does not survive MSVC's conforming
/// preprocessor (/Zc:preprocessor).
py::dtype makeNoteDtype() {
    py::list names;
    py::list formats;
    py::list offsets;
    const auto field = [&](const char* name, const char* format, std::size_t offset) {
        names.append(name);
        formats.append(format);
        offsets.append(offset);
    };
    field("id", "<u4", offsetof(NoteRecord, id));
    field("start", "<i8", offsetof(NoteRecord, start));
    field("length", "<i8", offsetof(NoteRecord, length));
    field("pitch", "u1", offsetof(NoteRecord, pitch));
    field("velocity", "u1", offsetof(NoteRecord, velocity));
    field("fine", "<i2", offsetof(NoteRecord, fine));
    field("release", "<u2", offsetof(NoteRecord, release));
    field("pan", "<f4", offsetof(NoteRecord, pan));
    field("cutoff", "<f4", offsetof(NoteRecord, cutoff));
    field("resonance", "<f4", offsetof(NoteRecord, resonance));
    field("muted", "u1", offsetof(NoteRecord, muted));
    return {names, formats, offsets, static_cast<py::ssize_t>(sizeof(NoteRecord))};
}

const py::dtype& noteDtype() {
    // Created once and deliberately never destroyed. A function-local static
    // py::dtype would be destroyed at DLL unload - after the interpreter has finalised
    // - and its Py_DECREF would touch freed memory: a crash at every process exit.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    static const py::dtype* const kDtype = new py::dtype(makeNoteDtype());
    return *kDtype;
}

/// `object` as contiguous NoteRecord rows, or a ValueError naming the dtype it needs.
py::array noteArray(const py::object& object) {
    py::array array = py::array::ensure(object, py::array::c_style);
    if (!array || array.ndim() != 1 || !array.dtype().equal(noteDtype())) {
        throw py::value_error("expected a 1-D array of adx_engine.NOTE_DTYPE");
    }
    return array;
}

using NoteArray = py::object;

[[nodiscard]] NoteRecord toRecord(const adx::project::Note& note) noexcept {
    return NoteRecord{.id = note.id.value,
                      .start = note.start.value,
                      .length = note.length.value,
                      .pitch = note.pitch,
                      .velocity = note.velocity,
                      .fine = note.fineTuneCents,
                      .release = note.releaseVelocity,
                      .pan = note.pan,
                      .cutoff = note.cutoff,
                      .resonance = note.resonance,
                      .muted = static_cast<std::uint8_t>(note.muted ? 1 : 0)};
}

[[nodiscard]] adx::project::Note fromRecord(const NoteRecord& record) noexcept {
    adx::project::Note note;
    note.id = adx::core::NoteId{record.id};
    note.start = adx::core::Ticks{record.start};
    note.length = adx::core::Ticks{record.length};
    note.pitch = std::min<std::uint8_t>(record.pitch, 127);
    note.velocity = std::min<std::uint8_t>(record.velocity, 127);
    note.fineTuneCents = record.fine;
    note.releaseVelocity = record.release;
    note.pan = record.pan;
    note.cutoff = record.cutoff;
    note.resonance = record.resonance;
    note.muted = record.muted != 0;
    return note;
}

[[nodiscard]] std::vector<adx::project::Note> notesOf(const py::object& object) {
    const py::array array = noteArray(object);
    const auto* rows = static_cast<const NoteRecord*>(array.data());
    const auto count = static_cast<std::size_t>(array.shape(0));
    std::vector<adx::project::Note> notes;
    const py::gil_scoped_release released;
    notes.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        notes.push_back(fromRecord(rows[i]));
    }
    return notes;
}

[[nodiscard]] std::string colorText(const adx::project::Color& color) {
    std::array<char, 8> text{};
    static_cast<void>(std::snprintf(text.data(), text.size(), "#%02x%02x%02x", color.red,
                                    color.green, color.blue));
    return {text.data()};
}

[[nodiscard]] const adx::project::NoteClip*
clipNamed(const ProjectHandle& handle, const std::string& pattern, const std::string& channel) {
    return patternNamed(handle.project, pattern).clipFor(channelNamed(handle.project, channel).id);
}

py::tuple lyricCells(const ProjectHandle& handle, const std::string& pattern,
                     const std::string& channel, std::int64_t tickStart, std::int64_t tickEnd) {
    const adx::project::NoteClip* clip = clipNamed(handle, pattern, channel);
    std::vector<std::uint32_t> ids;
    std::vector<std::int64_t> starts;
    std::vector<std::int64_t> lengths;
    std::vector<std::string> lyrics;
    std::vector<std::string> aliases;
    if (clip != nullptr) {
        const py::gil_scoped_release released;
        const std::vector<std::string> resolved = adx::instruments::resolvedAliases(
            handle.project, channelNamed(handle.project, channel), *clip);
        for (std::size_t i = 0; i < clip->notes.size(); ++i) {
            const adx::project::Note& note = clip->notes[i];
            if (note.end().value < tickStart || note.start.value > tickEnd) {
                continue;
            }
            const adx::project::NoteExtras* extras = clip->extrasFor(note.id);
            ids.push_back(note.id.value);
            starts.push_back(note.start.value);
            lengths.push_back(note.length.value);
            lyrics.push_back(extras == nullptr ? std::string{} : extras->lyric);
            aliases.push_back(resolved[i]);
        }
    }
    return py::make_tuple(
        py::array_t<std::uint32_t>(static_cast<py::ssize_t>(ids.size()), ids.data()),
        py::array_t<std::int64_t>(static_cast<py::ssize_t>(starts.size()), starts.data()),
        py::array_t<std::int64_t>(static_cast<py::ssize_t>(lengths.size()), lengths.data()), lyrics,
        aliases);
}

py::dict noteExtras(const ProjectHandle& handle, const std::string& pattern,
                    const std::string& channel, std::uint32_t noteId) {
    py::dict out;
    out["slide"] = py::none();
    out["bend"] = py::list();
    out["lyric"] = std::string{};
    const adx::project::NoteClip* clip = clipNamed(handle, pattern, channel);
    const adx::project::NoteExtras* extras =
        clip == nullptr ? nullptr : clip->extrasFor(adx::core::NoteId{noteId});
    if (extras == nullptr) {
        return out;
    }
    if (extras->slide) {
        const adx::project::NoteSlide& slide = *extras->slide;
        out["slide"] = py::make_tuple(slide.targetCents, slide.start.value, slide.length.value,
                                      static_cast<int>(slide.curve.kind), slide.curve.tension);
    }
    py::list bend;
    for (const adx::project::PitchPoint& point : extras->pitchCurve) {
        bend.append(
            py::make_tuple(point.at.value, point.cents, static_cast<int>(point.curve.kind)));
    }
    out["bend"] = bend;
    out["lyric"] = extras->lyric;
    return out;
}

} // namespace

namespace adx::bindings {

py::dtype noteDtypeObject() {
    return noteDtype();
}

void defineNoteAccess(py::class_<ProjectHandle>& project) {
    project
        .def(
            "notes",
            [](const ProjectHandle& handle, const std::string& pattern,
               const std::string& channel) {
                const project::NoteClip* clip = clipNamed(handle, pattern, channel);
                const std::size_t count = clip == nullptr ? 0 : clip->notes.size();
                py::array out(noteDtype(),
                              std::vector<py::ssize_t>{static_cast<py::ssize_t>(count)},
                              std::vector<py::ssize_t>{});
                if (count > 0) {
                    auto* rows = static_cast<NoteRecord*>(out.mutable_data());
                    const py::gil_scoped_release released;
                    for (std::size_t i = 0; i < count; ++i) {
                        rows[i] = toRecord(clip->notes[i]);
                    }
                }
                return out;
            },
            py::arg("pattern"), py::arg("channel"),
            "Every note one channel plays in one pattern, as ONE structured numpy array (a "
            "copy). Never a list of notes.")
        // GIL: trivial - one note's extras: a lookup and a short copy
        .def("note_extras", &noteExtras, py::arg("pattern"), py::arg("channel"), py::arg("note"),
             "One note's slide, pitch curve and lyric. One note: an interaction.")
        .def("lyric_cells", &lyricCells, py::arg("pattern"), py::arg("channel"),
             py::arg("tick_start"), py::arg("tick_end"),
             "The lyric lane's cells for the notes in a tick range: ids, starts, lengths, lyrics "
             "and the alias each will sing. O(visible), for one redraw of the lane.")
        .def(
            "pattern_length",
            [](const ProjectHandle& handle, const std::string& pattern) {
                return patternNamed(handle.project, pattern).length.value;
            }, // GIL: trivial
            py::arg("pattern"))
        .def(
            "pattern_channels",
            [](const ProjectHandle& handle, const std::string& pattern) {
                std::vector<std::string> names;
                for (const project::NoteClip& clip :
                     patternNamed(handle.project, pattern).noteClips) {
                    if (const project::Channel* channel = handle.project.find(clip.channel)) {
                        names.push_back(channel->name);
                    }
                }
                return names;
            }, // GIL: trivial - one name per channel
            py::arg("pattern"), "Channels with notes in a pattern, in clip order.")
        .def(
            "channel_info",
            [](const ProjectHandle& handle, const std::string& name) {
                const project::Channel& channel = channelNamed(handle.project, name);
                py::dict out;
                out["instrument"] = channel.instrument.type;
                out["voice"] = channel.instrument.type == "Voice";
                out["color"] = colorText(channel.color);
                return out;
            }, // GIL: trivial
            py::arg("channel"))
        .def(
            "meter_at",
            [](const ProjectHandle& handle, std::int64_t ticks) {
                const core::MeterEvent meter = handle.project.tempo.meterAt(core::Ticks{ticks});
                return py::make_tuple(meter.numerator, meter.denominator);
            }, // GIL: trivial
            py::arg("ticks") = 0)
        .def(
            "bpm_at",
            [](const ProjectHandle& handle, std::int64_t ticks) {
                return handle.project.tempo.bpmAt(core::Ticks{ticks});
            }, // GIL: trivial
            py::arg("ticks") = 0)
        .def(
            "pattern_placements",
            [](const ProjectHandle& handle, const std::string& name) {
                const project::Pattern& pattern = patternNamed(handle.project, name);
                std::vector<std::int64_t> rows;
                for (const project::PlaylistTrack& track : handle.project.playlist.tracks) {
                    for (const project::PlaylistItem& item : track.items) {
                        const auto* ref = std::get_if<project::PatternRef>(&item.content);
                        if (ref == nullptr || ref->pattern != pattern.id) {
                            continue;
                        }
                        const std::int64_t length =
                            item.length.value > 0 ? item.length.value : pattern.length.value;
                        rows.insert(rows.end(), {item.start.value, item.start.value + length,
                                                 item.sourceOffset.value});
                    }
                }
                py::array_t<std::int64_t> out(
                    {static_cast<py::ssize_t>(rows.size() / 3), py::ssize_t{3}});
                std::ranges::copy(rows, out.mutable_data());
                return out;
            }, // GIL: trivial - one row per placement
            py::arg("pattern"),
            "Where a pattern is placed: (n, 3) int64 rows of start, end, source offset. Read "
            "once per edit; the playhead maps through it every frame without a call.")
        .def(
            "seconds_at",
            [](const ProjectHandle& handle, std::int64_t ticks) {
                return handle.project.tempo.secondsAt(core::Ticks{ticks});
            }, // GIL: trivial
            py::arg("ticks"));
}

// NOLINTBEGIN(bugprone-exception-escape) - a builder throws std::invalid_argument for an
// unknown name, which pybind11 turns into the ValueError the caller sees.
void registerNoteCommands(py::module_& commands) {
    // GIL: trivial - builds a closure; the note arrays convert with the GIL released
    commands.def(
        "EditNotes",
        [](const std::string& pattern, const std::string& channel,
           const py::array_t<std::uint32_t, py::array::c_style | py::array::forcecast>& remove,
           const NoteArray& update, const NoteArray& add, const std::string& label) {
            std::vector<core::NoteId> removeIds;
            const auto ids = remove.unchecked<1>();
            removeIds.reserve(static_cast<std::size_t>(ids.shape(0)));
            for (py::ssize_t i = 0; i < ids.shape(0); ++i) {
                removeIds.push_back(core::NoteId{ids(i)});
            }
            const std::vector<project::Note> updates = notesOf(update);
            const std::vector<project::Note> additions = notesOf(add);
            return PendingCommand{
                .build =
                    [=](const project::Project& project) {
                        return std::unique_ptr<project::Command>(
                            std::make_unique<project::EditNotes>(patternNamed(project, pattern).id,
                                                                 channelNamed(project, channel).id,
                                                                 removeIds, updates, additions,
                                                                 label));
                    },
                .description = "EditNotes " + label,
            };
        },
        py::arg("pattern"), py::arg("channel"), py::arg("remove"), py::arg("update"),
        py::arg("add"), py::arg("label") = "Edit notes",
        "One gesture's note edit: ids to remove, NoteRecord rows to replace (matched by id) "
        "and rows to add. One command, one undo step.");

    // GIL: trivial - builds a closure over a few values
    commands.def(
        "SetNoteSlide",
        [](const std::string& pattern, const std::string& channel, std::uint32_t note,
           std::optional<int> targetCents, std::int64_t start, std::int64_t length, int curve) {
            std::optional<project::NoteSlide> slide;
            if (targetCents) {
                slide = project::NoteSlide{
                    .targetCents = static_cast<std::int16_t>(std::clamp(*targetCents, -9600, 9600)),
                    .start = core::Ticks{std::max<std::int64_t>(0, start)},
                    .length = core::Ticks{std::max<std::int64_t>(1, length)},
                    .curve =
                        core::Curve{.kind = static_cast<core::CurveKind>(std::clamp(
                                        curve, 0, static_cast<int>(core::kCurveKindCount) - 1))}};
            }
            return PendingCommand{
                .build =
                    [=](const project::Project& project) {
                        return std::unique_ptr<project::Command>(
                            std::make_unique<project::SetNoteSlide>(
                                patternNamed(project, pattern).id,
                                channelNamed(project, channel).id, core::NoteId{note}, slide));
                    },
                .description = "SetNoteSlide",
            };
        },
        py::arg("pattern"), py::arg("channel"), py::arg("note"), py::arg("target_cents"),
        py::arg("start") = 0, py::arg("length") = core::kPpq / 4, py::arg("curve") = 0,
        "Set a note's slide (target_cents=None clears it).");

    // GIL: trivial - builds a closure over one note's handful of points
    commands.def(
        "SetPitchCurve",
        [](const std::string& pattern, const std::string& channel, std::uint32_t note,
           const py::array_t<std::int64_t, py::array::c_style | py::array::forcecast>& points) {
            std::vector<project::PitchPoint> curve;
            if (points.ndim() == 2 && points.shape(1) >= 2) {
                const auto view = points.unchecked<2>();
                for (py::ssize_t i = 0; i < view.shape(0); ++i) {
                    const int kind = view.shape(1) > 2 ? static_cast<int>(view(i, 2)) : 0;
                    curve.push_back(project::PitchPoint{
                        .at = core::Ticks{std::max<std::int64_t>(0, view(i, 0))},
                        .cents = static_cast<std::int16_t>(
                            std::clamp<std::int64_t>(view(i, 1), -9600, 9600)),
                        .curve = core::Curve{
                            .kind = static_cast<core::CurveKind>(std::clamp(
                                kind, 0, static_cast<int>(core::kCurveKindCount) - 1))}});
                }
                std::ranges::sort(curve, {},
                                  [](const project::PitchPoint& p) { return p.at.value; });
            }
            return PendingCommand{
                .build =
                    [=](const project::Project& project) {
                        return std::unique_ptr<project::Command>(
                            std::make_unique<project::SetPitchCurve>(
                                patternNamed(project, pattern).id,
                                channelNamed(project, channel).id, core::NoteId{note}, curve));
                    },
                .description = "SetPitchCurve",
            };
        },
        py::arg("pattern"), py::arg("channel"), py::arg("note"), py::arg("points"),
        "Replace a note's pitch curve with an (n, 2|3) array of (at_ticks, cents[, curve]).");

    // GIL: trivial - builds a closure over one string
    commands.def(
        "SetLyric",
        [](const std::string& pattern, const std::string& channel, std::uint32_t note,
           const std::string& lyric) {
            return PendingCommand{
                .build =
                    [=](const project::Project& project) {
                        return std::unique_ptr<project::Command>(
                            std::make_unique<project::SetLyric>(patternNamed(project, pattern).id,
                                                                channelNamed(project, channel).id,
                                                                core::NoteId{note}, lyric));
                    },
                .description = "SetLyric",
            };
        },
        py::arg("pattern"), py::arg("channel"), py::arg("note"), py::arg("lyric"));
}
// NOLINTEND(bugprone-exception-escape)

} // namespace adx::bindings
