// The scripting surface's engine half (engine/script/ScriptApi.h), exposed to Python.
// app/adx/scripting/api.py builds the user-facing `adx` module on this and on the
// command builders every panel uses - there is no third path into the model.

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "bindings/PendingCommand.h"
#include "bindings/ProjectHandle.h"
#include "engine/script/ScriptApi.h"

namespace py = pybind11;

namespace adx::bindings {

// NOLINTBEGIN(bugprone-exception-escape) - a bad name or position is a ValueError by design.
void registerScript(py::module_& m) {
    py::module_ script =
        m.def_submodule("script", "What the scripting API needs beyond the command builders.");

    // GIL: trivial - parses one short string
    script.def(
        "position",
        [](const ProjectHandle& handle, const std::string& text) {
            return script::positionOf(handle.project, text).value;
        },
        py::arg("project"), py::arg("text"),
        "Ticks of a position in the file's spelling: bar:beat:tick (from 0:0:0).");

    // GIL: trivial - parses one short string
    script.def(
        "duration",
        [](const ProjectHandle& handle, const std::string& text) {
            return script::durationOf(handle.project, text).value;
        },
        py::arg("project"), py::arg("text"), "Ticks of a duration: 2:0:0 is two bars.");

    script.def(
        "mini_notes",
        [](const std::string& text, std::int64_t length, std::int64_t cycle) {
            std::vector<project::Note> compiled;
            {
                const py::gil_scoped_release released;
                compiled = script::miniNotes(text, core::Ticks{length}, core::Ticks{cycle});
            }
            // One array, never an object per note (FINAL_PLAN 2.2 Rule 2).
            py::array_t<std::int64_t> out(
                {static_cast<py::ssize_t>(compiled.size()), py::ssize_t{4}});
            auto view = out.mutable_unchecked<2>();
            for (std::size_t i = 0; i < compiled.size(); ++i) {
                const auto row = static_cast<py::ssize_t>(i);
                view(row, 0) = compiled[i].start.value;
                view(row, 1) = compiled[i].length.value;
                view(row, 2) = compiled[i].pitch;
                view(row, 3) = compiled[i].velocity;
            }
            return out;
        },
        py::arg("text"), py::arg("length"), py::arg("cycle"),
        "Rows of (start, length, pitch, velocity): every note `text` plays over `length`.");

    script.def(
        "add_mini_notes",
        [](ProjectHandle& handle, const std::string& pattern, const std::string& channel,
           const std::string& text) {
            const core::PatternId p = patternNamed(handle.project, pattern).id;
            const core::ChannelId c = channelNamed(handle.project, channel).id;
            const py::gil_scoped_release released;
            return script::addMiniNotes(handle.project, handle.stack, p, c, text);
        },
        py::arg("project"), py::arg("pattern"), py::arg("channel"), py::arg("text"),
        "Compile `text` into notes in (pattern, channel): one command. Returns the count.");

    script.def(
        "set_mini",
        [](ProjectHandle& handle, const std::string& pattern, const std::string& channel,
           const std::string& text) {
            const core::PatternId p = patternNamed(handle.project, pattern).id;
            const core::ChannelId c = channelNamed(handle.project, channel).id;
            const py::gil_scoped_release released;
            script::setMini(handle.project, handle.stack, p, c, text);
        },
        py::arg("project"), py::arg("pattern"), py::arg("channel"), py::arg("text"),
        "Set a live MINI block (empty text removes it): one command.");

    script.def(
        "add_effect",
        [](ProjectHandle& handle, std::uint32_t insert, std::string type,
           const std::map<std::string, double>& params, float mix) {
            const std::vector<std::pair<std::string, double>> ordered(params.begin(), params.end());
            const py::gil_scoped_release released;
            return script::addEffect(handle.project, handle.stack, core::InsertId{insert},
                                     std::move(type), ordered, mix)
                .value;
        },
        py::arg("project"), py::arg("insert"), py::arg("type"), py::arg("params"),
        py::arg("mix") = 1.0F,
        "Add an effect with its parameters: one command. Returns the slot id.");
}
// NOLINTEND(bugprone-exception-escape)

} // namespace adx::bindings
