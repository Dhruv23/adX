// engine/dsp/Math.h: the deterministic transcendentals, against the library.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

#include "engine/dsp/Math.h"

namespace dsp = adx::dsp;

TEST_CASE("dsp_math_matches_library", "[dsp][math]") {
    double worstSin = 0.0;
    double worstTan = 0.0;
    for (int i = -4000; i <= 4000; ++i) {
        const double turns = static_cast<double>(i) * 0.000731;
        worstSin =
            std::max(worstSin, std::abs(dsp::sinTurns(turns) - std::sin(turns * dsp::kTwoPi)));
        const double t = static_cast<double>(i) * 0.0000624; // inside (-0.25, 0.25)
        worstTan = std::max(worstTan, std::abs(dsp::tanTurns(t) - std::tan(t * dsp::kTwoPi)) /
                                          std::max(1.0, std::abs(std::tan(t * dsp::kTwoPi))));
    }
    CHECK(worstSin < 1e-13);
    CHECK(worstTan < 1e-12);

    double worstExp = 0.0;
    double worstLog = 0.0;
    double worstTanh = 0.0;
    for (int i = -3000; i <= 3000; ++i) {
        const double x = static_cast<double>(i) * 0.0331;
        worstExp = std::max(worstExp, std::abs(dsp::exp2(x) - std::exp2(x)) / std::exp2(x));
        worstTanh = std::max(worstTanh, std::abs(dsp::tanh(x * 0.1) - std::tanh(x * 0.1)));
        const double y = std::exp2(x * 0.3);
        worstLog = std::max(worstLog, std::abs(dsp::log2(y) - std::log2(y)));
    }
    CHECK(worstExp < 1e-14);
    CHECK(worstLog < 1e-13);
    CHECK(worstTanh < 1e-14);

    float worstSinF = 0.0F;
    float worstExpF = 0.0F;
    float worstTanhF = 0.0F;
    for (int i = -20000; i <= 20000; ++i) {
        const float turns = static_cast<float>(i) * 0.0001537F;
        worstSinF = std::max(
            worstSinF,
            std::abs(dsp::sinTurnsF(turns) -
                     static_cast<float>(std::sin(static_cast<double>(turns) * dsp::kTwoPi))));
        const float x = static_cast<float>(i) * 0.0019F;
        worstExpF = std::max(worstExpF, std::abs(dsp::exp2F(x) - std::exp2(x)) / std::exp2(x));
        worstTanhF = std::max(worstTanhF, std::abs(dsp::tanhF(x * 0.2F) - std::tanh(x * 0.2F)));
    }
    CHECK(worstSinF < 2e-6F);
    CHECK(worstExpF < 4e-7F);
    CHECK(worstTanhF < 5e-7F);

    CHECK(std::abs(dsp::dbToGain(-6.0) - 0.501187233627) < 1e-12);
    CHECK(std::abs(dsp::gainToDb(0.5) - (-6.020599913279)) < 1e-11);
    CHECK(dsp::gainToDb(0.0) == -200.0);
    CHECK(std::abs(dsp::midiToHz(69.0F) - 440.0F) < 1e-4F);
    CHECK(std::abs(dsp::midiToHz(81.0F) - 880.0F) < 1e-3F);
}

TEST_CASE("dsp_atan2_turns_matches_library", "[dsp][math]") {
    // Every octant, the axes, and the origin: within 1e-10 turns of the library.
    namespace dsp = adx::dsp;
    double worst = 0.0;
    for (int a = 0; a < 3600; ++a) {
        const double angle = (static_cast<double>(a) * 0.1 - 180.0) * dsp::kPi / 180.0;
        for (const double radius : {1e-6, 0.3, 1.0, 1e6}) {
            const double y = radius * std::sin(angle);
            const double x = radius * std::cos(angle);
            const double expected = std::atan2(y, x) / dsp::kTwoPi;
            double error = std::abs(dsp::atan2Turns(y, x) - expected);
            error = std::min(error, std::abs(error - 1.0)); // -1/2 and 1/2 are one angle
            worst = std::max(worst, error);
        }
    }
    CHECK(worst < 1e-10);
    CHECK(dsp::atan2Turns(0.0, 0.0) == 0.0);
    CHECK(dsp::atan2Turns(1.0, 0.0) == 0.25);
    CHECK(dsp::atan2Turns(0.0, -1.0) == 0.5);
}

