// Reloading while playing (phase_7.md §4.4, §6). The gate is reload_no_voice_cut: a
// sustained pad plays while its file is rewritten a hundred times, and nothing it
// does may be disturbed.
//
// "Zero voice restarts, zero discontinuities" is tested more strictly than it is
// stated: every edit here is inaudible by construction - a title, a muted channel's
// notes, a marker, an unplaced pattern, a channel or a slot added on a bus that
// carries silence - so the output with a hundred reloads must be **bit-identical** to
// the output with none. A restarted voice, a recreated node, a reset filter or a
// rebuilt reverb would each show up as a difference.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "engine/format/adx/Parser.h"
#include "engine/format/adx/Writer.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/rt/AllocGuard.h"
#include "engine/rt/Violation.h"
#include "engine/sync/TextSync.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::sync::ApplyStatus;
using adx::sync::TextSync;

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kBlock = 256;

const std::string kBase = R"([PROJECT]
ADX_VERSION=2
TITLE="Reload gate"

[TEMPO]
0:0:0 120

[CHANNEL Pad]
OUTPUT=insert.2
VOLUME=0.5

[CHANNEL Other]
OUTPUT=insert.3
MUTE=yes

[PATTERN Hold]
LENGTH=16:0:0
NOTES Pad
  C3 0:0:0 16:0:0 100
  G3 0:0:0 16:0:0 90

[PATTERN Riff]
LENGTH=1:0:0
NOTES Other
  C4 0:0:0 0:1:0 100

[PLAYLIST]
TRACK 1
  PATTERN Hold 0:0:0
TRACK 2
  PATTERN Riff 0:0:0

[MIXER]
INSERT 1 name="Master"
INSERT 2 name="Pad bus"
  SLOT 1 Reverb mix=0.3 room=0.5
INSERT 3 name="Other bus"
ROUTE insert.2 -> insert.1
ROUTE insert.3 -> insert.1
)";

std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

/// The i-th rewrite: one inaudible edit, different each time, some narrow and some
/// structural, so consecutive reloads add, change and remove things.
std::string rewrite(int i) {
    const std::string n = std::to_string(i);
    switch (i % 10) {
    case 0:
        return replaced(kBase, "TITLE=\"Reload gate\"", "TITLE=\"Reload " + n + "\"");
    case 1:
        return replaced(kBase, "C4 0:0:0 0:1:0 100", "C4 0:0:0 0:1:0 " + std::to_string(20 + i));
    case 2:
        return kBase + "\n[MARKERS]\n" + std::to_string(i % 16) + ":0:0 \"M" + n + "\"\n";
    case 3:
        return replaced(kBase, "INSERT 3 name=\"Other bus\"",
                        "INSERT 3 name=\"Other bus\" gain=0." + std::to_string(1 + (i % 9)));
    case 4:
        // Structural: a new channel, on the silent bus.
        return replaced(kBase, "[CHANNEL Other]",
                        "[CHANNEL Ghost" + n + "]\nOUTPUT=insert.3\n\n[CHANNEL Other]");
    case 5:
        return replaced(kBase, "[PATTERN Riff]\nLENGTH=1:0:0",
                        "[PATTERN Riff]\nLENGTH=" + std::to_string(1 + (i % 3)) + ":0:0");
    case 6:
        // Structural: a pattern nobody places.
        return kBase + "\n[PATTERN Spare]\nNOTES Other\n  D4 0:0:0 0:1:0 90\n";
    case 7:
        // Structural: an effect on the silent bus.
        return replaced(kBase, "INSERT 3 name=\"Other bus\"",
                        "INSERT 3 name=\"Other bus\"\n  SLOT 2 Reverb mix=0.5");
    case 8:
        // Sections moved: formatting, not music.
        return replaced(kBase, "[CHANNEL Other]\nOUTPUT=insert.3\nMUTE=yes\n\n", "") +
               "\n[CHANNEL Other]\nOUTPUT=insert.3\nMUTE=yes\n";
    default:
        return replaced(kBase, "MUTE=yes", "MUTE=yes\nPAN=" + std::to_string((i % 5) - 2) + "e-1");
    }
}

