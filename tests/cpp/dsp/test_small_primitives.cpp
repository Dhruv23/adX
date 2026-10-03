// The small primitives, each with a known-value or property test (FINAL_PLAN §9):
// Lfo.h, FormantBank.h, Interpolate.h, Saturate.h, Smooth.h, Pan.h, Window.h.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <vector>

#include "engine/dsp/FormantBank.h"
#include "engine/dsp/Interpolate.h"
#include "engine/dsp/Lfo.h"
#include "engine/dsp/Pan.h"
#include "engine/dsp/Saturate.h"
#include "engine/dsp/Smooth.h"
#include "engine/dsp/Window.h"
#include "tests/cpp/dsp/DspTestUtil.h"

namespace dsp = adx::dsp;

TEST_CASE("dsp_lfo_shapes_and_sync", "[dsp][lfo]") {
    dsp::Lfo lfo;
    lfo.reset(0.0F);
    CHECK(lfo.value(dsp::LfoShape::Sine) == 0.0F);
    CHECK(lfo.value(dsp::LfoShape::SawUp) == -1.0F);
    CHECK(lfo.value(dsp::LfoShape::SawDown) == 1.0F);
    CHECK(lfo.value(dsp::LfoShape::Square) == 1.0F);
    CHECK(lfo.value(dsp::LfoShape::Triangle) == 0.0F);
    lfo.reset(0.25F);
    CHECK(std::abs(lfo.value(dsp::LfoShape::Sine) - 1.0F) < 1e-6F);
    CHECK(lfo.value(dsp::LfoShape::Triangle) == 1.0F);
    lfo.reset(0.75F);
    CHECK(lfo.value(dsp::LfoShape::Triangle) == -1.0F);
    CHECK(lfo.value(dsp::LfoShape::Square) == -1.0F);

    // Free-running at 2 Hz: back to its start phase after 24000 samples at 48 kHz.
    lfo.reset(0.1F);
    for (int i = 0; i < 24000; ++i) {
        static_cast<void>(lfo.next(dsp::LfoShape::Sine, 2.0F / 48000.0F));
    }
    CHECK(std::abs(lfo.phase() - 0.1F) < 1e-3F);

    // Synced: the phase is a function of position alone, so two LFOs synced to the same
    // position agree whatever they did before - which is what makes a seek correct.
    dsp::Lfo a;
    dsp::Lfo b;
    for (int i = 0; i < 100; ++i) {
        a.syncTo(i * 0.013, 0.2F);
    }
    b.syncTo(1.287, 0.2F);
    a.syncTo(1.287, 0.2F);
    CHECK(a.phase() == b.phase());
    CHECK(std::abs(a.phase() - 0.487F) < 1e-5F);

    // Sample and hold: constant within a cycle, a new value at each wrap.
    dsp::Lfo hold{3};
    hold.reset(0.0F);
    // An increment of 1/16 is exact in binary, so the wrap lands on the 16th step.
    const float first = hold.next(dsp::LfoShape::SampleHold, 0.0625F);
    for (int i = 0; i < 15; ++i) {
        REQUIRE(hold.next(dsp::LfoShape::SampleHold, 0.0625F) == first);
    }
    CHECK(hold.next(dsp::LfoShape::SampleHold, 0.0625F) != first);
}

