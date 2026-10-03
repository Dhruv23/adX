// Loudness and true peak (phase_4.md §4.10, §6): validated against EBU TECH 3341.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "engine/dsp/Loudness.h"
#include "engine/graph/nodes/MeterNode.h"
#include "engine/rt/BlockArena.h"
#include "engine/transport/TimeSource.h"

using Catch::Matchers::WithinAbs;

namespace {

struct Stereo {
    std::vector<float> left;
    std::vector<float> right;
};

/// A stereo 1 kHz sine whose level steps through `segments` of (dBFS, seconds), the
/// phase continuous across the steps: how TECH 3341 builds its compliance signals.
/// dBFS as the standard means it - a full-scale sine is 0.
Stereo sequence(std::initializer_list<std::pair<double, double>> segments) {
    constexpr double kRate = 48000.0;
    Stereo out;
    std::size_t n = 0;
    for (const auto& [db, seconds] : segments) {
        const double amplitude = std::pow(10.0, db / 20.0);
        const auto count = static_cast<std::size_t>(std::llround(seconds * kRate));
        for (std::size_t i = 0; i < count; ++i, ++n) {
            out.left.push_back(
                static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * 1000.0 *
                                                        static_cast<double>(n) / kRate)));
        }
    }
    out.right = out.left;
    return out;
}

/// Runs `meter` over `signal` in 512-frame calls, as a callback would.
void feed(adx::dsp::LoudnessMeter& meter, const Stereo& signal) {
    for (std::size_t start = 0; start < signal.left.size(); start += 512) {
        const std::size_t count = std::min<std::size_t>(512, signal.left.size() - start);
        meter.process(std::span<const float>{signal.left}.subspan(start, count),
                      std::span<const float>{signal.right}.subspan(start, count));
    }
}

} // namespace

TEST_CASE("meter_k_weighting_matches_bs1770", "[dsp][loudness]") {
    // BS.1770-4 Table 1 and Table 2: the 48 kHz coefficients, as published.
    adx::dsp::KBiquad shelf;
    adx::dsp::KBiquad highPass;
    adx::dsp::LoudnessMeter::kWeighting(48000, shelf, highPass);
    CHECK_THAT(shelf.b0, WithinAbs(1.53512485958697, 1e-9));
    CHECK_THAT(shelf.b1, WithinAbs(-2.69169618940638, 1e-9));
    CHECK_THAT(shelf.b2, WithinAbs(1.19839281085285, 1e-9));
    CHECK_THAT(shelf.a1, WithinAbs(-1.69065929318241, 1e-9));
    CHECK_THAT(shelf.a2, WithinAbs(0.73248077421585, 1e-9));
    CHECK_THAT(highPass.a1, WithinAbs(-1.99004745483398, 1e-9));
    CHECK_THAT(highPass.a2, WithinAbs(0.99007225036621, 1e-9));
}

TEST_CASE("meter_lufs_matches_reference", "[dsp][loudness]") {
    // EBU TECH 3341 (v4) compliance signals 1-5, each to be read within 0.1 LU.
    adx::dsp::LoudnessMeter meter;
    meter.prepare(48000);

    SECTION("1: -23 dBFS stereo sine reads -23 LUFS momentary, short-term and integrated") {
        feed(meter, sequence({{-23.0, 20.0}}));
        CHECK_THAT(meter.momentary(), WithinAbs(-23.0, 0.1));
        CHECK_THAT(meter.shortTerm(), WithinAbs(-23.0, 0.1));
        CHECK_THAT(meter.integrated(), WithinAbs(-23.0, 0.1));
    }
    SECTION("2: -33 dBFS reads -33") {
        feed(meter, sequence({{-33.0, 20.0}}));
        CHECK_THAT(meter.momentary(), WithinAbs(-33.0, 0.1));
        CHECK_THAT(meter.shortTerm(), WithinAbs(-33.0, 0.1));
        CHECK_THAT(meter.integrated(), WithinAbs(-33.0, 0.1));
    }
    SECTION("3: the relative gate removes the -36 dBFS ends") {
        feed(meter, sequence({{-36.0, 10.0}, {-23.0, 60.0}, {-36.0, 10.0}}));
        CHECK_THAT(meter.integrated(), WithinAbs(-23.0, 0.1));
    }
    SECTION("4: and the absolute gate removes the -72 dBFS ends") {
        feed(meter,
             sequence({{-72.0, 10.0}, {-36.0, 10.0}, {-23.0, 60.0}, {-36.0, 10.0}, {-72.0, 10.0}}));
        CHECK_THAT(meter.integrated(), WithinAbs(-23.0, 0.1));
    }
    SECTION("5: -26/-20/-26 dBFS integrates to -23") {
        feed(meter, sequence({{-26.0, 20.0}, {-20.0, 20.1}, {-26.0, 20.0}}));
        CHECK_THAT(meter.integrated(), WithinAbs(-23.0, 0.1));
    }
    SECTION("silence is silence, and reset forgets") {
        feed(meter, sequence({{-23.0, 5.0}}));
        meter.reset();
        CHECK(meter.integrated() == adx::dsp::kSilentLufs);
        feed(meter, Stereo{.left = std::vector<float>(48000, 0.0F),
                           .right = std::vector<float>(48000, 0.0F)});
        CHECK(meter.momentary() == adx::dsp::kSilentLufs);
        CHECK(meter.integrated() == adx::dsp::kSilentLufs);
    }
}

