// Mini-notation, exposed to Python (phase_7.md §2): compile one cycle, realise a range,
// and the cycle length the engine uses - what the editor's step highlighting and the
// scripting API's add_mini need. The compiler itself runs here, so Python's idea of a
// pattern is the engine's.

#include <string>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "bindings/ProjectHandle.h"
#include "engine/format/adx/Diagnostics.h"
#include "engine/format/mininotation/PatternCompiler.h"

namespace py = pybind11;

namespace {

py::list problemsOf(const std::vector<adx::mini::Problem>& problems) {
    py::list out;
    for (const adx::mini::Problem& problem : problems) {
        py::dict entry;
        adx::format::Diagnostic probe;
        probe.code = problem.code;
        const adx::format::DiagnosticCode* row = adx::format::findDiagnosticCode(problem.code);
        entry["code"] = probe.codeString();
        entry["severity"] = std::string(
            adx::format::toString(row != nullptr ? row->severity : adx::format::Severity::Error));
        entry["start"] = problem.start;
        entry["length"] = problem.length;
        entry["message"] = problem.message;
        out.append(entry);
    }
    return out;
}

} // namespace

namespace adx::bindings {

void registerMini(py::module_& m) {
    py::module_ mini = m.def_submodule("mini", "Mini-notation (docs/mininotation.md).");

    mini.def(
        "compile",
        [](const std::string& text, std::int64_t cycleStart, std::int64_t cycleLength,
           std::int64_t cycleIndex, std::uint32_t seed) {
            adx::mini::CompileResult result;
            {
                const py::gil_scoped_release released;
                result = adx::mini::compile(text, core::Ticks{cycleStart}, core::Ticks{cycleLength},
                                            cycleIndex, seed);
            }
            py::list notes;
            for (std::size_t i = 0; i < result.notes.size(); ++i) {
                const auto& n = result.notes[i];
                notes.append(py::make_tuple(n.start.value, n.length.value, n.pitch, n.velocity,
                                            result.spans[i].start, result.spans[i].length));
            }
            py::dict out;
            out["ok"] = result.ok;
            out["notes"] = notes;
            out["problems"] = problemsOf(result.problems);
            return out;
        },
        py::arg("text"), py::arg("cycle_start") = 0, py::arg("cycle_length") = core::kPpq * 4,
        py::arg("cycle_index") = 0, py::arg("seed") = 0,
        "One cycle: {ok, notes: [(start, length, pitch, velocity, span_start, span_length)], "
        "problems: [{code, severity, start, length, message}]}.");

    mini.def(
        "realize",
        [](const std::string& text, std::int64_t from, std::int64_t to, std::int64_t cycleLength) {
            std::vector<adx::mini::RealizedNote> realized;
            {
                const py::gil_scoped_release released;
                realized = adx::mini::realize(text, core::Ticks{from}, core::Ticks{to},
                                              core::Ticks{cycleLength});
            }
            // One array: a pattern of sixteenths over a long song is thousands of notes,
            // and Rule 2 (FINAL_PLAN 2.2) forbids an object per note.
            py::array_t<std::int64_t> out(
                {static_cast<py::ssize_t>(realized.size()), py::ssize_t{8}});
            auto view = out.mutable_unchecked<2>();
            for (std::size_t i = 0; i < realized.size(); ++i) {
                const auto& r = realized[i];
                const auto row = static_cast<py::ssize_t>(i);
                view(row, 0) = r.note.start.value;
                view(row, 1) = r.note.length.value;
                view(row, 2) = r.note.pitch;
                view(row, 3) = r.note.velocity;
                view(row, 4) = r.span.start;
                view(row, 5) = r.span.length;
                view(row, 6) = r.cycle;
                view(row, 7) = r.index;
            }
            return out;
        },
        py::arg("text"), py::arg("from_tick"), py::arg("to_tick"), py::arg("cycle_length"),
        "Every note in [from, to) as rows of (start, length, pitch, velocity, span_start, "
        "span_length, cycle, index).");

    // GIL: trivial - a few string copies
    mini.def(
        "join_lines",
        [](const std::vector<std::string>& lines) { return adx::mini::joinLines(lines); },
        py::arg("lines"), "The text a MINI block's lines compile as.");

    // GIL: trivial - reads the meter at tick 0
    mini.def(
        "cycle_length",
        [](const ProjectHandle& handle) {
            return adx::mini::cycleLength(handle.project.tempo).value;
        },
        py::arg("project"), "One cycle, in ticks: a bar of the meter at tick 0.");

    // GIL: trivial - reads the project's MINI sources (a few short strings)
    mini.def(
        "sources",
        [](const ProjectHandle& handle) {
            std::vector<py::tuple> out;
            for (const project::Pattern& pattern : handle.project.patterns) {
                for (const project::MiniNotationSource& source : pattern.mini) {
                    const project::Channel* channel = handle.project.find(source.channel);
                    out.push_back(py::make_tuple(pattern.name,
                                                 channel != nullptr ? channel->name : std::string(),
                                                 source.lines));
                }
            }
            return out;
        },
        py::arg("project"), "Every MINI block as (pattern, channel, lines).");
}

} // namespace adx::bindings
