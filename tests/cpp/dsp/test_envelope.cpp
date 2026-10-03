// engine/dsp/Envelope.h.
//
// phase_4.md §4.1: "sample-exact stage boundaries at 5 sample rates; every CurveKind
// hits 0 and 1 at its endpoints; retrigger from a non-zero level never discontinues."
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "engine/dsp/Envelope.h"

namespace dsp = adx::dsp;
using adx::core::Curve;
using adx::core::CurveKind;

TEST_CASE("dsp_envelope_stage_timing", "[dsp][envelope]") {
    for (const std::uint32_t rate : {22050U, 44100U, 48000U, 96000U, 192000U}) {
        dsp::EnvelopeShape shape;
        shape.attackSeconds = 0.010F;
        shape.decaySeconds = 0.025F;
        shape.sustain = 0.4F;
        shape.releaseSeconds = 0.050F;
        const auto samples = [rate](float seconds) {
            return static_cast<std::uint32_t>(
                std::floor((static_cast<double>(seconds) * rate) + 0.5));
        };
        dsp::Envelope env;
        env.trigger(shape, rate);
        // Attack: exactly attack * rate samples, the last of them exactly 1.
        for (std::uint32_t i = 1; i <= samples(shape.attackSeconds); ++i) {
            const float level = env.next(shape);
            REQUIRE(env.stage() == dsp::EnvStage::Attack);
            if (i == samples(shape.attackSeconds)) {
                REQUIRE(level == 1.0F);
            } else {
                REQUIRE(level < 1.0F);
            }
        }
        for (std::uint32_t i = 1; i <= samples(shape.decaySeconds); ++i) {
            const float level = env.next(shape);
            REQUIRE(env.stage() == dsp::EnvStage::Decay);
            if (i == samples(shape.decaySeconds)) {
                REQUIRE(level == 0.4F);
            }
        }
        CHECK(env.next(shape) == 0.4F);
        CHECK(env.stage() == dsp::EnvStage::Sustain);
        env.release(shape, rate);
        for (std::uint32_t i = 1; i <= samples(shape.releaseSeconds); ++i) {
            static_cast<void>(env.next(shape));
            REQUIRE(env.stage() == dsp::EnvStage::Release);
        }
        CHECK(env.level() == 0.0F);
        CHECK(env.next(shape) == 0.0F);
        CHECK(env.finished());
    }
}

TEST_CASE("dsp_envelope_curves_hit_their_endpoints", "[dsp][envelope]") {
    const std::vector<Curve> curves{
        Curve{.kind = CurveKind::Linear},
        Curve{.kind = CurveKind::Exponential, .tension = 0.8F},
        Curve{.kind = CurveKind::Logarithmic, .tension = -0.5F},
        Curve{.kind = CurveKind::Step},
        Curve{.kind = CurveKind::Smooth},
        Curve{.kind = CurveKind::Bezier, .c1x = 0.9F, .c1y = 0.0F, .c2x = 0.1F, .c2y = 1.0F},
        Curve{.kind = CurveKind::Hold},
    };
    for (const Curve& curve : curves) {
        INFO("curve " << static_cast<int>(curve.kind));
        dsp::EnvelopeShape shape;
        shape.attackSeconds = 0.003F;
        shape.decaySeconds = 0.0F;
        shape.sustain = 1.0F;
        shape.attackCurve = curve;
        dsp::Envelope env;
        env.trigger(shape, 48000);
        float level = 0.0F;
        float lowest = 1.0F;
        for (int i = 0; i < 144; ++i) {
            level = env.next(shape);
            lowest = std::min(lowest, level);
            REQUIRE(level >= 0.0F);
            REQUIRE(level <= 1.0F);
        }
        CHECK(level == 1.0F);
        // Starting from 0: the first sample is at most one step up the curve.
        CHECK(lowest <= 0.5F);
    }
}

TEST_CASE("dsp_envelope_retrigger_is_continuous", "[dsp][envelope]") {
    dsp::EnvelopeShape shape;
    shape.attackSeconds = 0.05F;
    shape.decaySeconds = 0.1F;
    shape.sustain = 0.6F;
    shape.releaseSeconds = 0.2F;
    dsp::Envelope env;
    env.trigger(shape, 48000);
    float previous = 0.0F;
    // Retrigger mid-attack, mid-decay, in sustain and mid-release: never a jump bigger
    // than one attack step.
    const float maxStep = (1.0F / (0.05F * 48000.0F)) + 1e-6F;
    for (int i = 0; i < 48000; ++i) {
        if (i == 1000 || i == 4000 || i == 20000) {
            env.trigger(shape, 48000);
        }
        if (i == 30000) {
            env.release(shape, 48000);
        }
        if (i == 33000) {
            env.trigger(shape, 48000);
        }
        const float level = env.next(shape);
        REQUIRE(std::abs(level - previous) <=
                std::max(maxStep, (0.6F / (0.1F * 48000.0F)) + 1e-6F));
        previous = level;
    }
}

TEST_CASE("dsp_envelope_release_from_attack_does_not_jump", "[dsp][envelope]") {
    // Iteration one's release table started at the sustain level whatever the note
    // was doing. Here a release mid-attack starts where the attack had got to.
    dsp::EnvelopeShape shape;
    shape.attackSeconds = 0.1F;
    shape.sustain = 0.9F;
    shape.releaseSeconds = 0.01F;
    dsp::Envelope env;
    env.trigger(shape, 48000);
    float level = 0.0F;
    for (int i = 0; i < 480; ++i) {
        level = env.next(shape);
    }
    CHECK(std::abs(level - 0.1F) < 1e-3F);
    env.release(shape, 48000);
    const float first = env.next(shape);
    CHECK(first < level);
    CHECK(first > level - (0.1F / 480.0F) - 1e-6F);
}
