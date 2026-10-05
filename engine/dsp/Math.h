// Transcendental functions that produce the same bits in every build.
//
// The golden corpus hashes a render, and Debug, RelWithDebInfo and Release must all
// produce that hash (phase_3.md §4.10). A library std::sin, std::exp or std::tanh is
// free to be a different function in each configuration: an intrinsic in one, a CRT
// call in another, and - the one that actually bites - a vectorised approximation in a
// third, when the optimiser vectorises the loop it sits in. So DSP code under
// engine/dsp, engine/instruments, engine/effects and engine/mixer never calls them;
// `tools/lint.py dsp-math` enforces it.
//
// What is used instead is built from operations whose result IEEE 754 defines exactly
// - add, multiply, divide, sqrt, floor, and scaling by a power of two (ldexp, frexp) -
// so no implementation can disagree with another about them. Range reduction plus a
// short polynomial, evaluated in double where the result feeds a coefficient, in float
// where it runs per sample. Accuracy is stated per function and tested against the
// library in tests/cpp/dsp/test_math.cpp.
#pragma once

#include <cmath>
#include <cstdint>

namespace adx::dsp {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 6.28318530717958647692;
inline constexpr double kLn2 = 0.69314718055994530942;
inline constexpr double kLog2E = 1.44269504088896340736;
inline constexpr double kLog2Of10 = 3.32192809488736234787;

// --- trigonometry, in turns (1 turn = 2 pi) ---------------------------------

/// sin(2 pi turns). Error below 1e-13. For coefficients.
[[nodiscard]] inline double sinTurns(double turns) noexcept {
    double x = turns - std::floor(turns);
    double sign = 1.0;
    if (x >= 0.5) {
        x -= 0.5;
        sign = -1.0;
    }
    if (x > 0.25) {
        x = 0.5 - x;
    }
    const double z = x * kTwoPi;
    const double z2 = z * z;
    // Taylor to z^19 on [0, pi/2].
    double poly = 1.0 / 121645100408832000.0;
    poly = (poly * -z2) + (1.0 / 355687428096000.0);
    poly = (poly * -z2) + (1.0 / 1307674368000.0);
    poly = (poly * -z2) + (1.0 / 6227020800.0);
    poly = (poly * -z2) + (1.0 / 39916800.0);
    poly = (poly * -z2) + (1.0 / 362880.0);
    poly = (poly * -z2) + (1.0 / 5040.0);
    poly = (poly * -z2) + (1.0 / 120.0);
    poly = (poly * -z2) + (1.0 / 6.0);
    poly = (poly * -z2) + 1.0;
    return sign * z * poly;
}

[[nodiscard]] inline double cosTurns(double turns) noexcept {
    return sinTurns(turns + 0.25);
}

/// tan(2 pi turns), for turns in (-0.25, 0.25). What a bilinear-transform prewarp
/// needs: tan(pi f / fs) is tanTurns(0.5 f / fs).
[[nodiscard]] inline double tanTurns(double turns) noexcept {
    return sinTurns(turns) / cosTurns(turns);
}

/// sin(2 pi turns) in float, for per-sample oscillators. Quarter-wave reduction and an
/// odd polynomial to z^11: error below 1e-6, under the 24-bit noise floor.
[[nodiscard]] inline float sinTurnsF(float turns) noexcept {
    float x = turns - std::floor(turns);
    float sign = 1.0F;
    if (x >= 0.5F) {
        x -= 0.5F;
        sign = -1.0F;
    }
    if (x > 0.25F) {
        x = 0.5F - x;
    }
    const float z = x * 6.28318530717958647692F;
    const float z2 = z * z;
    const float poly =
        1.0F + (z2 * (-1.0F / 6.0F +
                      (z2 * (1.0F / 120.0F +
                             (z2 * (-1.0F / 5040.0F +
                                    (z2 * (1.0F / 362880.0F + (z2 * (-1.0F / 39916800.0F))))))))));
    return sign * z * poly;
}

[[nodiscard]] inline float cosTurnsF(float turns) noexcept {
    return sinTurnsF(turns + 0.25F);
}

// --- exponentials and logarithms -------------------------------------------

/// 2^x. Relative error below 1e-15 for x in [-1000, 1000]; saturates outside.
[[nodiscard]] inline double exp2(double x) noexcept {
    if (x > 1000.0) {
        x = 1000.0;
    } else if (x < -1000.0) {
        return 0.0;
    }
    const double whole = std::floor(x + 0.5);
    const double y = (x - whole) * kLn2; // |y| <= ln2 / 2
    // e^y, Taylor to y^12: the first omitted term is below 3e-17.
    double poly = 1.0 / 479001600.0;
    poly = (poly * y) + (1.0 / 39916800.0);
    poly = (poly * y) + (1.0 / 3628800.0);
    poly = (poly * y) + (1.0 / 362880.0);
    poly = (poly * y) + (1.0 / 40320.0);
    poly = (poly * y) + (1.0 / 5040.0);
    poly = (poly * y) + (1.0 / 720.0);
    poly = (poly * y) + (1.0 / 120.0);
    poly = (poly * y) + (1.0 / 24.0);
    poly = (poly * y) + (1.0 / 6.0);
    poly = (poly * y) + 0.5;
    poly = (poly * y) + 1.0;
    poly = (poly * y) + 1.0;
    return std::ldexp(poly, static_cast<int>(whole));
}

/// log2(x) for x > 0. Absolute error below 1e-15. Returns -1100 for x <= 0, which
/// exp2 maps back to 0.
[[nodiscard]] inline double log2(double x) noexcept {
    if (!(x > 0.0)) {
        return -1100.0;
    }
    int exponent = 0;
    double mantissa = std::frexp(x, &exponent); // [0.5, 1)
    if (mantissa < 0.70710678118654752440) {
        mantissa *= 2.0;
        --exponent;
    }
    // log(m) = 2 atanh(s), s = (m - 1) / (m + 1), |s| <= 0.1716.
    const double s = (mantissa - 1.0) / (mantissa + 1.0);
    const double s2 = s * s;
    double poly = 1.0 / 19.0;
    poly = (poly * s2) + (1.0 / 17.0);
    poly = (poly * s2) + (1.0 / 15.0);
    poly = (poly * s2) + (1.0 / 13.0);
    poly = (poly * s2) + (1.0 / 11.0);
    poly = (poly * s2) + (1.0 / 9.0);
    poly = (poly * s2) + (1.0 / 7.0);
    poly = (poly * s2) + (1.0 / 5.0);
    poly = (poly * s2) + (1.0 / 3.0);
    poly = (poly * s2) + 1.0;
    return static_cast<double>(exponent) + (2.0 * s * poly * kLog2E);
}

[[nodiscard]] inline double exp(double x) noexcept {
    return exp2(x * kLog2E);
}

[[nodiscard]] inline double log(double x) noexcept {
    return log2(x) * kLn2;
}

[[nodiscard]] inline double log10(double x) noexcept {
    return log2(x) / kLog2Of10;
}

/// base^power for base > 0; 0 for base <= 0.
[[nodiscard]] inline double pow(double base, double power) noexcept {
    return base > 0.0 ? exp2(power * log2(base)) : 0.0;
}

/// tanh(x). Absolute error below 1e-15.
[[nodiscard]] inline double tanh(double x) noexcept {
    const double magnitude = x < 0.0 ? -x : x;
    if (magnitude > 20.0) {
        return x < 0.0 ? -1.0 : 1.0;
    }
    const double t = exp2(-2.0 * magnitude * kLog2E);
    const double value = (1.0 - t) / (1.0 + t);
    return x < 0.0 ? -value : value;
}

/// atan2(y, x) in turns, in (-0.5, 0.5]: the angle of (x, y) over 2 pi. What a phase
/// vocoder reads a bin's phase with. Octant reduction to |u| <= tan(pi / 8), then the
/// odd series to u^25: error below 1e-10 turns. 0 at the origin.
[[nodiscard]] inline double atan2Turns(double y, double x) noexcept {
    const double ax = x < 0.0 ? -x : x;
    const double ay = y < 0.0 ? -y : y;
    if (ax == 0.0 && ay == 0.0) {
        return 0.0;
    }
    // The angle of (ax, ay) in [0, pi/2], from atan of the smaller over the larger.
    const bool swap = ay > ax;
    double z = swap ? ax / ay : ay / ax; // 0 .. 1
    double base = 0.0;
    if (z > 0.41421356237309503) {
        // atan(z) = pi/4 + atan((z - 1) / (z + 1))
        z = (z - 1.0) / (z + 1.0);
        base = kPi / 4.0;
    }
    const double z2 = z * z;
    double series = 0.0;
    for (int k = 12; k >= 0; --k) {
        series = (series * z2) + ((k % 2 == 0 ? 1.0 : -1.0) / static_cast<double>((2 * k) + 1));
    }
    double angle = base + (z * series);
    if (swap) {
        angle = (kPi / 2.0) - angle;
    }
    if (x < 0.0) {
        angle = kPi - angle;
    }
    if (y < 0.0) {
        angle = -angle;
    }
    return angle / kTwoPi;
}

// --- float, per sample ------------------------------------------------------

/// 2^x in float. Relative error below 2e-7 (float rounding) for x in [-126, 126].
[[nodiscard]] inline float exp2F(float x) noexcept {
    if (x > 126.0F) {
        x = 126.0F;
    } else if (x < -126.0F) {
        return 0.0F;
    }
    const float whole = std::floor(x + 0.5F);
    const float y = (x - whole) * 0.69314718055994530942F;
    // e^y, Taylor to y^6 on |y| <= 0.347: first omitted term below 4e-8.
    float poly = 1.0F / 720.0F;
    poly = (poly * y) + (1.0F / 120.0F);
    poly = (poly * y) + (1.0F / 24.0F);
    poly = (poly * y) + (1.0F / 6.0F);
    poly = (poly * y) + 0.5F;
    poly = (poly * y) + 1.0F;
    poly = (poly * y) + 1.0F;
    return std::ldexp(poly, static_cast<int>(whole));
}

/// tanh(x) in float. Absolute error below 3e-7.
[[nodiscard]] inline float tanhF(float x) noexcept {
    const float magnitude = x < 0.0F ? -x : x;
    if (magnitude > 9.0F) {
        return x < 0.0F ? -1.0F : 1.0F;
    }
    const float t = exp2F(-2.0F * magnitude * 1.44269504088896340736F);
    const float value = (1.0F - t) / (1.0F + t);
    return x < 0.0F ? -value : value;
}

/// The frequency ratio of `cents`: 2^(cents / 1200).
[[nodiscard]] inline float centsToRatio(float cents) noexcept {
    return exp2F(cents * (1.0F / 1200.0F));
}

/// Equal-tempered frequency of MIDI note `pitch` plus `cents`, A4 = `tuning`.
[[nodiscard]] inline float midiToHz(float pitch, float tuning = 440.0F) noexcept {
    return tuning * exp2F((pitch - 69.0F) * (1.0F / 12.0F));
}

// --- decibels ----------------------------------------------------------------

[[nodiscard]] inline double dbToGain(double db) noexcept {
    return exp2(db * (kLog2Of10 / 20.0));
}

/// 20 log10(gain); `floor` for gain <= 0 or below it.
[[nodiscard]] inline double gainToDb(double gain, double floor = -200.0) noexcept {
    if (!(gain > 0.0)) {
        return floor;
    }
    const double db = 20.0 * log10(gain);
    return db < floor ? floor : db;
}

[[nodiscard]] inline float dbToGainF(float db) noexcept {
    return exp2F(db * (3.32192809488736234787F / 20.0F));
}

} // namespace adx::dsp
