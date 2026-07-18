#pragma once

#include "AudioData.h"

// Regenerates AudioClip::pcmData from AudioClip::originalPcmData by applying
// pitchShiftSemitones/timeStretchFactor via RubberBand's Offline engine.
// Main Thread only — never call this from AudioEngine::process(). Heavy DSP
// work (allocation, RubberBand's internal FFT processing) happens here, same
// as AudioFileLoader::LoadAudioClip's decode step.
namespace AudioClipProcessor {
    void ReprocessClip(AudioClip& clip);
}
