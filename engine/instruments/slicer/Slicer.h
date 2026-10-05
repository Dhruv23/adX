// The Slicer: beat-sliced audio as playable pads (phase_4.md §4.6).
//
// A front end over the Sampler, not a second playback engine. Each slice is a zone on
// one key, from its start frame to its end frame, at its root pitch; slices play to
// their end and choke one another (kSlicerParams). So the node is the Sampler under
// another type name, and what is the Slicer's own is the main-thread half: cutting a
// sample into slice zones, and laying the slices out as notes (SliceLayout.h).
//
// BPM-aware: slicePattern() places each slice at its position in *beats* of the
// source loop, so the notes move with the project's tempo - a faster tempo plays the
// slices closer together - while each slice still plays at its recorded pitch and
// length. Nothing is re-sliced or re-pitched when the tempo changes
// (slicer_tempo_change).
//
// Slice points come from an even division here; Phase 10's onset detector supplies
// them later through slicesAt(). Either way they are stored in the project as ZONE
// lines with `end=`, and are editable there.
#pragma once

#include <string_view>

#include "engine/instruments/sampler/SamplerInstrument.h"

namespace adx::instruments {

class SlicerInstrument final : public SamplerInstrument {
public:
    using SamplerInstrument::SamplerInstrument;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "slicer";
    }
};

} // namespace adx::instruments
