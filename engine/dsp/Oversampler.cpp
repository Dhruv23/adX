// adx-thread: main
#include "engine/dsp/Oversampler.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"

namespace adx::dsp {
namespace {

/// The zeroth-order modified Bessel function, by its series: plain arithmetic, so the
/// taps are the same in every build.
[[nodiscard]] double besselI0(double x) noexcept {
    double sum = 1.0;
    double term = 1.0;
    const double quarter = x * x / 4.0;
    for (int k = 1; k < 40; ++k) {
        term *= quarter / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
    }
    return sum;
}

template<std::size_t Taps> std::array<float, Taps> design(double beta) {
    std::array<float, Taps> taps{};
    constexpr auto kCentre = static_cast<std::ptrdiff_t>(Taps / 2);
    const double norm = besselI0(beta);
    double sum = 0.0;
    for (std::size_t i = 0; i < Taps; ++i) {
        const std::ptrdiff_t n = static_cast<std::ptrdiff_t>(i) - kCentre;
        double h = 0.0;
        if (n == 0) {
            h = 0.5;
        } else if (n % 2 != 0) {
            // 0.5 sinc(n / 2): sin(pi n / 2) / (pi n), exactly zero at even n.
            h = sinTurns(static_cast<double>(n) / 4.0) / (kPi * static_cast<double>(n));
        }
        const double r = static_cast<double>(n) / static_cast<double>(kCentre);
        const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - (r * r)))) / norm;
        taps[i] = static_cast<float>(h * w);
        sum += h * w;
    }
    // Unity gain at DC.
    for (float& t : taps) {
        t = static_cast<float>(static_cast<double>(t) / sum);
    }
    return taps;
}

// Beta 9 gives about 90 dB of stopband on the 65-tap stage, where it matters.
const std::array<float, kHalfBandTaps1> kTaps1 = design<kHalfBandTaps1>(9.0);
const std::array<float, kHalfBandTaps2> kTaps2 = design<kHalfBandTaps2>(8.0);

} // namespace

const float* halfBandTaps1() noexcept {
    return kTaps1.data();
}

const float* halfBandTaps2() noexcept {
    return kTaps2.data();
}

void prepareOversampler() {
    // The taps are built during static initialisation; this exists so a node's
    // prepare() can name the dependency, and so a later lazy build has a place to go.
    static_cast<void>(kTaps1[0] + kTaps2[0]);
}

} // namespace adx::dsp
