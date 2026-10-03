// Sampler zones in the file format (phase_4.md §4.5; docs/adx-format-v2.md §7.2).
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "engine/format/adx/Parser.h"
#include "engine/format/adx/Writer.h"
#include "engine/project/Diff.h"
#include "engine/project/Project.h"
#include "engine/project/commands/ChannelCommands.h"
#include "engine/project/commands/CommandStack.h"

using adx::format::DiagnosticList;
using adx::project::CommandStack;
using adx::project::LoopMode;
using adx::project::Project;

namespace {

struct Loaded {
    Project project;
    CommandStack stack;
    DiagnosticList diagnostics;
};

std::size_t countCode(const DiagnosticList& list, std::uint16_t code) {
    std::size_t n = 0;
    for (const adx::format::Diagnostic& d : list.all()) {
        n += d.code == code ? 1 : 0;
    }
    return n;
}

std::string describe(const DiagnosticList& list) {
    std::string out;
    for (const adx::format::Diagnostic& d : list.all()) {
        out += d.codeString() + " " + d.message + '\n';
    }
    return out;
}

/// Canonical: what the writer produces, so a round trip is byte-identical.
const std::string kZones =
    "[PROJECT]\nADX_VERSION=2\nTUNING=440\n"
    "\n[TEMPO]\n0:0:0 120\n"
    "\n[METER]\n0:0:0 4/4\n"
    "\n[CHANNEL Drums]\nINSTRUMENT=sampler\nOUTPUT=insert.1\nPOLYPHONY=16\nVOLUME=1\nPAN=0\n"
    "ZONE \"kit/kick.wav\" key=36 root=36\n"
    "ZONE \"kit/snare 1.wav\" key=38 root=38 vel=1..90 rr=1:0\n"
    "ZONE \"kit/snare 2.wav\" key=38 root=38 vel=1..90 rr=1:1\n"
    "ZONE \"pads/string.wav\" key=48..72 root=60 start=120 loop=sustain loopStart=4410 "
    "loopEnd=88200 xfade=512 tune=-12.5 gain=-3 pan=0.25\n"
    "ZONE \"kit/kick.wav\" key=35 root=35 tune=100\n"
    "\n[PLAYLIST]\n"
    "\n[MIXER]\nINSERT 1 name=\"Master\"\n";

} // namespace

TEST_CASE("zones_parse_and_roundtrip", "[format][sampler]") {
    Loaded loaded;
    adx::format::load(kZones, loaded.project, loaded.stack, loaded.diagnostics);
    INFO(describe(loaded.diagnostics));
    CHECK_FALSE(loaded.diagnostics.hasErrors());
    REQUIRE(loaded.project.channels.size() == 1);
    const auto& zones = loaded.project.channels.front().instrument.zones;
    REQUIRE(zones.size() == 5);

    // The same file is one pool entry, however many zones name it.
    CHECK(loaded.project.resources.samples.size() == 4);
    CHECK(zones[0].sample == zones[4].sample);

    CHECK(zones[0].keyLow == 36);
    CHECK(zones[0].keyHigh == 36);
    CHECK(zones[1].velocityHigh == 90);
    CHECK(zones[1].roundRobinGroup == 1);
    CHECK(zones[2].roundRobinIndex == 1);
    const auto& pad = zones[3];
    CHECK(pad.keyLow == 48);
    CHECK(pad.keyHigh == 72);
    CHECK(pad.rootKey == 60);
    CHECK(pad.start == 120);
    CHECK(pad.loop == LoopMode::Sustain);
    CHECK(pad.loopStart == 4410);
    CHECK(pad.loopEnd == 88200);
    CHECK(pad.crossfade == 512);
    CHECK(pad.tuneCents == -12.5F);
    CHECK(pad.gainDb == -3.0F);
    CHECK(pad.pan == 0.25F);

    CHECK(adx::format::write(loaded.project) == kZones);
}

TEST_CASE("zones_accept_note_names_and_report_bad_fields", "[format][sampler]") {
    const std::string text = "[PROJECT]\nADX_VERSION=2\n\n[CHANNEL S]\nINSTRUMENT=sampler\n"
                             "ZONE \"a.wav\" key=C2..C3 root=F#2 loop=sideways colour=red\n"
                             "\n[MIXER]\nINSERT 1 name=\"Master\"\n";
    Loaded loaded;
    adx::format::load(text, loaded.project, loaded.stack, loaded.diagnostics);
    REQUIRE(loaded.project.channels.front().instrument.zones.size() == 1);
    const auto& zone = loaded.project.channels.front().instrument.zones.front();
    // Middle C is C4 = 60 (NoteName.h).
    CHECK(zone.keyLow == 36);
    CHECK(zone.keyHigh == 48);
    CHECK(zone.rootKey == 42);
    CHECK(zone.loop == LoopMode::Off); // the unknown mode left the default
    // Unknown loop mode and unknown key: both reported, neither an error.
    CHECK(countCode(loaded.diagnostics, adx::format::code::kUnknownInlineKey) == 2);
    CHECK_FALSE(loaded.diagnostics.hasErrors());
}

TEST_CASE("set_channel_zones_undoes_and_diffs", "[format][sampler]") {
    Loaded loaded;
    adx::format::load(kZones, loaded.project, loaded.stack, loaded.diagnostics);
    Project& project = loaded.project;
    const Project before = project;
    const std::string text = adx::format::write(project);

    auto zones = project.channels.front().instrument.zones;
    zones.pop_back();
    zones.front().tuneCents = 50.0F;
    loaded.stack.execute(
        std::make_unique<adx::project::SetChannelZones>(project.channels.front().id, zones),
        project);
    CHECK(project.channels.front().instrument.zones.size() == 4);

    const auto changes = adx::project::diff(before, project);
    REQUIRE(changes.size() == 1);
    CHECK(changes.front().detail.find("zones") != std::string::npos);

    CHECK(loaded.stack.undo(project));
    CHECK(adx::format::write(project) == text);
}
