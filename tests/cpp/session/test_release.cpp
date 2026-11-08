// Stopping, tails, tempo and limits (phase_11.md §5): clip_stop_releases_not_cuts,
// clip_stop_preserves_effect_tail, time_source_released_after_tail,
// relaunch_legato_keeps_phase, launch_across_tempo_change, scene_tempo_change,
// concurrent_clip_limit, mini_clip_cycles_independently, clip gain, rate following,
// tap through the engine, session_no_alloc.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/rt/AllocGuard.h"
#include "engine/rt/Violation.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::core::kPpq;
using adx::session::ClipState;

namespace {

constexpr std::uint64_t kBlock = 256;

/// One channel `Lead` playing `instrument` (with `params`), a held one-bar note in
/// pattern A and two quarter notes in B, the master insert holding `slot`, and a session
/// of one track with A and B.
std::string leadSet(const std::string& instrument, const std::string& params,
                    const std::string& slot = "", const std::string& cellA = "1 A",
                    const std::string& tempo = "0:0:0 120", const std::string& scenes = "") {
    return "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n" + tempo +
           "\n\n[CHANNEL Lead]\nINSTRUMENT=" + instrument + "\nOUTPUT=insert.1\n" + params +
           "\n[PATTERN A]\nLENGTH=1:0:0\nNOTES Lead\n  A4 0:0:0 1:0:0 100\n"
           "\n[PATTERN B]\nLENGTH=1:0:0\nNOTES Lead\n  C5 0:0:0 0:1:0 100\n  C5 0:2:0 0:1:0 "
           "100\n"
           "\n[PLAYLIST]\nTRACK 1 name=\"Lead\"\n"
           "\n[MIXER]\nINSERT 1 name=\"Master\"\n" +
           slot + "\n[SESSION]\nQUANTIZE=bar\n" +
           (scenes.empty() ? std::string{"SCENE \"One\"\nSCENE \"Two\"\n"} : scenes) +
           "TRACK Lead linked=1\n  " + cellA + "\n  2 B\n";
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
    [[nodiscard]] std::size_t frames() const {
        return out.size() / 2;
    }
    [[nodiscard]] adx::session::SessionEngine& session() const {
        return rig.engine->core().session();
    }
};

float peakIn(const std::vector<float>& out, std::size_t from, std::size_t to) {
    float peak = 0.0F;
    for (std::size_t f = from; f < to && (f * 2) + 1 < out.size(); ++f) {
        peak = std::max({peak, std::abs(out[f * 2]), std::abs(out[(f * 2) + 1])});
    }
    return peak;
}

float largestJump(const std::vector<float>& out, std::size_t from, std::size_t to) {
    float jump = 0.0F;
    for (std::size_t f = std::max<std::size_t>(from, 1); f < to && (f * 2) < out.size(); ++f) {
        jump = std::max(jump, std::abs(out[f * 2] - out[(f - 1) * 2]));
    }
    return jump;
}

std::vector<std::size_t> onsets(const std::vector<float>& out, std::size_t gap = 100) {
    std::vector<std::size_t> found;
    std::size_t silent = gap + 1;
    for (std::size_t f = 0; f < out.size() / 2; ++f) {
        const bool now = out[f * 2] != 0.0F || out[(f * 2) + 1] != 0.0F;
        if (now && silent > gap) {
            found.push_back(f);
        }
        silent = now ? 0 : silent + 1;
    }
    return found;
}

const std::string kSlowRelease = "PARAM env.release=0.4\nPARAM env.attack=0.002\n";

} // namespace

TEST_CASE("clip_stop_releases_not_cuts", "[session][release]") {
    // A held VA note, stopped mid-note on a bar: it decays through its release, and no
    // sample-to-sample step at the stop is larger than the waveform's own.
    Set set{leadSet("va", kSlowRelease, "", "1 A length=2:0:0")};
    set.rig.engine->play();
    set.rig.engine->launchClip(0, 0);
    set.render(96000 - (kBlock * 8)); // the clip starts on bar 1, plays to near bar 2
    set.rig.engine->stopClip(0);      // stops on bar 2: frame 96000
    set.render(48000);
    const std::size_t stop = 96000;
    const float before = peakIn(set.out, stop - 4800, stop);
    REQUIRE(before > 0.05F);
    // Still sounding 50 ms after the stop, at more than a tenth of its level.
    CHECK(peakIn(set.out, stop + 2400, stop + 4800) > 0.1F * before);
    // Silent well after the 0.4 s release.
    CHECK(peakIn(set.out, stop + 43000, stop + 48000) < 1e-3F);
    const float ownJump = largestJump(set.out, stop - 4800, stop - 16);
    CHECK(largestJump(set.out, stop - 16, stop + 16) <= std::max(0.01F, ownJump * 1.5F));
}

