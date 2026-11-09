// Fm1Effects (phase_11.5.md §4.7): the FM-1's chain from the native blocks. At its
// defaults it is the identity; each stage changes the signal when its mix is up; it
// allocates nothing; and its output is the same at any block size.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

#include "engine/effects/fm1/Fm1Effects.h"
#include "tests/cpp/effects/EffectHarness.h"

using adx::tests::noise;
using adx::tests::preparedEffect;
using adx::tests::runEffect;
using adx::tests::slotParamIndex;
using adx::tests::slotParams;

namespace {

double rmsDifference(const std::vector<float>& a, const std::vector<float>& b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        sum += (static_cast<double>(a[i]) - b[i]) * (static_cast<double>(a[i]) - b[i]);
    }
    return std::sqrt(sum / static_cast<double>(a.size()));
}

} // namespace

TEST_CASE("fm1_effects_default_is_identity", "[effects][fm1]") {
    const auto in = noise(3, 0.3F, 24000);
    const auto run = runEffect(*preparedEffect("Fm1Effects"), in, slotParams("Fm1Effects"));
    REQUIRE(run.violations == 0);
    REQUIRE(run.out.left == in.left);
    REQUIRE(run.out.right == in.right);
}

TEST_CASE("fm1_effects_each_stage_acts", "[effects][fm1]") {
    const auto in = noise(4, 0.3F, 48000);
    const std::vector<float> base = slotParams("Fm1Effects");
    for (const char* mix : {"dist_mix", "chorus_mix", "phaser_mix", "delay_mix", "reverb_mix"}) {
        INFO(mix);
        std::vector<float> params = base;
        params[slotParamIndex("Fm1Effects", mix)] = 127.0F;
        const auto run = runEffect(*preparedEffect("Fm1Effects"), in, params);
        REQUIRE(run.violations == 0);
        REQUIRE(rmsDifference(run.out.left, in.left) > 1e-3);
        for (const float s : run.out.left) {
            REQUIRE(std::isfinite(s));
        }
    }
    // The filter, closed down as a low-pass, takes the top off.
    std::vector<float> params = base;
    params[slotParamIndex("Fm1Effects", "filter_cutoff")] = 40.0F;
    const auto filtered = runEffect(*preparedEffect("Fm1Effects"), in, params);
    REQUIRE(rmsDifference(filtered.out.left, in.left) > 0.05);
}

TEST_CASE("fm1_effects_block_size_independent", "[effects][fm1]") {
    const auto in = noise(5, 0.3F, 24000);
    std::vector<float> params = slotParams("Fm1Effects");
    for (const char* name : {"dist_mix", "chorus_mix", "delay_mix", "reverb_mix"}) {
        params[slotParamIndex("Fm1Effects", name)] = 64.0F;
    }
    params[slotParamIndex("Fm1Effects", "reverb_predelay")] = 40.0F;
    const auto reference = runEffect(*preparedEffect("Fm1Effects"), in, params, 64);
    for (const std::uint32_t block : {37U, 256U, 1024U}) {
        INFO("block " << block);
        const auto run = runEffect(*preparedEffect("Fm1Effects"), in, params, block);
        REQUIRE(run.out.left == reference.out.left);
    }
}
