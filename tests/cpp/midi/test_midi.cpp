// MIDI file import and export (phase_8.md §3.8, §5).
//
// The corpus is generated rather than collected: twenty files written byte by byte by
// a writer of this test's own, which is independent of MidiExporter, over the spread
// of what real files do - format 0 and 1, divisions that divide adX's ticks and one
// that does not, running status, note-on at velocity 0 as note-off, nested same-pitch
// notes, tempo and meter changes, markers, notes left sounding at the end of a track.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "engine/format/adx/Parser.h"
#include "engine/format/midi/MidiExporter.h"
#include "engine/format/midi/MidiImporter.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::format::midi::MidiNote;
using adx::format::midi::MidiSong;

namespace {

/// A raw event for the test's own writer: absolute file ticks and the bytes after the
/// delta.
struct Raw {
    std::uint32_t tick;
    std::vector<std::uint8_t> bytes;
};

void be(std::vector<std::uint8_t>& out, std::uint32_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
    }
}

void vlq(std::vector<std::uint8_t>& out, std::uint32_t value) {
    std::vector<std::uint8_t> groups;
    do {
        groups.push_back(static_cast<std::uint8_t>(value & 0x7F));
        value >>= 7;
    } while (value != 0);
    for (std::size_t i = groups.size(); i-- > 0;) {
        out.push_back(i > 0 ? static_cast<std::uint8_t>(groups[i] | 0x80) : groups[i]);
    }
}

/// One MTrk. `runningStatus` drops repeated status bytes, as many writers do.
std::vector<std::uint8_t> track(std::vector<Raw> events, bool runningStatus) {
    std::ranges::stable_sort(events, {}, &Raw::tick);
    std::vector<std::uint8_t> body;
    std::uint32_t now = 0;
    std::uint8_t last = 0;
    for (const Raw& event : events) {
        vlq(body, event.tick - now);
        now = event.tick;
        std::size_t from = 0;
        if (runningStatus && event.bytes[0] < 0xF0 && event.bytes[0] == last) {
            from = 1;
        }
        last = event.bytes[0] < 0xF0 ? event.bytes[0] : 0;
        body.insert(body.end(), event.bytes.begin() + static_cast<std::ptrdiff_t>(from),
                    event.bytes.end());
    }
    body.insert(body.end(), {0x00, 0xFF, 0x2F, 0x00});
    std::vector<std::uint8_t> chunk{'M', 'T', 'r', 'k'};
    be(chunk, static_cast<std::uint32_t>(body.size()), 4);
    chunk.insert(chunk.end(), body.begin(), body.end());
    return chunk;
}

std::vector<std::uint8_t> corpusFile(std::uint32_t seed) {
    std::mt19937 random(seed);
    const std::array<std::uint16_t, 8> divisions{96, 120, 192, 240, 384, 480, 960, 1000};
    const std::uint16_t division = divisions.at(seed % divisions.size());
    const bool format0 = (seed % 3) == 0;
    const bool running = (seed % 2) == 0;
    const int channels = 1 + static_cast<int>(seed % 4);

    std::vector<Raw> conductor;
    const auto tempo = [&](std::uint32_t at, double bpm) {
        const auto us = static_cast<std::uint32_t>(60'000'000.0 / bpm);
        conductor.push_back(
            Raw{.tick = at,
                .bytes = {0xFF, 0x51, 0x03, static_cast<std::uint8_t>(us >> 16),
                          static_cast<std::uint8_t>(us >> 8), static_cast<std::uint8_t>(us)}});
    };
    tempo(0, 90.0 + (seed * 7 % 80));
    tempo(division * 8U, 140.0);
    conductor.push_back(Raw{.tick = 0, .bytes = {0xFF, 0x58, 0x04, 4, 2, 24, 8}});
    conductor.push_back(Raw{.tick = division * 4U, .bytes = {0xFF, 0x58, 0x04, 3, 2, 24, 8}});
    conductor.push_back(
        Raw{.tick = division * 4U, .bytes = {0xFF, 0x06, 0x05, 'V', 'e', 'r', 's', 'e'}});

    std::vector<std::vector<Raw>> perChannel(static_cast<std::size_t>(channels));
    std::uniform_int_distribution<int> pitch(36, 84);
    std::uniform_int_distribution<int> velocity(1, 127);
    std::uniform_int_distribution<std::uint32_t> step(0, division);
    for (int c = 0; c < channels; ++c) {
        auto& events = perChannel[static_cast<std::size_t>(c)];
        std::uint32_t at = 0;
        for (int n = 0; n < 40; ++n) {
            at += step(random);
            const auto p = static_cast<std::uint8_t>(pitch(random));
            const auto length = 1 + step(random);
            const auto status = static_cast<std::uint8_t>(c);
            events.push_back(Raw{.tick = at,
                                 .bytes = {static_cast<std::uint8_t>(0x90 | status), p,
                                           static_cast<std::uint8_t>(velocity(random))}});
            // Half the note-offs are note-on at velocity 0.
            if ((n % 2) == 0) {
                events.push_back(Raw{.tick = at + length,
                                     .bytes = {static_cast<std::uint8_t>(0x80 | status), p, 64}});
            } else {
                events.push_back(Raw{.tick = at + length,
                                     .bytes = {static_cast<std::uint8_t>(0x90 | status), p, 0}});
            }
            if ((n % 9) == 0) {
                // A nested same-pitch note inside this one.
                events.push_back(Raw{.tick = at + 1,
                                     .bytes = {static_cast<std::uint8_t>(0x90 | status), p, 50}});
                events.push_back(Raw{.tick = at + 1 + (length / 2),
                                     .bytes = {static_cast<std::uint8_t>(0x80 | status), p, 0}});
            }
        }
        if ((seed % 5) == 0) {
            // A note left sounding at the end of the track.
            events.push_back(
                Raw{.tick = at + 10, .bytes = {static_cast<std::uint8_t>(0x90 | c), 61, 99}});
        }
    }

    std::vector<std::uint8_t> file{'M', 'T', 'h', 'd'};
    be(file, 6, 4);
    be(file, format0 ? 0 : 1, 2);
    be(file, format0 ? 1 : static_cast<std::uint32_t>(channels + 1), 2);
    be(file, division, 2);
    if (format0) {
        std::vector<Raw> all = conductor;
        for (const auto& events : perChannel) {
            all.insert(all.end(), events.begin(), events.end());
        }
        const std::vector<std::uint8_t> chunk = track(all, running);
        file.insert(file.end(), chunk.begin(), chunk.end());
    } else {
        std::vector<std::uint8_t> chunk = track(conductor, running);
        file.insert(file.end(), chunk.begin(), chunk.end());
        for (int c = 0; c < channels; ++c) {
            std::vector<Raw> events = perChannel[static_cast<std::size_t>(c)];
            const std::string name = "Part " + std::to_string(c + 1);
            std::vector<std::uint8_t> meta{0xFF, 0x03, static_cast<std::uint8_t>(name.size())};
            meta.insert(meta.end(), name.begin(), name.end());
            events.insert(events.begin(), Raw{.tick = 0, .bytes = meta});
            chunk = track(events, running);
            file.insert(file.end(), chunk.begin(), chunk.end());
        }
    }
    return file;
}

