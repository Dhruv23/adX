#pragma once

#include <complex>
#include <cstddef>
#include <vector>

// UI-Refactor Phase 3: a small self-contained radix-2 Cooley-Tukey FFT --
// no third-party dependency, since none exists anywhere in the repo
// (confirmed by grep before this was written). UI-Thread-only: SPECTRUM/
// WATERFALL windowing and transforming happens once per rendered frame,
// never per-sample, so this never runs anywhere near AudioEngine::process.
namespace SimpleFFT {

// In-place iterative radix-2 FFT. `data.size()` MUST be a power of two.
void Transform(std::vector<std::complex<float>>& data);

// Applies a Hann window to the first `count` samples and writes the
// windowed result into `out` (resized to `count`) as complex, ready for
// Transform(). `count` must be a power of two.
void ApplyHannWindow(const float* samples, size_t count, std::vector<std::complex<float>>& out);

// Windows + transforms `count` samples (power of two) and converts the
// lower (Nyquist) half of bins to a roughly [0, 1]-normalized log-magnitude
// (dB mapped from a -100..0 dB floor/ceiling), written into `outMagnitudes`
// (resized to count/2). Bin 0 is DC.
void ComputeLogMagnitudeSpectrum(const float* samples, size_t count, std::vector<float>& outMagnitudes);

} // namespace SimpleFFT
