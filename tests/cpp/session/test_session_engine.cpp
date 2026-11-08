// The session through the engine (phase_11.md §5): launch_quantize_exact,
// instant_launch_next_sample, scene_fires_column, stop_quantized,
// clip_stop_preserves_other_clips_voices, follow_actions (engine half),
// relaunch_before_boundary, concurrent_clip_limit.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/session/ClipState.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::core::kPpq;
using adx::session::ClipState;

namespace {

constexpr std::uint64_t kBlock = 256;

/// A set: channels Lead and Bass (testtone), pattern A (one beat-long note at 0, one bar
/// long) and B (a note at 0 and on beat 3, two bars), and a session of two tracks.
std::string setText(double bpm, const std::string& quantize, const std::string& extraCell = "",
                    const std::string& playlist = "") {
    return "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 " + std::to_string(bpm) +
           "\n\n[CHANNEL Lead]\nINSTRUMENT=testtone\nOUTPUT=insert.1\n"
           "\n[CHANNEL Bass]\nINSTRUMENT=testtone\nOUTPUT=insert.1\n"
           "\n[PATTERN A]\nLENGTH=1:0:0\nNOTES Lead\n  C4 0:0:0 0:1:0 100\nNOTES Bass\n  C2 0:0:0 "
           "0:1:0 100\n"
           "\n[PATTERN B]\nLENGTH=2:0:0\nNOTES Lead\n  E4 0:0:0 0:0:960 100\n  E4 0:2:0 0:0:960 "
           "100\n"
           "NOTES Bass\n  E2 0:0:0 0:0:960 100\n"
           "\n[PATTERN HOLD]\nLENGTH=8:0:0\nNOTES Lead\n  G3 0:0:0 8:0:0 100\n"
           "\n[PLAYLIST]\nTRACK 1 name=\"Lead\"\n" +
           playlist +
           "TRACK 2 name=\"Bass\"\n"
           "\n[MIXER]\nINSERT 1 name=\"Master\"\n"
           "\n[SESSION]\nQUANTIZE=" +
           quantize +
           "\nSCENE \"One\"\nSCENE \"Two\"\n"
           "TRACK Lead linked=1\n  1 A\n  2 B" +
           extraCell +
           "\n"
           "TRACK Bass linked=2\n  1 A\n";
}

std::vector<std::size_t> onsets(const std::vector<float>& out) {
    std::vector<std::size_t> found;
    std::size_t silent = 1000;
    for (std::size_t f = 0; f < out.size() / 2; ++f) {
        const bool now = out[f * 2] != 0.0F || out[(f * 2) + 1] != 0.0F;
        if (now && silent > 100) {
            found.push_back(f);
        }
        silent = now ? 0 : silent + 1;
    }
    return found;
}

std::int64_t samplesAt(std::int64_t tick, double bpm) {
    return std::llround(static_cast<double>(tick) * 60.0 / (bpm * kPpq) * 48000.0);
}

struct Set {
    std::unique_ptr<adx::tests::Loaded> loaded;
    adx::tests::OfflineRig rig{static_cast<std::uint32_t>(kBlock)};
    std::vector<float> out;

    explicit Set(const std::string& text) : loaded(adx::tests::loadText(text)) {
        REQUIRE(rig.engine->setProject(loaded->project, 1).rebuilt);
    }
    void render(std::uint64_t frames) {
        rig.render(frames, out);
    }
    [[nodiscard]] ClipState state(std::uint32_t track, std::uint32_t row) const {
        return rig.engine->core().session().state(track, row);
    }
};

} // namespace