std::vector<std::vector<MidiNote>> notesOf(const MidiSong& song) {
    std::vector<std::vector<MidiNote>> out;
    for (const auto& t : song.tracks) {
        if (!t.notes.empty()) {
            out.push_back(t.notes);
        }
    }
    return out;
}

} // namespace

TEST_CASE("midi_import_roundtrip", "[midi]") {
    for (std::uint32_t seed = 1; seed <= 20; ++seed) {
        INFO("corpus file " << seed);
        std::string error;
        const std::optional<MidiSong> first = adx::format::midi::parseMidi(corpusFile(seed), error);
        REQUIRE(first.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): checked by the REQUIRE above.
        const MidiSong& parsedFirst = *first;
        REQUIRE_FALSE(parsedFirst.tracks.empty());

        adx::project::Project project;
        adx::project::CommandStack stack;
        static_cast<void>(adx::format::midi::importSong(*first, project, stack));
        const std::vector<std::uint8_t> exported =
            adx::format::midi::writeMidi(adx::format::midi::songOf(project));
        const std::optional<MidiSong> again = adx::format::midi::parseMidi(exported, error);
        REQUIRE(again.has_value());
        REQUIRE(notesOf(*again) == notesOf(*first));

        // And the import is one undo step that leaves nothing behind.
        REQUIRE(stack.undo(project));
        CHECK(project.channels.empty());
        CHECK(project.patterns.empty());
    }
}

TEST_CASE("midi_export_tempo_meta", "[midi]") {
    const auto loaded = adx::tests::loadText(R"([PROJECT]
ADX_VERSION=2

[TEMPO]
0:0:0 100
2:0:0 150
4:0:0 90 ramp

[METER]
0:0:0 4/4
3:0:0 7/8

[CHANNEL Lead]
OUTPUT=insert.1

[PATTERN A]
LENGTH=1:0:0
NOTES Lead
  C4 0:0:0 0:1:0 100

[PLAYLIST]
TRACK 1
  PATTERN A 0:0:0
  PATTERN A 5:0:0 offset=0:1:0

[MIXER]
INSERT 1 name="Master"

[MARKERS]
2:0:0 Chorus
)");
    std::string error;
    const std::optional<MidiSong> song = adx::format::midi::parseMidi(
        adx::format::midi::writeMidi(adx::format::midi::songOf(loaded->project)), error);
    REQUIRE(song.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): checked by the REQUIRE above.
    const MidiSong& parsed = *song;
    CHECK(parsed.division == adx::format::midi::kExportDivision);
    const std::int64_t bar = adx::core::kPpq * 4;
    REQUIRE(parsed.tempos.size() >= 3);
    CHECK(parsed.tempos[0].at == 0);
    CHECK_THAT(parsed.tempos[0].bpm, Catch::Matchers::WithinAbs(100.0, 1e-3));
    CHECK(parsed.tempos[1].at == 2 * bar);
    CHECK_THAT(parsed.tempos[1].bpm, Catch::Matchers::WithinAbs(150.0, 1e-3));
    REQUIRE(parsed.meters.size() == 2);
    CHECK(parsed.meters[1].at == 3 * bar);
    CHECK(parsed.meters[1].numerator == 7);
    CHECK(parsed.meters[1].denominator == 8);
    REQUIRE(parsed.markers.size() == 1);
    CHECK(parsed.markers[0].name == "Chorus");
    // The slipped second placement plays nothing: its only note starts before the slip.
    REQUIRE(parsed.tracks.size() == 1);
    CHECK(parsed.tracks[0].name == "Lead");
    REQUIRE(parsed.tracks[0].notes.size() == 1);
    CHECK(parsed.tracks[0].notes[0].length == adx::core::kPpq);
}
