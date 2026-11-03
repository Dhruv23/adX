// adx-thread: main
#include "engine/analysis/BpmDetector.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace adx::analysis {

std::optional<double> estimateBpm(std::span<const float> mono, std::uint32_t sampleRate) {
    // One-shots (single kicks, stabs) have no repeating pulse to measure.
    if (sampleRate == 0 || mono.size() < static_cast<std::size_t>(sampleRate) * 2) {
        return std::nullopt;
    }

    // 1. Energy envelope: RMS per hop. 512-sample hops at 44.1 kHz are an envelope rate
    //    of ~86 Hz - plenty for tempo (at most ~3.3 beats a second).
    constexpr std::size_t kHop = 512;
    const std::size_t count = mono.size() / kHop;
    if (count < 8) {
        return std::nullopt;
    }
    std::vector<float> envelope(count);
    for (std::size_t f = 0; f < count; ++f) {
        float sum = 0.0F;
        for (std::size_t i = 0; i < kHop; ++i) {
            const float s = mono[(f * kHop) + i];
            sum += s * s;
        }
        envelope[f] = std::sqrt(sum / static_cast<float>(kHop));
    }

    // 2. Onset strength: the half-wave rectified envelope difference, which weights
    //    transient attacks (kicks) over sustained content. Mean removed, so the
    //    autocorrelation is not dominated by an offset.
    std::vector<float> onset(count, 0.0F);
    for (std::size_t f = 1; f < count; ++f) {
        onset[f] = std::max(0.0F, envelope[f] - envelope[f - 1]);
    }
    float mean = 0.0F;
    for (const float v : onset) {
        mean += v;
    }
    mean /= static_cast<float>(count);
    for (float& v : onset) {
        v -= mean;
    }

    // 3. Autocorrelation over lags spanning 60..200 BPM.
    const float rate = static_cast<float>(sampleRate) / static_cast<float>(kHop);
    const auto minLag = static_cast<std::size_t>(rate * 60.0F / 200.0F);
    const std::size_t maxLag = std::min(static_cast<std::size_t>(rate), count / 2);
    if (minLag >= maxLag) {
        return std::nullopt;
    }
    const auto corrAt = [&onset, count](std::size_t lag) {
        float c = 0.0F;
        for (std::size_t f = 0; f + lag < count; ++f) {
            c += onset[f] * onset[f + lag];
        }
        return c / static_cast<float>(count - lag);
    };
    float best = 0.0F;
    std::size_t bestLag = 0;
    float total = 0.0F;
    std::size_t lags = 0;
    for (std::size_t lag = minLag; lag <= maxLag; ++lag) {
        const float c = corrAt(lag);
        total += std::max(c, 0.0F);
        ++lags;
        if (c > best) {
            best = c;
            bestLag = lag;
        }
    }
    // Confidence: the winning peak must clearly dominate the average, or the material
    // has no usable pulse.
    const float average = lags > 0 ? total / static_cast<float>(lags) : 0.0F;
    if (bestLag == 0 || average <= 0.0F || best < average * 2.0F) {
        return std::nullopt;
    }

    // Double time: a backbeat - snare on two and four - correlates more strongly at two
    // beats than at one, and the peak lands on half the tempo. When the half lag is a
    // strong peak of its own, it is the beat. (Added on port: the archive's corpus was
    // four-on-the-floor, where this never comes up.)
    if (const std::size_t half = (bestLag + 1) / 2; half >= minLag) {
        std::size_t local = half;
        for (const std::size_t lag : {half - 1, half + 1}) {
            if (lag >= minLag && corrAt(lag) > corrAt(local)) {
                local = lag;
            }
        }
        if (corrAt(local) >= best * 0.5F) {
            bestLag = local;
            best = corrAt(local);
        }
    }

    // 4. Parabolic refinement around the peak, for a fraction of a lag.
    auto refined = static_cast<float>(bestLag);
    if (bestLag > minLag && bestLag < maxLag) {
        const float c0 = corrAt(bestLag - 1);
        const float c2 = corrAt(bestLag + 1);
        const float denominator = c0 - (2.0F * best) + c2;
        if (std::abs(denominator) > 1e-9F) {
            refined += 0.5F * (c0 - c2) / denominator;
        }
    }
    double bpm = 60.0 * rate / refined;
    // Octave errors folded into the conventional range.
    while (bpm < 70.0) {
        bpm *= 2.0;
    }
    while (bpm > 200.0) {
        bpm *= 0.5;
    }
    return bpm;
}

} // namespace adx::analysis
