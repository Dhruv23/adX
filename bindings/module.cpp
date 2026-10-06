// The one and only bridge between Python and the engine (FINAL_PLAN §2.1).
//
// Every binding added after this obeys FINAL_PLAN §2.2:
//   Rule 2 - cross this boundary O(interactions), never O(notes) or O(frames);
//            anything returning a list of per-note Python objects is a bug.
//   Rule 3 - wrap real work in py::gil_scoped_release.
// Neither applies to the two functions below, which do no work at all.

#include <string>

#include <pybind11/pybind11.h>

#include "bindings/Bindings.h"
#include "engine/core/Version.h"

PYBIND11_MODULE(adx_engine, m) {
    m.doc() = "Native engine for adX.";

    registerAudioBindings(m);
    registerProjectBindings(m);
    registerFormatBindings(m);
    registerTransportBindings(m);
    registerRenderBindings(m);
    registerGeometryBindings(m);

    // GIL: trivial - a string constant
    m.def(
        "version", []() { return std::string(adx::version()); },
        R"(Engine version as "MAJOR.MINOR.PATCH".)");

    // GIL: trivial - a string constant
    m.def(
        "git_sha", []() { return std::string(adx::gitSha()); },
        R"(Commit this extension was configured from, or "unknown".)");

    m.attr("__version__") = std::string(adx::version());
}
