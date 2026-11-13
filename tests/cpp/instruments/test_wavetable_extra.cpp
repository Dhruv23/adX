// Wavetable's extension (phase_12.md §4.3): osc2, the sub, noise, the VA's filter types
// with an envelope, and the nine warp modes. Inert by default is the golden gate's job;
// these check that each part, switched on, does what it says.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <string_view>
#include <vector>

#include "tests/cpp/instruments/InstrumentHarness.h"

using adx::tests::noteOn;

namespace {

constexpr double kRate = 48000.0;

std::vector<float> render(std::initializer_list<std::pair<std::string_view, float>> values,
                          std::uint8_t key = 57, std::uint32_t frames = 24000) {
    std::vector<float> params = adx::tests::instrumentParams("wavetable");
    for (const auto& [name, value] : values) {
        params[adx::tests::instrumentParamIndex("wavetable", name)] = value;
    }
    const auto run = adx::tests::runInstrument(*adx::tests::preparedInstrument("wavetable"),
                                               {noteOn(0, 1, key, 110)}, frames, params);
    CHECK(run.violations == 0);
    for (const float s : run.left) {
        REQUIRE(std::isfinite(s));
        REQUIRE(std::abs(s) < 8.0F);
    }
    return run.left;
}

/// Level at `hz`, dB, over [from, to): one DFT bin with a Hann window.
double levelDb(const std::vector<float>& x, std::size_t from, std::size_t to, double hz) {
    std::complex<double> sum{};
    const auto n = static_cast<double>(to - from);
    for (std::size_t i = from; i < to; ++i) {
        const auto t = static_cast<double>(i - from);
        const double w = 0.5 - (0.5 * std::cos(2.0 * std::numbers::pi * t / n));
        sum += std::polar(static_cast<double>(x[i]) * w, -2.0 * std::numbers::pi * hz * t / kRate);
    }
    return 20.0 * std::log10((std::abs(sum) / n) + 1e-12);
}

/// Mean power over 21 bins 10 Hz apart around `hz`, dB: steady on noise.
double bandDb(const std::vector<float>& x, std::size_t from, std::size_t to, double hz) {
    double power = 0.0;
    for (int k = -10; k <= 10; ++k) {
        power += std::pow(10.0, levelDb(x, from, to, hz + (10.0 * k)) / 10.0);
    }
    return 10.0 * std::log10(power / 21.0);
}

double rmsDb(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double s = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        s += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    }
    return 10.0 * std::log10((s / static_cast<double>(to - from)) + 1e-24);
}

double differenceDb(const std::vector<float>& a, const std::vector<float>& b) {
    std::vector<float> d(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        d[i] = a[i] - b[i];
    }
    return rmsDb(d, 4800, a.size()) - rmsDb(a, 4800, a.size());
}

constexpr double kA3 = 220.0;

} // namespace

TEST_CASE("wavetable_osc2_sub_noise", "[wavetable]") {
    // Table 2 (Harmonic) position 0 is a sine: one partial, so what the extras add shows.
    const auto plain = render({{"table", 2.0F}});
    // osc2 a fifth up adds its partial there.
    const auto fifth =
        render({{"table", 2.0F}, {"osc2.table", 2.0F}, {"osc2.semi", 7.0F}, {"osc2.level", 1.0F}});
    const double up = kA3 * std::pow(2.0, 7.0 / 12.0);
    CHECK(levelDb(fifth, 4800, 24000, up) > levelDb(plain, 4800, 24000, up) + 30.0);
    // The sub: an octave below.
    const auto sub = render({{"table", 2.0F}, {"sub.level", 1.0F}});
    CHECK(levelDb(sub, 4800, 24000, kA3 / 2.0) > levelDb(plain, 4800, 24000, kA3 / 2.0) + 30.0);
    // Noise: energy far from any partial.
    const auto noise = render({{"table", 2.0F}, {"noise.level", 1.0F}});
    CHECK(levelDb(noise, 4800, 24000, 3170.0) > levelDb(plain, 4800, 24000, 3170.0) + 20.0);
}

TEST_CASE("wavetable_filter_types_and_envelope", "[wavetable]") {
    // Noise through each type at 1 kHz, the note at A0: low-pass keeps 200 Hz over 8 kHz, high-pass
    // the reverse, band-pass keeps 1 kHz over both, notch takes 1 kHz out; the ladder is a steeper
    // low-pass than the SVF.
    const auto with = [](float type) {
        return render({{"noise.level", 1.0F},
                       {"filter.type", type},
                       {"filter.cutoff", 1000.0F},
                       {"filter.resonance", 0.2F}},
                      21); // A0: the oscillator's own sine sits below every band read
    };
    const auto low = with(0.0F);
    const auto ladder = with(1.0F);
    const auto band = with(2.0F);
    const auto high = with(3.0F);
    const auto notch = with(4.0F);
    const auto at = [](const std::vector<float>& x, double hz) {
        return bandDb(x, 4800, 24000, hz);
    };
    CHECK(at(low, 200.0) > at(low, 8000.0) + 24.0);
    CHECK(at(high, 8000.0) > at(high, 200.0) + 24.0);
    CHECK(at(band, 1000.0) > at(band, 200.0) + 10.0);
    CHECK(at(band, 1000.0) > at(band, 8000.0) + 10.0);
    CHECK(at(notch, 1000.0) < at(notch, 300.0) - 10.0);
    CHECK(at(ladder, 200.0) - at(ladder, 8000.0) > at(low, 200.0) - at(low, 8000.0) + 12.0);
    // The filter envelope opens the filter at the note's start and closes it after.
    const auto swept = render({{"noise.level", 1.0F},
                               {"filter.cutoff", 200.0F},
                               {"filter.envAmount", 5.0F},
                               {"filterEnv.attack", 0.001F},
                               {"filterEnv.decay", 0.15F},
                               {"filterEnv.sustain", 0.0F}},
                              21);
    CHECK(bandDb(swept, 480, 2880, 4000.0) > bandDb(swept, 19200, 24000, 4000.0) + 15.0);
}

TEST_CASE("wavetable_warp_modes", "[wavetable]") {
    // Each warp changes the sound, more as the amount grows, and none blows up.
    const auto none = render({});
    for (int mode = 1; mode < 9; ++mode) {
        INFO("warp.mode " << mode);
        const auto half = render(
            {{"warp.mode", static_cast<float>(mode)}, {"warp.amount", 0.5F}, {"osc2.semi", 12.0F}});
        CHECK(differenceDb(half, none) > -40.0);
    }
    // Amount 0 is no warp, in every mode that needs no osc2.
    for (const int mode : {1, 2, 3, 4, 5, 7}) {
        INFO("warp.mode " << mode << " at amount 0");
        const auto zero = render({{"warp.mode", static_cast<float>(mode)}, {"warp.amount", 0.0F}});
        CHECK(differenceDb(zero, none) < -100.0);
    }
}
