// Phase 12's last four effects (phase_12.md §4.6, §5): Shaper, OTT, Hall, PitchCorrect.
// The shared contract (bypass, mix law, clones, no allocation, goldens) comes through the
// catalog in test_effect_base.cpp and test_golden_units.cpp; these check what each is for.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

#include "engine/dsp/Oversampler.h"
#include "engine/effects/Hall.h"
#include "engine/effects/PitchCorrect.h"
#include "engine/effects/Shaper.h"
#include "engine/project/TypeCatalog.h"
#include "tests/cpp/effects/EffectHarness.h"

using adx::tests::kTestRate;
using adx::tests::preparedEffect;
using adx::tests::runEffect;
using adx::tests::slotParamIndex;
using adx::tests::slotParams;
using adx::tests::StereoSignal;

namespace {

std::vector<float> params(std::string_view type,
                          std::initializer_list<std::pair<std::string_view, float>> values) {
    std::vector<float> out = slotParams(type);
    for (const auto& [name, value] : values) {
        const std::uint32_t index = slotParamIndex(type, name);
        REQUIRE(index != adx::project::kNoParam);
        out[index] = value;
    }
    return out;
}

double rmsDb(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    }
    return 10.0 * std::log10(sum / static_cast<double>(to - from) + 1e-30);
}

/// Mean frequency over [from, to) from rising zero crossings.
double frequency(const std::vector<float>& x, std::size_t from, std::size_t to) {
    std::vector<double> crossings;
    for (std::size_t i = from + 1; i < to; ++i) {
        if (x[i - 1] < 0.0F && x[i] >= 0.0F) {
            crossings.push_back(static_cast<double>(i - 1) +
                                (static_cast<double>(-x[i - 1]) / (x[i] - x[i - 1])));
        }
    }
    REQUIRE(crossings.size() > 2);
    return kTestRate * static_cast<double>(crossings.size() - 1) /
           (crossings.back() - crossings.front());
}

/// Schroeder RT60 from the -5 to -35 dB slope of `x`.
double rt60(const std::vector<float>& x) {
    std::vector<double> energy(x.size());
    double sum = 0.0;
    for (std::size_t i = x.size(); i-- > 0;) {
        sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
        energy[i] = sum;
    }
    const auto db = [&](std::size_t i) { return 10.0 * std::log10(energy[i] / energy[0] + 1e-30); };
    std::size_t start = 0;
    while (start < x.size() && db(start) > -5.0) {
        ++start;
    }
    std::size_t stop = start;
    while (stop < x.size() && db(stop) > -35.0) {
        ++stop;
    }
    REQUIRE(stop < x.size());
    const double slope = (db(stop) - db(start)) / (static_cast<double>(stop - start) / kTestRate);
    return -60.0 / slope;
}

} // namespace

TEST_CASE("shaper_transfer", "[effects][phase12]") {
    // The curve itself: through every knot, monotone between them, identity by default.
    std::array<float, adx::effects::kShaperKnots> identity{};
    for (std::size_t k = 0; k < identity.size(); ++k) {
        identity[k] = -1.0F + (2.0F * static_cast<float>(k) / 6.0F);
    }
    for (const float x : {-1.0F, -0.4F, 0.0F, 0.25F, 0.9F}) {
        CHECK(std::abs(adx::effects::shaperCurve(identity, true, x) - x) < 1e-6F);
        CHECK(std::abs(adx::effects::shaperCurve(identity, false, x) - x) < 1e-6F);
    }
    const std::array<float, 7> knee{-1.0F, -0.95F, -0.8F, 0.0F, 0.8F, 0.95F, 1.0F};
    float previous = -2.0F;
    for (int i = 0; i <= 600; ++i) {
        const float x = -1.0F + (static_cast<float>(i) / 300.0F);
        const float y = adx::effects::shaperCurve(knee, true, x);
        CHECK(y >= previous - 1e-6F); // monotone: no overshoot
        CHECK(std::abs(y) <= 1.0F + 1e-6F);
        previous = y;
    }
    CHECK(std::abs(adx::effects::shaperCurve(knee, true, 2.0F / 3.0F) - 0.95F) < 1e-6F);

    // Through the effect: a slow sine comes out as the curve of the sine, delayed by
    // the oversampler.
    const StereoSignal in = adx::tests::sine(50.0, 0.9F, 9600);
    const auto node = preparedEffect("Shaper");
    std::vector<float> p = params("Shaper", {{"dcBlock", 0.0F}});
    for (std::size_t k = 0; k < knee.size(); ++k) {
        p[slotParamIndex("Shaper", "knot" + std::to_string(k + 1))] = knee[k];
    }
    const auto run = runEffect(*node, in, p);
    CHECK(run.violations == 0);
    double worst = 0.0;
    for (std::size_t i = 2000; i < 9000; ++i) {
        const float expected =
            adx::effects::shaperCurve(knee, true, in.left[i - adx::dsp::kOversampleLatency]);
        worst = std::max(worst, static_cast<double>(std::abs(run.out.left[i] - expected)));
    }
    CHECK(worst < 0.01);
}

