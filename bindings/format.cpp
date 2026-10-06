// Load, format, validate and diff, exposed to Python for the CLI.
//
// Diagnostics cross as plain dicts and become dataclasses on the Python side
// (app/adx/format.py). That is O(diagnostics), which Rule 2 allows - it forbids
// O(notes) and O(frames), not O(problems a person has to read).

#include <string>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "bindings/ProjectHandle.h"
#include "engine/format/adx/Diagnostics.h"
#include "engine/format/adx/Writer.h"
#include "engine/project/Diff.h"
#include "engine/project/Validate.h"

namespace py = pybind11;

py::list toPython(const adx::format::DiagnosticList& diagnostics) {
    py::list out;
    for (const adx::format::Diagnostic& item : diagnostics.all()) {
        py::dict entry;
        entry["severity"] = adx::format::toString(item.severity);
        entry["code"] = item.codeString();
        entry["line"] = item.span.line;
        entry["column"] = item.span.column;
        entry["length"] = item.span.length;
        entry["message"] = item.message;
        entry["hint"] = item.hint;
        out.append(entry);
    }
    return out;
}

void registerFormatBindings(py::module_& m) {
    // GIL: trivial - a fixed table of a few dozen rows
    m.def(
        "diagnostic_codes",
        []() {
            std::vector<py::tuple> out;
            for (const auto& row : adx::format::allDiagnosticCodes()) {
                adx::format::Diagnostic probe;
                probe.code = row.code;
                out.push_back(py::make_tuple(probe.codeString(),
                                             std::string(adx::format::toString(row.severity)),
                                             std::string(row.summary)));
            }
            return out;
        },
        "Every diagnostic code this build can emit, as (code, severity, summary).");

    m.def(
        "format_text",
        [](const std::string& text) {
            adx::bindings::ProjectHandle handle;
            adx::format::DiagnosticList diagnostics;
            std::string formatted;
            {
                const py::gil_scoped_release released;
                adx::bindings::loadInto(handle, text, {}, diagnostics);
                adx::format::WriteOptions options;
                options.lineEnding = handle.lineEnding;
                options.emitBom = handle.bom;
                formatted = adx::format::write(handle.project, options);
            }
            return py::make_tuple(formatted, toPython(diagnostics), handle.sourceVersion);
        },
        py::arg("text"),
        "Canonical form of `text`. Returns (formatted, diagnostics, source_version).");

    m.def(
        "validate_text",
        [](const std::string& text, const std::string& baseDirectory, bool checkSampleFiles) {
            adx::bindings::ProjectHandle handle;
            adx::format::DiagnosticList diagnostics;
            {
                const py::gil_scoped_release released;
                adx::bindings::loadInto(handle, text, baseDirectory, diagnostics);
                adx::project::ValidationOptions options;
                options.checkSampleFiles = checkSampleFiles;
                (void)adx::project::validate(handle.project, diagnostics, options);
            }
            return toPython(diagnostics);
        },
        py::arg("text"), py::arg("base_dir") = "", py::arg("check_sample_files") = false,
        "Parse and validate. Returns every diagnostic, parse and invariant alike.");

    m.def(
        "diff_text",
        [](const std::string& before, const std::string& after) {
            adx::bindings::ProjectHandle left;
            adx::bindings::ProjectHandle right;
            std::vector<adx::project::Change> changes;
            {
                const py::gil_scoped_release released;
                adx::format::DiagnosticList ignored;
                adx::bindings::loadInto(left, before, {}, ignored);
                adx::bindings::loadInto(right, after, {}, ignored);
                changes = adx::project::diff(left.project, right.project);
            }
            std::vector<py::tuple> out;
            out.reserve(changes.size());
            for (const auto& change : changes) {
                out.push_back(py::make_tuple(std::string(adx::project::toString(change.kind)),
                                             change.subject, change.detail));
            }
            return out;
        },
        py::arg("before"), py::arg("after"),
        "Semantic difference, as (kind, subject, detail) with kind one of + - ~.");
}
