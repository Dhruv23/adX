// adx-thread: main
//
// Sample-rate conversion by a polyphase windowed sinc.
//
// The quality path: decoding a 44.1 kHz file into a 48 kHz project, and the Voice
// render cache's one conversion at its boundary (phase_4.md §4.13). Main thread or a
// worker - it converts whole buffers - so it may be as long as quality wants: 64 taps
// of a Kaiser-windowed sinc (beta 10, about -100 dB stopband), cut off just below the
// lower of the two Nyquists, tabulated at 1024 phases and interpolated between them.
// dsp_resample_thd holds it to THD+N below -80 dB on a 1 kHz sine, 44.1 <-> 48 <-> 96.
//
// Per-voice pitch shifting on the audio thread is Interpolate.h's 8-point kernel; this
// is not that.
#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace adx::dsp {

class Resampler {
public:
    /// Builds the kernel for converting `inputRate` to `outputRate`.
    void prepare(double inputRate, double outputRate);

    /// How many output samples `inputLength` input samples become.
    [[nodiscard]] std::size_t outputLength(std::size_t inputLength) const noexcept;

    /// Converts one channel. `out.size()` should be outputLength(in.size()). Samples
    /// before the start and after the end of `in` are taken as silence.
    void process(std::span<const float> in, std::span<float> out) const noexcept;

    [[nodiscard]] double ratio() const noexcept {
        return m_ratio;
    }

private:
    static constexpr std::size_t kHalfTaps = 32;
    static constexpr std::size_t kPhases = 1024;

    double m_ratio{1.0};
    /// [phase][tap], kPhases + 1 rows of 2 * kHalfTaps.
    std::vector<float> m_kernel;
};

} // namespace adx::dsp
