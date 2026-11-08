// Performance recording (phase_11.md §4.6, §5): performance_record_roundtrip (the phase
// gate, part 1), performance_record_is_ordinary, performance_record_one_command.
//
// A scripted two-minute performance - 40 launches, 6 scenes, follow actions, 3 tempo
// nudges - is played offline with the log armed, committed, written as text, read back
// and rendered from the top: the two renders are compared sample for sample.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "engine/format/adx/Writer.h"
#include "engine/record/PerformanceRecorder.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace {

constexpr std::uint64_t kBlock = 256;
constexpr std::uint64_t kBar = 96000; // at 120 bpm

/// Three channels on test tones, six patterns (two of them shared between channels, so
/// commit has to split them), six scenes and three tracks with follow actions.
std::string performanceSet() {
    std::string text = "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 120\n";
    for (const char* name : {"Lead", "Bass", "Keys"}) {
        text += std::string{"\n[CHANNEL "} + name + "]\nINSTRUMENT=testtone\nOUTPUT=insert.1\n";
    }
    text += "\n[PATTERN L1]\nLENGTH=1:0:0\nNOTES Lead\n  C5 0:0:0 0:1:0 90\n  E5 0:2:0 0:0:960 "
            "80\n"
            "\n[PATTERN L2]\nLENGTH=2:0:0\nNOTES Lead\n  G4 0:0:0 0:2:0 100\n  A4 1:0:0 0:3:0 70\n"
            "\n[PATTERN B1]\nLENGTH=1:0:0\nNOTES Bass\n  C2 0:0:0 0:0:960 110\n  C2 0:1:0 0:0:960 "
            "100\n  G2 0:3:0 0:0:960 100\n"
            "\n[PATTERN SHARED]\nLENGTH=1:0:0\nNOTES Bass\n  F2 0:0:0 0:2:0 100\nNOTES Keys\n  A3 "
            "0:0:0 0:3:0 60\n  C4 0:0:0 0:3:0 60\n"
            "\n[PATTERN K1]\nLENGTH=1:0:0\nNOTES Keys\n  E4 0:1:0 0:1:0 70\n  G4 0:3:0 0:1:0 70\n"
            "\n[PATTERN K2]\nLENGTH=2:0:0\nNOTES Keys\n  D4 0:0:0 1:0:0 80\n"
            "\n[PLAYLIST]\nTRACK 1 name=\"Lead\"\n"
            "\n[MIXER]\nINSERT 1 name=\"Master\"\n"
            "\n[SESSION]\nQUANTIZE=bar\n";
    for (int s = 1; s <= 6; ++s) {
        text += "SCENE \"S" + std::to_string(s) + "\"\n";
    }
    text += "TRACK Lead linked=1\n  1 L1\n  2 L2 follow=next\n  3 L1 gain=0.7\n  4 L2\n"
            "  5 L1 follow=repeat repeats=2\n  6 L2 legato=yes\n"
            "TRACK Bass\n  1 B1\n  2 SHARED\n  3 B1 follow=previous\n  4 SHARED quantize=1/2\n"
            "  6 B1\n"
            "TRACK Keys\n  1 K1\n  2 SHARED\n  3 K2 loop=no\n  4 K1 follow=random\n  5 K2\n";
    return text;
}

struct Action {
    std::uint64_t at; // block index
    int kind;         // 0 launch, 1 scene, 2 stop, 3 rate, 4 stop all
    std::uint32_t track;
    std::uint32_t row;
    double value;
};