TEST_CASE("ott_upward", "[effects][phase12]") {
    // The static curve: above the downward threshold, pulled down; below the upward
    // one, pulled up by (threshold - level)(1 - 1/ratio); neither between.
    using adx::effects::ottGainDb;
    CHECK(ottGainDb(-50.0F, -24.0F, 66.0F, -40.0F, 2.0F, 1.0F, 1.0F, 1.0F) == 5.0F);
    CHECK(ottGainDb(-30.0F, -24.0F, 66.0F, -40.0F, 2.0F, 1.0F, 1.0F, 1.0F) == 0.0F);
    CHECK(std::abs(ottGainDb(-6.0F, -24.0F, 4.0F, -40.0F, 2.0F, 1.0F, 1.0F, 1.0F) + 13.5F) < 1e-5F);
    CHECK(ottGainDb(-50.0F, -24.0F, 66.0F, -40.0F, 2.0F, 0.5F, 1.0F, 1.0F) == 2.5F);
    CHECK(ottGainDb(-200.0F, -24.0F, 66.0F, -40.0F, 20.0F, 1.0F, 1.0F, 1.0F) ==
          adx::effects::kOttMaxUpwardDb);

    // Through the effect, a 1 kHz tone in the mid band. Its peak level is what the
    // follower reads, so levels are peak dBFS and the measured change is RMS against RMS.
    const auto through = [](float peakDb, float upRatio, float downRatio) {
        const auto amplitude = static_cast<float>(std::pow(10.0, peakDb / 20.0));
        const StereoSignal in = adx::tests::sine(1000.0, amplitude, 48000);
        const auto node = preparedEffect("OTT");
        const auto run = runEffect(*node, in,
                                   params("OTT", {{"low.up.ratio", 1.0F},
                                                  {"low.down.ratio", 1.0F},
                                                  {"high.up.ratio", 1.0F},
                                                  {"high.down.ratio", 1.0F},
                                                  {"mid.up.threshold", -40.0F},
                                                  {"mid.up.ratio", upRatio},
                                                  {"mid.down.threshold", -24.0F},
                                                  {"mid.down.ratio", downRatio},
                                                  {"time", 0.3F}}));
        CHECK(run.violations == 0);
        return rmsDb(run.out.left, 24000, 48000) - rmsDb(in.left, 24000, 48000);
    };
    // A -50 dBFS band rises by the configured ratio: 10 dB under, ratio 2, so +5.
    CHECK(std::abs(through(-50.0F, 2.0F, 1.0F) - 5.0) < 0.5);
    CHECK(std::abs(through(-50.0F, 4.0F, 1.0F) - 7.5) < 0.5);
    // A -6 dBFS band is pulled down: 18 over at 4:1 is -13.5.
    // The follower releases between peaks, so on a sine it reads a little under the peak
    // and reduces about 1 dB less than the static curve (checked exactly above) says.
    CHECK(std::abs(through(-6.0F, 1.0F, 4.0F) + 13.5) < 1.5);
}

