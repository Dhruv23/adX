// VA (phase_4.md §4.4): band-limited oscillators, the filter, unison, LFO routing.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include "tests/cpp/dsp/DspTestUtil.h"
#include "tests/cpp/instruments/InstrumentHarness.h"

using adx::tests::instrumentParamIndex;
using adx::tests::instrumentParams;
using adx::tests::noteOn;
using adx::tests::preparedInstrument;
using adx::tests::runInstrument;

namespace {

std::vector<float> vaParams() {
    std::vector<float> params = instrumentParams("va");
    params[instrumentParamIndex("va", "env.attack")] = 0.0F;
    params[instrumentParamIndex("va", "env.sustain")] = 1.0F;
    params[instrumentParamIndex("va", "level")] = 0.0F;
    return params;
}

void set(std::vector<float>& params, const char* name, float value) {
    params[instrumentParamIndex("va", name)] = value;
}

/// 32768 frames of the left channel from frame 4800, past the attack.
std::vector<float> steady(const adx::tests::InstrumentRun& run) {
    return {run.left.begin() + 4800, run.left.begin() + 4800 + 32768};
}

double energyAbove(const std::vector<float>& x, double hz) {
    const std::vector<double> power = adx::tests::powerSpectrum(x);
    const double binHz = 48000.0 / static_cast<double>(x.size());
    double sum = 0.0;
    for (std::size_t k = 0; k < power.size(); ++k) {
        if (static_cast<double>(k) * binHz >= hz) {
            sum += power[k];
        }
    }
    return sum;
}

} // namespace

TEST_CASE("va_oscillator_is_band_limited", "[instruments][va]") {
    // One saw, no unison, filter open, at a high note: what aliasing the output has is
    // the oscillator's, and phase_4.md §4.1 asks for under -60 dBc.
    std::vector<float> params = vaParams();
    set(params, "unison.voices", 1);
    const auto node = preparedInstrument("va");
    // MIDI 112 is ~2637 Hz: harmonics fold back from ~9 of them up.
    const auto run = runInstrument(*node, {noteOn(0, 1, 112)}, 48000, params);
    const double fundamental = 440.0 * std::pow(2.0, (112.0 - 69.0) / 12.0);
    CHECK(adx::tests::aliasingDbc(steady(run), fundamental, 48000.0) < -60.0);
}

TEST_CASE("va_filter_removes_highs", "[instruments][va]") {
    std::vector<float> const open = vaParams();
    std::vector<float> closed = open;
    set(closed, "filter.cutoff", 400.0F);
    const auto a = runInstrument(*preparedInstrument("va"), {noteOn(0, 1, 45)}, 48000, open);
    const auto b = runInstrument(*preparedInstrument("va"), {noteOn(0, 1, 45)}, 48000, closed);
    // A 24 dB/octave ladder at 400 Hz: energy above 3.2 kHz (three octaves up) falls by
    // far more than 40 dB.
    const double drop =
        10.0 * std::log10(energyAbove(steady(b), 3200.0) / energyAbove(steady(a), 3200.0));
    CHECK(drop < -40.0);

    // And every state-variable mode is stable at full resonance across the range.
    for (int type = 1; type <= 4; ++type) {
        for (const float cutoff : {30.0F, 1000.0F, 19000.0F}) {
            std::vector<float> p = vaParams();
            set(p, "filter.type", static_cast<float>(type));
            set(p, "filter.cutoff", cutoff);
            set(p, "filter.resonance", 1.0F);
            const auto run = runInstrument(*preparedInstrument("va"), {noteOn(0, 1, 40)}, 24000, p);
            for (const float s : run.left) {
                REQUIRE(std::isfinite(s));
                REQUIRE(std::abs(s) < 8.0F);
            }
        }
    }
}

TEST_CASE("va_unison_spreads_across_the_field", "[instruments][va]") {
    std::vector<float> mono = vaParams();
    set(mono, "unison.voices", 1);
    std::vector<float> wide = vaParams();
    set(wide, "unison.voices", 6);
    set(wide, "unison.spread", 1.0F);
    const auto a = runInstrument(*preparedInstrument("va"), {noteOn(0, 1, 57)}, 9600, mono);
    const auto b = runInstrument(*preparedInstrument("va"), {noteOn(0, 1, 57)}, 9600, wide);
    double monoDiff = 0.0;
    double wideDiff = 0.0;
    for (std::size_t i = 0; i < 9600; ++i) {
        monoDiff += std::abs(a.left[i] - a.right[i]);
        wideDiff += std::abs(b.left[i] - b.right[i]);
    }
    CHECK(monoDiff < 1e-3);
    CHECK(wideDiff > 10.0);
}

TEST_CASE("va_lfo_routes_to_pitch", "[instruments][va]") {
    // A sine through an LFO on pitch at depth 1/12 (one semitone): the zero-crossing
    // rate swings by about a semitone either side.
    std::vector<float> params = vaParams();
    set(params, "osc1.wave", 0);   // sine
    set(params, "lfo1.target", 1); // pitch
    set(params, "lfo1.depth", 1.0F / 12.0F);
    set(params, "lfo1.rate", 2.0F);
    const auto run = runInstrument(*preparedInstrument("va"), {noteOn(0, 1, 69)}, 48000, params);
    std::vector<double> freqs;
    double last = -1.0;
    for (std::size_t i = 1; i < run.left.size(); ++i) {
        if (run.left[i - 1] < 0.0F && run.left[i] >= 0.0F) {
            const double at =
                static_cast<double>(i - 1) + (run.left[i - 1] / (run.left[i - 1] - run.left[i]));
            if (last >= 0.0) {
                freqs.push_back(48000.0 / (at - last));
            }
            last = at;
        }
    }
    const auto [low, high] = std::ranges::minmax_element(freqs);
    CHECK(1200.0 * std::log2(*high / 440.0) > 90.0);
    CHECK(1200.0 * std::log2(*low / 440.0) < -90.0);
}