/// 40 launches, 6 scenes, a few stops and 3 nudges over two minutes (60 bars), on
/// block starts spread through the bars.
std::vector<Action> script(bool nudges) {
    std::vector<Action> out;
    const std::uint64_t blocksPerBar = kBar / kBlock; // 375
    std::uint32_t seed = 12345;
    const auto next = [&seed] {
        seed = (seed * 1103515245U) + 12345U;
        return (seed >> 16U) & 0x7FFFU;
    };
    int launches = 0;
    for (std::uint64_t bar = 0; bar < 56; ++bar) {
        const std::uint64_t at = (bar * blocksPerBar) + 10 + (next() % 300);
        if (bar % 9 == 4) {
            out.push_back({.at = at,
                           .kind = 1,
                           .track = 0,
                           .row = static_cast<std::uint32_t>(next() % 6),
                           .value = 0.0});
            continue;
        }
        if (bar % 13 == 7) {
            out.push_back({.at = at, .kind = 2, .track = next() % 3, .row = 0, .value = 0.0});
        }
        if (launches < 40) {
            out.push_back({.at = at + 1,
                           .kind = 0,
                           .track = next() % 3,
                           .row = static_cast<std::uint32_t>(next() % 6),
                           .value = 0.0});
            ++launches;
        }
    }
    for (std::uint64_t bar = 0; bar < 60 && launches < 40; bar += 3) {
        out.push_back({.at = (bar * blocksPerBar) + 200,
                       .kind = 0,
                       .track = next() % 3,
                       .row = next() % 6,
                       .value = 0.0});
        ++launches;
    }
    if (nudges) {
        out.push_back(
            {.at = (20 * blocksPerBar) + 3, .kind = 3, .track = 0, .row = 0, .value = 1.04});
        out.push_back(
            {.at = (20 * blocksPerBar) + 3 + 40, .kind = 3, .track = 0, .row = 0, .value = 1.0});
        out.push_back(
            {.at = (35 * blocksPerBar) + 100, .kind = 3, .track = 0, .row = 0, .value = 0.96});
        out.push_back(
            {.at = (35 * blocksPerBar) + 160, .kind = 3, .track = 0, .row = 0, .value = 1.0});
        out.push_back(
            {.at = (48 * blocksPerBar) + 7, .kind = 3, .track = 0, .row = 0, .value = 1.02});
        out.push_back(
            {.at = (48 * blocksPerBar) + 80, .kind = 3, .track = 0, .row = 0, .value = 1.0});
    }
    std::ranges::stable_sort(out, {}, &Action::at);
    return out;
}

struct Performed {
    std::vector<float> live;
    std::unique_ptr<adx::tests::Loaded> loaded;
    adx::record::PerformanceSummary summary;
    std::size_t undoBefore{0};
    int launches{0};
    int scenes{0};
};

Performed perform(bool nudges, std::uint64_t frames) {
    Performed p;
    p.loaded = adx::tests::loadText(performanceSet());
    adx::tests::OfflineRig const rig{static_cast<std::uint32_t>(kBlock)};
    REQUIRE(rig.engine->setProject(p.loaded->project, 1).rebuilt);
    rig.engine->recordPerformance(true);
    rig.engine->play();
    const std::vector<Action> actions = script(nudges);
    std::size_t a = 0;
    const std::uint64_t blocks = frames / kBlock;
    for (std::uint64_t b = 0; b < blocks; ++b) {
        while (a < actions.size() && actions[a].at == b) {
            const Action& act = actions[a++];
            switch (act.kind) {
            case 0:
                rig.engine->launchClip(act.track, act.row);
                ++p.launches;
                break;
            case 1:
                rig.engine->fireScene(act.row);
                ++p.scenes;
                break;
            case 2:
                rig.engine->stopClip(act.track);
                break;
            case 3:
                rig.engine->setSessionRate(act.value);
                break;
            default:
                rig.engine->stopAllClips();
                break;
            }
        }
        rig.render(kBlock, p.live);
    }
    adx::record::PerformanceRecorder recorder;
    adx::session::PerformanceEvent event;
    while (rig.engine->core().session().takeLogged(event)) {
        recorder.add(event);
    }
    CHECK(rig.engine->core().session().logLost() == 0);
    p.undoBefore = p.loaded->stack.undoDepth();
    p.summary =
        recorder.commit(p.loaded->project, p.loaded->stack, rig.engine->positionTicks().value);
    return p;
}

std::vector<float> renderArrangement(const adx::project::Project& project, std::uint64_t frames) {
    adx::tests::OfflineRig const rig{static_cast<std::uint32_t>(kBlock)};
    REQUIRE(rig.engine->setProject(project, 1).rebuilt);
    rig.engine->play();
    std::vector<float> out;
    rig.render(frames, out);
    return out;
}

} // namespace

