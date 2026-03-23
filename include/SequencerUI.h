#pragma once

#include "AudioData.h"
#include <readerwriterqueue.h>

void DrawSequencerUI(SequencerState& state, moodycamel::ReaderWriterQueue<AudioEvent>& eventQueue);