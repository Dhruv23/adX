// The `physical` instrument (phase_12.md §4.4, §5): pluck tuning and decay, each model
// sounding, and the CPU budget.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "engine/instruments/physical/PhysicalInstrument.h"
#include "tests/cpp/instruments/InstrumentHarness.h"

using adx::tests::instrumentParamIndex;
using adx::tests::instrumentParams;
using adx::tests::noteOff;
using adx::tests::noteOn;
using adx::tests::preparedInstrument;
using adx::tests::runInstrument;

namespace {

constexpr double kRate = 48000.0;

std::vector<float>
physicalParams(std::initializer_list<std::pair<std::string_view, float>> values) {
    std::vector<float> params = instrumentParams("physical");
    params[instrumentParamIndex("physical", "level")] = 0.0F;
    for (const auto& [name, value] : values) {
        const std::uint32_t index = instrumentParamIndex("physical", name);
        REQUIRE(index != adx::project::kNoParam);
        params[index] = value;
    }
    return params;
}

/// The fundamental's frequency in [from, to): the peak of the magnitude spectrum's
/// lowest strong partial, refined by phase across two half-windows.
double fundamental(const std::vector<float>& x, std::size_t from, std::size_t length,
                   double expected) {
    // Goertzel phase at the expected frequency over two windows a hop apart; the phase
    // advance gives the exact frequency.
    const auto phaseAt = [&](std::size_t start, double hz) {
        double re = 0.0;
        double im = 0.0;
        for (std::size_t n = 0; n < length; ++n) {
            const double w = 0.5 - (0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) /
                                                   static_cast<double>(length)));
            const double angle = 2.0 * std::numbers::pi * hz * static_cast<double>(n) / kRate;
            re += w * x[start + n] * std::cos(angle);
            im -= w * x[start + n] * std::sin(angle);
        }
        return std::atan2(im, re);
    };
    // A sinusoid at f advances 2 pi f hop / fs between the windows; what the measured
    // advance differs from the expected one by, wrapped, is the frequency error.
    const std::size_t hop = length / 4;
    const double expectedAdvance =
        2.0 * std::numbers::pi * expected * static_cast<double>(hop) / kRate;
    const double error =
        std::remainder(phaseAt(from + hop, expected) - phaseAt(from, expected) - expectedAdvance,
                       2.0 * std::numbers::pi);
    return (expectedAdvance + error) * kRate / (2.0 * std::numbers::pi * static_cast<double>(hop));
}

/// The fundamental's level in dB over a window, by Goertzel.
double levelDb(const std::vector<float>& x, std::size_t from, std::size_t length, double hz) {
    double re = 0.0;
    double im = 0.0;
    for (std::size_t n = 0; n < length; ++n) {
        const double angle = 2.0 * std::numbers::pi * hz * static_cast<double>(n) / kRate;
        re += x[from + n] * std::cos(angle);
        im -= x[from + n] * std::sin(angle);
    }
    return 10.0 * std::log10((re * re) + (im * im) + 1e-30);
}

double hzOf(int midi) {
    return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
}

} // namespace

TEST_CASE("physical_tuning_maths", "[physical]") {
    // The first-order allpass: delay 1 at eta 0, (1 - eta)/(1 + eta) toward DC, and
    // allpassFor its inverse at any frequency.
    CHECK(std::abs(adx::instruments::allpassDelay(0.0, 0.3) - 1.0) < 1e-9);
    CHECK(std::abs(adx::instruments::allpassDelay(0.2, 1e-4) - (0.8 / 1.2)) < 1e-4);
    for (const double omega : {0.01, 0.2, 0.55}) {
        for (const double delay : {0.6, 1.0, 1.4}) {
            const double eta = adx::instruments::allpassFor(delay, omega);
            CHECK(std::abs(adx::instruments::allpassDelay(eta, omega) - delay) < 1e-6);
        }
    }
    CHECK(adx::instruments::onePoleDelay(0.0, 0.3) == 0.0);
    CHECK(std::abs(adx::instruments::onePoleGain(0.5, 1e-6) - 1.0) < 1e-6);
}

TEST_CASE("physical_pluck_pitch", "[physical]") {
    // 40 notes across A0..C8, each within 3 cents. Damping and dispersion on, so the
    // loop filters' phase delay is part of what the tuning has to absorb.
    std::vector<int> notes;
    notes.reserve(40);
    for (int i = 0; i < 40; ++i) {
        notes.push_back(21 + ((i * (108 - 21)) / 39));
    }
    for (const float dispersion : {0.0F, 0.5F}) {
        for (const int note : notes) {
            INFO("note " << note << " dispersion " << dispersion);
            const auto run = runInstrument(
                *preparedInstrument("physical"), {noteOn(0, 1, static_cast<std::uint8_t>(note))},
                24000,
                physicalParams({{"string.decay", 20.0F}, {"string.dispersion", dispersion}}));
            CHECK(run.violations == 0);
            const double expected = hzOf(note);
            // A window long enough for 8 periods of A0, after the burst.
            const std::size_t length = note < 40 ? 8192 : 4096;
            const double hz = fundamental(run.left, 4000, length, expected);
            if (dispersion == 0.0F) {
                CHECK(std::abs(1200.0 * std::log2(hz / expected)) < 3.0);
            } else {
                // Dispersion stretches the partials; the fundamental is still tuned.
                CHECK(std::abs(1200.0 * std::log2(hz / expected)) < 3.0);
            }
        }
    }
}