TEST_CASE("performance_record_roundtrip", "[session][recorder][gate]") {
    // Without rate changes: the reopened arrangement renders bit for bit what was heard.
    const std::uint64_t frames = kBar * 60;
    Performed p = perform(false, frames);
    CHECK(p.launches == 40);
    CHECK(p.scenes == 6);
    REQUIRE(p.summary.items > 40);
    CHECK(p.summary.patternsMade == 2); // SHARED, split into its Bass and Keys halves
    const std::string text = adx::format::write(p.loaded->project);
    const auto reopened = adx::tests::loadText(text);
    const std::vector<float> again = renderArrangement(reopened->project, frames);
    REQUIRE(again.size() == p.live.size());
    std::size_t differing = 0;
    std::size_t first = again.size();
    for (std::size_t i = 0; i < again.size(); ++i) {
        if (again[i] != p.live[i]) {
            ++differing;
            first = std::min(first, i);
        }
    }
    INFO(differing << " samples differ, the first at frame " << first / 2);
    CHECK(differing == 0);
    float peak = 0.0F;
    for (const float s : p.live) {
        peak = std::max(peak, std::abs(s));
    }
    CHECK(peak > 0.05F); // it played something
}

TEST_CASE("performance_record_roundtrip_with_nudges", "[session][recorder][gate]") {
    // Three nudges: the recording writes them as tempo events. Rounding differs between
    // a rate and a tempo, so every onset is compared, not every sample: each within a
    // sample of where it was heard.
    const std::uint64_t frames = kBar * 60;
    Performed const p = perform(true, frames);
    CHECK(p.summary.tempoEvents >= 6);
    const auto reopened = adx::tests::loadText(adx::format::write(p.loaded->project));
    const std::vector<float> again = renderArrangement(reopened->project, frames);
    const auto onsets = [](const std::vector<float>& out) {
        std::vector<std::size_t> found;
        bool was = false;
        for (std::size_t f = 0; f < out.size() / 2; ++f) {
            const bool now = out[f * 2] != 0.0F;
            if (now && !was) {
                found.push_back(f);
            }
            was = now;
        }
        return found;
    };
    const std::vector<std::size_t> heard = onsets(p.live);
    const std::vector<std::size_t> played = onsets(again);
    REQUIRE(heard.size() > 40);
    CHECK(played.size() == heard.size());
    std::size_t worst = 0;
    for (std::size_t i = 0; i < std::min(heard.size(), played.size()); ++i) {
        worst = std::max(worst, heard[i] > played[i] ? heard[i] - played[i] : played[i] - heard[i]);
    }
    INFO("worst onset difference " << worst << " frames");
    CHECK(worst <= 1);
}

TEST_CASE("performance_record_is_ordinary", "[session][recorder]") {
    // Patterns, items and tempo events: every item is a pattern placement on a playlist
    // track, and the text has nothing for the recording but what any arrangement has.
    Performed const p = perform(true, kBar * 20);
    for (const adx::project::PlaylistTrack& track : p.loaded->project.playlist.tracks) {
        for (const adx::project::PlaylistItem& item : track.items) {
            CHECK(std::holds_alternative<adx::project::PatternRef>(item.content));
        }
    }
    // Each session track is linked to the playlist track it wrote.
    for (const adx::project::SessionTrack& track : p.loaded->project.session.tracks) {
        CHECK(track.linked.valid());
    }
}

TEST_CASE("performance_record_one_command", "[session][recorder]") {
    Performed const p = perform(true, kBar * 20);
    CHECK(p.loaded->stack.undoDepth() == p.undoBefore + 1);
    const std::size_t patterns = p.loaded->project.patterns.size();
    REQUIRE(p.loaded->stack.undo(p.loaded->project));
    CHECK(p.loaded->project.patterns.size() == patterns - p.summary.patternsMade);
    std::size_t items = 0;
    for (const adx::project::PlaylistTrack& track : p.loaded->project.playlist.tracks) {
        items += track.items.size();
    }
    CHECK(items == 0);
}
