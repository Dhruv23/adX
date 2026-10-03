// PDC with a real latent effect (phase_4.md §6, `pdc_compensates_limiter`).
//
// Phase 3 proved PDC on hand-built latency nodes; this proves it on the thing it is
// for. Two identical channels feed two parallel inserts into the master: one with a
// lookahead Limiter on it, one with nothing. The signal sits far below the ceiling, so
// the Limiter is the identity apart from its latency - and with that latency
// compensated, each path rendered alone is the other, sample for sample. Without PDC
// they would differ by the Limiter's lookahead.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "engine/project/Project.h"
#include "engine/render/OfflineRender.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace {

adx::project::Project parallelLimiterProject(bool muteLimited, bool muteDry) {
    // Quiet notes: velocity 20 keeps the test tone near -16 dBFS, far from the ceiling.
    adx::project::Project project =
        adx::tests::toneProject({{.start = 0, .length = 960, .pitch = 69, .velocity = 20},
                                 {.start = 1200, .length = 480, .pitch = 76, .velocity = 20}});
    const adx::core::InsertId master = project.mixer.master;
    adx::project::Channel second = project.channels.front();

    adx::project::Insert limited;
    limited.id = project.newInsertId();
    limited.name = "Limited";
    adx::project::Slot limiter;
    limiter.id = project.newSlotId();
    limiter.type = "Limiter";
    limiter.params = {adx::project::SlotParam{.name = "lookahead", .value = 5.0},
                      adx::project::SlotParam{.name = "ceiling", .value = 0.0}};
    limited.slots.push_back(limiter);
    adx::project::Insert dry;
    dry.id = project.newInsertId();
    dry.name = "Dry";
    project.mixer.inserts.push_back(limited);
    project.mixer.inserts.push_back(dry);
    project.mixer.routes.push_back(
        adx::project::Route{.id = project.newRouteId(), .from = limited.id, .to = master});
    project.mixer.routes.push_back(
        adx::project::Route{.id = project.newRouteId(), .from = dry.id, .to = master});

    project.channels.front().output = limited.id;
    project.channels.front().muted = muteLimited;
    second.id = project.newChannelId();
    second.name = "ToneDry";
    second.output = dry.id;
    second.muted = muteDry;
    project.channels.push_back(second);
    // The second channel plays the same pattern: give it its own clip of the same notes.
    adx::project::NoteClip clip = project.patterns.front().noteClips.front();
    clip.channel = second.id;
    project.patterns.front().noteClips.push_back(clip);
    return project;
}

std::vector<float> render(const adx::project::Project& project) {
    adx::render::OfflineRenderOptions options;
    options.frames = 48000;
    adx::render::RenderStats stats;
    std::vector<float> out = adx::render::renderOffline(project, options, stats);
    REQUIRE(stats.error.empty());
    return out;
}

} // namespace

TEST_CASE("pdc_compensates_limiter", "[render][pdc][effects]") {
    const std::vector<float> limitedAlone = render(parallelLimiterProject(false, true));
    const std::vector<float> dryAlone = render(parallelLimiterProject(true, false));
    REQUIRE(limitedAlone.size() == dryAlone.size());

    // The signal is there at all, and late by the same amount on both paths.
    std::size_t firstLimited = 0;
    while (firstLimited < limitedAlone.size() && limitedAlone[firstLimited] == 0.0F) {
        ++firstLimited;
    }
    std::size_t firstDry = 0;
    while (firstDry < dryAlone.size() && dryAlone[firstDry] == 0.0F) {
        ++firstDry;
    }
    REQUIRE(firstLimited < limitedAlone.size());
    CHECK(firstLimited == firstDry);
    // 5 ms of lookahead at 48 kHz, interleaved stereo: at least 240 frames late.
    CHECK(firstDry / 2 >= 240);

    // And identical throughout: the Limiter, below its ceiling, is the identity.
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < limitedAlone.size(); ++i) {
        mismatches += limitedAlone[i] == dryAlone[i] ? 0 : 1;
    }
    CHECK(mismatches == 0);
}