struct Session {
    adx::project::Project project;
    adx::project::CommandStack stack;
    adx::format::DiagnosticList diagnostics;
    std::unique_ptr<TextSync> sync;
    adx::tests::OfflineRig rig{kBlock, kRate};

    explicit Session(const std::string& text) {
        adx::format::load(text, project, stack, diagnostics);
        stack.clear();
        sync = std::make_unique<TextSync>(project, stack, adx::format::WriteOptions{}, "");
        sync->markSynced(text);
        REQUIRE(rig.engine->setProject(project, stack.revision()).rebuilt);
        rig.engine->play();
    }

    /// The file changed on disk: reload it and bring the audio thread up to date.
    [[nodiscard]] ApplyStatus reload(const std::string& text) const {
        const auto result = sync->applyExternal(text);
        rig.engine->commit(project, stack);
        return result.status;
    }
};

float rms(const std::vector<float>& out, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t f = from; f < to; ++f) {
        sum += static_cast<double>(out[f * 2]) * out[f * 2];
    }
    return static_cast<float>(
        std::sqrt(sum / static_cast<double>(std::max<std::size_t>(1, to - from))));
}

float maxJump(const std::vector<float>& out, std::size_t from, std::size_t to) {
    float jump = 0.0F;
    for (std::size_t f = std::max<std::size_t>(from, 1); f < to; ++f) {
        jump = std::max(jump, std::abs(out[f * 2] - out[(f - 1) * 2]));
        jump = std::max(jump, std::abs(out[(f * 2) + 1] - out[((f - 1) * 2) + 1]));
    }
    return jump;
}

} // namespace

TEST_CASE("reload_no_voice_cut", "[sync][reload][gate]") {
    // Reference: the same 100 stretches of audio with no reloads at all.
    constexpr int kReloads = 100;
    constexpr std::uint64_t kStretch = 2048; // frames between reloads
    Session const reference(kBase);
    std::vector<float> expected;
    reference.rig.render(kStretch, expected);
    for (int i = 0; i < kReloads; ++i) {
        reference.rig.render(kStretch, expected);
    }

    const Session live(kBase);
    std::vector<float> actual;
    live.rig.render(kStretch, actual);
    const std::uint64_t violationsBefore = adx::rt::ViolationLog::instance().count();
    int applied = 0;
    for (int i = 0; i < kReloads; ++i) {
        const std::string text = rewrite(i);
        const ApplyStatus status = live.reload(text);
        INFO("rewrite " << i);
        REQUIRE((status == ApplyStatus::Applied || status == ApplyStatus::Unchanged));
        applied += status == ApplyStatus::Applied ? 1 : 0;
        live.rig.render(kStretch, actual);
    }
    const std::uint64_t violations = adx::rt::ViolationLog::instance().count() - violationsBefore;

    CHECK(applied >= kReloads * 8 / 10); // nearly every rewrite really changed the project
    REQUIRE(actual.size() == expected.size());
    // The pad is genuinely sounding throughout, so "identical" is not "both silent".
    CHECK(rms(expected, expected.size() / 4, expected.size() / 2) > 0.01F);
    std::size_t firstDifference = actual.size();
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (actual[i] != expected[i]) {
            firstDifference = i;
            break;
        }
    }
    INFO("first differing sample: " << firstDifference << " of " << actual.size());
    CHECK(firstDifference == actual.size());
    CHECK(maxJump(actual, 0, actual.size() / 2) <= maxJump(expected, 0, expected.size() / 2));
    if (adx::rt::allocGuardCompiledIn()) {
        CHECK(violations == 0);
    }
}

TEST_CASE("reload_preserves_equivalent_effects", "[sync][reload]") {
    // An edit that leaves the reverb as it was keeps its instance - so its tail rings
    // on, sample for sample, through the reload.
    const std::string shortNote = replaced(
        replaced(kBase, "C3 0:0:0 16:0:0 100", "C3 0:0:0 0:1:0 100"), "  G3 0:0:0 16:0:0 90\n", "");
    const std::string wet =
        replaced(shortNote, "SLOT 1 Reverb mix=0.3 room=0.5", "SLOT 1 Reverb mix=1 room=0.95");
    Session const reference(wet);
    const Session live(wet);
    std::vector<float> expected;
    std::vector<float> actual;
    reference.rig.render(kRate, expected); // a second: the note and its release are over
    live.rig.render(kRate, actual);
    REQUIRE(live.reload(replaced(wet, "TITLE=\"Reload gate\"", "TITLE=\"Edited\"")) ==
            ApplyStatus::Applied);
    reference.rig.render(kRate / 2, expected);
    live.rig.render(kRate / 2, actual);
    CHECK(rms(expected, kRate, kRate + (kRate / 10)) > 1e-4F); // the tail is really there
    CHECK(actual == expected);
}

