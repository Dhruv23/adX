// engine/dsp/Math.h: the deterministic transcendentals, against the library.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

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
