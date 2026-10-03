#include "engine/dsp/Convolve.h"

#include <algorithm>

namespace adx::dsp {

void Convolver::prepare(std::span<const float> impulse, std::size_t blockSize) {
    if (blockSize < 2 || (blockSize & (blockSize - 1)) != 0 || impulse.empty()) {
        m_block = 0;
        m_partitions = 0;
        return;
    }
    m_block = blockSize;
    const std::size_t size = 2 * blockSize;
    m_fft.prepare(size);
    m_partitions = (impulse.size() + blockSize - 1) / blockSize;

    m_spectra.allocate(m_partitions * size);
    for (std::size_t p = 0; p < m_partitions; ++p) {
        const std::span<Complex> spectrum = m_spectra.view().subspan(p * size, size);
        for (std::size_t i = 0; i < size; ++i) {
            const std::size_t at = (p * blockSize) + i;
            spectrum[i] = Complex{i < blockSize && at < impulse.size() ? impulse[at] : 0.0F, 0.0F};
        }
        m_fft.forward(spectrum);
    }
    m_history.allocate(m_partitions * size);
    m_input.allocate(size);
    m_work.allocate(size);
    m_accumulator.allocate(size);
    m_output.allocate(blockSize);
    reset();
}

void Convolver::reset() noexcept {
    std::ranges::fill(m_history.view(), Complex{});
    std::ranges::fill(m_input.view(), 0.0F);
    std::ranges::fill(m_output.view(), 0.0F);
    m_historyHead = 0;
    m_fill = 0;
}

void Convolver::runBlock() noexcept {
    const std::size_t size = 2 * m_block;
    const std::span<float> input = m_input.view();
    const std::span<Complex> work = m_work.view();
    for (std::size_t i = 0; i < size; ++i) {
        work[i] = Complex{input[i], 0.0F};
    }
    m_fft.forward(work);

    // The newest block's spectrum goes in at the head; partition p multiplies the block
    // from p blocks ago.
    m_historyHead = (m_historyHead + m_partitions - 1) % m_partitions;
    std::ranges::copy(work, m_history.view().subspan(m_historyHead * size, size).begin());

    const std::span<Complex> sum = m_accumulator.view();
    std::ranges::fill(sum, Complex{});
    for (std::size_t p = 0; p < m_partitions; ++p) {
        const std::size_t slot = (m_historyHead + p) % m_partitions;
        const std::span<const Complex> x = m_history.view().subspan(slot * size, size);
        const std::span<const Complex> h = m_spectra.view().subspan(p * size, size);
        for (std::size_t k = 0; k < size; ++k) {
            const float re = (x[k].real() * h[k].real()) - (x[k].imag() * h[k].imag());
            const float im = (x[k].real() * h[k].imag()) + (x[k].imag() * h[k].real());
            sum[k] = Complex{sum[k].real() + re, sum[k].imag() + im};
        }
    }
    m_fft.inverse(sum);
    // Overlap-save: the first B outputs are the circular wrap; the last B are valid.
    const std::span<float> output = m_output.view();
    for (std::size_t i = 0; i < m_block; ++i) {
        output[i] = sum[m_block + i].real();
    }
    // Slide the input window: the current block becomes the previous one.
    std::copy(input.begin() + static_cast<std::ptrdiff_t>(m_block), input.end(), input.begin());
}

void Convolver::process(std::span<const float> in, std::span<float> out) noexcept {
    if (m_partitions == 0) {
        std::ranges::fill(out, 0.0F);
        return;
    }
    const std::span<float> input = m_input.view();
    const std::span<float> output = m_output.view();
    for (std::size_t i = 0; i < in.size() && i < out.size(); ++i) {
        // Out first, from the block computed when the FIFO last filled: that is the
        // B samples of latency.
        out[i] = output[m_fill];
        input[m_block + m_fill] = in[i];
        ++m_fill;
        if (m_fill == m_block) {
            runBlock();
            m_fill = 0;
        }
    }
}

} // namespace adx::dsp