TEST_CASE("physical_pluck_decay", "[physical]") {
    // The fundamental falls 60 dB in string.decay at C4: measured from the slope of its
    // level between 0.1 s and 1.1 s.
    for (const float decay : {1.0F, 3.0F}) {
        INFO("decay " << decay);
        const auto run =
            runInstrument(*preparedInstrument("physical"), {noteOn(0, 1, 60)}, 72000,
                          physicalParams({{"string.decay", decay}, {"string.damping", 0.5F}}));
        const double hz = hzOf(60);
        const double early = levelDb(run.left, 4800, 4800, hz);
        const double late = levelDb(run.left, 52800, 4800, hz);
        const double perSecond = (late - early) / 1.0;
        const double t60 = -60.0 / perSecond;
        CHECK(std::abs(t60 - decay) < 0.1 * decay);
    }
}

TEST_CASE("physical_models_sound", "[physical]") {
    // Each model speaks, stays finite and in range, and goes quiet after its release.
    for (int model = 0; model < 4; ++model) {
        for (const int note : {36, 60, 84}) {
            INFO("model " << model << " note " << note);
            const auto run = runInstrument(*preparedInstrument("physical"),
                                           {noteOn(0, 1, static_cast<std::uint8_t>(note)),
                                            noteOff(24000, 1, static_cast<std::uint8_t>(note))},
                                           96000,
                                           physicalParams({{"model", static_cast<float>(model)},
                                                           {"body.type", 1.0F},
                                                           {"env.release", 0.1F},
                                                           {"string.decay", 1.0F},
                                                           {"modes.decay", 0.5F}}));
            CHECK(run.violations == 0);
            float peak = 0.0F;
            bool finite = true;
            for (const float s : run.left) {
                finite = finite && std::isfinite(s);
                peak = std::max(peak, std::abs(s));
            }
            CHECK(finite);
            CHECK(peak > 0.01F);
            CHECK(peak < 4.0F);
            CHECK(run.sounding.back() == 0);
        }
    }
}

TEST_CASE("physical_modal_partials", "[physical]") {
    // The marimba table: its second mode at 3.9 times the first.
    const auto run = runInstrument(*preparedInstrument("physical"), {noteOn(0, 1, 60)}, 24000,
                                   physicalParams({{"model", 2.0F},
                                                   {"modes.table", 2.0F},
                                                   {"modes.decay", 2.0F},
                                                   {"modes.decayTilt", 0.0F}}));
    const double f = hzOf(60);
    CHECK(levelDb(run.left, 2400, 8192, f * 3.9) > levelDb(run.left, 2400, 8192, f * 3.0) + 20.0);
    CHECK(levelDb(run.left, 2400, 8192, f) > levelDb(run.left, 2400, 8192, f * 1.5) + 20.0);
}

TEST_CASE("physical_cpu_budget", "[physical][.perf]") {
    // phase_12.md §4.4.5: 16 voices of each model render within 25 % of real time at
    // 256-frame blocks. A measurement in Debug; the gate is the Release run.
    for (int model = 0; model < 4; ++model) {
        std::vector<adx::graph::BlockEvent> events;
        events.reserve(16);
        for (std::uint32_t v = 0; v < 16; ++v) {
            events.push_back(noteOn(0, v + 1, static_cast<std::uint8_t>(40 + (v * 2))));
        }
        const auto node = preparedInstrument("physical", 16);
        const auto start = std::chrono::steady_clock::now();
        const auto run = runInstrument(*node, events, 48000 * 4,
                                       physicalParams({{"model", static_cast<float>(model)}}), 256);
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const double load = seconds / 4.0;
        WARN("model " << model << ": " << (100.0 * load) << " % of real time for 16 voices");
#ifdef NDEBUG
        CHECK(load < 0.25);
#endif
        CHECK(run.violations == 0);
    }
}

TEST_CASE("physical_one_pole_delay", "[physical]") {
    // The one-pole low-pass delays the signal by a / (1 - a) samples toward DC.
    CHECK(std::abs(adx::instruments::onePoleDelay(0.3, 1e-5) - (0.3 / 0.7)) < 1e-4);
}
