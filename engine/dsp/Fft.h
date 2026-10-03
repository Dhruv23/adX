// Radix-2 FFT, ported from _archive/src-cpp/src/SimpleFFT.cpp.
//
// The algorithm is SimpleFFT's - bit-reversal permutation, then iterative
// Cooley-Tukey butterflies - and FINAL_PLAN §8 keeps it: sufficient for UI-rate
// analysis and for the convolution and spectral effects here, to be swapped for pffft
// only if profiling asks. Three things changed in the port, all so it can run on the
// audio thread:
//
//   - twiddles are a table built in prepare(), not a running product w *= wlen,
//     whose rounding error grew along every butterfly row;
//   - the table comes from dsp::cosTurns, so the bits are the same in every build;
//   - it transforms a caller's span in place and never allocates.
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>

#include "engine/rt/OwnedArray.h"

namespace adx::dsp {

using Complex = std::complex<float>;

class Fft {
public:
    Fft() = default;

    /// Main thread. `size` must be a power of two, at least 2.
    void prepare(std::size_t size);

    [[nodiscard]] std::size_t size() const noexcept {
        return m_size;
    }

    /// In place, unscaled: X[k] = sum x[n] e^(-2 pi i k n / N). `data.size()` must be
    /// size(); a span of any other size is left untouched.
    void forward(std::span<Complex> data) const noexcept;

    /// In place, scaled by 1/N, so inverse(forward(x)) == x.
    void inverse(std::span<Complex> data) const noexcept;

private:
    void permute(std::span<Complex> data) const noexcept;
    void butterflies(std::span<Complex> data, bool inverse) const noexcept;

    std::size_t m_size{0};
    /// e^(-2 pi i k / N) for k in [0, N/2).
    rt::OwnedArray<Complex> m_twiddles;
    rt::OwnedArray<std::uint32_t> m_reversed;
};

/// SimpleFFT's ComputeLogMagnitudeSpectrum: Hann-windowed, the lower half of the bins
/// mapped from -100..0 dB to 0..1. `scratch` must hold fft.size() values and `out`
/// fft.size() / 2. `samples` holds fft.size() values.
void logMagnitudeSpectrum(const Fft& fft, std::span<const float> samples,
                          std::span<Complex> scratch, std::span<float> out) noexcept;

} // namespace adx::dsp
