#include "engine/dsp/Fft.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/dsp/Math.h"
#include "engine/dsp/Window.h"

namespace adx::dsp {

void Fft::prepare(std::size_t size) {
    if (size < 2 || (size & (size - 1)) != 0) {
        m_size = 0;
        return;
    }
    m_size = size;
    m_twiddles.allocate(size / 2);
    const std::span<Complex> twiddles = m_twiddles.view();
    for (std::size_t k = 0; k < size / 2; ++k) {
        const double turns = -static_cast<double>(k) / static_cast<double>(size);
        twiddles[k] =
            Complex{static_cast<float>(cosTurns(turns)), static_cast<float>(sinTurns(turns))};
    }
    m_reversed.allocate(size);
    const std::span<std::uint32_t> reversed = m_reversed.view();
    std::size_t bits = 0;
    while ((std::size_t{1} << bits) < size) {
        ++bits;
    }
    for (std::size_t i = 0; i < size; ++i) {
        std::size_t r = 0;
        for (std::size_t b = 0; b < bits; ++b) {
            r |= ((i >> b) & 1U) << (bits - 1 - b);
        }
        reversed[i] = static_cast<std::uint32_t>(r);
    }
}

void Fft::permute(std::span<Complex> data) const noexcept {
    const std::span<const std::uint32_t> reversed = m_reversed.view();
    for (std::size_t i = 0; i < m_size; ++i) {
        const std::size_t j = reversed[i];
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }
}

void Fft::butterflies(std::span<Complex> data, bool inverse) const noexcept {
    const std::span<const Complex> twiddles = m_twiddles.view();
    for (std::size_t length = 2; length <= m_size; length <<= 1U) {
        const std::size_t half = length / 2;
        const std::size_t stride = m_size / length;
        for (std::size_t start = 0; start < m_size; start += length) {
            for (std::size_t k = 0; k < half; ++k) {
                const Complex w = twiddles[k * stride];
                const float wr = w.real();
                const float wi = inverse ? -w.imag() : w.imag();
                Complex& a = data[start + k];
                Complex& b = data[start + k + half];
                // v = b * w, written out so no library complex multiply - with its
                // NaN-recovery branches - is in the loop.
                const float vr = (b.real() * wr) - (b.imag() * wi);
                const float vi = (b.real() * wi) + (b.imag() * wr);
                const float ur = a.real();
                const float ui = a.imag();
                a = Complex{ur + vr, ui + vi};
                b = Complex{ur - vr, ui - vi};
            }
        }
    }
}

void Fft::forward(std::span<Complex> data) const noexcept {
    if (m_size == 0 || data.size() != m_size) {
        return;
    }
    permute(data);
    butterflies(data, false);
}

void Fft::inverse(std::span<Complex> data) const noexcept {
    if (m_size == 0 || data.size() != m_size) {
        return;
    }
    permute(data);
    butterflies(data, true);
    const float scale = 1.0F / static_cast<float>(m_size);
    for (Complex& value : data) {
        value *= scale;
    }
}

void logMagnitudeSpectrum(const Fft& fft, std::span<const float> samples,
                          std::span<Complex> scratch, std::span<float> out) noexcept {
    const std::size_t n = fft.size();
    if (n == 0 || samples.size() < n || scratch.size() < n || out.size() < n / 2) {
        return;
    }
    for (std::size_t i = 0; i < n; ++i) {
        // SimpleFFT's symmetric Hann, kept so the analyzers read the same as before.
        const auto w = static_cast<float>(windowValue(WindowKind::Hann, i, n, false));
        scratch[i] = Complex{samples[i] * w, 0.0F};
    }
    fft.forward(scratch.first(n));
    for (std::size_t i = 0; i < n / 2; ++i) {
        const float re = scratch[i].real();
        const float im = scratch[i].imag();
        const float magnitude = std::sqrt((re * re) + (im * im)) / static_cast<float>(n);
        const auto db = static_cast<float>(gainToDb(std::max(magnitude, 1e-6F)));
        out[i] = std::clamp((db + 100.0F) / 100.0F, 0.0F, 1.0F);
    }
}

} // namespace adx::dsp
