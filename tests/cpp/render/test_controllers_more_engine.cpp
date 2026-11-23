// The newer controllers, rendered: a formula on a tone's volume, an X-Y pad on it, and a
// peak follower ducking two channels through one macro.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <string>
#include <vector>

#include "tests/cpp/render/RenderFixtures.h"

namespace {

double rms(const std::vector<float>& samples, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t frame = from; frame < to; ++frame) {
        sum += static_cast<double>(samples[frame * 2]) * samples[frame * 2];
    }
    return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(1, to - from)));
}

double correlation(const std::vector<double>& a, const std::vector<double>& b) {
    const auto n = static_cast<double>(a.size());
    const double meanA = std::accumulate(a.begin(), a.end(), 0.0) / n;
    const double meanB = std::accumulate(b.begin(), b.end(), 0.0) / n;
    double cov = 0.0;
    double varA = 0.0;
    double varB = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        cov += (a[i] - meanA) * (b[i] - meanB);
        varA += (a[i] - meanA) * (a[i] - meanA);
        varB += (b[i] - meanB) * (b[i] - meanB);
    }
    return cov / std::sqrt(varA * varB);
}

constexpr const char* kToneProject = R"([PROJECT]
ADX_VERSION=2

[CHANNEL Tone]
INSTRUMENT=testtone
OUTPUT=insert.1

[PATTERN P]
LENGTH=8:0:0
NOTES Tone
  A4 0:0:0 8:0:0 100

[PLAYLIST]
TRACK 1
  PATTERN P 0:0:0

[MIXER]
INSERT 1 name="Master"
)";

} // namespace

TEST_CASE("controller_formula_drives_param", "[render][controllers][formula]") {
    // The same swell the LFO test draws, written as a formula: volume follows
    // 0.25 + 0.25 sin(2 pi t / 4), one cycle a bar at 120 bpm (two seconds).
    auto loaded = adx::tests::loadText(std::string(kToneProject) + R"adx(
[CONTROLLER Swell]
TYPE=formula
TARGET=channel.Tone.volume
FORMULA="sin(2 * pi * t / 4)"
DEPTH=0.25
CENTER=0.25
)adx");
    adx::render::RenderStats stats;
    const std::vector<float> out = adx::render::renderOffline(
        loaded->project, adx::render::OfflineRenderOptions{.frames = std::uint64_t{48000} * 4},
        stats);
    constexpr std::size_t kWindow = 960;
    std::vector<double> measured;
    std::vector<double> expected;
    for (std::size_t from = 0; from + kWindow <= std::size_t{48000} * 4; from += kWindow) {
        measured.push_back(rms(out, from, from + kWindow));
        const double seconds = (static_cast<double>(from) + (kWindow / 2.0)) / 48000.0;
        expected.push_back(2.0 *
                           (0.25 + (0.25 * std::sin(2.0 * std::numbers::pi * seconds / 2.0))));
    }
    const double r = correlation(measured, expected);
    INFO("correlation " << r);
    CHECK(r > 0.99);
}

TEST_CASE("controller_xy_pad_sets_level", "[render][controllers][xy]") {
    // X at 0.25 puts the tone's volume a quarter of the way up its 0..2 range.
    const auto level = [](float x) {
        auto loaded = adx::tests::loadText(std::string(kToneProject) + R"(
[CONTROLLER Pad]
TYPE=xy
TARGET=channel.Tone.volume
X=)" + std::to_string(x) + "\n");
        adx::render::RenderStats stats;
        const std::vector<float> out = adx::render::renderOffline(
            loaded->project, adx::render::OfflineRenderOptions{.frames = 48000}, stats);
        return rms(out, 4800, 43200);
    };
    const double quarter = level(0.25F);
    const double full = level(1.0F);
    INFO("quarter " << quarter << ", full " << full);
    REQUIRE(full > 0.0);
    // Volume's range is logarithmic, so a quarter of the pad is well under full level
    // without being silence.
    CHECK(quarter < full * 0.9);
    CHECK(quarter > 0.0);
}

TEST_CASE("controller_peak_ducks_two_channels_through_a_macro", "[render][controllers][macro]") {
    // The Key's level ducks both pads, through one macro, each its own way.
    auto loaded = adx::tests::loadText(R"([PROJECT]
ADX_VERSION=2

[CHANNEL Key]
INSTRUMENT=testtone
OUTPUT=insert.2

[CHANNEL PadA]
INSTRUMENT=testtone
OUTPUT=insert.3

[CHANNEL PadB]
INSTRUMENT=testtone
OUTPUT=insert.4

[PATTERN P]
LENGTH=2:0:0
NOTES Key
  A4 0:0:0 0:2:0 100
NOTES PadA
  E5 0:0:0 2:0:0 100
NOTES PadB
  C5 0:0:0 2:0:0 100

[PLAYLIST]
TRACK 1
  PATTERN P 0:0:0

[MIXER]
INSERT 1 name="Master"
INSERT 2 name="Key"
INSERT 3 name="A"
INSERT 4 name="B"
ROUTE insert.3 -> insert.1
ROUTE insert.4 -> insert.1

[MACRO Duck]
TARGET channel.PadA.volume range=1..0
TARGET channel.PadB.volume range=1..0

[CONTROLLER Follow]
TYPE=peak
TARGET=macro.Duck
SOURCE=insert.2
CENTER=0
DEPTH=1
GAIN=24
ATTACK=1
RELEASE=20
)");
    adx::render::RenderStats stats;
    const std::vector<float> out = adx::render::renderOffline(
        loaded->project, adx::render::OfflineRenderOptions{.frames = std::uint64_t{48000} * 2},
        stats);
    const double during = rms(out, 4800, 43200); // the key plays 0 .. 1 s: both ducked
    const double after = rms(out, 60000, 96000); // then both come back
    INFO("during " << during << ", after " << after);
    CHECK(after > 0.05);
    CHECK(during < after * 0.05);
}
