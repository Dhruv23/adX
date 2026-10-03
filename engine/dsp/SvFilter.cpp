#include "engine/dsp/SvFilter.h"

#include <algorithm>

#include "engine/dsp/Math.h"

namespace adx::dsp {

SvfCoefficients svfCoefficientsK(double cutoff, double k, double sampleRate) noexcept {
    // Prewarped: the analog prototype's cutoff lands exactly at `cutoff` after the
    // bilinear transform. Clamped short of Nyquist, where tan blows up.
    const double nyquistGuard = sampleRate * 0.4999;
    const double fc = std::clamp(cutoff, 1.0, nyquistGuard);
    const double g = tanTurns(0.5 * fc / sampleRate);
    const double damping = std::clamp(k, 0.0, 40.0);
    const double a1 = 1.0 / (1.0 + (g * (g + damping)));
    const double a2 = g * a1;
    const double a3 = g * a2;
    return SvfCoefficients{.a1 = static_cast<float>(a1),
                           .a2 = static_cast<float>(a2),
                           .a3 = static_cast<float>(a3),
                           .k = static_cast<float>(damping)};
}

SvfCoefficients svfCoefficients(double cutoff, double q, double sampleRate) noexcept {
    return svfCoefficientsK(cutoff, 1.0 / std::clamp(q, 0.025, 1000.0), sampleRate);
}

} // namespace adx::dsp