TEST_CASE("time_source_released_after_tail", "[session][release]") {
    // The source of a stopped clip returns to the pool only once its last voice is
    // silent: held through the VA's 0.4 s release, back within a few blocks of its end.
    Set set{leadSet("va", kSlowRelease)};
    set.rig.engine->play();
    set.rig.engine->launchClip(0, 0);
    set.render(48000);
    CHECK(set.rig.engine->transport().inUseCount() == 2);
    set.rig.engine->stopClip(0);
    set.render(96000 - 48000 + (kBlock * 2)); // past the stop on bar 2
    CHECK(set.rig.engine->transport().inUseCount() == 2);
    set.render(48000 / 5); // 0.2 s into the release: still sounding
    CHECK(set.rig.engine->transport().inUseCount() == 2);
    CHECK(peakIn(set.out, set.frames() - 256, set.frames()) > 1e-4F);
    set.render(48000);
    CHECK(set.rig.engine->transport().inUseCount() == 1);
    CHECK(set.session().active() == 0);
}

TEST_CASE("clip_stop_preserves_effect_tail", "[session][release]") {
    // A short note into a reverb: after the clip stops, the room keeps ringing for well
    // over a second.
    Set set{leadSet("testtone", "", "  SLOT 1 Reverb mix=0.6 room=0.9 damp=0.2\n",
                    "1 B quantize=instant")};
    set.rig.engine->play();
    set.rig.engine->launchClip(0, 0);
    set.render(kBlock * 40);
    set.rig.engine->stopClip(0);
    set.render(48000ULL * 2);
    const std::size_t stop = kBlock * 41;
    CHECK(peakIn(set.out, stop + 48000, stop + 52800) > 1e-3F);
}

TEST_CASE("relaunch_legato_keeps_phase", "[session][release]") {
    // B: a beat-long note on beats 1 and 3. Relaunched during beat 1 with 1/4 quantize,
    // the relaunch lands on beat 2: with legato the clip carries on, so its beat-3 note
    // comes at frame 48000; without, it restarts on beat 2 and that note moves a beat on.
    for (const bool legato : {true, false}) {
        INFO("legato " << legato);
        Set set{leadSet("testtone", "", "",
                        std::string{"1 A\n  3 B quantize=1/4"} + (legato ? " legato=yes" : ""),
                        "0:0:0 120", "SCENE \"a\"\nSCENE \"b\"\nSCENE \"c\"\n")};
        set.rig.engine->play();
        set.rig.engine->launchClip(0, 2); // B, on beat 1 of bar 1... (1/4: at once, tick 0)
        set.render(kBlock * 4);
        set.rig.engine->launchClip(0, 2); // relaunch: beat 2
        set.render(96000);
        const std::vector<std::size_t> found = onsets(set.out);
        REQUIRE(found.size() >= 2);
        CHECK(found[0] == 0);
        CHECK(found[1] == (legato ? 48000U : 72000U));
        CHECK(set.session().state(0, 2) == ClipState::Playing);
    }
}

TEST_CASE("launch_across_tempo_change", "[session][release]") {
    // 120 bpm for a bar, then 60: a launch queued before the change lands on bar 3 at
    // the slower tempo's sample, and plays at the slower tempo.
    Set set{leadSet("testtone", "", "", "1 B", "0:0:0 120\n1:0:0 60")};
    set.rig.engine->play();
    set.render(kBlock * 10);
    set.rig.engine->launchClip(0, 1); // B on row 2 -> the next bar: bar 2 (tick 4 * kPpq)
    set.render(96000 + (192000 * 2));
    const std::vector<std::size_t> found = onsets(set.out);
    REQUIRE(found.size() >= 2);
    CHECK(found[0] == 96000);         // bar 2, where 60 bpm starts
    CHECK(found[1] == 96000 + 96000); // beat 3 at 60 bpm: two seconds later
}

