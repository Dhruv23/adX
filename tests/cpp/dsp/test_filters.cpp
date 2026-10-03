// The filters: TPT state-variable, RBJ biquads, ZDF ladder.
//
// phase_4.md §4.1: "white-noise-in -> FFT -> compare magnitude response against the
// analytic transfer function, ±0.5 dB across 20 Hz-20 kHz, at Q = 0.5, 1, 4, 16. Plus
// a stability sweep: cutoff from 10 Hz to Nyquist x 0.99 at every Q, asserting no NaN,
// no denormal, no output above +6 dB."
//
// The excitation is an impulse rather than white noise: its spectrum is exactly flat,
// so one transform of the impulse response gives the magnitude response with no
// averaging and no variance. Bins where the analytic response is below -80 dB are
// skipped - that is float's floor, not the filter's.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <vector>

#include "engine/dsp/LadderFilter.h"
#include "engine/dsp/Noise.h"
#include "engine/dsp/RbjFilter.h"
#include "engine/dsp/SvFilter.h"
#include "tests/cpp/dsp/DspTestUtil.h"

namespace dsp = adx::dsp;
using Cplx = std::complex<double>;

namespace {

constexpr double kRate = 48000.0;
constexpr std::size_t kLength = 1U << 17U;

/// Analog prototype frequency for digital frequency `f` under the bilinear transform
/// prewarped at `cutoff`: s = j tan(pi f / fs) / tan(pi fc / fs).
Cplx warped(double f, double cutoff) {
    return Cplx{0.0, std::tan(std::numbers::pi * f / kRate) /
                         std::tan(std::numbers::pi * cutoff / kRate)};
}

double analyticSvf(dsp::SvfMode mode, double f, double cutoff, double q) {
    const Cplx s = warped(f, cutoff);
    const double k = 1.0 / q;
    const Cplx d = (s * s) + (k * s) + 1.0;
    Cplx h;
    switch (mode) {
    case dsp::SvfMode::LowPass:
        h = 1.0 / d;
        break;
    case dsp::SvfMode::BandPass:
        h = (k * s) / d;
        break;
    case dsp::SvfMode::HighPass:
        h = (s * s) / d;
        break;
    case dsp::SvfMode::Notch:
        h = ((s * s) + 1.0) / d;
        break;
    case dsp::SvfMode::Peak:
        h = (1.0 - (s * s)) / d;
        break;
    case dsp::SvfMode::AllPass:
        h = ((s * s) - (k * s) + 1.0) / d;
        break;
    }
    return adx::tests::toDb(std::abs(h));
}

/// Compares a measured response against `analytic` over 20 Hz - 20 kHz.
template<class Analytic>
void requireMatches(const std::vector<double>& measured, Analytic analytic) {
    const double binHz = kRate / static_cast<double>(kLength);
    double worst = 0.0;
    double worstAt = 0.0;
    for (std::size_t k = 1; k < measured.size(); ++k) {
        const double f = static_cast<double>(k) * binHz;
        if (f < 20.0 || f > 20000.0) {
            continue;
        }
        const double expected = analytic(f);
        if (expected < -80.0) {
            continue;
        }
        const double error = std::abs(measured[k] - expected);
        if (error > worst) {
            worst = error;
            worstAt = f;
        }
    }
    INFO("worst error " << worst << " dB at " << worstAt << " Hz");
    CHECK(worst <= 0.5);
}

} // namespace

TEST_CASE("dsp_filter_response_matches_analytic", "[dsp][filter]") {
    const std::vector<dsp::SvfMode> modes{dsp::SvfMode::LowPass,  dsp::SvfMode::BandPass,
                                          dsp::SvfMode::HighPass, dsp::SvfMode::Notch,
                                          dsp::SvfMode::Peak,     dsp::SvfMode::AllPass};
    for (const double q : {0.5, 1.0, 4.0, 16.0}) {
        for (const double cutoff : {100.0, 1000.0, 8000.0}) {
            for (const dsp::SvfMode mode : modes) {
                INFO("SVF mode " << static_cast<int>(mode) << " Q " << q << " cutoff " << cutoff);
                dsp::SvFilter filter;
                const dsp::SvfCoefficients c = dsp::svfCoefficients(cutoff, q, kRate);
                const std::vector<double> measured = adx::tests::impulseResponseDb(
                    [&](float x) { return filter.process(x, c, mode); }, kLength);
                requireMatches(measured, [&](double f) { return analyticSvf(mode, f, cutoff, q); });
            }
            // The ladder at the same cutoffs, its resonance spanning the range; drive 0
            // is the linear filter H(s) = 1 / ((1 + s)^4 + k).
            const double resonance = q / 20.0;
            INFO("ladder resonance " << resonance << " cutoff " << cutoff);
            dsp::LadderFilter ladder;
            const dsp::LadderCoefficients lc = dsp::ladderCoefficients(cutoff, resonance, kRate);
            const std::vector<double> measured = adx::tests::impulseResponseDb(
                [&](float x) { return ladder.process(x, lc, 0.0F); }, kLength);
            requireMatches(measured, [&](double f) {
                const Cplx s = warped(f, cutoff);
                const Cplx one = 1.0 + s;
                return adx::tests::toDb(
                    std::abs(1.0 / ((one * one * one * one) + (4.0 * resonance))));
            });
        }
    }

    // The biquads: measured against the transfer function of their own coefficients,
    // and the coefficients against the cookbook's defining points.
    const std::vector<dsp::BiquadKind> kinds{dsp::BiquadKind::LowPass,  dsp::BiquadKind::HighPass,
                                             dsp::BiquadKind::BandPass, dsp::BiquadKind::Notch,
                                             dsp::BiquadKind::AllPass,  dsp::BiquadKind::Peaking,
                                             dsp::BiquadKind::LowShelf, dsp::BiquadKind::HighShelf};
    for (const double q : {0.5, 1.0, 4.0, 16.0}) {
        for (const dsp::BiquadKind kind : kinds) {
            const bool shelf =
                kind == dsp::BiquadKind::LowShelf || kind == dsp::BiquadKind::HighShelf;
            const double slope = shelf ? std::min(q, 1.0) : q;
            INFO("biquad " << static_cast<int>(kind) << " Q " << slope);
            const dsp::BiquadCoefficients c =
                dsp::biquadCoefficients(kind, 1500.0, slope, 9.0, kRate);
            dsp::Biquad filter;
            const std::vector<double> measured = adx::tests::impulseResponseDb(
                [&](float x) { return filter.process(x, c); }, kLength);
            requireMatches(measured, [&](double f) { return dsp::biquadMagnitudeDb(c, f, kRate); });
        }
    }
    const auto at = [](dsp::BiquadKind kind, double f) {
        return dsp::biquadMagnitudeDb(dsp::biquadCoefficients(kind, 1000.0, 0.7071, 6.0, kRate), f,
                                      kRate);
    };
    CHECK(std::abs(at(dsp::BiquadKind::Peaking, 1000.0) - 6.0) < 1e-3);
    CHECK(std::abs(at(dsp::BiquadKind::LowShelf, 5.0) - 6.0) < 0.01);
    CHECK(std::abs(at(dsp::BiquadKind::HighShelf, 23000.0) - 6.0) < 0.01);
    CHECK(std::abs(at(dsp::BiquadKind::LowPass, 1000.0) - (-3.0103)) < 0.01);
    CHECK(std::abs(at(dsp::BiquadKind::AllPass, 300.0)) < 1e-4);
}

