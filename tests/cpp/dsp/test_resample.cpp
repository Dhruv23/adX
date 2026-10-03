// engine/dsp/Resample.h.
//
// phase_4.md §6: "THD+N < -80 dB for a 1 kHz sine resampled 44.1 <-> 48 <-> 96 kHz."
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <vector>

#include "engine/dsp/Resample.h"
#include "tests/cpp/dsp/DspTestUtil.h"

namespace dsp = adx::dsp;

namespace {

/// THD+N in dB: everything except the fundamental's main lobe, over the fundamental,
/// measured on the middle of the output so the edges' zero padding is not counted.
double thdN(const std::vector<float>& signal, double frequency, double rate) {
    constexpr std::size_t kWindow = 1U << 15U;
    const std::size_t start = (signal.size() - kWindow) / 2;
    const std::vector<float> middle(signal.begin() + static_cast<std::ptrdiff_t>(start),
                                    signal.begin() + static_cast<std::ptrdiff_t>(start + kWindow));
    const std::vector<double> power = adx::tests::powerSpectrum(middle);
    const double binHz = rate / static_cast<double>(kWindow);
    const auto centre = static_cast<std::ptrdiff_t>(std::llround(frequency / binHz));
    double fundamental = 0.0;
    double rest = 0.0;
    for (std::size_t k = 1; k < power.size(); ++k) {
        // Only the audio band: 20 Hz - 20 kHz.
        const double f = static_cast<double>(k) * binHz;
        if (f < 20.0 || f > 20000.0) {
            continue;
        }
        if (std::abs(static_cast<std::ptrdiff_t>(k) - centre) <= 8) {
            fundamental += power[k];
        } else {
            rest += power[k];
        }
    }
    return 10.0 * std::log10(rest / fundamental);
}

} // namespace

TEST_CASE("dsp_resample_thd", "[dsp][resample]") {
    const std::vector<std::pair<double, double>> pairs{{44100.0, 48000.0}, {48000.0, 44100.0},
                                                       {48000.0, 96000.0}, {96000.0, 48000.0},
                                                       {44100.0, 96000.0}, {96000.0, 44100.0}};
    for (const auto& [from, to] : pairs) {
        INFO(from << " -> " << to);
        std::vector<float> input(static_cast<std::size_t>(from)); // one second
        for (std::size_t n = 0; n < input.size(); ++n) {
            input[n] = 0.9F * static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 *
                                                          static_cast<double>(n) / from));
        }
        dsp::Resampler resampler;
        resampler.prepare(from, to);
        std::vector<float> output(resampler.outputLength(input.size()));
        resampler.process(input, output);
        CHECK(output.size() == static_cast<std::size_t>(to));
        CHECK(thdN(output, 1000.0, to) < -80.0);
        // And the level survives.
        CHECK(std::abs(adx::tests::toneAmplitude(
                           std::vector<float>(output.begin() + 8192, output.begin() + 8192 + 16384),
                           1000.0, to) -
                       0.9) < 0.01);
    }
}
