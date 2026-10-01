// Arbitrary routing sums correctly, and sends tap where they say they do.
//
// Gains are powers of two, so every expected value below is exact arithmetic on the
// reference render rather than a tolerance guess.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

#include "tests/cpp/render/RenderFixtures.h"

namespace {

constexpr std::uint64_t kFrames = 24000;

std::vector<float> render(std::string_view text) {
    const auto loaded = adx::tests::loadText(text);
    adx::render::RenderStats stats;
    return adx::tests::renderFrames(loaded->project, 256, kFrames, stats);
}

std::string channelOnly(std::string_view name, std::string_view note) {
    return std::string{R"([PROJECT]
ADX_VERSION=2

[CHANNEL )"} +
           std::string{name} + R"(]
OUTPUT=insert.1

[PATTERN P]
LENGTH=1:0:0
NOTES )" + std::string{name} +
           "\n  " + std::string{note} + R"( 0:0:0 0:2:0 100

[PLAYLIST]
TRACK 1
  PATTERN P 0:0:0

[MIXER]
INSERT 1 name="Master"
)";
}

} // namespace

TEST_CASE("routing_arbitrary_dag", "[graph][routing]") {
    //   A -> 2 -> 3 -> 4 -> master        chain
    //        2 --send--> 4                a send into the middle of it
    //   B -> 5 -> 3                       fan-in to 3
    //        5 ------------> master        fan-out from 5
    const std::vector<float> out = render(R"([PROJECT]
ADX_VERSION=2

[CHANNEL A]
OUTPUT=insert.2

[CHANNEL B]
OUTPUT=insert.5

[PATTERN P]
LENGTH=1:0:0
NOTES A
  C4 0:0:0 0:2:0 100
NOTES B
  E4 0:0:0 0:2:0 100

[PLAYLIST]
TRACK 1
  PATTERN P 0:0:0

[MIXER]
INSERT 1 name="Master"
INSERT 2 gain=0.5
  SEND 1 insert.4 level=0.5
INSERT 3 gain=0.25
INSERT 4 gain=0.125
INSERT 5 gain=0.5
ROUTE insert.2 -> insert.3
ROUTE insert.3 -> insert.4
ROUTE insert.4 -> insert.1
ROUTE insert.5 -> insert.3
ROUTE insert.5 -> insert.1
)");
    const std::vector<float> a = render(channelOnly("A", "C4"));
    const std::vector<float> b = render(channelOnly("B", "E4"));

    // A reaches the master two ways: 2 -> 3 -> 4 (1/2 * 1/4 * 1/8 = 1/64) and
    // 2 -send-> 4 (1/2 * 1/2 * 1/8 = 1/32). B reaches it two ways too: 5 -> 3 -> 4
    // (1/2 * 1/4 * 1/8 = 1/64) and 5 directly (1/2).
    const float gainA = (1.0F / 64.0F) + (1.0F / 32.0F);
    const float gainB = (1.0F / 64.0F) + 0.5F;

    REQUIRE(out.size() == a.size());
    float worst = 0.0F;
    float loudest = 0.0F;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float expected = (gainA * a[i]) + (gainB * b[i]);
        worst = std::max(worst, std::abs(out[i] - expected));
        loudest = std::max(loudest, std::abs(expected));
    }
    INFO("largest error " << worst << " against a peak of " << loudest);
    REQUIRE(loudest > 0.01F);
    // Sums of the same terms in a different order may differ in the last bit or two.
    CHECK(worst <= loudest * 1e-6F);
}

TEST_CASE("sends_pre_post_fader", "[graph][routing]") {
    // One source, two sends: pre-fader to a strip panned hard left, post-fader to one
    // panned hard right. Pulling the source's fader down must leave the left side
    // untouched and scale the right.
    const auto project = [](std::string_view gain) {
        return std::string{R"([PROJECT]
ADX_VERSION=2

[CHANNEL A]
OUTPUT=insert.2

[PATTERN P]
LENGTH=1:0:0
NOTES A
  C4 0:0:0 0:2:0 100

[PLAYLIST]
TRACK 1
  PATTERN P 0:0:0

[MIXER]
INSERT 1 name="Master"
INSERT 2 gain=)"} +
               std::string{gain} + R"(
  SEND 1 insert.3 level=0.5 pre=yes
  SEND 2 insert.4 level=0.5
INSERT 3 pan=-1
INSERT 4 pan=1
ROUTE insert.3 -> insert.1
ROUTE insert.4 -> insert.1
)";
    };

    const std::vector<float> full = render(project("1"));
    const std::vector<float> down = render(project("0.25"));
    REQUIRE(full.size() == down.size());

    float leftPeak = 0.0F;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        const float leftFull = full[frame * 2];
        const float rightFull = full[(frame * 2) + 1];
        leftPeak = std::max(leftPeak, std::abs(leftFull));
        // Pre-fader: identical, bit for bit.
        REQUIRE(down[frame * 2] == leftFull);
        // Post-fader: follows the fader exactly.
        REQUIRE(down[(frame * 2) + 1] == rightFull * 0.25F);
    }
    CHECK(leftPeak > 0.01F);
}
