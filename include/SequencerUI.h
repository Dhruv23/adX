#pragma once

#include "AudioData.h"
#include <readerwriterqueue.h>

void DrawSequencerUI(SequencerState& state, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue);

// Ships a fresh copy of state.tracks to the audio thread (lock-free). Shared
// by the sequencer UI, the sample browser, and main.cpp.
void DispatchSequenceUpdate(SequencerState& state, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue);

// Requests that DrawSequencerUI show/select the track at `index` on its next
// call (e.g. after main.cpp appends a new track from ML melody extraction).
// Safe to call with an index that's momentarily out of range.
void SelectTrack(int index);