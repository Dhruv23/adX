// Uniformly partitioned FFT convolution (overlap-save).
//
// For reverb impulse responses (phase_4.md §4.9). The IR is cut into partitions of
// the block size B, each transformed once in prepare(); every B input samples cost one
// forward FFT, one inverse FFT and one complex multiply-accumulate per partition -
// the cost grows with the IR's length only linearly, where direct convolution's grows
// with it per sample.
//
// Latency is exactly B samples, and latencySamples() says so; PDC does the rest
// (phase_3.md §4.6). convolution_matches_direct holds the output to a direct
// time-domain convolution of the same IR, delayed by B.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "engine/dsp/Fft.h"
#include "engine/rt/OwnedArray.h"

namespace adx::dsp {

class Convolver {
public:
    /// Main thread. `blockSize` must be a power of two.
    void prepare(std::span<const float> impulse, std::size_t blockSize);

    /// Clears the input history and the pending output: a fresh tail.
    void reset() noexcept;

    /// Any number of samples; `out` receives the convolution delayed by latency().
    void process(std::span<const float> in, std::span<float> out) noexcept;

    [[nodiscard]] std::uint32_t latency() const noexcept {
        return static_cast<std::uint32_t>(m_block);
    }
    [[nodiscard]] bool ready() const noexcept {
        return m_partitions > 0;
    }

private:
    void runBlock() noexcept;

    std::size_t m_block{0};
    std::size_t m_partitions{0};
    Fft m_fft;
    /// The IR's partitions, transformed: m_partitions x 2B bins.
    rt::OwnedArray<Complex> m_spectra;
    /// The input's last m_partitions transformed blocks, a ring.
    rt::OwnedArray<Complex> m_history;
    std::size_t m_historyHead{0};
    /// The previous and current input block, time domain, 2B.
    rt::OwnedArray<float> m_input;
    rt::OwnedArray<Complex> m_work;
    rt::OwnedArray<Complex> m_accumulator;
    /// The output block being played out, B.
    rt::OwnedArray<float> m_output;
    std::size_t m_fill{0};
};

} // namespace adx::dsp
