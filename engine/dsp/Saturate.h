// Waveshapers: tanh, soft clip, asymmetric tube, hard clip.
//
// Each is a pure function of one sample - stateless, so a saturator can be applied at
// any rate and inside any oversampler - and branch-light. "Normalised" versions divide
// by the shaper's value at the drive, so turning drive up changes the shape and not
// the level: iteration one's Distortion did this, and it is what makes drive a timbre
// control rather than a volume knob.
#pragma once

#include <cstdint>

#include "engine/dsp/Math.h"

namespace adx::dsp {

enum class ShapeKind : std::uint8_t { Tanh, Soft, Tube, Hard };

[[nodiscard]] inline float shapeTanh(float x) noexcept {
    return tanhF(x);
}

/// Cubic soft clip: x - x^3/3 inside |x| < 1, ±2/3 beyond. Continuous slope at the
/// knee, so no hard corner, and cheaper than tanh.
[[nodiscard]] inline float shapeSoft(float x) noexcept {
    const float c = x < -1.0F ? -1.0F : (x > 1.0F ? 1.0F : x);
    return c - ((c * c * c) * (1.0F / 3.0F));
}

/// Asymmetric "tube": a bias shifts the operating point, so positive and negative
/// halves clip differently and even harmonics appear. The bias is subtracted back out
/// at the shaper's own output for zero input, so silence stays silence.
[[nodiscard]] inline float shapeTube(float x) noexcept {
    constexpr float kBias = 0.25F;
    constexpr float kRest = 0.24491866240370913F; // tanh(0.25)
    return tanhF(x + kBias) - kRest;
}

[[nodiscard]] inline float shapeHard(float x) noexcept {
    return x < -1.0F ? -1.0F : (x > 1.0F ? 1.0F : x);
}

[[nodiscard]] inline float shape(ShapeKind kind, float x) noexcept {
    switch (kind) {
    case ShapeKind::Tanh:
        return shapeTanh(x);
    case ShapeKind::Soft:
        return shapeSoft(x);
    case ShapeKind::Tube:
        return shapeTube(x);
    case ShapeKind::Hard:
        return shapeHard(x);
    }
    return x;
}

/// tanh(x * drive) / tanh(drive): iteration one's drive stage, exactly. `drive` >= 1.
[[nodiscard]] inline float driveTanh(float x, float drive, float inverseNorm) noexcept {
    return tanhF(x * drive) * inverseNorm;
}

/// 1 / tanh(drive), for driveTanh. Computed once per control block, not per sample.
[[nodiscard]] inline float driveNorm(float drive) noexcept {
    const float t = tanhF(drive);
    return t > 0.0F ? 1.0F / t : 1.0F;
}

} // namespace adx::dsp
