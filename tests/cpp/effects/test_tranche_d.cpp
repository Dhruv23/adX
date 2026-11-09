// Phase 12's effects (phase_12.md §4.6, §6). The shared contract - click-free bypass,
// the wet/dry law, clone independence, no allocation, golden hashes - comes from
// test_effect_base.cpp and test_golden_units.cpp through the catalog. These check what
// each one is for.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "engine/core/TempoMap.h"
#include "engine/effects/Clipper.h"
#include "engine/effects/Motion.h"
#include "engine/project/Project.h"
#include "engine/project/TypeCatalog.h"
#include "engine/rt/BlockArena.h"
#include "engine/transport/TimeSource.h"
#include "tests/cpp/dsp/DspTestUtil.h"
#include "tests/cpp/effects/EffectHarness.h"

using adx::tests::kTestRate;
using adx::tests::preparedEffect;
using adx::tests::runEffect;
using adx::tests::slotParamIndex;
using adx::tests::slotParams;
using adx::tests::StereoSignal;
using Catch::Matchers::WithinAbs;

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

std::vector<float> window(const std::vector<float>& x, std::size_t from, std::size_t length) {
    return {x.begin() + static_cast<std::ptrdiff_t>(from),
            x.begin() + static_cast<std::ptrdiff_t>(from + length)};
}

StereoSignal constant(float value, std::size_t frames) {
    return {.left = std::vector<float>(frames, value), .right = std::vector<float>(frames, value)};
}

/// Runs `node` against a time source rolling from tick 0 at `bpm`.
StereoSignal runRolling(adx::graph::SlotNode& node, const StereoSignal& in, std::vector<float> slot,
                        double bpm, std::uint32_t block = 64) {
    adx::core::TempoMap tempo;
    tempo.setTempo(adx::core::Ticks{0}, bpm, false);
    adx::transport::TimeSource time;
    time.bindTempo(tempo.view(), kTestRate);
    time.requestState(adx::transport::PlayState::Playing);
    StereoSignal out{.left = std::vector<float>(in.size()), .right = std::vector<float>(in.size())};
    std::vector<std::byte> storage(1U << 20U);
    adx::rt::BlockArena arena{storage.data(), storage.size()};
    for (std::size_t start = 0; start < in.size(); start += block) {
        const auto count =
            static_cast<std::uint32_t>(std::min<std::size_t>(block, in.size() - start));
        static_cast<void>(time.beginBlock());
        const std::array<std::span<const float>, 2> inputs{
            std::span<const float>{in.left}.subspan(start, count),
            std::span<const float>{in.right}.subspan(start, count)};
        const std::array<std::span<float>, 2> outputs{
            std::span<float>{out.left}.subspan(start, count),
            std::span<float>{out.right}.subspan(start, count)};
        arena.reset();
        adx::graph::ProcessContext context{.time = time,
                                           .outputs = outputs,
                                           .inputs =
                                               std::span<const std::span<const float>>{inputs},
                                           .frames = count,
                                           .sampleRate = kTestRate,
                                           .events = {},
                                           .params = slot,
                                           .automation = {},
                                           .arena = arena};
        node.process(context);
        static_cast<void>(time.advance(count));
    }
    return out;
}

std::size_t peakIndex(const std::vector<float>& x) {
    const auto it = std::ranges::max_element(x, {}, [](float v) { return std::abs(v); });
    return static_cast<std::size_t>(it - x.begin());
}

} // namespace

// --- Clipper -----------------------------------------------------------------------------

