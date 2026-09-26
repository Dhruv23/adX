// The one curve evaluator, which automation, envelopes, the UI and the renderer all
// share. If it is wrong, they are all wrong together - which is the point.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <string>

#include "engine/core/Curve.h"

using adx::core::Curve;
using adx::core::CurveKind;
using adx::core::kCurveKindCount;

TEST_CASE("curve_endpoints", "[curve]") {
    for (std::size_t i = 0; i < kCurveKindCount; ++i) {
        const auto kind = static_cast<CurveKind>(i);
        INFO("kind " << adx::core::toString(kind));
        Curve curve;
        curve.kind = kind;
        CHECK_THAT(curve.evaluate(0.0F), Catch::Matchers::WithinAbs(0.0, 1e-5));
        CHECK_THAT(curve.evaluate(1.0F), Catch::Matchers::WithinAbs(1.0, 1e-5));

        // And at both extremes of tension, where an exponent of zero or infinity
        // would show up.
        for (const float tension : {-1.0F, -0.5F, 0.5F, 1.0F}) {
            curve.tension = tension;
            CHECK_THAT(curve.evaluate(0.0F), Catch::Matchers::WithinAbs(0.0, 1e-5));
            CHECK_THAT(curve.evaluate(1.0F), Catch::Matchers::WithinAbs(1.0, 1e-5));
        }
    }
}

TEST_CASE("curve_bezier_monotone", "[curve]") {
    Curve curve;
    curve.kind = CurveKind::Bezier;

    float previous = -1.0F;
    for (int i = 0; i <= 1000; ++i) {
        const float t = static_cast<float>(i) / 1000.0F;
        const float value = curve.evaluate(t);
        INFO("t = " << t << " -> " << value);
        REQUIRE(!std::isnan(value));
        REQUIRE(value >= previous - 1e-4F);
        previous = value;
    }

    // The degenerate handles: x at the endpoints makes the Newton slope vanish, which
    // is exactly the case that returns NaN if bisection is not behind it.
    for (const auto& handles : {std::array<float, 4>{0.0F, 0.0F, 0.0F, 1.0F},
                                std::array<float, 4>{1.0F, 0.0F, 1.0F, 1.0F},
                                std::array<float, 4>{0.0F, 1.0F, 1.0F, 0.0F}}) {
        curve.c1x = handles[0];
        curve.c1y = handles[1];
        curve.c2x = handles[2];
        curve.c2y = handles[3];
        for (int i = 0; i <= 100; ++i) {
            const float value = curve.evaluate(static_cast<float>(i) / 100.0F);
            INFO("handles " << handles[0] << ',' << handles[1] << ',' << handles[2] << ','
                            << handles[3]);
            REQUIRE(!std::isnan(value));
        }
    }
}

TEST_CASE("evaluate clamps rather than extrapolating", "[curve]") {
    Curve curve;
    for (std::size_t i = 0; i < kCurveKindCount; ++i) {
        curve.kind = static_cast<CurveKind>(i);
        CHECK(curve.evaluate(-5.0F) == curve.evaluate(0.0F));
        CHECK(curve.evaluate(5.0F) == curve.evaluate(1.0F));
    }
}

TEST_CASE("step holds the start value and hold jumps to the end one", "[curve]") {
    // The distinction is documented in docs/adx-format-v2.md §8 and matters for the
    // v1 shim: v1's `step` returned the *previous* breakpoint's value for the whole
    // segment, which is this Step and not this Hold.
    Curve step;
    step.kind = CurveKind::Step;
    CHECK(step.evaluate(0.5F) == 0.0F);
    CHECK(step.evaluate(0.999F) == 0.0F);
    CHECK(step.evaluate(1.0F) == 1.0F);

    Curve hold;
    hold.kind = CurveKind::Hold;
    CHECK(hold.evaluate(0.0F) == 0.0F);
    CHECK(hold.evaluate(0.001F) == 1.0F);
}

TEST_CASE("tension bends exponential and logarithmic in opposite directions", "[curve]") {
    Curve easeIn;
    easeIn.kind = CurveKind::Exponential;
    Curve easeOut;
    easeOut.kind = CurveKind::Logarithmic;

    // Quadratic at neutral tension, and mirror images of each other.
    CHECK_THAT(easeIn.evaluate(0.5F), Catch::Matchers::WithinAbs(0.25, 1e-5));
    CHECK_THAT(easeOut.evaluate(0.5F), Catch::Matchers::WithinAbs(0.75, 1e-5));

    // Tension -1 degenerates to linear, which is what makes the control's whole range
    // usable rather than half of it.
    easeIn.tension = -1.0F;
    CHECK_THAT(easeIn.evaluate(0.5F), Catch::Matchers::WithinAbs(0.5, 1e-5));
}

TEST_CASE("curve names round-trip", "[curve]") {
    for (std::size_t i = 0; i < kCurveKindCount; ++i) {
        const auto kind = static_cast<CurveKind>(i);
        const char* name = adx::core::toString(kind);
        CurveKind parsed{};
        REQUIRE(adx::core::curveKindFromString(name, std::char_traits<char>::length(name), parsed));
        CHECK(parsed == kind);
    }
    CurveKind ignored{};
    CHECK_FALSE(adx::core::curveKindFromString("wobble", 6, ignored));
}

TEST_CASE("interpolate uses the curve between real endpoints", "[curve]") {
    Curve curve;
    curve.kind = CurveKind::Smooth;
    CHECK_THAT(adx::core::interpolate(curve, 100.0F, 200.0F, 0.5F),
               Catch::Matchers::WithinAbs(150.0, 1e-4));
    CHECK_THAT(adx::core::interpolate(curve, 100.0F, 200.0F, 0.0F),
               Catch::Matchers::WithinAbs(100.0, 1e-4));
}