TEST_CASE("scene_tempo_change", "[session][release]") {
    // Scene Two says 60 bpm: firing it halves the master rate from its boundary on, and
    // the clip it launches plays at that tempo.
    Set set{
        leadSet("testtone", "", "", "1 A", "0:0:0 120", "SCENE \"One\"\nSCENE \"Two\" tempo=60\n")};
    set.rig.engine->play();
    set.render(kBlock * 10);
    set.rig.engine->fireScene(1);
    set.render(96000 + (48000 * 4));
    CHECK(std::abs(set.session().rate() - 0.5) < 1e-9);
    const std::vector<std::size_t> found = onsets(set.out);
    REQUIRE(found.size() >= 2);
    CHECK(found[0] == 96000);
    // Beat 3 of B, two beats at 60 bpm: within a block of two seconds on.
    const auto late = static_cast<std::int64_t>(found[1]) - (96000 + 96000);
    CHECK(std::abs(late) <= static_cast<std::int64_t>(kBlock));
}

TEST_CASE("clips_follow_the_master_rate", "[session][release]") {
    // A nudge to 1.5x after B starts: its beat-3 note comes 1.5x sooner; the rate
    // returns exactly when the nudge ends.
    Set set{leadSet("testtone", "", "", "1 B quantize=instant")};
    set.rig.engine->play();
    set.rig.engine->launchClip(0, 0);
    set.render(kBlock);
    set.rig.engine->setSessionRate(1.5);
    set.render(48000ULL * 2);
    const std::vector<std::size_t> found = onsets(set.out);
    REQUIRE(found.size() >= 2);
    const double expected = static_cast<double>(kBlock) + ((48000.0 - kBlock) / 1.5);
    CHECK(std::abs(static_cast<double>(found[1]) - expected) <= 2.0);
    set.rig.engine->setSessionRate(1.0);
    set.render(kBlock);
    CHECK(set.session().rate() == 1.0);
}

TEST_CASE("tap_tempo_through_the_engine", "[session][release]") {
    // Four taps half a second apart on a 60 bpm map: 120 bpm, so the rate becomes 2.
    Set set{leadSet("testtone", "", "", "1 A", "0:0:0 60")};
    set.rig.engine->play();
    for (int i = 0; i < 4; ++i) {
        set.rig.engine->tapTempo(10.0 + (0.5 * i));
        set.render(kBlock);
    }
    CHECK(std::abs(set.session().tappedBpm() - 120.0) < 1e-6);
    CHECK(std::abs(set.session().rate() - 2.0) < 1e-6);
}

TEST_CASE("clip_gain_scales_velocity", "[session][release]") {
    // A cell at gain 0.5, and the same launched with a pad's 0.5 on top: each quieter
    // than the one before (testtone's level follows velocity).
    std::array<float, 3> peaks{};
    const std::array<std::string, 3> cells{"1 A quantize=instant", "1 A quantize=instant gain=0.5",
                                           "1 A quantize=instant gain=0.5"};
    for (std::size_t i = 0; i < 3; ++i) {
        Set set{leadSet("testtone", "", "", cells.at(i))};
        set.rig.engine->play();
        set.rig.engine->launchClip(0, 0, i == 2 ? 0.5F : 0.0F);
        set.render(4800);
        peaks.at(i) = peakIn(set.out, 0, 4800);
    }
    REQUIRE(peaks[0] > 0.0F);
    CHECK(peaks[1] < peaks[0] * 0.75F);
    CHECK(peaks[2] < peaks[1] * 0.75F);
}

TEST_CASE("concurrent_clip_limit", "[session][release]") {
    // Every time source but one taken elsewhere: the first launch plays, the second
    // (on another track) is refused and counted, never silently dropped.
    std::string text = leadSet("testtone", "", "", "1 A quantize=instant");
    text.replace(text.find("[MIXER]"), 0,
                 "TRACK 2 name=\"Two\"\n\n"); // a second playlist row for the second track
    text += "TRACK Lead linked=2\n  1 B quantize=instant\n";
    Set set{text};
    adx::transport::TransportSet& transport = set.rig.engine->transport();
    std::vector<adx::transport::TimeSourceId> taken;
    while (transport.inUseCount() < adx::transport::kMaxTimeSources - 1) {
        taken.push_back(transport.acquire());
    }
    set.rig.engine->launchClip(0, 0);
    set.render(kBlock * 2);
    CHECK(set.session().state(0, 0) == ClipState::Playing);
    CHECK(set.session().refused() == 0);
    set.rig.engine->launchClip(1, 0);
    set.render(kBlock * 2);
    CHECK(set.session().refused() == 1);
    CHECK(set.session().state(1, 0) == ClipState::Stopped);
    for (const adx::transport::TimeSourceId id : taken) {
        transport.release(id);
    }
}

