// The binding layer's one internal declaration.
//
// A header for a single function looks like overkill, but the alternative is a
// free-standing extern declaration in module.cpp, which makes the definition in
// audio.cpp look like a function that should have been static. Headers declare;
// implementation files implement (FINAL_PLAN §9), even here.
#pragma once

#include <pybind11/pybind11.h>

/// Registers device enumeration and stream control on the module.
void registerAudioBindings(pybind11::module_& m);