TEST_CASE("launch_quantize_exact", "[session][engine]") {
    // The arrangement plays (an empty playlist); a launch requested at a block start lands
    // on the next boundary of its quantize, on the exact sample, at four tempos.
    for (const double bpm : {90.0, 120.0, 140.0, 174.0}) {
        for (const auto& [name, step] : std::vector<std::pair<std::string, std::int64_t>>{
                 {"1/4", kPpq}, {"1/2", kPpq * 2}, {"bar", kPpq * 4}, {"2bar", kPpq * 8}}) {
            INFO(bpm << " bpm, quantize " << name);
            Set set{setText(bpm, name)};
            set.rig.engine->play();
            set.render(kBlock * 37); // somewhere inside the first bar
            const std::uint64_t requested = kBlock * 37;
            set.rig.engine->launchClip(0, 0);
            set.render(48000ULL * 6);
            const auto tick = static_cast<std::int64_t>(
                std::floor(static_cast<double>(requested) / 48000.0 * (bpm / 60.0) * kPpq));
            const std::int64_t boundary = ((tick + step - 1) / step) * step;
            const auto expected = static_cast<std::size_t>(samplesAt(boundary, bpm));
            const std::vector<std::size_t> found = onsets(set.out);
            REQUIRE_FALSE(found.empty());
            CHECK(found.front() == expected);
            CHECK(set.state(0, 0) == ClipState::Playing);
        }
    }
}

TEST_CASE("instant_launch_next_sample", "[session][engine]") {
    // Instant, with nothing playing: the session's own clock starts and the clip with it,
    // at the first frame of the next block.
    Set set{setText(120.0, "instant")};
    set.render(kBlock * 10);
    set.rig.engine->launchClip(0, 0);
    set.render(48000);
    const std::vector<std::size_t> found = onsets(set.out);
    REQUIRE_FALSE(found.empty());
    CHECK(found.front() == kBlock * 10);
}

TEST_CASE("scene_fires_column", "[session][engine]") {
    // Both tracks' cells in row 1 start on the same sample: the next bar.
    Set set{setText(120.0, "bar")};
    set.rig.engine->play();
    set.render(kBlock * 50);
    set.rig.engine->fireScene(0);
    set.render(96000ULL * 2);
    CHECK(set.state(0, 0) == ClipState::Playing);
    CHECK(set.state(1, 0) == ClipState::Playing);
    const std::vector<std::size_t> found = onsets(set.out);
    REQUIRE_FALSE(found.empty());
    CHECK(found.front() == 96000); // bar 2 at 120 bpm
    // Row 2 has no Bass cell: firing it stops Bass (stop on empty) and moves Lead to B.
    set.rig.engine->fireScene(1);
    set.render(96000ULL * 2);
    CHECK(set.state(0, 1) == ClipState::Playing);
    CHECK(set.state(0, 0) == ClipState::Stopped);
    CHECK(set.state(1, 0) == ClipState::Stopped);
}

TEST_CASE("stop_quantized", "[session][engine]") {
    // A stop waits for the boundary (QueuedStop until then), then the clip releases.
    Set set{setText(120.0, "bar")};
    set.rig.engine->play();
    set.rig.engine->launchClip(0, 0);
    set.render(96000 + (kBlock * 20)); // playing, part way into its second pass
    REQUIRE(set.state(0, 0) == ClipState::Playing);
    set.rig.engine->stopClip(0);
    set.render(kBlock);
    CHECK(set.state(0, 0) == ClipState::QueuedStop);
    set.render(96000);
    CHECK(set.state(0, 0) == ClipState::Stopped);
    // A pass begins on every bar while it plays; after the stop at bar 3, none does.
    const std::vector<std::size_t> found = onsets(set.out);
    CHECK(found == std::vector<std::size_t>{0, 96000});
    set.out.clear();
    set.render(96000ULL * 2);
    CHECK(onsets(set.out).empty());
}

TEST_CASE("relaunch_before_boundary", "[session][engine]") {
    // Two launches before the boundary: the second replaces the first, and only it plays.
    Set set{setText(120.0, "bar")};
    set.rig.engine->play();
    set.render(kBlock * 4);
    set.rig.engine->launchClip(0, 0);
    set.render(kBlock);
    CHECK(set.state(0, 0) == ClipState::Queued);
    set.rig.engine->launchClip(0, 1);
    set.render(kBlock);
    CHECK(set.state(0, 0) == ClipState::Stopped);
    CHECK(set.state(0, 1) == ClipState::Queued);
    set.render(96000ULL * 2);
    CHECK(set.state(0, 1) == ClipState::Playing);
    CHECK(set.state(0, 0) == ClipState::Stopped);
}

