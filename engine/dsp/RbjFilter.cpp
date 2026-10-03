#include "engine/dsp/RbjFilter.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"

namespace adx::dsp {

BiquadCoefficients biquadCoefficients(BiquadKind kind, double frequency, double q, double gainDb,
                                      double sampleRate) noexcept {
    const double f = std::clamp(frequency, 1.0, sampleRate * 0.4999);
    const double w0 = f / sampleRate; // in turns
    const double cosw = cosTurns(w0);
    const double sinw = sinTurns(w0);
    const double qq = std::max(q, 1e-4);
    const double a = pow(10.0, gainDb / 40.0);

    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a0 = 1.0;
    double a1 = 0.0;
    double a2 = 0.0;
    switch (kind) {
    case BiquadKind::LowPass: {
        const double alpha = sinw / (2.0 * qq);
        b0 = (1.0 - cosw) / 2.0;
        b1 = 1.0 - cosw;
        b2 = (1.0 - cosw) / 2.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosw;
        a2 = 1.0 - alpha;
        break;
    }
    case BiquadKind::HighPass: {
        const double alpha = sinw / (2.0 * qq);
        b0 = (1.0 + cosw) / 2.0;
        b1 = -(1.0 + cosw);
        b2 = (1.0 + cosw) / 2.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosw;
        a2 = 1.0 - alpha;
        break;
    }
    case BiquadKind::BandPass: {
        // Constant 0 dB peak gain.
        const double alpha = sinw / (2.0 * qq);
        b0 = alpha;
        b1 = 0.0;
        b2 = -alpha;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosw;
        a2 = 1.0 - alpha;
        break;
    }
    case BiquadKind::Notch: {
        const double alpha = sinw / (2.0 * qq);
        b0 = 1.0;
        b1 = -2.0 * cosw;
        b2 = 1.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosw;
        a2 = 1.0 - alpha;
        break;
    }
    case BiquadKind::AllPass: {
        const double alpha = sinw / (2.0 * qq);
        b0 = 1.0 - alpha;
        b1 = -2.0 * cosw;
        b2 = 1.0 + alpha;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosw;
        a2 = 1.0 - alpha;
        break;
    }
    case BiquadKind::Peaking: {
        const double alpha = sinw / (2.0 * qq);
        b0 = 1.0 + (alpha * a);
        b1 = -2.0 * cosw;
        b2 = 1.0 - (alpha * a);
        a0 = 1.0 + (alpha / a);
        a1 = -2.0 * cosw;
        a2 = 1.0 - (alpha / a);
        break;
    }
    case BiquadKind::LowShelf:
    case BiquadKind::HighShelf: {
        // Shelf slope S = q. At S = 1, alpha = sin(w0) / 2 * sqrt(2): the archived EQ's
        // exact expression.
        const double alpha =
            (sinw / 2.0) * std::sqrt(std::max(0.0, ((a + (1.0 / a)) * ((1.0 / qq) - 1.0)) + 2.0));
        const double sqrtA2alpha = 2.0 * std::sqrt(a) * alpha;
        if (kind == BiquadKind::LowShelf) {
            b0 = a * ((a + 1.0) - ((a - 1.0) * cosw) + sqrtA2alpha);
            b1 = 2.0 * a * ((a - 1.0) - ((a + 1.0) * cosw));
            b2 = a * ((a + 1.0) - ((a - 1.0) * cosw) - sqrtA2alpha);
            a0 = (a + 1.0) + ((a - 1.0) * cosw) + sqrtA2alpha;
            a1 = -2.0 * ((a - 1.0) + ((a + 1.0) * cosw));
            a2 = (a + 1.0) + ((a - 1.0) * cosw) - sqrtA2alpha;
        } else {
            b0 = a * ((a + 1.0) + ((a - 1.0) * cosw) + sqrtA2alpha);
            b1 = -2.0 * a * ((a - 1.0) + ((a + 1.0) * cosw));
            b2 = a * ((a + 1.0) + ((a - 1.0) * cosw) - sqrtA2alpha);
            a0 = (a + 1.0) - ((a - 1.0) * cosw) + sqrtA2alpha;
            a1 = 2.0 * ((a - 1.0) - ((a + 1.0) * cosw));
            a2 = (a + 1.0) - ((a - 1.0) * cosw) - sqrtA2alpha;
        }
        break;
    }
    }
    return BiquadCoefficients{.b0 = static_cast<float>(b0 / a0),
                              .b1 = static_cast<float>(b1 / a0),
                              .b2 = static_cast<float>(b2 / a0),
                              .a1 = static_cast<float>(a1 / a0),
                              .a2 = static_cast<float>(a2 / a0)};
}

double biquadMagnitudeDb(const BiquadCoefficients& c, double frequency,
                         double sampleRate) noexcept {
    const double w = frequency / sampleRate;
    const double c1 = cosTurns(w);
    const double s1 = sinTurns(w);
    const double c2 = cosTurns(2.0 * w);
    const double s2 = sinTurns(2.0 * w);
    // H = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2), z^-n = cos(nw) - i sin(nw).
    const double nr = c.b0 + (c.b1 * c1) + (c.b2 * c2);
    const double ni = -(c.b1 * s1) - (c.b2 * s2);
    const double dr = 1.0 + (c.a1 * c1) + (c.a2 * c2);
    const double di = -(c.a1 * s1) - (c.a2 * s2);
    const double magnitude = std::sqrt(((nr * nr) + (ni * ni)) / ((dr * dr) + (di * di)));
    return gainToDb(magnitude);
}

} // namespace adx::dsp