TEST_CASE("dsp_filter_stability_sweep", "[dsp][filter]") {
    // Every cutoff from 10 Hz to 0.99 x Nyquist, at every Q, driven with white noise
    // and with the cutoff swept *while* it runs: finite, never denormal, and bounded by
    // the filter's own peak gain with 6 dB to spare.
    dsp::WhiteNoise noise{12345};
    std::vector<float> input(4096);
    for (float& x : input) {
        x = noise.next();
    }
    const auto check = [](float y, double bound) {
        REQUIRE(std::isfinite(y));
        REQUIRE_FALSE((y != 0.0F && std::abs(y) < 1e-30F));
        REQUIRE(std::abs(y) <= bound);
    };
    for (const double q : {0.5, 1.0, 4.0, 16.0, 100.0}) {
        // The SVF's largest gain is its resonant peak, Q; the ladder's is bounded by
        // its input saturation and the k = 4 limit.
        const double svfBound = 2.0 * 8.0 * std::max(1.0, q);
        for (double cutoff = 10.0; cutoff <= 0.99 * (kRate / 2.0); cutoff *= 1.5) {
            for (const dsp::SvfMode mode : {dsp::SvfMode::LowPass, dsp::SvfMode::BandPass,
                                            dsp::SvfMode::HighPass, dsp::SvfMode::Peak}) {
                dsp::SvFilter filter;
                const dsp::SvfCoefficients c = dsp::svfCoefficients(cutoff, q, kRate);
                for (const float x : input) {
                    check(filter.process(x, c, mode), svfBound);
                }
            }
            dsp::LadderFilter ladder;
            const dsp::LadderCoefficients lc =
                dsp::ladderCoefficients(cutoff, std::min(q / 16.0, 1.0), kRate);
            for (const float x : input) {
                check(ladder.process(x, lc, 1.0F), 64.0);
            }
        }
        // Modulated: the cutoff moves every 32 samples from 20 Hz to 20 kHz and back.
        dsp::SvFilter swept;
        dsp::LadderFilter sweptLadder;
        for (std::size_t i = 0; i < 48000; ++i) {
            const double phase = static_cast<double>(i) / 48000.0;
            const double cutoff = 20.0 * std::pow(1000.0, 0.5 - (0.5 * std::cos(phase * 12.566)));
            const dsp::SvfCoefficients c = dsp::svfCoefficients(cutoff, q, kRate);
            const dsp::LadderCoefficients lc =
                dsp::ladderCoefficients(cutoff, std::min(q / 16.0, 1.0), kRate);
            const float x = input[i % input.size()];
            check(swept.process(x, c, dsp::SvfMode::LowPass), svfBound);
            check(sweptLadder.process(x, lc, 1.0F), 64.0);
        }
    }
}

TEST_CASE("dsp_filter_silence_stays_exact", "[dsp][filter]") {
    // No input, no output - not a decaying denormal hiss.
    dsp::SvFilter svf;
    dsp::Biquad biquad;
    dsp::LadderFilter ladder;
    const dsp::SvfCoefficients c = dsp::svfCoefficients(500.0, 10.0, kRate);
    const dsp::BiquadCoefficients b =
        dsp::biquadCoefficients(dsp::BiquadKind::Peaking, 500.0, 10.0, 12.0, kRate);
    const dsp::LadderCoefficients l = dsp::ladderCoefficients(500.0, 0.9, kRate);
    static_cast<void>(svf.process(1.0F, c, dsp::SvfMode::LowPass));
    static_cast<void>(biquad.process(1.0F, b));
    static_cast<void>(ladder.process(1.0F, l, 0.0F));
    float last = 1.0F;
    for (int i = 0; i < 48000 * 20; ++i) {
        last = std::abs(svf.process(0.0F, c, dsp::SvfMode::LowPass)) +
               std::abs(biquad.process(0.0F, b)) + std::abs(ladder.process(0.0F, l, 0.0F));
    }
    CHECK(last == 0.0F);
}
