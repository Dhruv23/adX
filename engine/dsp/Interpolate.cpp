#include "engine/dsp/Interpolate.h"

#include "engine/dsp/Math.h"

namespace adx::dsp {
namespace {

/// Zeroth-order modified Bessel function of the first kind, by its power series.
[[nodiscard]] double besselI0(double x) noexcept {
    double sum = 1.0;
    double term = 1.0;
    const double quarter = (x * x) / 4.0;
    for (int k = 1; k < 40; ++k) {
        term *= quarter / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-17) {
            break;
        }
    }
    return sum;
}

[[nodiscard]] SincTable buildSincTable() noexcept {
    // Kaiser beta 6: about -60 dB stopband from an 8-tap kernel - the balance between
    // passband flatness and image rejection that an 8-point interpolator can afford.
    constexpr double kBeta = 6.0;
    const double normaliser = besselI0(kBeta);
    constexpr double kHalf = static_cast<double>(kSincTaps) / 2.0;

    SincTable table{};
    for (std::size_t phase = 0; phase <= kSincPhases; ++phase) {
        const double fraction = static_cast<double>(phase) / static_cast<double>(kSincPhases);
        double sum = 0.0;
        for (std::size_t tap = 0; tap < kSincTaps; ++tap) {
            // Tap t reads sample index - 3 + t; its distance from the read position.
            const double x = static_cast<double>(tap) - 3.0 - fraction;
            const double sinc = x == 0.0 ? 1.0 : sinTurns(x * 0.5) / (kPi * x);
            const double ratio = x / kHalf;
            const double window =
                ratio <= -1.0 || ratio >= 1.0
                    ? 0.0
                    : besselI0(kBeta * std::sqrt(1.0 - (ratio * ratio))) / normaliser;
            const double value = sinc * window;
            table[phase][tap] = static_cast<float>(value);
            sum += value;
        }
        // Unity DC gain at every phase, so a constant signal stays exactly constant.
        for (std::size_t tap = 0; tap < kSincTaps; ++tap) {
            table[phase][tap] = static_cast<float>(static_cast<double>(table[phase][tap]) / sum);
        }
    }
    return table;
}

} // namespace

const SincTable& sincTable() noexcept {
    static const SincTable kTable = buildSincTable();
    return kTable;
}

} // namespace adx::dsp
