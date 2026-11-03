// adx-thread: main
#include "engine/analysis/OnsetDetector.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "engine/dsp/Fft.h"

namespace adx::analysis {

namespace {

constexpr std::size_t kSize = 1024;
constexpr std::size_t kHop = 256;
constexpr int kPeakRadius = 6;
constexpr int kMeanRadius = 16;

} // namespace

std::vector<double> detectOnsets(std::span<const float> mono, std::uint32_t sampleRate,
                                 const OnsetOptions& options) {
    std::vector<double> onsets;
    if (sampleRate == 0 || mono.size() < kSize) {
        return onsets;
    }
    dsp::Fft fft;
    fft.prepare(kSize);
    std::vector<float> window(kSize);
    for (std::size_t i = 0; i < kSize; ++i) {
        window[i] = static_cast<float>(
            0.5 - (0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / kSize)));
    }
    std::vector<dsp::Complex> spectrum(kSize);
    std::vector<float> previous(kSize / 2, 0.0F);
    std::vector<float> current(kSize / 2, 0.0F);
    // A frame per hop, each ending at f * hop. The first full window is the reference
    // the next is compared with: a window sliding from silence before the file onto its
    // first sample would read the file's own noise floor as an onset.
    const std::size_t frames = (mono.size() / kHop) + 1;
    const std::size_t firstFrame = kSize / kHop;
    std::vector<float> flux(frames, 0.0F);
    for (std::size_t f = 0; f < frames; ++f) {
        const auto start =
            static_cast<std::ptrdiff_t>(f * kHop) - static_cast<std::ptrdiff_t>(kSize);
        for (std::size_t i = 0; i < kSize; ++i) {
            const std::ptrdiff_t at = start + static_cast<std::ptrdiff_t>(i);
            const float s = at >= 0 && static_cast<std::size_t>(at) < mono.size()
                                ? mono[static_cast<std::size_t>(at)]
                                : 0.0F;
            spectrum[i] = dsp::Complex{s * window[i], 0.0F};
        }
        fft.forward(spectrum);
        float rise = 0.0F;
        for (std::size_t k = 0; k < kSize / 2; ++k) {
            current[k] = std::log1p(100.0F * std::abs(spectrum[k]));
            rise += std::max(0.0F, current[k] - previous[k]);
        }
        flux[f] = f > firstFrame ? rise : 0.0F;
        std::swap(previous, current);
    }

    const auto gap = static_cast<double>(options.minimumGapSeconds);
    double last = -1e9;
    for (std::size_t f = 0; f < frames; ++f) {
        const auto lo = static_cast<std::size_t>(
            std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(f) - kPeakRadius));
        const std::size_t hi = std::min(frames - 1, f + kPeakRadius);
        if (std::ranges::any_of(flux.begin() + static_cast<std::ptrdiff_t>(lo),
                                flux.begin() + static_cast<std::ptrdiff_t>(hi) + 1,
                                [&](float v) { return v > flux[f]; })) {
            continue;
        }
        const auto mlo = static_cast<std::size_t>(
            std::max<std::ptrdiff_t>(0, static_cast<std::ptrdiff_t>(f) - kMeanRadius));
        const std::size_t mhi = std::min(frames - 1, f + kMeanRadius);
        float mean = 0.0F;
        for (std::size_t i = mlo; i <= mhi; ++i) {
            mean += flux[i];
        }
        mean /= static_cast<float>(mhi - mlo + 1);
        if (flux[f] < options.floor || flux[f] < mean * options.sensitivity) {
            continue;
        }
        // Where the flux peaks, the attack sits three quarters of the way into the
        // window (see the header).
        const double seconds = (static_cast<double>(f * kHop) - static_cast<double>(kSize) +
                                (static_cast<double>(kSize) * 0.75)) /
                               sampleRate;
        if (seconds - last >= gap) {
            onsets.push_back(std::max(0.0, seconds));
            last = seconds;
        }
    }
    return onsets;
}

} // namespace adx::analysis
