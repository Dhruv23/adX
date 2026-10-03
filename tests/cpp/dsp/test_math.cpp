// engine/dsp/Math.h: the deterministic transcendentals, against the library.
#include <catch2/catch_test_macros.hpp>

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
