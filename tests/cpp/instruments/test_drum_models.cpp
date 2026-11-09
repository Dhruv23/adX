// drum_new_models (phase_12.md §4.5, §5): the 808's percussion on DrumSynth. The cowbell's
// spectrum peaks at its two oscillators (540 and 800 Hz, times `tune`); rim and clave
// ring only as long as their windows; the shaker is high-passed noise; the cymbal and
// conga sound and end; `character` 0 is the 808 every earlier sound already was.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

#include "engine/instruments/drumsynth/DrumSynthParams.h"
#include "tests/cpp/dsp/DspTestUtil.h"
#include "tests/cpp/instruments/InstrumentHarness.h"

using adx::instruments::DrumModel;
using adx::tests::instrumentParamIndex;
using adx::tests::instrumentParams;
using adx::tests::InstrumentRun;
using adx::tests::noteOn;
using adx::tests::preparedInstrument;
using adx::tests::runInstrument;

namespace {

InstrumentRun hit(DrumModel model, float decay = 0.4F, float tone = 0.5F, float tune = 0.0F,
                  std::uint8_t key = 60, float character = 0.0F) {
    std::vector<float> p = instrumentParams("drumsynth");
    p[instrumentParamIndex("drumsynth", "model")] = static_cast<float>(model);
    p[instrumentParamIndex("drumsynth", "decay")] = decay;
    p[instrumentParamIndex("drumsynth", "tone")] = tone;
    p[instrumentParamIndex("drumsynth", "tune")] = tune;
    p[instrumentParamIndex("drumsynth", "character")] = character;
    return runInstrument(*preparedInstrument("drumsynth"), {noteOn(0, 1, key, 120)}, 48000, p);
}

double energy(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to && i < x.size(); ++i) {
        sum += static_cast<double>(x[i]) * x[i];
    }
    return sum;
}

std::vector<float> slice(const std::vector<float>& x, std::size_t from, std::size_t n) {
    return {x.begin() + static_cast<std::ptrdiff_t>(from),
            x.begin() + static_cast<std::ptrdiff_t>(from + n)};
}

} // namespace

TEST_CASE("drum_new_models", "[instruments][drumsynth]") {
    SECTION("cowbell peaks at 540 and 800 Hz, and moves with tune") {
        for (const float tune : {0.0F, 12.0F}) {
            INFO("tune " << tune);
            const InstrumentRun run = hit(DrumModel::Cowbell, 1.0F, 0.5F, tune);
            REQUIRE(run.violations == 0);
            const std::vector<float> body = slice(run.left, 2400, 16384);
            const double ratio = std::exp2(tune / 12.0);
            const double low = adx::tests::toneAmplitude(body, 540.0 * ratio, 48000.0, 3);
            const double high = adx::tests::toneAmplitude(body, 800.0 * ratio, 48000.0, 3);
            const double between = adx::tests::toneAmplitude(body, 670.0 * ratio, 48000.0, 3);
            const double off = adx::tests::toneAmplitude(body, 400.0 * ratio, 48000.0, 3);
            CHECK(low > 4.0 * between);
            CHECK(high > 4.0 * between);
            CHECK(low > 4.0 * off);
        }
    }
    SECTION("rim and clave ring only for their windows") {
        for (const DrumModel model : {DrumModel::Rim, DrumModel::Clave}) {
            const InstrumentRun run = hit(model, 4.0F); // decay asks for far too long
            const double head = energy(run.left, 0, 480);
            REQUIRE(head > 1e-3);
            // Rim at most 40 ms to -60 dB, clave 60 ms: nothing audible at 100 ms.
            CHECK(energy(run.left, 4800, 9600) < head * 1e-5);
            CHECK(run.sounding.back() == 0);
        }
        // The ping is at its pitch: the clave higher than the rim.
        const InstrumentRun rim = hit(DrumModel::Rim, 0.4F);
        const InstrumentRun clave = hit(DrumModel::Clave, 0.4F);
        const std::vector<float> r = slice(rim.left, 48, 1024);
        const std::vector<float> c = slice(clave.left, 48, 1024);
        CHECK(adx::tests::toneAmplitude(r, 1700.0, 48000.0, 4) >
              adx::tests::toneAmplitude(r, 2500.0, 48000.0, 4));
        CHECK(adx::tests::toneAmplitude(c, 2500.0, 48000.0, 4) >
              adx::tests::toneAmplitude(c, 1700.0, 48000.0, 4));
    }
    SECTION("the shaker is high-passed noise; tone moves the cutoff") {
        const InstrumentRun dark = hit(DrumModel::Shaker, 0.3F, 0.0F);
        const InstrumentRun bright = hit(DrumModel::Shaker, 0.3F, 1.0F);
        const std::vector<double> spectrum = adx::tests::powerSpectrum(slice(dark.left, 0, 8192));
        const double bin = 48000.0 / 8192.0;
        double below = 0.0;
        double above = 0.0;
        for (std::size_t b = 1; b < spectrum.size(); ++b) {
            (static_cast<double>(b) * bin < 1000.0 ? below : above) += spectrum[b];
        }
        CHECK(below < above * 0.02);
        const std::vector<double> brighter = adx::tests::powerSpectrum(slice(bright.left, 0, 8192));
        double mid = 0.0;
        double midBright = 0.0;
        for (std::size_t b = 0; b < spectrum.size(); ++b) {
            const double hz = static_cast<double>(b) * bin;
            if (hz > 3000.0 && hz < 6000.0) {
                mid += spectrum[b];
                midBright += brighter[b];
            }
        }
        CHECK(midBright < mid); // a 12 kHz cutoff passes less of 3-6 kHz than a 3 kHz one
        // A rise, not a click: the first millisecond is quieter than the fifth.
        CHECK(energy(dark.left, 0, 48) < energy(dark.left, 192, 240));
    }
    SECTION("cymbal and conga sound and end") {
        for (const DrumModel model : {DrumModel::Cymbal, DrumModel::Conga}) {
            const InstrumentRun run = hit(model, 0.3F);
            REQUIRE(run.violations == 0);
            CHECK(energy(run.left, 0, 4800) > 1e-3);
            CHECK(run.sounding.back() == 0);
        }
        // The conga follows its key in a kit; a cymbal does not.
        std::vector<float> kit = instrumentParams("drumsynth");
        kit[instrumentParamIndex("drumsynth", "model")] = static_cast<float>(DrumModel::Kit);
        const InstrumentRun low =
            runInstrument(*preparedInstrument("drumsynth"), {noteOn(0, 1, 62, 120)}, 9600, kit);
        const InstrumentRun high =
            runInstrument(*preparedInstrument("drumsynth"), {noteOn(0, 1, 64, 120)}, 9600, kit);
        CHECK(low.left != high.left);
    }
    SECTION("character 0 is the 808 voicing; 1 changes the kick") {
        const InstrumentRun base = hit(DrumModel::Kick);
        const std::vector<float> p = instrumentParams("drumsynth");
        const InstrumentRun defaults =
            runInstrument(*preparedInstrument("drumsynth"), {noteOn(0, 1, 60, 120)}, 48000, p);
        CHECK(base.left == defaults.left);
        CHECK(hit(DrumModel::Kick, 0.4F, 0.5F, 0.0F, 60, 1.0F).left != base.left);
    }
}