TEST_CASE("dsp_formant_bank_peaks_at_its_formants", "[dsp][formant]") {
    // White-flat input (an impulse); the response peaks at F1 of each vowel.
    for (const dsp::VowelSet set : {dsp::VowelSet::Classic3, dsp::VowelSet::Tenor5}) {
        for (std::size_t vowel = 0; vowel < dsp::kVowelCount; ++vowel) {
            dsp::FormantBank bank;
            bank.configure(set, vowel, vowel, 0.0F, 48000.0);
            const std::vector<double> response =
                adx::tests::impulseResponseDb([&](float x) { return bank.process(x); }, 1U << 15U);
            const double binHz = 48000.0 / (1U << 15U);
            const dsp::VowelSpec& spec = dsp::vowelSpec(set, vowel);
            const auto f1 = static_cast<std::size_t>(spec.bands[0].frequency / binHz);
            // F1 stands well above the valley halfway to F2.
            const auto valley = static_cast<std::size_t>(
                0.5 * (spec.bands[0].frequency + spec.bands[1].frequency) / binHz);
            INFO("set " << static_cast<int>(set) << " vowel " << vowel);
            CHECK(response[f1] > response[valley] + 6.0);
        }
    }
    // Morph halfway: F1 lands between the two vowels' F1.
    dsp::FormantBank morph;
    morph.configure(dsp::VowelSet::Tenor5, 0, 2, 0.5F, 48000.0);
    const std::vector<double> response =
        adx::tests::impulseResponseDb([&](float x) { return morph.process(x); }, 1U << 15U);
    std::size_t peak = 10;
    for (std::size_t k = 10; k < 1200; ++k) {
        if (response[k] > response[peak]) {
            peak = k;
        }
    }
    const double peakHz = static_cast<double>(peak) * 48000.0 / (1U << 15U);
    CHECK(peakHz > 290.0);
    CHECK(peakHz < 650.0);
}

TEST_CASE("dsp_interpolators", "[dsp][interpolate]") {
    // Linear and Hermite reproduce a line exactly; the sinc reproduces DC exactly and a
    // band-limited sine closely.
    std::vector<float> line(64);
    for (std::size_t i = 0; i < line.size(); ++i) {
        line[i] = 0.25F * static_cast<float>(i) - 3.0F;
    }
    CHECK(std::abs(dsp::interpolateLinear(line.data(), 10, 0.3F) - (0.25F * 10.3F - 3.0F)) < 1e-6F);
    CHECK(std::abs(dsp::interpolateHermite(line.data(), 10, 0.3F) - (0.25F * 10.3F - 3.0F)) <
          1e-5F);

    const dsp::SincTable& table = dsp::sincTable();
    std::vector<float> dc(64, 0.7F);
    for (float fraction = 0.0F; fraction < 1.0F; fraction += 0.0371F) {
        REQUIRE(std::abs(dsp::interpolateSinc(table, dc.data(), 20, fraction) - 0.7F) < 1e-6F);
    }
    std::vector<float> sine(256);
    const double frequency = 0.05; // cycles per sample, well inside the passband
    for (std::size_t i = 0; i < sine.size(); ++i) {
        sine[i] = static_cast<float>(
            std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(i)));
    }
    float worst = 0.0F;
    for (float position = 50.0F; position < 200.0F; position += 0.173F) {
        const auto index = static_cast<std::size_t>(position);
        const float fraction = position - static_cast<float>(index);
        const auto expected =
            static_cast<float>(std::sin(2.0 * std::numbers::pi * frequency * position));
        worst = std::max(
            worst, std::abs(dsp::interpolateSinc(table, sine.data(), index, fraction) - expected));
    }
    CHECK(worst < 2e-3F);
}

TEST_CASE("dsp_saturators", "[dsp][saturate]") {
    for (const dsp::ShapeKind kind :
         {dsp::ShapeKind::Tanh, dsp::ShapeKind::Soft, dsp::ShapeKind::Tube, dsp::ShapeKind::Hard}) {
        INFO("shape " << static_cast<int>(kind));
        // Silence stays silence, output is bounded, and the curve is monotone.
        CHECK(std::abs(dsp::shape(kind, 0.0F)) < 1e-7F);
        float previous = -10.0F;
        for (float x = -20.0F; x <= 20.0F; x += 0.01F) {
            const float y = dsp::shape(kind, x);
            REQUIRE(std::abs(y) <= 1.3F);
            REQUIRE(y >= previous - 1e-6F);
            previous = y;
        }
    }
    // The tube is asymmetric (even harmonics); the others are odd functions.
    CHECK(std::abs(dsp::shape(dsp::ShapeKind::Tube, 0.8F) +
                   dsp::shape(dsp::ShapeKind::Tube, -0.8F)) > 0.01F);
    CHECK(std::abs(dsp::shape(dsp::ShapeKind::Tanh, 0.8F) +
                   dsp::shape(dsp::ShapeKind::Tanh, -0.8F)) < 1e-6F);
    // Normalised drive: a full-scale input stays full scale at any drive.
    for (const float drive : {1.0F, 4.0F, 30.0F}) {
        CHECK(std::abs(dsp::driveTanh(1.0F, drive, dsp::driveNorm(drive)) - 1.0F) < 1e-5F);
    }
}

