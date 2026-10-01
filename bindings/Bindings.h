// The binding layer's internal declarations.
//
// The alternative is free-standing extern declarations in module.cpp, which make the
// definitions look like functions that should have been static. Headers declare;
// implementation files implement (FINAL_PLAN §9), even here.
#pragma once

#include <pybind11/pybind11.h>

#include "engine/format/adx/Diagnostics.h"

/// Registers device enumeration and stream control on the module.
void registerAudioBindings(pybind11::module_& m);

/// Registers the Project handle, command submission and undo/redo.
void registerProjectBindings(pybind11::module_& m);

/// Registers load/format/validate/diff for the CLI.
void registerFormatBindings(pybind11::module_& m);

/// Registers the render engine and its transport.
void registerTransportBindings(pybind11::module_& m);

/// Registers offline render.
void registerRenderBindings(pybind11::module_& m);

/// Diagnostics as a list of plain dicts. The Python side turns them into dataclasses.
pybind11::list toPython(const adx::format::DiagnosticList& diagnostics);