TEST_CASE("reload_effect_swap_fades", "[sync][reload]") {
    // Replacing the reverb by another effect type crossfades the old instance out over
    // 20 ms instead of cutting its tail. The pad has stopped by then, so the new effect
    // has nothing to play: whatever sounds after the swap is the old tail, fading.
    const std::string shortNote = replaced(
        replaced(kBase, "C3 0:0:0 16:0:0 100", "C3 0:0:0 0:1:0 100"), "  G3 0:0:0 16:0:0 90\n", "");
    const std::string wet =
        replaced(shortNote, "SLOT 1 Reverb mix=0.3 room=0.5", "SLOT 1 Reverb mix=1 room=0.95");
    const Session live(wet);
    std::vector<float> out;
    live.rig.render(kRate, out);
    const std::size_t swap = out.size() / 2;
    REQUIRE(live.reload(replaced(wet, "SLOT 1 Reverb mix=1 room=0.95", "SLOT 1 Chorus mix=1")) ==
            ApplyStatus::Applied);
    live.rig.render(kRate / 4, out);

    const std::size_t ms = kRate / 1000;
    const float before = rms(out, swap - (10 * ms), swap);
    const float firstMs = rms(out, swap, swap + ms);
    const float during = rms(out, swap + (5 * ms), swap + (15 * ms));
    const float after = rms(out, swap + (25 * ms), swap + (100 * ms));
    INFO("rms before " << before << ", first ms " << firstMs << ", 5-15 ms " << during
                       << ", 25-100 ms " << after);
    REQUIRE(before > 1e-4F);
    // Not cut: the first millisecond after the swap still carries nearly all of it...
    CHECK(firstMs > 0.8F * before);
    // ...it is fading through the 20 ms...
    CHECK(during < firstMs);
    CHECK(during > 0.1F * before);
    // ...and gone after.
    CHECK(after < 0.05F * before);
    // No step at the swap: the largest sample-to-sample change across it is no bigger
    // than the tail's own.
    CHECK(maxJump(out, swap - 2, swap + 2) <=
          (1.5F * maxJump(out, swap - (10 * ms), swap - 2)) + 1e-6F);
}

TEST_CASE("reload_instrument_change_releases", "[sync][reload]") {
    // A new instrument on a sounding channel: the old voices are released and ring out
    // through their own release, rather than stopping dead.
    const std::string slowRelease =
        replaced(kBase, "VOLUME=0.5", "VOLUME=0.5\nPARAM env.release=0.5");
    const Session live(slowRelease);
    std::vector<float> out;
    live.rig.render(kRate / 2, out);
    const std::size_t swap = out.size() / 2;
    REQUIRE(live.reload(replaced(slowRelease, "VOLUME=0.5", "INSTRUMENT=va\nVOLUME=0.5")) ==
            ApplyStatus::Applied);
    live.rig.render(kRate, out);
    const std::size_t ms = kRate / 1000;
    const float before = rms(out, swap - (20 * ms), swap);
    const float justAfter = rms(out, swap, swap + (20 * ms));
    INFO("rms before " << before << ", 20 ms after " << justAfter);
    REQUIRE(before > 0.01F);
    CHECK(justAfter > 0.5F * before);
    CHECK(maxJump(out, swap - 2, swap + 2) <=
          (1.5F * maxJump(out, swap - (20 * ms), swap - 2)) + 1e-6F);
}

