// The binding layer's internal declarations.
//
// The alternative is free-standing extern declarations in module.cpp, which make the
// definitions look like functions that should have been static. Headers declare;
// implementation files implement (FINAL_PLAN §9), even here.
#pragma once

#include <pybind11/pybind11.h>

#include <pybind11/numpy.h>

#include "bindings/EngineHandle.h"
#include "bindings/ProjectHandle.h"
#include "engine/format/adx/Diagnostics.h"
#include "engine/geometry/MeterGeometry.h"

/// Registers device enumeration and stream control on the module.
void registerAudioBindings(pybind11::module_& m);

/// Registers the Project handle, command submission and undo/redo.
void registerProjectBindings(pybind11::module_& m);

/// Registers load/format/validate/diff for the CLI.
void registerFormatBindings(pybind11::module_& m);

/// Registers the render engine and its transport.
void registerTransportBindings(pybind11::module_& m);

/// Engine.frame(): everything the UI's 60 Hz timer reads, in one call (phase_5.md §4.8).
void defineEngineFrame(pybind11::class_<adx::bindings::EngineHandle>& engine);

/// Registers offline render.
void registerRenderBindings(pybind11::module_& m);

/// Registers geometry buffers, builders, hit tests and waveform peaks (phase_5.md §4).
void registerGeometryBindings(pybind11::module_& m);

/// Diagnostics as a list of plain dicts. The Python side turns them into dataclasses.
pybind11::list toPython(const adx::format::DiagnosticList& diagnostics);

namespace adx::bindings {

/// Phase 6: the project's structure for the panels (structure.cpp).
void defineStructureAccess(pybind11::class_<ProjectHandle>& project);
/// Phase 6: grouping, history jumps, drops and presets on the Project (session.cpp).
void defineSession(pybind11::class_<ProjectHandle>& project);
/// Phase 6: the type catalog and the engine's curve evaluator (structure.cpp).
void registerCatalog(pybind11::module_& m);
/// Phase 6: id-addressed command constructors on adx_engine.commands (edit.cpp).
void registerEditCommands(pybind11::module_& m);
/// Phase 6: the preset library (session.cpp).
void registerPresets(pybind11::module_& m);
/// Phase 6: the playlist, step grid, automation, meter and EQ builders (panels.cpp).
void registerPanelGeometry(pybind11::module_& m);
/// Phase 6: audition, metronome and EQ spectrum on Engine (panels.cpp).
void defineEngineExtras(pybind11::class_<EngineHandle>& engine);

using LevelArray = pybind11::array_t<float, pybind11::array::c_style | pybind11::array::forcecast>;

/// Engine.frame()'s body: pumps, fills `levels`, returns (ticks, rolling, strips, samples).
pybind11::tuple frameInto(EngineHandle& self, LevelArray& levels);
/// The same, also building `meters` from the strips read - still one call (§4.3).
pybind11::tuple frameWithMeters(EngineHandle& self, LevelArray& levels,
                                adx::geometry::MeterGeometry& meters);

} // namespace adx::bindings
