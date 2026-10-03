// The corpus through the real instruments and effects.
//
// P3-1: "the master limiter exists and the corpus renders with peak <= 1.0". Every
// example renders to the end with no NaN, no realtime violation and no sample past
// full scale.
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "engine/format/adx/Parser.h"
#include "engine/render/OfflineRender.h"
#include "tests/cpp/Corpus.h"
#include "tests/cpp/render/RenderFixtures.h"

TEST_CASE("corpus_renders_within_full_scale", "[render][corpus]") {
    for (const auto& path : adx::tests::corpusPaths()) {
        INFO(path.string());
        adx::project::Project project;
        adx::project::CommandStack stack;
        adx::format::DiagnosticList diagnostics;
        adx::format::load(adx::tests::readFile(path), project, stack, diagnostics);
        REQUIRE_FALSE(diagnostics.hasErrors());

        adx::render::OfflineRenderOptions options;
        options.tailSeconds = 2.0;
        adx::render::RenderStats stats;
        const std::vector<float> samples = adx::render::renderOffline(project, options, stats);
        REQUIRE(stats.error.empty());
        CHECK(stats.violations == 0);
        bool finite = true;
        for (const float s : samples) {
            finite = finite && std::isfinite(s);
        }
        CHECK(finite);
        CHECK(stats.peak <= 1.0F);
    }
}