TEST_CASE("reload_channel_removed_releases", "[sync][reload]") {
    // Removing a sounding channel releases its voices into the insert it fed; they ring
    // out through their release instead of stopping dead, and the departure ends once
    // they are silent.
    const std::string slowRelease =
        replaced(kBase, "VOLUME=0.5", "VOLUME=0.5\nPARAM env.release=0.3");
    const Session live(slowRelease);
    std::vector<float> out;
    live.rig.render(kRate / 2, out);
    const std::size_t swap = out.size() / 2;
    std::string removed = replaced(slowRelease,
                                   "[CHANNEL Pad]\nOUTPUT=insert.2\nVOLUME=0.5\n"
                                   "PARAM env.release=0.3\n\n",
                                   "");
    removed = replaced(removed, "NOTES Pad\n  C3 0:0:0 16:0:0 100\n  G3 0:0:0 16:0:0 90\n", "");
    REQUIRE(live.reload(removed) == ApplyStatus::Applied);
    live.rig.render(kRate, out);
    const std::size_t ms = kRate / 1000;
    const float before = rms(out, swap - (20 * ms), swap);
    const float justAfter = rms(out, swap, swap + (20 * ms));
    const float later = rms(out, swap + (900 * ms), swap + (1000 * ms));
    INFO("rms before " << before << ", 20 ms after " << justAfter << ", a second later " << later);
    REQUIRE(before > 0.01F);
    CHECK(justAfter > 0.5F * before);
    CHECK(later < 0.05F * before); // only the bus reverb's own tail is left
    CHECK(maxJump(out, swap - 2, swap + 2) <=
          (1.5F * maxJump(out, swap - (20 * ms), swap - 2)) + 1e-6F);
    // The next rebuild leaves the silent departure out.
    REQUIRE(live.reload(replaced(removed, "TITLE=\"Reload gate\"", "TITLE=\"After\"")) ==
            ApplyStatus::Applied);
    live.rig.render(kBlock, out);
    CHECK(live.rig.engine->builder().nodes().departing() == 0);
}

TEST_CASE("reload_tempo_change_no_glitch", "[sync][reload]") {
    // A tempo edit while playing: no discontinuity in the sustained pad, and what comes
    // after is timed by the new tempo from the musical position the playhead had.
    const std::string withHit =
        replaced(replaced(kBase, "MUTE=yes", "MUTE=no"),
                 "[PLAYLIST]\nTRACK 1\n  PATTERN Hold 0:0:0\nTRACK 2\n  PATTERN Riff 0:0:0",
                 "[PLAYLIST]\nTRACK 1\n  PATTERN Hold 0:0:0\nTRACK 2\n  PATTERN Riff 4:0:0");
    // The pad alone, to look for a discontinuity; the hit alone, to time.
    const std::string padOnly = replaced(withHit, "  PATTERN Riff 4:0:0\n", "");
    const Session pad(padOnly);
    std::vector<float> padOut;
    pad.rig.render(kRate, padOut);
    const std::size_t swap = padOut.size() / 2;
    REQUIRE(pad.reload(replaced(padOnly, "0:0:0 120", "0:0:0 140")) == ApplyStatus::Applied);
    pad.rig.render(kRate, padOut);
    CHECK(maxJump(padOut, swap - 2, swap + 2) <=
          (1.5F * maxJump(padOut, swap - 2000, swap - 2)) + 1e-6F);

    const std::string hitOnly = replaced(withHit, "  PATTERN Hold 0:0:0\n", "");
    const Session hit(hitOnly);
    std::vector<float> hitOut;
    hit.rig.render(kRate, hitOut); // one second at 120 bpm: two beats
    const double beatsAtSwap =
        static_cast<double>(hit.rig.engine->positionTicks().value) / adx::core::kPpq;
    REQUIRE(hit.reload(replaced(hitOnly, "0:0:0 120", "0:0:0 240")) == ApplyStatus::Applied);
    hit.rig.render(std::uint64_t{kRate} * 4, hitOut);
    const std::size_t onset = adx::tests::firstSoundFrom(hitOut, kRate);
    // Bar 5 is beat 16; the remaining beats go by at 240 bpm, a quarter second each.
    const double expected = kRate + ((16.0 - beatsAtSwap) * 0.25 * kRate);
    INFO("beats at swap " << beatsAtSwap << ", onset " << onset << ", expected " << expected);
    CHECK(std::abs(static_cast<double>(onset) - expected) <= kBlock);
}
