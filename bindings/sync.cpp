// The text-first layer, exposed to Python (phase_7.md §2): the coordinator, the file
// watcher, the editor's incremental diagnostics and its token stream.
//
// The app drives all of it from Qt timers on the main thread - the watcher's poll, Live
// mode's debounced write, the editor's re-highlight - so nothing here calls back into
// Python, and every result is a value the caller looks at.

#include <memory>
#include <string>
#include <utility>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings/Bindings.h"
#include "bindings/ProjectHandle.h"
#include "engine/format/adx/Incremental.h"
#include "engine/format/adx/Tokenize.h"
#include "engine/sync/FileWatcher.h"
#include "engine/sync/ReloadPolicy.h"
#include "engine/sync/TextSync.h"

namespace py = pybind11;

namespace {

using adx::bindings::ProjectHandle;
using adx::sync::ApplyResult;
using adx::sync::SyncMode;
using adx::sync::TextSync;

/// A TextSync and the line-ending text its options view.
struct SyncHandle {
    std::unique_ptr<TextSync> sync;
};

py::dict resultToPython(const ApplyResult& result) {
    py::dict out;
    out["status"] = std::string(adx::sync::toString(result.status));
    out["commands"] = result.patch.commands;
    out["structural"] = result.patch.structural;
    out["diagnostics"] = ::toPython(result.diagnostics);
    return out;
}

SyncMode modeFrom(const std::string& name) {
    if (name == "off") {
        return SyncMode::Off;
    }
    if (name == "live") {
        return SyncMode::Live;
    }
    if (name == "on-save") {
        return SyncMode::OnSave;
    }
    throw std::invalid_argument("sync mode is one of off, on-save, live; not '" + name + "'");
}

} // namespace

