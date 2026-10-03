// Fractional reads: linear, cubic Hermite, 8-point windowed sinc.
//
// Each reads `data` around position `index + fraction`; the caller guarantees the
// taps are in range (a sample buffer keeps guard frames for exactly this). The sinc
// table is built once, on the main thread, by prepareSincTable().
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace adx::dsp {

[[nodiscard]] inline float interpolateLinear(const float* data, std::size_t index,
                                             float fraction) noexcept {
    return data[index] + ((data[index + 1] - data[index]) * fraction);
}

/// 4-point, 3rd-order Hermite (Catmull-Rom). Reads data[index - 1 .. index + 2].
[[nodiscard]] inline float interpolateHermite(const float* data, std::size_t index,
                                              float fraction) noexcept {
    const float y0 = data[index - 1];
    const float y1 = data[index];
    const float y2 = data[index + 1];
    const float y3 = data[index + 2];
    const float c1 = 0.5F * (y2 - y0);
    const float c2 = y0 - (2.5F * y1) + (2.0F * y2) - (0.5F * y3);
    const float c3 = (0.5F * (y3 - y0)) + (1.5F * (y1 - y2));
    return ((((c3 * fraction) + c2) * fraction + c1) * fraction) + y1;
}

/// Taps of the windowed sinc, and fractional phases tabulated between samples.
inline constexpr std::size_t kSincTaps = 8;
inline constexpr std::size_t kSincPhases = 512;

/// [phase][tap], one extra phase so phase+1 interpolation never reads past the end.
using SincTable = std::array<std::array<float, kSincTaps>, kSincPhases + 1>;

/// The shared table, built on first use. Main thread: call it from prepare() before
/// any audio-thread read.
[[nodiscard]] const SincTable& sincTable() noexcept;

/// 8-point Kaiser-windowed sinc, cutoff at the source's Nyquist. Reads
/// data[index - 3 .. index + 4]. For pitch ratios above 1 the caller should use a
/// band-limited source (a sampler zone pitched up more than an octave reads a
/// decimated copy - Resample.h), because this kernel does not lower its cutoff.
[[nodiscard]] inline float interpolateSinc(const SincTable& table, const float* data,
                                           std::size_t index, float fraction) noexcept {
    const float position = fraction * static_cast<float>(kSincPhases);
    const auto phase = static_cast<std::size_t>(position);
    const float blend = position - static_cast<float>(phase);
    const std::array<float, kSincTaps>& a = table[phase];
    const std::array<float, kSincTaps>& b = table[phase + 1];
    float sum = 0.0F;
    const float* base = data + index - 3;
    for (std::size_t t = 0; t < kSincTaps; ++t) {
        const float tap = a[t] + ((b[t] - a[t]) * blend);
        sum += base[t] * tap;
    }
    return sum;
}

} // namespace adx::dsp
