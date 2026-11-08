// Pitch bend and MPE in MIDI files (STATE.md P9-6, phase_9.md §4.1): bend into
// Note.pitchCurve on import, honouring RPN 0's range and an MPE zone, and slides and
// pitch curves out as per-note bend on rotated member channels.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/format/midi/MidiExporter.h"
#include "engine/format/midi/MidiImporter.h"
#include "engine/project/EventCompile.h"
#include "engine/project/Knot.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::format::midi::MidiSong;

namespace {

void be(std::vector<std::uint8_t>& out, std::uint32_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
    }
}

/// A one-track SMF at `division`, from (delta, bytes) pairs; deltas under 128.
std::vector<std::uint8_t>
smf(std::uint16_t division,
    const std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>>& events) {
    std::vector<std::uint8_t> body;
    for (const auto& [delta, bytes] : events) {
        body.push_back(delta);
        body.insert(body.end(), bytes.begin(), bytes.end());
    }
    body.insert(body.end(), {0x00, 0xFF, 0x2F, 0x00});
    std::vector<std::uint8_t> out{'M', 'T', 'h', 'd'};
    be(out, 6, 4);
    be(out, 0, 2);
    be(out, 1, 2);
    be(out, division, 2);
    out.insert(out.end(), {'M', 'T', 'r', 'k'});
    be(out, static_cast<std::uint32_t>(body.size()), 4);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::vector<std::uint8_t> bend(std::uint8_t channel, int value) {
    return {static_cast<std::uint8_t>(0xE0 | channel), static_cast<std::uint8_t>(value & 0x7F),
            static_cast<std::uint8_t>((value >> 7) & 0x7F)};
}

/// The project's notes in start order, with each one's pitch offset sampled every 120
/// ticks across it.
struct Sampled {
    std::int64_t start;
    std::uint8_t pitch;
    std::vector<float> cents;
};

std::vector<Sampled> sampled(const adx::project::Project& project) {
    std::vector<Sampled> out;
    for (const adx::project::Pattern& pattern : project.patterns) {
        for (const adx::project::NoteClip& clip : pattern.noteClips) {
            for (const adx::project::Note& note : clip.notes) {
                Sampled s{.start = note.start.value, .pitch = note.pitch, .cents = {}};
                const adx::project::NoteExtras* extras = clip.extrasFor(note.id);
                const std::vector<adx::project::Knot> chain =
                    extras != nullptr ? adx::project::pitchOffsetChain(*extras)
                                      : std::vector<adx::project::Knot>{};
                for (std::int64_t at = 0; at < note.length.value; at += 120) {
                    s.cents.push_back(
                        chain.empty() ? 0.0F
                                      : adx::project::knotValueAt(chain.data(), chain.size(), at));
                }
                out.push_back(std::move(s));
            }
        }
    }
    std::ranges::sort(out, [](const Sampled& a, const Sampled& b) {
        return std::tie(a.start, a.pitch) < std::tie(b.start, b.pitch);
    });
    return out;
}

const char* const kBent = R"([PROJECT]
ADX_VERSION=2

[TEMPO]
0:0:0 120

[CHANNEL Lead]
OUTPUT=insert.1

[CHANNEL Pad]
OUTPUT=insert.1

[PATTERN A]
LENGTH=2:0:0
NOTES Lead
  A4 0:0:0 0:2:0 100 slide=7st@0:0:1920+0:1:0~smooth
  C5 0:1:0 0:2:0 90 bend=0st@0:0:0|-130c@0:0:2400|50c@0:1:0
  E5 0:1:0 0:1:0 80
  G4 1:0:0 0:2:0 100 slide=-12st@0:0:0+0:0:3000 bend=0c@0:0:0|35c@0:1:0
NOTES Pad
  C3 0:0:0 1:0:0 70

[PLAYLIST]
TRACK 1
  PATTERN A 0:0:0

[MIXER]
INSERT 1 name="Master"
)";

} // namespace