TEST_CASE("clipper_transfer", "[effects][clipper]") {
    using adx::effects::ClipMode;
    using adx::effects::clipShape;
    SECTION("each mode's static curve is its formula") {
        for (float x = -4.0F; x <= 4.0F; x += 0.0625F) {
            INFO("x = " << x);
            CHECK_THAT(clipShape(ClipMode::Soft, x), WithinAbs(std::tanh(x), 2e-6));
            CHECK(clipShape(ClipMode::Hard, x) == std::clamp(x, -1.0F, 1.0F));
            CHECK_THAT(clipShape(ClipMode::SineFold, x),
                       WithinAbs(std::sin(x * std::numbers::pi / 2.0), 2e-6));
            // Foldback: a triangle through the origin, period 4, peak 1 at x = 1.
            const double t = std::fmod(std::fmod(x + 1.0, 4.0) + 4.0, 4.0);
            const double fold = t < 2.0 ? t - 1.0 : 3.0 - t;
            CHECK_THAT(clipShape(ClipMode::Foldback, x), WithinAbs(fold, 2e-6));
            const double tube = x >= 0.0F ? std::tanh(x * 1.4) / std::tanh(1.4)
                                          : std::tanh(x * 0.7) / std::tanh(0.7);
            CHECK_THAT(clipShape(ClipMode::AsymTube, x), WithinAbs(tube, 4e-6));
        }
    }
    SECTION("16x oversampling keeps a 10 kHz tone's aliases down") {
        constexpr std::size_t kFrames = 65536;
        const StereoSignal in = adx::tests::sine(10000.0, 0.5F, kFrames);
        for (const float mode : {0.0F, 1.0F}) {
            const auto node = preparedEffect("Clipper");
            const auto run =
                runEffect(*node, in, params("Clipper", {{"mode", mode}, {"drive", 12.0F}}));
            CHECK(run.violations == 0);
            const double dbc =
                adx::tests::aliasingDbc(window(run.out.left, 16384, 32768), 10000.0, kTestRate);
            INFO("mode " << mode << ": aliasing " << dbc << " dBc");
            // The plan's −90 holds for the soft curve; a hard clip's corner keeps
            // harmonics far past 16x's 384 kHz Nyquist (phase_12.md §11).
            CHECK(dbc < (mode == 0.0F ? -90.0 : -60.0));
        }
    }
    SECTION("the ceiling caps the output") {
        const StereoSignal in = adx::tests::sine(100.0, 1.0F, 48000);
        const auto run = runEffect(
            *preparedEffect("Clipper"), in,
            params("Clipper",
                   {{"mode", 1.0F}, {"drive", 24.0F}, {"ceiling", -6.0F}, {"dcBlock", 0.0F}}));
        const auto tail = window(run.out.left, 4800, 40000);
        const float peak = *std::ranges::max_element(tail, {}, [](float v) { return std::abs(v); });
        // The 4x stages' halfbands ring a little on the corners.
        CHECK_THAT(std::abs(peak), WithinAbs(0.5012, 0.03));
    }
}

// --- VolumeShaper ----------------------------------------------------------------------------

TEST_CASE("volume_shaper_sync", "[effects][volumeshaper]") {
    const StereoSignal in = constant(1.0F, 192000);
    for (const double bpm : {90.0, 100.0, 120.0, 150.0}) {
        // A quarter note at each tempo, in samples: 60 / bpm seconds.
        const auto cycle = static_cast<std::size_t>(std::lround(60.0 * kTestRate / bpm));
        for (const float steps : {0.0F, 0xA5A5 * 1.0F}) {
            INFO("bpm " << bpm << ", steps " << steps);
            const auto node = preparedEffect("VolumeShaper");
            const StereoSignal out = runRolling(
                *node, in, params("VolumeShaper", {{"length", 2.0F}, {"steps", steps}}), bpm);
            for (std::size_t i = 4800; i < 4800 + cycle; i += 7) {
                REQUIRE_THAT(out.left[i + cycle], WithinAbs(out.left[i], 1e-5));
                REQUIRE_THAT(out.left[i + (2 * cycle)], WithinAbs(out.left[i], 1e-5));
            }
            if (steps == 0.0F) {
                // The pump: silent on the beat, back to unity by 60 % of it.
                CHECK(out.left[2 * cycle] < 1e-4F);
                CHECK_THAT(out.left[(2 * cycle) + ((cycle * 7) / 10)], WithinAbs(1.0, 1e-5));
            }
        }
    }
    SECTION("stopped, it runs on its own clock at the tempo") {
        const auto run = runEffect(*preparedEffect("VolumeShaper"), in,
                                   params("VolumeShaper", {{"length", 2.0F}}));
        CHECK(run.out.left[24000] < 1e-4F);
        CHECK(run.out.left[48000] < 1e-4F);
        CHECK_THAT(run.out.left[24000 + 18000], WithinAbs(1.0, 1e-5));
    }
}

// --- Filter and Resonator -----------------------------------------------------------------

TEST_CASE("filter_types_shape_the_spectrum", "[effects][filter]") {
    const StereoSignal low = adx::tests::sine(200.0, 0.25F, 65536);
    const StereoSignal high = adx::tests::sine(8000.0, 0.25F, 65536);
    const auto gainOf = [](const StereoSignal& in, float type, double hz) {
        const auto run =
            runEffect(*preparedEffect("Filter"), in,
                      params("Filter", {{"type", type}, {"cutoff", 1000.0F}, {"resonance", 0.0F}}));
        return adx::tests::toneAmplitude(window(run.out.left, 16384, 32768), hz, kTestRate) / 0.25;
    };
    // Low-pass at 1 kHz: 200 Hz through, 8 kHz down by its 12 dB/octave three octaves.
    CHECK(gainOf(low, 1.0F, 200.0) > 0.9);
    CHECK(gainOf(high, 1.0F, 8000.0) < 0.03);
    // High-pass the other way round.
    CHECK(gainOf(low, 3.0F, 200.0) < 0.06);
    CHECK(gainOf(high, 3.0F, 8000.0) > 0.9);
    // The ladder is 24 dB/octave.
    CHECK(gainOf(high, 0.0F, 8000.0) < 0.005);
}