TEST_CASE("dsp_fast_float_math_matches_library", "[dsp][math]") {
    // The per-sample float versions the optimization pass added (phase_10.md 11): the
    // branchless unit-range sine and cosine, the float atan2 and log2.
    float worstSin = 0.0F;
    float worstCos = 0.0F;
    for (int i = 0; i <= 200000; ++i) {
        const float x = static_cast<float>(i) / 200000.0F; // [0, 1], 1 included
        const double angle = static_cast<double>(x) * dsp::kTwoPi;
        worstSin =
            std::max(worstSin, std::abs(dsp::sinUnitF(x) - static_cast<float>(std::sin(angle))));
        worstCos =
            std::max(worstCos, std::abs(dsp::cosUnitF(x) - static_cast<float>(std::cos(angle))));
    }
    CHECK(worstSin < 1e-6F);
    CHECK(worstCos < 1e-6F);
    CHECK(dsp::sinUnitF(0.0F) == 0.0F);
    CHECK(dsp::sinUnitF(0.25F) > 0.999999F);
    CHECK(dsp::sinUnitF(0.75F) < -0.999999F);

    double worstAtan = 0.0;
    for (int a = 0; a < 3600; ++a) {
        const double angle = (static_cast<double>(a) * 0.1 - 180.0) * dsp::kPi / 180.0;
        for (const double radius : {1e-6, 0.3, 1.0, 1e6}) {
            const auto y = static_cast<float>(radius * std::sin(angle));
            const auto x = static_cast<float>(radius * std::cos(angle));
            const double expected =
                std::atan2(static_cast<double>(y), static_cast<double>(x)) / dsp::kTwoPi;
            double error = std::abs(static_cast<double>(dsp::atan2TurnsF(y, x)) - expected);
            error = std::min(error, std::abs(error - 1.0));
            worstAtan = std::max(worstAtan, error);
        }
    }
    CHECK(worstAtan < 1e-7);
    CHECK(dsp::atan2TurnsF(0.0F, 0.0F) == 0.0F);

    float worstLog = 0.0F;
    for (int i = -3000; i <= 3000; ++i) {
        const float y = std::exp2(static_cast<float>(i) * 0.0331F);
        worstLog = std::max(worstLog, std::abs(dsp::log2F(y) - std::log2(y)));
    }
    CHECK(worstLog < 2e-6F);
    CHECK(dsp::log2F(0.0F) == -150.0F);
    CHECK(std::abs(dsp::gainToDbF(0.5F) - (-6.0206F)) < 1e-4F);
    CHECK(dsp::gainToDbF(0.0F, -120.0F) == -120.0F);
}

TEST_CASE("dsp_log2_bit_path_matches_frexp", "[dsp][math]") {
    // log2 reads the exponent off the bits instead of calling frexp: the same mantissa
    // and exponent, so the same result, bit for bit, for every normal input.
    for (int i = -4000; i <= 4000; ++i) {
        const double x = std::exp2(static_cast<double>(i) * 0.25) * 1.2345;
        int exponent = 0;
        const double mantissa = std::frexp(x, &exponent);
        const auto bits = std::bit_cast<std::uint64_t>(x);
        const auto fromBits =
            std::bit_cast<double>((bits & 0x800FFFFFFFFFFFFFULL) | (1022ULL << 52U));
        REQUIRE(fromBits == mantissa);
        REQUIRE(static_cast<int>((bits >> 52U) & 0x7FFU) - 1022 == exponent);
    }
}
