// Window functions: Hann, Hamming, Blackman-Harris, Tukey.
//
// w(i) for i in [0, n). "Periodic" (DFT-even) by default - the form an STFT with
// overlap-add wants, because its shifted copies sum to a constant; "symmetric" is the
// form a filter design wants.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/dsp/Math.h"

namespace adx::dsp {

enum class WindowKind : std::uint8_t { Rectangular, Hann, Hamming, BlackmanHarris, Tukey };

/// `tukeyAlpha` is the tapered fraction for WindowKind::Tukey (0 = rectangular,
/// 1 = Hann).
[[nodiscard]] inline double windowValue(WindowKind kind, std::size_t i, std::size_t n,
                                        bool periodic = true, double tukeyAlpha = 0.5) noexcept {
    if (n <= 1) {
        return 1.0;
    }
    const double denominator = periodic ? static_cast<double>(n) : static_cast<double>(n - 1);
    const double x = static_cast<double>(i) / denominator; // [0, 1)
    switch (kind) {
    case WindowKind::Rectangular:
        return 1.0;
    case WindowKind::Hann:
        return 0.5 - (0.5 * cosTurns(x));
    case WindowKind::Hamming:
        return 0.54 - (0.46 * cosTurns(x));
    case WindowKind::BlackmanHarris:
        // 4-term, -92 dB sidelobes: what the aliasing and THD tests measure through.
        return 0.35875 - (0.48829 * cosTurns(x)) + (0.14128 * cosTurns(2.0 * x)) -
               (0.01168 * cosTurns(3.0 * x));
    case WindowKind::Tukey: {
        if (tukeyAlpha <= 0.0) {
            return 1.0;
        }
        const double half = tukeyAlpha * 0.5;
        if (x < half) {
            return 0.5 - (0.5 * cosTurns(x / tukeyAlpha));
        }
        if (x > 1.0 - half) {
            return 0.5 - (0.5 * cosTurns((1.0 - x) / tukeyAlpha));
        }
        return 1.0;
    }
    }
    return 1.0;
}

/// The window's coherent gain: its mean. Divide a windowed FFT magnitude by it (and
/// by n/2) to read a sinusoid's amplitude.
[[nodiscard]] inline double windowCoherentGain(WindowKind kind, std::size_t n) noexcept {
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        sum += windowValue(kind, i, n);
    }
    return n == 0 ? 1.0 : sum / static_cast<double>(n);
}

} // namespace adx::dsp