TEST_CASE("filter_lfo_moves_the_cutoff", "[effects][filter]") {
    const StereoSignal in = adx::tests::noise(7, 0.25F, 96000);
    const auto still = runEffect(*preparedEffect("Filter"), in, params("Filter", {}));
    const auto swept = runEffect(*preparedEffect("Filter"), in,
                                 params("Filter", {{"lfo.rate", 2.0F}, {"lfo.depth", 3.0F}}));
    CHECK(still.violations == 0);
    CHECK(swept.violations == 0);
    double difference = 0.0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        difference += std::abs(still.out.left[i] - swept.out.left[i]);
    }
    CHECK(difference / static_cast<double>(in.size()) > 0.01);
}

TEST_CASE("resonator_rings_at_its_note", "[effects][resonator]") {
    const StereoSignal in = adx::tests::impulse(0, 1.0F, 65536);
    const auto run = runEffect(*preparedEffect("Resonator"), in,
                               params("Resonator", {{"feedback", 0.99F}, {"damping", 0.0F}}));
    const std::vector<float> tail = window(run.out.left, 1024, 32768);
    const double c4 = 261.6255653;
    const double atNote = adx::tests::toneAmplitude(tail, c4, kTestRate);
    const double between = adx::tests::toneAmplitude(tail, c4 * 1.5, kTestRate);
    CHECK(atNote > 10.0 * between);
}

// --- Tape, Ensemble, Rotary ------------------------------------------------------------

TEST_CASE("tape_declares_its_delay_as_latency", "[effects][tape]") {
    // Wow and flutter at zero, no saturation: the impulse comes out the centre late, and
    // the dry path is delayed by the same so a 50 % mix lines up.
    const StereoSignal in = adx::tests::impulse(1000, 1.0F, 8192);
    const auto clean =
        params("Tape",
               {{"wow.depth", 0.0F}, {"flutter.depth", 0.0F}, {"saturation", 0.0F}, {"age", 0.0F}});
    const auto wet = runEffect(*preparedEffect("Tape"), in, clean);
    const auto centre = static_cast<std::size_t>(adx::effects::Tape::kCentreSeconds *
                                                 static_cast<double>(kTestRate));
    CHECK(peakIndex(wet.out.left) == 1000 + centre);
    std::vector<float> half = clean;
    half[0] = 0.5F;
    const auto mixed = runEffect(*preparedEffect("Tape"), in, half);
    CHECK(peakIndex(mixed.out.left) == 1000 + centre);
}

TEST_CASE("ensemble_width_zero_is_mono", "[effects][ensemble]") {
    const StereoSignal in = adx::tests::sine(440.0, 0.5F, 48000);
    const auto narrow =
        runEffect(*preparedEffect("Ensemble"), in, params("Ensemble", {{"width", 0.0F}}));
    const auto wide = runEffect(*preparedEffect("Ensemble"), in, params("Ensemble", {}));
    double narrowSide = 0.0;
    double wideSide = 0.0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        narrowSide += std::abs(narrow.out.left[i] - narrow.out.right[i]);
        wideSide += std::abs(wide.out.left[i] - wide.out.right[i]);
    }
    CHECK(narrowSide < 1e-3);
    CHECK(wideSide > 100.0);
}

TEST_CASE("rotary_fast_modulates_faster_than_slow", "[effects][rotary]") {
    // A 2 kHz tone is all horn: its level swings at the horn's rotation rate.
    const StereoSignal in = adx::tests::sine(2000.0, 0.5F, std::size_t{4} * 48000);
    const auto swings = [&](float speed) {
        const auto run = runEffect(*preparedEffect("Rotary"), in,
                                   params("Rotary", {{"speed", speed}, {"ramp", 0.1F}}));
        // The envelope over the last two seconds in 20 ms windows; count its crossings
        // of its own mean.
        std::vector<double> envelope;
        for (std::size_t at = 96000; at + 960 <= run.out.left.size(); at += 960) {
            double energy = 0.0;
            for (std::size_t i = at; i < at + 960; ++i) {
                energy += static_cast<double>(run.out.left[i]) * run.out.left[i];
            }
            envelope.push_back(energy);
        }
        double mean = 0.0;
        for (const double e : envelope) {
            mean += e / static_cast<double>(envelope.size());
        }
        int crossings = 0;
        for (std::size_t i = 1; i < envelope.size(); ++i) {
            if ((envelope[i - 1] < mean) != (envelope[i] < mean)) {
                ++crossings;
            }
        }
        return crossings;
    };
    const int slow = swings(0.0F);
    const int fast = swings(1.0F);
    INFO("slow " << slow << ", fast " << fast);
    // Slow turns at 0.8 Hz and fast at 6.7: eight times as many swings.
    CHECK(fast > 4 * std::max(slow, 1));
}
