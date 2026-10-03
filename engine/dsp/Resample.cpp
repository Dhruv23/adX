// adx-thread: main
#include "engine/dsp/Resample.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"

namespace adx::dsp {
namespace {

[[nodiscard]] double besselI0(double x) noexcept {
    double sum = 1.0;
    double term = 1.0;
    const double quarter = (x * x) / 4.0;
    for (int k = 1; k < 60; ++k) {
        term *= quarter / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-17) {
            break;
        }
    }
    return sum;
}

} // namespace

void Resampler::prepare(double inputRate, double outputRate) {
    m_ratio = outputRate > 0.0 && inputRate > 0.0 ? outputRate / inputRate : 1.0;
    constexpr std::size_t kTaps = 2 * kHalfTaps;
    constexpr double kBeta = 10.0;
    // The cutoff, as a fraction of the input rate: just below the lower Nyquist, so a
    // downward conversion removes what the new rate cannot carry before it can alias.
    const double cutoff = 0.5 * std::min(1.0, m_ratio) * 0.94;
    const double normaliser = besselI0(kBeta);

    m_kernel.assign((kPhases + 1) * kTaps, 0.0F);
    for (std::size_t phase = 0; phase <= kPhases; ++phase) {
        const double fraction = static_cast<double>(phase) / static_cast<double>(kPhases);
        double sum = 0.0;
        for (std::size_t tap = 0; tap < kTaps; ++tap) {
            // Tap t multiplies input sample floor(position) - (kHalfTaps - 1) + t.
            const double x =
                static_cast<double>(tap) - static_cast<double>(kHalfTaps - 1) - fraction;
            const double arg = 2.0 * cutoff * x;
            const double sinc = arg == 0.0 ? 1.0 : sinTurns(arg * 0.5) / (kPi * arg);
            const double ratio = x / static_cast<double>(kHalfTaps);
            const double window =
                ratio <= -1.0 || ratio >= 1.0
                    ? 0.0
                    : besselI0(kBeta * std::sqrt(1.0 - (ratio * ratio))) / normaliser;
            const double value = 2.0 * cutoff * sinc * window;
            m_kernel[(phase * kTaps) + tap] = static_cast<float>(value);
            sum += value;
        }
        // Unity DC gain at every phase.
        for (std::size_t tap = 0; tap < kTaps; ++tap) {
            float& value = m_kernel[(phase * kTaps) + tap];
            value = static_cast<float>(static_cast<double>(value) / sum);
        }
    }
}

std::size_t Resampler::outputLength(std::size_t inputLength) const noexcept {
    return static_cast<std::size_t>(std::floor((static_cast<double>(inputLength) * m_ratio) + 0.5));
}

void Resampler::process(std::span<const float> in, std::span<float> out) const noexcept {
    constexpr std::size_t kTaps = 2 * kHalfTaps;
    const auto inputLength = static_cast<std::ptrdiff_t>(in.size());
    for (std::size_t n = 0; n < out.size(); ++n) {
        const double position = static_cast<double>(n) / m_ratio;
        const double whole = std::floor(position);
        const double fraction = position - whole;
        const double phasePosition = fraction * static_cast<double>(kPhases);
        const auto phase = static_cast<std::size_t>(phasePosition);
        const auto blend = static_cast<float>(phasePosition - static_cast<double>(phase));
        const float* a = &m_kernel[phase * kTaps];
        const float* b = &m_kernel[std::min(phase + 1, kPhases) * kTaps];
        const auto first =
            static_cast<std::ptrdiff_t>(whole) - static_cast<std::ptrdiff_t>(kHalfTaps - 1);
        double sum = 0.0;
        for (std::size_t tap = 0; tap < kTaps; ++tap) {
            const std::ptrdiff_t index = first + static_cast<std::ptrdiff_t>(tap);
            if (index < 0 || index >= inputLength) {
                continue;
            }
            const float weight = a[tap] + ((b[tap] - a[tap]) * blend);
            sum += static_cast<double>(weight) *
                   static_cast<double>(in[static_cast<std::size_t>(index)]);
        }
        out[n] = static_cast<float>(sum);
    }
}

} // namespace adx::dsp
