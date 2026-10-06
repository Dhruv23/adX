// Notes across the boundary: one structured numpy array per clip, never a list of
// per-note Python objects (FINAL_PLAN §2.2 Rule 2, phase_5.md §3).
//
// A NoteRecord row is the fixed part of a Note. Python reads a clip as one copy of
// these (a memcpy-sized loop in C++, one Python object for the whole clip), computes an
// edit with numpy, and hands it back as one EditNotes command.
#pragma once

#include <cstdint>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

#include "bindings/ProjectHandle.h"

namespace adx::bindings {

struct NoteRecord {
    std::uint32_t id;
    std::int64_t start;
    std::int64_t length;
    std::uint8_t pitch;
    std::uint8_t velocity;
    std::int16_t fine;
    std::uint16_t release;
    float pan;
    float cutoff;
    float resonance;
    std::uint8_t muted;
};

/// The numpy dtype of a NoteRecord row: adx_engine.NOTE_DTYPE.
pybind11::dtype noteDtypeObject();

/// Project.notes(), note_extras(), lyric_cells() and the per-pattern/channel facts the
/// piano roll reads.
void defineNoteAccess(pybind11::class_<ProjectHandle>& project);

/// EditNotes, SetNoteSlide, SetPitchCurve and SetLyric on the commands submodule.
void registerNoteCommands(pybind11::module_& commands);

} // namespace adx::bindings
