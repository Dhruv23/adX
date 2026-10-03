// engine/dsp/Convolve.h.
//
// phase_4.md §6: "FFT convolution matches naive time-domain convolution within 1e-5
// for a 1 s IR."
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "engine/dsp/Convolve.h"
#include "engine/dsp/Noise.h"

namespace dsp = adx::dsp;

TEST_CASE("convolution_matches_direct", "[dsp][convolve]") {
    // A one-second IR shaped like a reverb tail - noise under an exponential decay,
    // normalised so its energy is one - and two seconds of noise through it.
    constexpr std::size_t kIr = 48000;
    constexpr std::size_t kSignal = 96000;
    dsp::WhiteNoise noise{2024};
    std::vector<float> ir(kIr);
    double energy = 0.0;
    for (std::size_t i = 0; i < kIr; ++i) {
        ir[i] = noise.next() * static_cast<float>(std::exp(-6.9 * static_cast<double>(i) / kIr));
        energy += static_cast<double>(ir[i]) * ir[i];
    }
    for (float& tap : ir) {
        tap = static_cast<float>(tap / std::sqrt(energy));
    }
    std::vector<float> signal(kSignal);
    for (float& x : signal) {
        x = noise.next() * 0.5F;
    }

    for (const std::size_t block : {64U, 512U}) {
        INFO("partition " << block);
        dsp::Convolver convolver;
        convolver.prepare(ir, block);
        REQUIRE(convolver.latency() == block);
        std::vector<float> out(kSignal);
        // Fed in uneven pieces, so the FIFO is exercised across partition boundaries.
        std::size_t at = 0;
        std::size_t piece = 37;
        while (at < kSignal) {
            const std::size_t n = std::min(piece, kSignal - at);
            convolver.process(std::span<const float>{signal}.subspan(at, n),
                              std::span<float>{out}.subspan(at, n));
            at += n;
            piece = (piece * 7) % 301 + 1;
        }
        // Direct, in double, at a sample of output positions (all 96k would be 4.6e9
        // multiplies): the output at n is the direct result at n - latency.
        float worst = 0.0F;
        for (std::size_t n = block; n < kSignal; n += 997) {
            const std::size_t m = n - block;
            double sum = 0.0;
            for (std::size_t k = 0; k < kIr && k <= m; ++k) {
                sum += static_cast<double>(ir[k]) * signal[m - k];
            }
            worst = std::max(worst, std::abs(out[n] - static_cast<float>(sum)));
        }
        CHECK(worst < 1e-5F);
        // Before the latency has passed there is nothing.
        for (std::size_t n = 0; n < block; ++n) {
            REQUIRE(out[n] == 0.0F);
        }
    }
}
