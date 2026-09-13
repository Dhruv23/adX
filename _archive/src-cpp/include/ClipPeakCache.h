#pragma once

#include "AudioData.h"
#include <vector>

// Precomputed min/max waveform envelope for one audio clip, built lazily on
// the Main Thread from AudioClip::pcmData (UI-Refactor Phase 2). Consumed by
// the Arranger's clip-drawing loop so clips render as real waveforms instead
// of flat rectangles; never touched from AudioEngine::process.
struct ClipPeaks {
    // Mipmap-style tiers: tiers[0] is the finest (fewest source samples
    // merged per bucket); each following tier doubles samplesPerBucket by
    // merging adjacent bucket pairs (min-of-mins/max-of-maxs). Letting the
    // renderer pick a tier close to the current on-screen zoom means it
    // never has to rescan the full sample buffer per frame at any zoom
    // level.
    struct Tier {
        int samplesPerBucket = 1;
        std::vector<float> mins;
        std::vector<float> maxs;
    };
    std::vector<Tier> tiers;
    size_t frameCount = 0; // per-channel sample count spanned by the tiers
};

// Returns cached peaks for `clip`, building and caching them on first look-up
// for this exact (filePath, pitchShiftSemitones, timeStretchFactor, reversed)
// identity — the same identity AudioClip::operator== compares, minus
// startTimeSeconds, which doesn't affect waveform shape. A cache hit whose
// underlying pcmData was swapped out from under it (AudioClipProcessor::
// ReprocessClip re-decoding in place) is detected and rebuilt automatically.
// Returns nullptr if the clip has no decoded audio yet. Main-Thread-only.
const ClipPeaks* GetOrBuildClipPeaks(const AudioClip& clip);

// Drops every cached entry. Not needed in normal use; exposed for tests.
void ClearClipPeakCache();
