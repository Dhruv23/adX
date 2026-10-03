// engine/dsp/Noise.h.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "engine/dsp/Noise.h"
#include "tests/cpp/dsp/DspTestUtil.h"

namespace dsp = adx::dsp;

TEST_CASE("dsp_white_noise_statistics", "[dsp][noise]") {
    dsp::WhiteNoise noise{1};
    double sum = 0.0;
    double squares = 0.0;
    float low = 1.0F;
    float high = -1.0F;
    constexpr int kCount = 1 << 20;
    for (int i = 0; i < kCount; ++i) {
        const float x = noise.next();
        sum += x;
        squares += static_cast<double>(x) * x;
        low = std::min(low, x);
        high = std::max(high, x);
    }
    CHECK(std::abs(sum / kCount) < 0.005);
    CHECK(std::abs((squares / kCount) - (1.0 / 3.0)) < 0.005); // uniform on [-1, 1)
    CHECK(low >= -1.0F);
    CHECK(high < 1.0F);

    // Seeded: the same seed is the same stream; a zero seed is not stuck at zero.
    dsp::WhiteNoise a{77};
    dsp::WhiteNoise b{77};
    dsp::WhiteNoise zero{0};
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(a.next() == b.next());
    }
    CHECK(zero.nextBits() != 0U);
}

TEST_CASE("dsp_pink_noise_slope", "[dsp][noise]") {
    // -3 dB per octave: fit the power in octave bands from 100 Hz to 6.4 kHz.
    dsp::PinkNoise pink{5};
    constexpr std::size_t kLength = 1U << 16U;
    std::vector<double> bands(7, 0.0);
    for (int trial = 0; trial < 8; ++trial) {
        std::vector<float> samples(kLength);
        for (float& x : samples) {
            x = pink.next();
        }
        const std::vector<double> power = adx::tests::powerSpectrum(samples);
        const double binHz = 48000.0 / kLength;
        for (std::size_t b = 0; b < bands.size(); ++b) {
            const double lo = 100.0 * std::pow(2.0, static_cast<double>(b));
            for (std::size_t k = 1; k < power.size(); ++k) {
                const double f = static_cast<double>(k) * binHz;
                if (f >= lo && f < lo * 2.0) {
                    bands[b] += power[k];
                }
            }
        }
    }
    // Per-octave power falls 3 dB per octave in density, so the octave's total is flat
    // for pink noise. Within 1.5 dB of flat across six octaves.
    const double first = 10.0 * std::log10(bands.front());
    for (const double band : bands) {
        CHECK(std::abs((10.0 * std::log10(band)) - first) < 1.5);
    }
}
