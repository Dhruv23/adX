// Oscillators and the band-limited tables they read (Oscillator.h, WaveTable.h).
//
// phase_4.md §4.1: "assert harmonic amplitudes follow the ideal series and that
// aliasing (energy at non-harmonic bins) is below -60 dBc at fundamental = 40 Hz,
// 440 Hz, 4 kHz and 10 kHz. The 10 kHz case is the one that catches a bad polyBLEP."
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <vector>

#include "engine/dsp/Oscillator.h"
#include "engine/dsp/WaveTable.h"
#include "tests/cpp/dsp/DspTestUtil.h"

namespace dsp = adx::dsp;

namespace {

constexpr double kRate = 48000.0;
constexpr std::size_t kLength = 1U << 16U;

std::vector<float> render(dsp::OscShape shape, double frequency, bool bandLimited,
                          float pulseWidth = 0.5F) {
    dsp::Oscillator::prepareTables();
    dsp::Oscillator osc;
    const auto increment = static_cast<float>(frequency / kRate);
    std::vector<float> out(kLength);
    for (float& sample : out) {
        sample = bandLimited ? osc.nextBandLimited(shape, increment, pulseWidth)
                             : osc.nextPolyBlep(shape, increment, pulseWidth);
    }
    return out;
}

} // namespace

TEST_CASE("dsp_polyblep_aliasing", "[dsp][oscillator]") {
    // The band-limited oscillator: below -60 dBc at all four fundamentals, for every
    // shape that has harmonics to alias.
    for (const double f : {40.0, 440.0, 4000.0, 10000.0}) {
        for (const dsp::OscShape shape : {dsp::OscShape::Saw, dsp::OscShape::Square,
                                          dsp::OscShape::Pulse, dsp::OscShape::Triangle}) {
            INFO("band-limited, " << f << " Hz, shape " << static_cast<int>(shape));
            CHECK(adx::tests::aliasingDbc(render(shape, f, true, 0.3F), f, kRate) < -60.0);
        }
    }

    // Iteration one's 2-point polyBLEP, kept for the Additive port, measured rather
    // than assumed. Counting *all* aliased energy, as this test does, it meets the bar
    // only at the bottom of the range: about -33 dBc at 440 Hz and -21 dBc at 10 kHz.
    // That is the finding phase_4.md §11 records, and the reason the VA reads
    // band-limited tables. The bounds below pin today's measurement, so a change to the
    // polyBLEP in either direction is noticed.
    CHECK(adx::tests::aliasingDbc(render(dsp::OscShape::Saw, 40.0, false), 40.0, kRate) < -60.0);
    const double at440 =
        adx::tests::aliasingDbc(render(dsp::OscShape::Saw, 440.0, false), 440.0, kRate);
    const double at10k =
        adx::tests::aliasingDbc(render(dsp::OscShape::Saw, 10000.0, false), 10000.0, kRate);
    WARN("polyBLEP saw: " << at440 << " dBc at 440 Hz, " << at10k << " dBc at 10 kHz");
    CHECK(at440 < -30.0);
    CHECK(at10k < -18.0);
    CHECK(at10k > -60.0); // if this ever passes -60, the finding in §11 is out of date
}

TEST_CASE("dsp_oscillator_harmonic_series", "[dsp][oscillator]") {
    // A 375 Hz saw: harmonic k at 2/(pi k) of the -1..1 ramp's... amplitude 2/(pi k);
    // a square's odd harmonics at 4/(pi k), its even ones absent.
    constexpr double kFrequency = 375.0;
    const std::vector<float> saw = render(dsp::OscShape::Saw, kFrequency, true);
    const std::vector<float> square = render(dsp::OscShape::Square, kFrequency, true);
    for (int k = 1; k <= 20; ++k) {
        const double expectedSaw = 2.0 / (std::numbers::pi * k);
        INFO("harmonic " << k);
        CHECK(std::abs(adx::tests::toDb(adx::tests::toneAmplitude(saw, kFrequency * k, kRate)) -
                       adx::tests::toDb(expectedSaw)) < 0.1);
        const double squareAmplitude = adx::tests::toneAmplitude(square, kFrequency * k, kRate);
        if (k % 2 == 1) {
            CHECK(std::abs(adx::tests::toDb(squareAmplitude) -
                           adx::tests::toDb(4.0 / (std::numbers::pi * k))) < 0.1);
        } else {
            CHECK(adx::tests::toDb(squareAmplitude) < -90.0);
        }
    }
}

TEST_CASE("dsp_wavetable_levels_are_band_limited", "[dsp][oscillator]") {
    const dsp::WaveTable& saw = dsp::builtinWaveTable(dsp::Waveform::Saw);
    for (std::size_t level = 0; level < dsp::kWaveTableLevels; ++level) {
        // One table cycle as a signal: its spectrum is the level's harmonics, exactly.
        std::vector<float> cycle(dsp::kWaveTableSize);
        for (std::size_t i = 0; i < cycle.size(); ++i) {
            cycle[i] = saw.levelData(0, level)[i];
        }
        dsp::Fft fft;
        fft.prepare(cycle.size());
        std::vector<dsp::Complex> work(cycle.size());
        for (std::size_t i = 0; i < cycle.size(); ++i) {
            work[i] = dsp::Complex{cycle[i], 0.0F};
        }
        fft.forward(work);
        double above = 0.0;
        double total = 0.0;
        for (std::size_t k = 1; k < cycle.size() / 2; ++k) {
            const double p = std::norm(std::complex<double>(work[k].real(), work[k].imag()));
            total += p;
            if (k > dsp::harmonicsAtLevel(level)) {
                above += p;
            }
        }
        INFO("level " << level);
        CHECK(10.0 * std::log10(std::max(above, 1e-300) / total) < -120.0);
    }
    // Level choice: a 10 kHz fundamental at 48 kHz may carry two harmonics.
    CHECK(dsp::harmonicsAtLevel(dsp::waveTableLevel(10000.0F / 48000.0F)) <= 2);
    CHECK(dsp::harmonicsAtLevel(dsp::waveTableLevel(40.0F / 48000.0F)) * 40 <= 24000);
}

TEST_CASE("dsp_oscillator_sine_is_pure", "[dsp][oscillator]") {
    const std::vector<float> sine = render(dsp::OscShape::Sine, 1000.0, true);
    // -90: the Blackman-Harris window's own sidelobes are the floor below that.
    CHECK(adx::tests::aliasingDbc(sine, 1000.0, kRate) < -90.0);
    CHECK(std::abs(adx::tests::toneAmplitude(sine, 1000.0, kRate) - 1.0) < 1e-3);
}