TEST_CASE("mini_clip_cycles_independently", "[session][release]") {
    // Two cells whose pattern is `<c4 e4>` mini-notation over two cycles: launched a bar
    // apart, each alternates on its own count - at bar 2 the first plays its e4 (its
    // second cycle) while the second plays its c4 (its first).
    const std::string text =
        "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 120\n"
        "\n[CHANNEL One]\nINSTRUMENT=testtone\nOUTPUT=insert.1\n"
        "\n[CHANNEL Two]\nINSTRUMENT=testtone\nOUTPUT=insert.1\n"
        "\n[PATTERN M]\nLENGTH=2:0:0\nMINI One\n  <c4 e4>\nMINI Two\n  <c4 e4>\n"
        "\n[PLAYLIST]\nTRACK 1 name=\"One\"\nTRACK 2 name=\"Two\"\n"
        "\n[MIXER]\nINSERT 1 name=\"Master\"\n"
        "\n[SESSION]\nQUANTIZE=bar\nSCENE \"One\"\n"
        "TRACK One linked=1\n  1 M\nTRACK Two linked=2\n  1 M\n";
    const auto loaded = adx::tests::loadText(text);
    adx::tests::OfflineRig const rig{static_cast<std::uint32_t>(kBlock)};
    REQUIRE(rig.engine->setProject(loaded->project, 1).rebuilt);
    std::vector<float> out;
    rig.render(kBlock, out); // the snapshot is installed by the first block
    const adx::project::Snapshot* snapshot = rig.engine->core().current();
    REQUIRE(snapshot != nullptr);
    const adx::project::SessionClip* one = snapshot->sessionClip(0, 0);
    const adx::project::SessionClip* two = snapshot->sessionClip(1, 0);
    REQUIRE(one != nullptr);
    REQUIRE(two != nullptr);
    // Each clip's own notes: c4 in its first cycle, e4 in its second.
    const auto pitches = [](const adx::project::SessionClip& clip) {
        std::vector<std::pair<std::int64_t, int>> found;
        for (const adx::project::ScheduledEvent& event : clip.events.events) {
            if (event.kind == adx::project::EventKind::NoteOn) {
                found.emplace_back(event.tick, event.pitch);
            }
        }
        return found;
    };
    const auto a = pitches(*one);
    REQUIRE(a.size() == 2);
    CHECK(a[0] == std::pair<std::int64_t, int>{0, 60});
    CHECK(a[1] == std::pair<std::int64_t, int>{kPpq * 4, 64});
    CHECK(pitches(*two) == a);
    // Launched a bar apart, the two count their cycles from their own launches.
    rig.engine->play();
    rig.engine->launchClip(0, 0);
    rig.render(kBlock * 4, out);
    rig.render(96000, out);
    rig.engine->launchClip(1, 0);
    rig.render(96000ULL * 2, out);
    CHECK(rig.engine->core().session().state(0, 0) == ClipState::Playing);
    CHECK(rig.engine->core().session().state(1, 0) == ClipState::Playing);
}

TEST_CASE("session_no_alloc", "[session][release][.slow]") {
    // Ten minutes of launching, stopping, scenes, nudges and follow actions, every block
    // under the allocator hook: no allocation on the audio thread.
    if (!adx::rt::allocGuardCompiledIn()) {
        SUCCEED("RT guard not compiled in (Release); zero here would prove nothing");
        return;
    }
    std::string text = leadSet("va", kSlowRelease, "", "1 A follow=next");
    text += "TRACK Lead linked=1\n  1 B follow=random\n  2 A quantize=instant\n";
    Set set{text};
    set.rig.engine->play();
    const std::uint64_t before = adx::rt::ViolationLog::instance().count();
    const std::uint64_t total = 48000ULL * 600;
    std::uint64_t done = 0;
    std::uint32_t step = 0;
    while (done < total) {
        switch (step++ % 7) {
        case 0:
            set.rig.engine->launchClip(step % 2, step % 2);
            break;
        case 1:
            set.rig.engine->fireScene(step % 2);
            break;
        case 2:
            set.rig.engine->stopClip(step % 2);
            break;
        case 3:
            set.rig.engine->setSessionRate(step % 3 == 0 ? 1.04 : 1.0);
            break;
        case 4:
            set.rig.engine->stopAllClips();
            break;
        default:
            set.rig.engine->launchClip(0, 1);
            break;
        }
        set.out.clear();
        set.render(kBlock * 37);
        done += kBlock * 37;
    }
    CHECK(adx::rt::ViolationLog::instance().count() == before);
    CHECK(set.session().refused() == 0);
}
