#pragma once

#include "AudioData.h"
#include "AudioEngine.h"
#include <readerwriterqueue.h>

// `engine` is read-only here (live-PLAN L4: per-track peak-hold meters next
// to each track tab, via AudioEngine::GetTrackPeak).
void DrawSequencerUI(SequencerState& state, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue, const AudioEngine& engine);

// Ships a fresh copy of state.tracks + state.patches to the audio thread
// (lock-free) as one SequenceSnapshot, so it can resolve each track's own
// patch itself (Phase 1). Shared by the sequencer UI, the sample browser,
// and main.cpp — this is also how patch edits reach the audio thread now,
// since there's no separate single-active-patch event anymore.
void DispatchSequenceUpdate(SequencerState& state, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue);

// Requests that DrawSequencerUI show/select the track at `index` on its next
// call (e.g. after main.cpp appends a new track from ML melody extraction).
// Safe to call with an index that's momentarily out of range.
void SelectTrack(int index);