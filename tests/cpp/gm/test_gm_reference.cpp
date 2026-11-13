// gm against TinySoundFont, an independent SF2 player (phase_12.md §5,
// sf2_matches_reference): the fixture bank's programs rendered by both, pitch within a
// cent and the amplitude envelope within 0.5 dB, measured every 10 ms.
//
// The two players' fixed gains differ (TinySoundFont pans at sqrt(1/2) and scales by
// velocity rather than its square), so each envelope is taken relative to its own
// loudest window; what is compared is the shape the bank describes. Decay and release
// are held to the ratio of the two players' laws, 96 dB to 80 dB per stage time
// (phase_12.md §11, correction 25).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/instruments/gm/GmSetup.h"
#include "tests/cpp/Corpus.h"
#include "tests/cpp/instruments/InstrumentHarness.h"

extern "C" {
#include "tsf.h"
}

namespace fs = std::filesystem;

namespace {

constexpr int kRate = 48000;
constexpr std::size_t kWindow = kRate / 100; // 10 ms

fs::path tinyBank() {
    return adx::tests::repoRoot() / "tests" / "data" / "banks" / "tiny_gm.sf2";
}

/// TinySoundFont's left channel for one note: on at 0, off at `offAt`.
std::vector<float> tsfRender(int bank, int program, int key, std::size_t offAt,
                             std::size_t frames) {
    tsf* font = tsf_load_filename(tinyBank().string().c_str());
    REQUIRE(font != nullptr);
    tsf_set_output(font, TSF_STEREO_INTERLEAVED, kRate, 0.0F);
    const int preset = tsf_get_presetindex(font, bank, program);
    REQUIRE(preset >= 0);
    std::vector<float> stereo(frames * 2, 0.0F);
    tsf_note_on(font, preset, key, 1.0F);
    tsf_render_float(font, stereo.data(), static_cast<int>(offAt), 0);
    tsf_note_off(font, preset, key);
    tsf_render_float(font, stereo.data() + (offAt * 2), static_cast<int>(frames - offAt), 0);
    tsf_close(font);
    std::vector<float> left(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        left[i] = stereo[i * 2];
    }
    return left;
}

std::vector<float> gmRender(bool drums, int key, std::size_t offAt, std::size_t frames) {
    auto node = std::make_shared<adx::instruments::GmInstrument>(
        1, 8, adx::project::VoiceStealMode::OldestReleased);
    std::string error;
    adx::instruments::setGmBank(*node, adx::instruments::GmBankCache::get(tinyBank(), error),
                                tinyBank());
    node->prepare(adx::graph::PrepareInfo{.sampleRate = kRate, .maxBlockFrames = 2048});
    std::vector<float> params = adx::tests::instrumentParams("gm");
    params[adx::tests::instrumentParamIndex("gm", "drums")] = drums ? 1.0F : 0.0F;
    const auto pitch = static_cast<std::uint8_t>(key);
    const auto run = adx::tests::runInstrument(
        *node,
        {adx::tests::noteOn(0, 1, pitch, 127),
         adx::tests::noteOff(static_cast<std::uint32_t>(offAt), 1, pitch)},
        static_cast<std::uint32_t>(frames), params);
    CHECK(run.violations == 0);
    return run.left;
}

/// RMS per 10 ms window, in dB relative to the loudest window.
std::vector<double> envelope(const std::vector<float>& x) {
    std::vector<double> out;
    for (std::size_t at = 0; at + kWindow <= x.size(); at += kWindow) {
        double sum = 0.0;
        for (std::size_t i = at; i < at + kWindow; ++i) {
            sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
        }
        out.push_back(10.0 * std::log10((sum / kWindow) + 1e-30));
    }
    const double peak = *std::ranges::max_element(out);
    for (double& v : out) {
        v -= peak;
    }
    return out;
}

double hertz(const std::vector<float>& x, std::size_t from, std::size_t to) {
    std::vector<double> crossings;
    for (std::size_t i = from + 1; i < to; ++i) {
        if (x[i - 1] < 0.0F && x[i] >= 0.0F) {
            crossings.push_back(static_cast<double>(i - 1) +
                                (static_cast<double>(-x[i - 1]) / (x[i] - x[i - 1])));
        }
    }
    REQUIRE(crossings.size() > 2);
    return kRate * static_cast<double>(crossings.size() - 1) /
           (crossings.back() - crossings.front());
}

} // namespace