TEST_CASE("midi_bend_import_roundtrip", "[midi][bend]") {
    SECTION("a file's bend becomes the note's pitch curve, in its RPN range") {
        // Division 480, range set to 12 semitones by RPN 0: centre at the note-on, then
        // a quarter, a half and all of the way up a sixteenth apart; off at the beat.
        const std::vector<std::uint8_t> file = smf(480, {{0, {0xB0, 101, 0}},
                                                         {0, {0xB0, 100, 0}},
                                                         {0, {0xB0, 6, 12}},
                                                         {0, {0xB0, 38, 0}},
                                                         {0, bend(0, 8192)},
                                                         {0, {0x90, 60, 100}},
                                                         {120, bend(0, 8192 + 2048)},
                                                         {120, bend(0, 8192 + 4096)},
                                                         {120, bend(0, 16383)},
                                                         {120, {0x80, 60, 0}}});
        std::string error;
        const std::optional<MidiSong> song = adx::format::midi::parseMidi(file, error);
        REQUIRE(song.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): checked above.
        const MidiSong& parsed = *song;
        REQUIRE(parsed.tracks.size() == 1);
        REQUIRE(parsed.tracks[0].notes.size() == 1);
        adx::project::Project project;
        adx::project::CommandStack stack;
        static_cast<void>(adx::format::midi::importSong(parsed, project, stack));
        const std::vector<Sampled> notes = sampled(project);
        REQUIRE(notes.size() == 1);
        // 480 file ticks = 3840 adX; samples every 120 ticks: 32 of them.
        REQUIRE(notes[0].cents.size() == 32);
        CHECK(std::abs(notes[0].cents[0]) < 1.0F);
        CHECK(std::abs(notes[0].cents[8] - 300.0F) < 1.0F);    // tick 960
        CHECK(std::abs(notes[0].cents[16] - 600.0F) < 1.0F);   // tick 1920
        CHECK(std::abs(notes[0].cents[24] - 1199.85F) < 1.0F); // the top, 16383
        CHECK(std::abs(notes[0].cents[31] - 1199.85F) < 1.0F); // held to the end
        // Between two bend messages the curve is a straight line, not a step.
        CHECK(std::abs(notes[0].cents[4] - 150.0F) < 1.0F);
    }

    SECTION("slides and pitch curves survive export and re-import") {
        const auto loaded = adx::tests::loadText(kBent);
        const std::vector<Sampled> before = sampled(loaded->project);
        std::string error;
        const std::optional<MidiSong> song = adx::format::midi::parseMidi(
            adx::format::midi::writeMidi(adx::format::midi::songOf(loaded->project)), error);
        REQUIRE(song.has_value());
        adx::project::Project project;
        adx::project::CommandStack stack;
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): checked above.
        static_cast<void>(adx::format::midi::importSong(*song, project, stack));
        const std::vector<Sampled> after = sampled(project);
        REQUIRE(after.size() == before.size());
        for (std::size_t n = 0; n < before.size(); ++n) {
            INFO("note " << n << " pitch " << int(before[n].pitch));
            CHECK(after[n].start == before[n].start);
            CHECK(after[n].pitch == before[n].pitch);
            REQUIRE(after[n].cents.size() == before[n].cents.size());
            float worst = 0.0F;
            for (std::size_t i = 0; i < before[n].cents.size(); ++i) {
                worst = std::max(worst, std::abs(after[n].cents[i] - before[n].cents[i]));
            }
            INFO("worst " << worst << " cents");
            // 60-tick steps along each line and 14 bits over 48 semitones: a few cents.
            CHECK(worst < 4.0F);
        }
    }
}

TEST_CASE("midi_export_slide_mpe", "[midi][bend]") {
    const auto loaded = adx::tests::loadText(kBent);
    const MidiSong song = adx::format::midi::songOf(loaded->project);
    REQUIRE(song.tracks.size() == 2);
    CHECK(song.tracks[0].mpe);       // Lead bends
    CHECK_FALSE(song.tracks[1].mpe); // Pad does not
    const std::vector<std::uint8_t> bytes = adx::format::midi::writeMidi(song);

    // The zone: RPN 6 on the manager channel (MIDI channel 1) with fifteen members, and
    // each member's bend range set to 48 semitones.
    const std::vector<std::uint8_t> mcm{0xB0, 100, 6, 0x00, 0xB0, 6, 15};
    CHECK(std::ranges::search(bytes, mcm).begin() != bytes.end());
    const std::vector<std::uint8_t> range{0xB1, 100, 0, 0x00, 0xB1, 6, 48};
    CHECK(std::ranges::search(bytes, range).begin() != bytes.end());

    // A4 (sliding) and C5 (bending) overlap, as does E5 (straight): three member channels.
    // Re-imported, each keeps its own bend - shared channels would have merged them.
    std::string error;
    const std::optional<MidiSong> back = adx::format::midi::parseMidi(bytes, error);
    REQUIRE(back.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): checked above.
    const MidiSong& parsed = *back;
    const auto lead = std::ranges::find_if(parsed.tracks, [](const auto& t) { return t.mpe; });
    REQUIRE(lead != parsed.tracks.end());
    REQUIRE(lead->notes.size() == 4);
    const auto byPitch = [&](std::uint8_t pitch) {
        return *std::ranges::find_if(lead->notes, [&](const auto& n) { return n.pitch == pitch; });
    };
    CHECK(byPitch(76).bend.empty()); // E5 stays straight beside the two bent notes
    CHECK_FALSE(byPitch(69).bend.empty());
    CHECK_FALSE(byPitch(72).bend.empty());
    CHECK(byPitch(69).bend.back().cents == 700);
    // The pad's track is plain: one channel, no bend.
    const auto pad = std::ranges::find_if(parsed.tracks, [](const auto& t) { return !t.mpe; });
    REQUIRE(pad != parsed.tracks.end());
    CHECK(pad->notes.size() == 1);
    CHECK(pad->notes[0].bend.empty());
}