TEST_CASE("clip_stop_preserves_other_clips_voices", "[session][engine]") {
    // The arrangement holds a long G3 on Lead; a session clip on Lead starts and stops.
    // The arrangement's note sounds throughout: the stop released only the clip's voices.
    Set set{setText(120.0, "instant", "", "  PATTERN HOLD 0:0:0\n")};
    set.rig.engine->play();
    set.render(kBlock * 8);
    set.rig.engine->launchClip(0, 0);
    set.render(48000);
    set.rig.engine->stopClip(0);
    set.render(48000ULL * 2);
    // Never silent from the first frame to the end: the held note never stopped.
    std::size_t longestSilence = 0;
    std::size_t run = 0;
    for (std::size_t f = 0; f < set.out.size() / 2; ++f) {
        run = set.out[f * 2] == 0.0F ? run + 1 : 0;
        longestSilence = std::max(longestSilence, run);
    }
    CHECK(longestSilence < 100);
    CHECK(set.state(0, 0) == ClipState::Stopped);
}

TEST_CASE("follow_actions_through_the_engine", "[session][engine]") {
    // A at row 1 with follow=next: after one pass (a bar) B starts on its own.
    Set set{
        setText(120.0, "bar")
            .replace(setText(120.0, "bar").find("  1 A\n  2 B"), 11, "  1 A follow=next\n  2 B")};
    REQUIRE(set.loaded->project.session.tracks[0]
                .slots.at(0)
                .value_or(adx::project::ClipSlot{})
                .follow == adx::session::FollowAction::Next);
    set.rig.engine->play();
    set.rig.engine->launchClip(0, 0);
    set.render(96000 + (kBlock * 8));
    CHECK(set.state(0, 1) == ClipState::Playing);
    CHECK(set.state(0, 0) == ClipState::Stopped);
}

TEST_CASE("time_sources_return_after_the_tail", "[session][engine]") {
    // A stopped clip keeps its source for its release, then gives it back.
    Set set{setText(120.0, "instant")};
    set.rig.engine->play();
    set.rig.engine->launchClip(0, 0);
    set.render(48000);
    const std::uint32_t inUse = set.rig.engine->transport().inUseCount();
    CHECK(inUse == 2); // the arrangement and the clip
    // A test tone has no release; test_release.cpp holds a VA through its tail.
    set.rig.engine->stopClip(0);
    set.render(48000);
    CHECK(set.rig.engine->transport().inUseCount() == 1);
    CHECK(set.rig.engine->core().session().active() == 0);
}

TEST_CASE("follow_into_empty_and_stop_beats_follow", "[session][engine]") {
    // Lead has A (follow=next) in row 1 and nothing in row 2 on the Bass track: Bass's A
    // follows into an empty cell, which stops it at the end of its pass.
    {
        std::string text = setText(120.0, "bar");
        text.replace(text.find("TRACK Bass linked=2\n  1 A\n"), 26,
                     "TRACK Bass linked=2\n  1 A follow=next\n");
        Set set{text};
        set.rig.engine->play();
        set.rig.engine->launchClip(1, 0);
        set.render(96000 + (kBlock * 8));
        CHECK(set.state(1, 0) == ClipState::Stopped);
        CHECK(set.rig.engine->core().session().active() <= 1); // released, or releasing
    }
    // A user's stop, waiting for the bar, is not undone by the clip's follow action there.
    {
        std::string text = setText(120.0, "bar");
        text.replace(text.find("  1 A\n  2 B"), 11, "  1 A follow=next\n  2 B");
        Set set{text};
        set.rig.engine->play();
        set.rig.engine->launchClip(0, 0);
        set.render(kBlock * 40);
        set.rig.engine->stopClip(0);
        set.render(96000);
        CHECK(set.state(0, 0) == ClipState::Stopped);
        CHECK(set.state(0, 1) == ClipState::Stopped); // B never followed
    }
}