namespace {

/// dB per second of a straight-line fit through windows [from, to).
double slope(const std::vector<double>& env, std::size_t from, std::size_t to) {
    double sx = 0.0;
    double sy = 0.0;
    double sxx = 0.0;
    double sxy = 0.0;
    const auto n = static_cast<double>(to - from);
    for (std::size_t w = from; w < to; ++w) {
        const double x = static_cast<double>(w) * 0.01;
        sx += x;
        sy += env[w];
        sxx += x * x;
        sxy += x * env[w];
    }
    return ((n * sxy) - (sx * sy)) / ((n * sxx) - (sx * sx));
}

} // namespace

TEST_CASE("sf2_matches_reference", "[gm][soundfont]") {
    SECTION("the sine, over its keyboard") {
        // attack 0.05 s, hold 0.1 s, decay 0.4 s to -12 dB, release 0.3 s, vibrato 5 Hz
        // +-20 cents after 0.2 s (make_bank_fixtures.py).
        for (const int key : {45, 57, 69, 81}) {
            INFO("key " << key);
            const std::size_t frames = static_cast<std::size_t>(kRate) * 2;
            const auto off = static_cast<std::size_t>(kRate);
            const std::vector<float> ours = gmRender(false, key, off, frames);
            const std::vector<float> theirs = tsfRender(0, 0, key, off, frames);
            // Pitch before the vibrato's 0.2 s delay, and the same through it.
            const double expected = 441.0 * std::pow(2.0, (key - 69) / 12.0);
            const double before = hertz(ours, kRate / 50, kRate / 6);
            CHECK(std::abs(1200.0 * std::log2(before / hertz(theirs, kRate / 50, kRate / 6))) <
                  1.0);
            CHECK(std::abs(1200.0 * std::log2(before / expected)) < 1.0);
            CHECK(std::abs(1200.0 * std::log2(hertz(ours, kRate / 4, kRate) /
                                              hertz(theirs, kRate / 4, kRate))) < 1.0);

            const std::vector<double> a = envelope(ours);
            const std::vector<double> b = envelope(theirs);
            // The attack's ramp and the hold (10-150 ms), and the sustain plateau
            // (450-1000 ms): the same level within 0.5 dB.
            for (std::size_t w = 1; w < 15; ++w) {
                INFO("window " << w * 10 << " ms: gm " << a[w] << ", tsf " << b[w]);
                CHECK(std::abs(a[w] - b[w]) < 0.5);
            }
            for (std::size_t w = 45; w < 100; ++w) {
                INFO("window " << w * 10 << " ms: gm " << a[w] << ", tsf " << b[w]);
                CHECK(std::abs(a[w] - b[w]) < 0.5);
            }
            // Decay and release are exponential in both, at different rates: gm falls 96
            // dB in the stage's time (SF2's 960 cB full scale, as FluidSynth does),
            // TinySoundFont 80 dB (its "mystery slope", ln 10^4). Their slopes are 96:80.
            const double ratio = slope(a, 101, 109) / slope(b, 101, 109);
            INFO("release slopes: gm " << slope(a, 101, 109) << " dB/s, tsf "
                                       << slope(b, 101, 109));
            CHECK(std::abs(ratio - (96.0 / 80.0)) < 0.05);
            CHECK(std::abs(slope(a, 101, 109) - (-96.0 / 0.3)) < 16.0);
        }
    }
    SECTION("the kit's hats") {
        // Noise bursts whose decay is in the sample: the same envelope while they sound.
        for (const int key : {42, 46}) {
            INFO("key " << key);
            const std::size_t frames = kRate;
            const std::vector<double> a = envelope(gmRender(true, key, kRate / 2, frames));
            const std::vector<double> b = envelope(tsfRender(128, 0, key, kRate / 2, frames));
            std::size_t compared = 0;
            for (std::size_t w = 0; w < a.size(); ++w) {
                if (a[w] < -40.0 || b[w] < -40.0) {
                    continue;
                }
                INFO("window " << w * 10 << " ms: gm " << a[w] << ", tsf " << b[w]);
                CHECK(std::abs(a[w] - b[w]) < 0.5);
                ++compared;
            }
            CHECK(compared >= 5);
        }
    }
}