namespace adx::bindings {

// NOLINTBEGIN(bugprone-exception-escape) - a bad mode name is a ValueError by design.
namespace {

void defineTextSync(py::module_& sync) {
    py::class_<SyncHandle>(sync, "TextSync")
        // GIL: trivial - stores references and copies two short strings
        .def(py::init([](ProjectHandle& handle, const std::string& baseDirectory) {
                 format::WriteOptions spelling;
                 spelling.lineEnding = handle.lineEnding;
                 spelling.emitBom = handle.bom;
                 return SyncHandle{.sync = std::make_unique<TextSync>(handle.project, handle.stack,
                                                                      spelling, baseDirectory)};
             }),
             py::arg("project"), py::arg("base_dir") = "",
             // The project must outlive the coordinator that holds references into it.
             py::keep_alive<1, 2>())
        // GIL: trivial - stores an enum
        .def(
            "set_mode",
            [](SyncHandle& self, const std::string& mode) { self.sync->setMode(modeFrom(mode)); },
            py::arg("mode"), "off, on-save (the default) or live.")
        // GIL: trivial - reads an enum
        .def("mode",
             [](const SyncHandle& self) { return std::string(sync::toString(self.sync->mode())); })
        .def(
            "mark_synced",
            [](SyncHandle& self, const std::string& text) {
                const py::gil_scoped_release released;
                self.sync->markSynced(text);
            },
            py::arg("text"), "The file and the model agree on `text` (after load, save, write).")
        .def(
            "render_text",
            [](const SyncHandle& self) {
                std::string text;
                {
                    const py::gil_scoped_release released;
                    text = self.sync->renderText();
                }
                return text;
            },
            "The model's canonical text, spelled as the file is.")
        .def(
            "apply_text",
            [](SyncHandle& self, const std::string& text) {
                ApplyResult result;
                {
                    const py::gil_scoped_release released;
                    result = self.sync->applyText(text);
                }
                return resultToPython(result);
            },
            py::arg("text"), "The editor's buffer changed. Errors leave the model alone.")
        .def(
            "apply_external",
            [](SyncHandle& self, const std::string& text) {
                ApplyResult result;
                {
                    const py::gil_scoped_release released;
                    result = self.sync->applyExternal(text);
                }
                return resultToPython(result);
            },
            py::arg("text"),
            "The file changed on disk: applied, unchanged, invalid, conflict or self-write.")
        .def(
            "apply_forced",
            [](SyncHandle& self, const std::string& text) {
                ApplyResult result;
                {
                    const py::gil_scoped_release released;
                    result = self.sync->applyForced(text);
                }
                return resultToPython(result);
            },
            py::arg("text"), "Take theirs: apply over unsynced model edits (one undo away).")
        // GIL: trivial - hashes a string the caller just wrote
        .def(
            "note_write",
            [](SyncHandle& self, const std::string& text) { self.sync->noteWrite(text); },
            py::arg("text"), "adX wrote `text` to the file; its watcher event is an echo.")
        // GIL: trivial - compares two integers
        .def("model_dirty", [](const SyncHandle& self) { return self.sync->modelDirty(); })
        // GIL: trivial - copies a string
        .def("synced_text", [](const SyncHandle& self) { return self.sync->syncedText(); })
        // GIL: trivial - reads a counter
        .def("applies", [](const SyncHandle& self) { return self.sync->applies(); });
}

void defineFileWatcher(py::module_& sync) {
    py::class_<sync::FileWatcher>(sync, "FileWatcher")
        // GIL: trivial - opens a directory handle and starts the watcher thread
        .def(py::init([](const std::string& path, int debounceMs) {
                 // Python hands over UTF-8; a path built from char would read it in the
                 // ANSI code page.
                 return std::make_unique<sync::FileWatcher>(
                     std::filesystem::path(std::u8string(path.begin(), path.end())),
                     std::chrono::milliseconds(debounceMs));
             }),
             py::arg("path"), py::arg("debounce_ms") = sync::kWatcherDebounceMs)
        // GIL: trivial - two atomic loads
        .def("poll", &sync::FileWatcher::poll,
             "True once per burst of changes, after the debounce window of quiet.")
        // GIL: trivial - an atomic load
        .def("watching", &sync::FileWatcher::watching)
        // GIL: trivial - an atomic load
        .def("events", &sync::FileWatcher::events);
}

void defineIncrementalParse(py::module_& sync) {
    py::class_<format::IncrementalParse>(sync, "IncrementalParse")
        .def(py::init<>())
        .def(
            "reset",
            [](format::IncrementalParse& self, const std::string& text) {
                const py::gil_scoped_release released;
                self.reset(text);
            },
            py::arg("text"))
        .def(
            "update",
            [](format::IncrementalParse& self, const std::string& text) {
                const py::gil_scoped_release released;
                self.update(text);
            },
            py::arg("text"), "Re-diagnose `text`, re-parsing as little as phase_7.md §4.3 allows.")
        // GIL: trivial - O(diagnostics) dicts
        .def("diagnostics",
             [](const format::IncrementalParse& self) { return toPython(self.diagnostics()); })
        // GIL: trivial - reads a double
        .def("last_ms", &format::IncrementalParse::lastMilliseconds)
        // GIL: trivial - reads a flag
        .def("last_incremental", &format::IncrementalParse::lastWasIncremental);
}

void defineTokens(py::module_& sync) {
    // GIL: trivial - a fixed table
    sync.def(
        "semantic_kinds",
        []() {
            std::vector<std::string> names;
            for (int kind = 0; kind <= static_cast<int>(format::SemanticKind::Error); ++kind) {
                names.emplace_back(format::toString(static_cast<format::SemanticKind>(kind)));
            }
            return names;
        },
        "Token kind names, indexed by the kind column of semantic_tokens().");

    sync.def(
        "semantic_tokens",
        [](const std::string& text) {
            std::vector<format::SemanticToken> tokens;
            {
                const py::gil_scoped_release released;
                tokens = format::semanticTokens(text);
            }
            // One array, not a list of tuples: a 10,000-line file is ~50,000 tokens.
            py::array_t<std::int32_t> out(
                {static_cast<py::ssize_t>(tokens.size()), py::ssize_t{4}});
            auto view = out.mutable_unchecked<2>();
            for (std::size_t i = 0; i < tokens.size(); ++i) {
                const auto row = static_cast<py::ssize_t>(i);
                view(row, 0) = static_cast<std::int32_t>(tokens[i].line);
                view(row, 1) = static_cast<std::int32_t>(tokens[i].column);
                view(row, 2) = static_cast<std::int32_t>(tokens[i].length);
                view(row, 3) = static_cast<std::int32_t>(tokens[i].kind);
            }
            return out;
        },
        py::arg("text"),
        "Every token as rows of (line 1-based, byte column 0-based, byte length, kind).");
}

} // namespace

void registerSync(py::module_& m) {
    py::module_ sync = m.def_submodule(
        "sync", "Bidirectional text sync, the file watcher, incremental diagnostics, tokens.");
    defineTextSync(sync);
    defineFileWatcher(sync);
    defineIncrementalParse(sync);
    defineTokens(sync);
}
// NOLINTEND(bugprone-exception-escape)

} // namespace adx::bindings
