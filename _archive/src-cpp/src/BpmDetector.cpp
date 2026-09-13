#include "BpmDetector.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace BpmDetector {

float EstimateBpm(const std::vector<float>& monoSamples, unsigned int sampleRate) {
    if (sampleRate == 0) return 0.0f;

    // One-shots (single kicks, stabs) have no repeating pulse to measure.
    if (monoSamples.size() < static_cast<size_t>(sampleRate) * 2) return 0.0f;

    // 1. Energy envelope: RMS per hop. 512-sample hops @44.1kHz give an
    //    envelope rate of ~86Hz — plenty for tempo (max ~3.3 beats/sec).
    constexpr size_t kHop = 512;
    const size_t numFrames = monoSamples.size() / kHop;
    if (numFrames < 8) return 0.0f;

    std::vector<float> envelope(numFrames);
    for (size_t f = 0; f < numFrames; ++f) {
        float sum = 0.0f;
        const float* p = monoSamples.data() + f * kHop;
        for (size_t i = 0; i < kHop; ++i) sum += p[i] * p[i];
        envelope[f] = std::sqrt(sum / kHop);
    }

    // 2. Onset strength: half-wave rectified envelope difference. Emphasizes
    //    transient attacks (kicks) over sustained content.
    std::vector<float> onset(numFrames, 0.0f);
    for (size_t f = 1; f < numFrames; ++f) {
        onset[f] = std::max(0.0f, envelope[f] - envelope[f - 1]);
    }

    // Remove the mean so the autocorrelation isn't dominated by DC offset.
    float mean = 0.0f;
    for (float v : onset) mean += v;
    mean /= static_cast<float>(numFrames);
    for (float& v : onset) v -= mean;

    // 3. Autocorrelation over lags spanning 60..200 BPM.
    const float envRate = static_cast<float>(sampleRate) / kHop; // envelope frames per second
    const size_t minLag = static_cast<size_t>(envRate * 60.0f / 200.0f); // 200 BPM
    const size_t maxLag = std::min(static_cast<size_t>(envRate * 60.0f / 60.0f), numFrames / 2); // 60 BPM
    if (minLag >= maxLag) return 0.0f;

    float bestCorr = 0.0f;
    size_t bestLag = 0;
    float corrSum = 0.0f;
    size_t corrCount = 0;

    for (size_t lag = minLag; lag <= maxLag; ++lag) {
        float corr = 0.0f;
        for (size_t f = 0; f + lag < numFrames; ++f) {
            corr += onset[f] * onset[f + lag];
        }
        corr /= static_cast<float>(numFrames - lag);
        corrSum += std::max(corr, 0.0f);
        corrCount++;
        if (corr > bestCorr) {
            bestCorr = corr;
            bestLag = lag;
        }
    }
    if (bestLag == 0 || corrCount == 0) return 0.0f;

    // Confidence gate: the winning peak must clearly dominate the average
    // correlation, otherwise the material has no usable pulse.
    float avgCorr = corrSum / static_cast<float>(corrCount);
    if (avgCorr <= 0.0f || bestCorr < avgCorr * 2.0f) return 0.0f;

    // 4. Parabolic refinement around the peak for sub-lag precision.
    float refinedLag = static_cast<float>(bestLag);
    if (bestLag > minLag && bestLag < maxLag) {
        auto corrAt = [&](size_t lag) {
            float c = 0.0f;
            for (size_t f = 0; f + lag < numFrames; ++f) c += onset[f] * onset[f + lag];
            return c / static_cast<float>(numFrames - lag);
        };
        float c0 = corrAt(bestLag - 1);
        float c1 = bestCorr;
        float c2 = corrAt(bestLag + 1);
        float denom = c0 - 2.0f * c1 + c2;
        if (std::abs(denom) > 1e-9f) {
            refinedLag += 0.5f * (c0 - c2) / denom;
        }
    }

    float bpm = 60.0f * envRate / refinedLag;

    // Fold octave errors into the conventional EDM range.
    while (bpm < 70.0f) bpm *= 2.0f;
    while (bpm > 200.0f) bpm *= 0.5f;

    return bpm;
}

}