TEST_CASE("hall_decay", "[effects][phase12]") {
    // Each line loses exactly 60 dB in `decay` (hallLineGain), so the tail's measured
    // RT60 matches within 10 %, at 0.5, 4 and 30 s.
    CHECK(std::abs(adx::effects::hallLineGain(0.03, 0.03) - 0.001) < 1e-9);
    for (const float decay : {0.5F, 4.0F, 30.0F}) {
        INFO("decay " << decay);
        // Long enough that the end is ~70 dB down: a shorter record truncates Schroeder's
        // integral and reads short.
        const auto frames =
            static_cast<std::size_t>(((decay * 1.2F) + 0.5F) * static_cast<float>(kTestRate));
        const StereoSignal in = adx::tests::impulse(0, 1.0F, frames);
        const auto node = preparedEffect("Hall");
        const auto run = runEffect(
            *node, in,
            params("Hall",
                   {{"decay", decay}, {"damping", 0.0F}, {"modulation", 0.0F}, {"predelay", 0.0F}}),
            512);
        CHECK(run.violations == 0);
        const double measured = rt60(run.out.left);
        CHECK(std::abs(measured - decay) < 0.1 * decay);
    }

    // Freeze holds the tail: no decay at all over a second, and new input is ignored.
    const StereoSignal burst = adx::tests::noise(3U, 0.3F, 96000);
    StereoSignal input = burst;
    std::fill(input.left.begin() + 24000, input.left.end(), 0.0F);
    std::fill(input.right.begin() + 24000, input.right.end(), 0.0F);
    const auto node = preparedEffect("Hall");
    const std::uint32_t freeze = slotParamIndex("Hall", "freeze");
    const auto run = runEffect(*node, input, params("Hall", {{"decay", 1.0F}}), 64,
                               [freeze](std::uint64_t frame, std::vector<float>& p) {
                                   p[freeze] = frame >= 24000 ? 1.0F : 0.0F;
                               });
    const double held = rmsDb(run.out.left, 48000, 60000) - rmsDb(run.out.left, 84000, 96000);
    CHECK(std::abs(held) < 1.0);
}

TEST_CASE("pitchcorrect_snaps", "[effects][phase12]") {
    // C major: a sine 30 cents flat of A4 comes out on A4. With retune 0 it is there by
    // the first analysis; with retune 50 ms within about 3 time constants of it.
    CHECK(adx::effects::snapToScale(68.7F, 0, 1) == 69.0F);
    CHECK(adx::effects::snapToScale(66.4F, 0, 1) == 67.0F); // F#+40: G is nearer than F
    CHECK(adx::effects::snapToScale(61.0F, 0, 1) == 60.0F); // C# ties down to C
    CHECK(adx::effects::snapToScale(61.0F, 0, 0) == 61.0F);
    const double flat = 440.0 * std::pow(2.0, -0.3 / 12.0);
    for (const float retune : {0.0F, 50.0F}) {
        INFO("retune " << retune);
        const StereoSignal in = adx::tests::sine(flat, 0.5F, 48000);
        const auto node = preparedEffect("PitchCorrect");
        const auto run =
            runEffect(*node, in,
                      params("PitchCorrect", {{"key", 0.0F}, {"scale", 1.0F}, {"retune", retune}}));
        CHECK(run.violations == 0);
        const auto settle = static_cast<std::size_t>(
            (static_cast<double>(adx::effects::kYinWindow) / kTestRate + (retune * 0.003)) *
            kTestRate);
        const double hz = frequency(run.out.left, settle + 2400, 48000);
        CHECK(std::abs(1200.0 * std::log2(hz / 440.0)) < 3.0);
    }
    // Humanize: a note already within 40 cents is left alone.
    const StereoSignal in = adx::tests::sine(flat, 0.5F, 48000);
    const auto node = preparedEffect("PitchCorrect");
    const auto run =
        runEffect(*node, in, params("PitchCorrect", {{"scale", 1.0F}, {"humanize", 40.0F}}));
    CHECK(std::abs(1200.0 * std::log2(frequency(run.out.left, 12000, 48000) / flat)) < 3.0);
}
