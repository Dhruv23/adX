#pragma once

#include "AudioData.h"
#include <readerwriterqueue.h>

// Phase 5 Sample Browser: an ImGui window listing audio files from ./samples.
// Files can be dragged onto the Arranger timeline (payload type
// "ADX_SAMPLE_PATH", a null-terminated path string accepted by
// DrawSequencerUI's drop target), and the window hosts the Hardstyle
// Auto-Generation macro (kicks following the extracted melody's root notes).
void DrawSampleBrowser(SequencerState& state, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue);