TEST_CASE("meter_true_peak_catches_intersample", "[dsp][loudness]") {
    // A sine at a quarter of the sample rate, 45 degrees off the sample grid: every
    // sample lands at sin(45) of the peak, so the sample peak reads -3 dB under the true
    // one. Scaled to a +1.2 dBTP true peak, its samples read -1.8 dBFS - a signal a
    // sample-peak meter passes and a true-peak meter must catch.
    const double peak = std::pow(10.0, 1.2 / 20.0);
    std::vector<float> left(48000);
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] =
            static_cast<float>(peak * std::sin((std::numbers::pi / 2.0 * static_cast<double>(i)) +
                                               (std::numbers::pi / 4.0)));
    }
    adx::graph::MeterNode meter;
    meter.setTruePeak(true);
    meter.prepare(adx::graph::PrepareInfo{.sampleRate = 48000, .maxBlockFrames = 2048});
    std::vector<std::byte> storage(1U << 16U);
    adx::rt::BlockArena arena{storage.data(), storage.size()};
    const adx::transport::TimeSource time;
    for (std::size_t start = 0; start < left.size(); start += 256) {
        const auto count =
            static_cast<std::uint32_t>(std::min<std::size_t>(256, left.size() - start));
        const std::array<std::span<const float>, 2> inputs{
            std::span<const float>{left}.subspan(start, count),
            std::span<const float>{left}.subspan(start, count)};
        adx::graph::ProcessContext context{.time = time,
                                           .outputs = {},
                                           .inputs = inputs,
                                           .frames = count,
                                           .sampleRate = 48000,
                                           .events = {},
                                           .params = {},
                                           .automation = {},
                                           .arena = arena};
        meter.process(context);
    }
    adx::rt::LevelFrame frame;
    REQUIRE(meter.ring().readLatest(&frame, 1) == 1);
    CHECK_THAT(20.0 * std::log10(frame.peakLeft), WithinAbs(1.2 - 3.0103, 0.05));
    CHECK_THAT(20.0 * std::log10(frame.truePeak), WithinAbs(1.2, 0.2));

    // A meter that is not the master's does not pay for it.
    adx::graph::MeterNode plain;
    plain.prepare(adx::graph::PrepareInfo{.sampleRate = 48000, .maxBlockFrames = 2048});
    const std::array<std::span<const float>, 2> inputs{std::span<const float>{left}.first(256),
                                                       std::span<const float>{left}.first(256)};
    adx::graph::ProcessContext context{.time = time,
                                       .outputs = {},
                                       .inputs = inputs,
                                       .frames = 256,
                                       .sampleRate = 48000,
                                       .events = {},
                                       .params = {},
                                       .automation = {},
                                       .arena = arena};
    plain.process(context);
    REQUIRE(plain.ring().readLatest(&frame, 1) == 1);
    CHECK(frame.truePeak == 0.0F);
}
