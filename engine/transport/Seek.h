// What a seek does to the things playing from the source that jumped.
//
// Written down as a function rather than left implicit in the scheduler, because
// seek handling is the classic source of stuck notes and it is far easier to get
// right once, in one place, with a test per row (phase_3.md §4.11):
//
//   1. the seek generation is bumped - every event cursor re-searches on its next
//      block (TimeSource does this itself);
//   2. every voice driven by the source is *released*, not cut - a hard cut is a
//      click - unless the source was stopped, when nothing is sounding from it;
//   3. only DSP whose output depends on the timeline position is reset - a reverb
//      tail keeps ringing through a seek-while-playing, because cutting it sounds
//      broken;
//   4. audio clips recompute their read offset from the new tick (Phase 8).
//
// A loop wrap is *not* a seek. The cursors are already positioned by the sub-block
// split, nothing is invalidated, and voices sustain across the wrap.
#pragma once

#include "engine/transport/TimeSource.h"

namespace adx::transport {

struct SeekEffects {
    bool releaseVoices{false};
    bool resetPositionalDsp{false};
};

/// The voice and DSP policy for the transition a source reported at the top of a
/// block. Stopping releases voices exactly as a seek-while-playing does.
[[nodiscard]] SeekEffects effectsOf(const BlockTransition& transition) noexcept;

} // namespace adx::transport
