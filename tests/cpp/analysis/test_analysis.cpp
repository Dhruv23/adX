// Tempo and onset detection (phase_8.md §5). The corpora are synthesized, so every
// tempo and every onset time is known exactly rather than hand-labelled: drum loops of
// kick, snare and hat at twenty tempos over noise, and decaying hits of five kinds at
// irregular times and levels.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

#include "engine/analysis/BpmDetector.h"
#include "engine/analysis/OnsetDetector.h"

namespace {

constexpr std::uint32_t kRate = 44100;

/// Adds one drum hit at `at` seconds.
void hit(std::vector<float>& out, double at, int kind, float level, std::mt19937& random) {
    std::uniform_real_distribution<float> noise(-1.0F, 1.0F);
    const auto start = static_cast<std::size_t>(at * kRate);
    // Long enough to have decayed, then a 5 ms fade: a hit cut off short is a transient
    // of its own, and a fair detector reports it.
    constexpr std::array<double, 5> kLengths{0.25, 0.18, 0.06, 0.45, 0.6};
    const double length = kLengths.at(static_cast<std::size_t>(std::clamp(kind, 0, 4)));
    const auto count = static_cast<std::size_t>(length * kRate);
    const auto fade = static_cast<std::size_t>(0.005 * kRate);
    for (std::size_t i = 0; i < count && start + i < out.size(); ++i) {
        const double t = static_cast<double>(i) / kRate;
        double s = 0.0;
        if (kind == 0) {
            // Kick: a sine falling from 120 to 50 Hz.
            const double phase =
                2.0 * std::numbers::pi * ((50.0 * t) + (70.0 * (1.0 - std::exp(-t * 30.0)) / 30.0));
            s = std::sin(phase) * std::exp(-t * 18.0);
        } else if (kind == 1) {
            s = ((0.6 * noise(random)) + (0.4 * std::sin(2.0 * std::numbers::pi * 190.0 * t))) *
                std::exp(-t * 25.0);
        } else if (kind == 2) {
            s = noise(random) * std::exp(-t * 90.0) * 0.5;
        } else if (kind == 3) {
            // A plucked tone.
            s = std::sin(2.0 * std::numbers::pi * 440.0 * t) * std::exp(-t * 12.0);
        } else {
            // A soft-ish pad entry: 3 ms attack.
            s = std::sin(2.0 * std::numbers::pi * 330.0 * t) * std::min(1.0, t / 0.003) *
                std::exp(-t * 6.0);
        }
        const double tail = i + fade >= count ? static_cast<double>(count - i) / fade : 1.0;
        out[start + i] += static_cast<float>(s * tail) * level;
    }
}

std::vector<float> loop(double bpm, std::uint32_t seed) {
    std::mt19937 random(seed);
    std::normal_distribution<float> hiss(0.0F, 0.003F);
    std::vector<float> out(static_cast<std::size_t>(16.0 * kRate));
    for (float& s : out) {
        s = hiss(random);
    }
    const double beat = 60.0 / bpm;
    for (int b = 0; static_cast<double>(b) * beat < 15.5; ++b) {
        hit(out, b * beat, 0, 0.9F, random);
        if ((b % 2) == 1) {
            hit(out, b * beat, 1, 0.5F, random);
        }
        hit(out, (b + 0.5) * beat, 2, 0.3F, random);
    }
    return out;
}

} // namespace

TEST_CASE("bpm_detector_corpus", "[analysis]") {
    const std::vector<double> tempos{72,  80,  87.5, 90,  95,  100, 105, 110, 115, 120,
                                     124, 126, 128,  132, 138, 140, 145, 150, 160, 174};
    REQUIRE(tempos.size() == 20);
    for (std::size_t i = 0; i < tempos.size(); ++i) {
        INFO(tempos[i] << " bpm");
        const std::optional<double> found =
            adx::analysis::estimateBpm(loop(tempos[i], static_cast<std::uint32_t>(i + 1)), kRate);
        REQUIRE(found.has_value());
        CHECK_THAT(*found, Catch::Matchers::WithinRel(tempos[i], 0.01));
    }
    // A one-shot has no tempo, and says so.
    std::vector<float> single(static_cast<std::size_t>(kRate) * 3, 0.0F);
    std::mt19937 random(1);
    hit(single, 0.1, 0, 1.0F, random);
    CHECK_FALSE(adx::analysis::estimateBpm(single, kRate).has_value());
}

TEST_CASE("onset_detector_corpus", "[analysis]") {
    std::size_t truePositives = 0;
    std::size_t detected = 0;
    std::size_t expected = 0;
    for (std::uint32_t seed = 1; seed <= 10; ++seed) {
        std::mt19937 random(seed);
        std::uniform_real_distribution<double> gap(0.12, 0.6);
        std::uniform_int_distribution<int> kind(0, 4);
        std::uniform_real_distribution<float> level(0.15F, 0.9F);
        std::normal_distribution<float> hiss(0.0F, 0.002F);
        std::vector<float> audio(static_cast<std::size_t>(10.0 * kRate));
        for (float& s : audio) {
            s = hiss(random);
        }
        std::vector<double> truth;
        for (double at = 0.2; at < 9.5; at += gap(random)) {
            truth.push_back(at);
            hit(audio, at, kind(random), level(random), random);
        }
        const std::vector<double> found = adx::analysis::detectOnsets(audio, kRate);
        expected += truth.size();
        detected += found.size();
        std::vector<bool> used(found.size(), false);
        for (const double t : truth) {
            for (std::size_t i = 0; i < found.size(); ++i) {
                if (!used[i] && std::abs(found[i] - t) <= 0.010) {
                    used[i] = true;
                    ++truePositives;
                    break;
                }
            }
        }
    }
    const double precision = static_cast<double>(truePositives) / static_cast<double>(detected);
    const double recall = static_cast<double>(truePositives) / static_cast<double>(expected);
    const double f1 = 2.0 * precision * recall / (precision + recall);
    INFO("precision " << precision << " recall " << recall << " over " << expected << " onsets");
    CHECK(f1 > 0.9);
}
