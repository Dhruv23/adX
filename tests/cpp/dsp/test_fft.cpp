// engine/dsp/Fft.h: the ported SimpleFFT.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

#include "engine/dsp/Fft.h"
#include "engine/dsp/Noise.h"

namespace dsp = adx::dsp;

TEST_CASE("dsp_fft_roundtrip", "[dsp][fft]") {
    // ifft(fft(x)) == x within 1e-6, at every size the engine uses.
    dsp::WhiteNoise noise{99};
    for (std::size_t size = 2; size <= 8192; size *= 2) {
        dsp::Fft fft;
        fft.prepare(size);
        std::vector<dsp::Complex> data(size);
        std::vector<dsp::Complex> original(size);
        for (std::size_t i = 0; i < size; ++i) {
            original[i] = data[i] = dsp::Complex{noise.next(), noise.next()};
        }
        fft.forward(data);
        fft.inverse(data);
        float worst = 0.0F;
        for (std::size_t i = 0; i < size; ++i) {
            worst = std::max(worst, std::abs(data[i] - original[i]));
        }
        INFO("size " << size);
        CHECK(worst < 1e-6F);
    }
}

TEST_CASE("dsp_fft_matches_the_dft", "[dsp][fft]") {
    // Against a direct O(n^2) DFT in double, on a signal with energy everywhere.
    constexpr std::size_t kSize = 256;
    dsp::WhiteNoise noise{7};
    std::vector<dsp::Complex> data(kSize);
    std::vector<std::complex<double>> exact(kSize);
    for (dsp::Complex& value : data) {
        value = dsp::Complex{noise.next(), 0.0F};
    }
    for (std::size_t k = 0; k < kSize; ++k) {
        std::complex<double> sum;
        for (std::size_t n = 0; n < kSize; ++n) {
            const double angle = -2.0 * std::numbers::pi * static_cast<double>(k * n) / kSize;
            sum += static_cast<double>(data[n].real()) *
                   std::complex<double>{std::cos(angle), std::sin(angle)};
        }
        exact[k] = sum;
    }
    dsp::Fft fft;
    fft.prepare(kSize);
    fft.forward(data);
    double worst = 0.0;
    for (std::size_t k = 0; k < kSize; ++k) {
        worst = std::max(worst,
                         std::abs(std::complex<double>(data[k].real(), data[k].imag()) - exact[k]));
    }
    CHECK(worst < 1e-4);

    // A pure tone at bin 9 is all at bin 9 (and its mirror).
    std::vector<dsp::Complex> tone(kSize);
    for (std::size_t n = 0; n < kSize; ++n) {
        tone[n] = dsp::Complex{static_cast<float>(std::cos(2.0 * std::numbers::pi * 9.0 *
                                                           static_cast<double>(n) / kSize)),
                               0.0F};
    }
    fft.forward(tone);
    CHECK(std::abs(std::abs(tone[9]) - (kSize / 2.0F)) < 1e-3F);
    CHECK(std::abs(tone[10]) < 1e-3F);
}

TEST_CASE("dsp_fft_log_magnitude_spectrum", "[dsp][fft]") {
    // SimpleFFT's analyzer path, mapping preserved: a full-scale sine under its Hann
    // window reads |X| / N = 1/4, i.e. -12 dB, which is 0.88 on its -100..0 dB scale.
    constexpr std::size_t kSize = 1024;
    dsp::Fft fft;
    fft.prepare(kSize);
    std::vector<float> samples(kSize);
    for (std::size_t n = 0; n < kSize; ++n) {
        samples[n] = static_cast<float>(
            std::sin(2.0 * std::numbers::pi * 64.0 * static_cast<double>(n) / kSize));
    }
    std::vector<dsp::Complex> scratch(kSize);
    std::vector<float> out(kSize / 2);
    dsp::logMagnitudeSpectrum(fft, samples, scratch, out);
    CHECK(std::abs(out[64] - 0.8796F) < 0.002F);
    CHECK(out[300] < 0.2F);
}