TEST_CASE("dsp_smoother", "[dsp][smooth]") {
    dsp::Smoother smoother;
    smoother.prepare(48000, 0.010F);
    // The first value is taken as-is.
    CHECK(smoother.next(0.8F) == 0.8F);
    // A step reaches 1 - 1/e of the way in one time constant (480 samples).
    float value = 0.0F;
    for (int i = 0; i < 480; ++i) {
        value = smoother.next(0.0F);
    }
    CHECK(std::abs(value - (0.8F * static_cast<float>(std::exp(-1.0)))) < 0.002F);
    for (int i = 0; i < 48000; ++i) {
        value = smoother.next(0.0F);
    }
    CHECK(std::abs(value) < 1e-9F);
}

TEST_CASE("dsp_pan_laws", "[dsp][pan]") {
    const auto centreDb = [](dsp::PanLaw law) {
        return adx::tests::toDb(dsp::panGains(0.0F, law).left);
    };
    CHECK(std::abs(centreDb(dsp::PanLaw::Linear)) < 1e-6);
    CHECK(std::abs(centreDb(dsp::PanLaw::ConstantPower) - (-3.0103)) < 0.01);
    CHECK(std::abs(centreDb(dsp::PanLaw::Minus4_5) - (-4.5154)) < 0.01);
    CHECK(std::abs(centreDb(dsp::PanLaw::Minus6) - (-6.0206)) < 0.01);
    // Constant power is constant power everywhere on the arc.
    for (float p = -1.0F; p <= 1.0F; p += 0.05F) {
        const dsp::StereoGains g = dsp::panGains(p, dsp::PanLaw::ConstantPower);
        REQUIRE(std::abs((g.left * g.left) + (g.right * g.right) - 1.0F) < 1e-5F);
    }
    // Hard right silences the left in every law.
    for (const dsp::PanLaw law : {dsp::PanLaw::Linear, dsp::PanLaw::ConstantPower,
                                  dsp::PanLaw::Minus4_5, dsp::PanLaw::Minus6}) {
        CHECK(std::abs(dsp::panGains(1.0F, law).left) < 1e-6F);
    }
}

TEST_CASE("dsp_windows", "[dsp][window]") {
    constexpr std::size_t kN = 1024;
    // Periodic Hann at 50 % overlap sums to exactly one: what overlap-add relies on.
    for (std::size_t i = 0; i < kN / 2; ++i) {
        const double sum = dsp::windowValue(dsp::WindowKind::Hann, i, kN) +
                           dsp::windowValue(dsp::WindowKind::Hann, i + (kN / 2), kN);
        REQUIRE(std::abs(sum - 1.0) < 1e-12);
    }
    CHECK(std::abs(dsp::windowValue(dsp::WindowKind::Hann, 0, kN)) < 1e-15);
    CHECK(std::abs(dsp::windowValue(dsp::WindowKind::Hamming, 0, kN) - 0.08) < 1e-12);
    CHECK(std::abs(dsp::windowValue(dsp::WindowKind::BlackmanHarris, kN / 2, kN) - 1.0) < 1e-12);
    CHECK(dsp::windowValue(dsp::WindowKind::Tukey, kN / 2, kN, true, 0.5) == 1.0);
    CHECK(std::abs(dsp::windowValue(dsp::WindowKind::Tukey, 0, kN, true, 0.5)) < 1e-15);
    CHECK(std::abs(dsp::windowCoherentGain(dsp::WindowKind::Hann, kN) - 0.5) < 1e-12);
}
