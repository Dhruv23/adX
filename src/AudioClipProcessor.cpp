#include "AudioClipProcessor.h"
#include <rubberband/RubberBandStretcher.h>

#include <cmath>
#include <vector>

namespace AudioClipProcessor {

void ReprocessClip(AudioClip& clip) {
    if (!clip.originalPcmData || clip.originalPcmData->empty() || clip.channels == 0) {
        return;
    }

    bool isIdentity = (clip.pitchShiftSemitones == 0.0f && clip.timeStretchFactor == 1.0f && !clip.reversed);
    if (isIdentity) {
        clip.pcmData = clip.originalPcmData; // zero-copy alias, no RubberBand invocation
        return;
    }

    const size_t channels = clip.channels;
    const size_t frameCount = clip.originalPcmData->size() / channels;
    if (frameCount == 0) return;

    // De-interleave into planar per-channel buffers (RubberBand's API wants
    // float* per channel), reversing the frame order first if requested
    // (STAKILLAZ suite: reversed chops feed RubberBand like any other source).
    std::vector<std::vector<float>> planarIn(channels, std::vector<float>(frameCount));
    for (size_t i = 0; i < frameCount; ++i) {
        size_t srcFrame = clip.reversed ? (frameCount - 1 - i) : i;
        for (size_t c = 0; c < channels; ++c) {
            planarIn[c][i] = (*clip.originalPcmData)[srcFrame * channels + c];
        }
    }

    // Reverse-only: no pitch/stretch work for RubberBand to do.
    if (clip.pitchShiftSemitones == 0.0f && clip.timeStretchFactor == 1.0f) {
        auto reversedOut = std::make_shared<std::vector<float>>(frameCount * channels);
        for (size_t i = 0; i < frameCount; ++i) {
            for (size_t c = 0; c < channels; ++c) {
                (*reversedOut)[i * channels + c] = planarIn[c][i];
            }
        }
        clip.pcmData = reversedOut;
        return;
    }
    std::vector<const float*> inputPtrs(channels);
    for (size_t c = 0; c < channels; ++c) inputPtrs[c] = planarIn[c].data();

    double timeRatio = static_cast<double>(clip.timeStretchFactor);
    double pitchScale = std::pow(2.0, static_cast<double>(clip.pitchShiftSemitones) / 12.0);

    RubberBand::RubberBandStretcher stretcher(
        clip.sampleRate, channels,
        RubberBand::RubberBandStretcher::OptionProcessOffline | RubberBand::RubberBandStretcher::OptionEngineFiner,
        timeRatio, pitchScale);

    stretcher.setExpectedInputDuration(frameCount);
    stretcher.setMaxProcessSize(frameCount); // whole buffer in one call; avoids internal buffer-resize warnings

    stretcher.study(inputPtrs.data(), frameCount, true);
    stretcher.process(inputPtrs.data(), frameCount, true);

    std::vector<std::vector<float>> planarOut(channels);

    // Offline mode with NO_THREADING is synchronous, so process() above already did
    // all the work; available() should only ever go from >0 straight to -1 (done).
    // Bound the loop defensively anyway so a misbehaving edge case can't hang the
    // Main/UI thread — fail safe to the unprocessed buffer rather than spin forever.
    size_t noProgressSpins = 0;
    constexpr size_t kMaxNoProgressSpins = 1000;

    while (true) {
        int avail = stretcher.available();
        if (avail < 0) break;
        if (avail == 0) {
            if (++noProgressSpins > kMaxNoProgressSpins) {
                clip.pcmData = clip.originalPcmData;
                return;
            }
            continue;
        }
        noProgressSpins = 0;

        std::vector<std::vector<float>> chunk(channels, std::vector<float>(static_cast<size_t>(avail)));
        std::vector<float*> chunkPtrs(channels);
        for (size_t c = 0; c < channels; ++c) chunkPtrs[c] = chunk[c].data();

        size_t got = stretcher.retrieve(chunkPtrs.data(), static_cast<size_t>(avail));
        for (size_t c = 0; c < channels; ++c) {
            planarOut[c].insert(planarOut[c].end(), chunk[c].begin(), chunk[c].begin() + got);
        }
    }

    if (planarOut.empty() || planarOut[0].empty()) {
        // Defensive fallback — shouldn't happen, but never leave pcmData null on the audio thread's behalf
        clip.pcmData = clip.originalPcmData;
        return;
    }

    size_t outFrameCount = planarOut[0].size();
    auto processed = std::make_shared<std::vector<float>>(outFrameCount * channels);
    for (size_t i = 0; i < outFrameCount; ++i) {
        for (size_t c = 0; c < channels; ++c) {
            (*processed)[i * channels + c] = planarOut[c][i];
        }
    }
    clip.pcmData = processed;
}

}
