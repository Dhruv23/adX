// The v2 parser: sections in any order, indentation-based blocks, references, and
// the guarantee that nothing reaches the model except through a command.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <variant>

#include "engine/format/adx/Parser.h"
#include "engine/project/Project.h"
#include "engine/project/Validate.h"
#include "engine/project/commands/CommandStack.h"

using adx::format::DiagnosticList;
using adx::project::CommandStack;
using adx::project::Project;

namespace {

struct Loaded {
    Project project;
    CommandStack stack;
    DiagnosticList diagnostics;
};

void loadInto(Loaded& out, const std::string& text, bool group = true) {
    adx::format::ParseOptions options;
    options.groupAsOneStep = group;
    adx::format::load(text, out.project, out.stack, out.diagnostics, options);
}

void reportErrors(const DiagnosticList& diagnostics) {
    for (const auto& item : diagnostics.all()) {
        if (item.severity == adx::format::Severity::Error) {
            UNSCOPED_INFO(item.codeString() << " " << item.span.line << ':' << item.span.column
                                            << " " << item.message);
        }
    }
}

/// A small but complete v2 project touching every section.
const std::string kSample = "[PROJECT]\n"
                            "ADX_VERSION=2\n"
                            "TITLE=\"Suffocation\"\n"
                            "TUNING=440.0\n"
                            "LOOP=0:0:0-8:0:0\n"
                            "\n"
                            "[TEMPO]\n"
                            "0:0:0 140.0\n"
                            "32:0:0 150.0 ramp\n"
                            "\n"
                            "[METER]\n"
                            "0:0:0 4/4\n"
                            "\n"
                            "[CHANNEL Lead]\n"
                            "INSTRUMENT=additive\n"
                            "OUTPUT=insert.2\n"
                            "POLYPHONY=8\n"
                            "VOLUME=0.8\n"
                            "PAN=0\n"
                            "PARAM env.attack=0.01 curve=bezier(0.2,0,0.8,1)\n"
                            "PARAM filter.cutoff=1200\n"
                            "ARP mode=up rate=1/16 octaves=2 gate=0.8\n"
                            "\n"
                            "[CHANNEL Drums]\n"
                            "INSTRUMENT=sampler\n"
                            "OUTPUT=insert.1\n"
                            "\n"
                            "[PATTERN Verse]\n"
                            "LENGTH=8:0:0\n"
                            "NOTES Lead\n"
                            "  F#5 0:0:0 0:1:0 102\n"
                            "  A5 0:1:0 0:1:0 92 pan=0.25\n"
                            "MINI Drums\n"
                            "  bd*4, [~ sn]*2, hh(5,8)\n"
                            "AUTOMATION channel.Lead.filter.cutoff\n"
                            "  0:0:0 400 smooth\n"
                            "  4:0:0 3200 bezier(0.3,0,0.7,1)\n"
                            "\n"
                            "[PLAYLIST]\n"
                            "TRACK 1 name=\"Drums\"\n"
                            "  PATTERN Verse 0:0:0\n"
                            "  PATTERN Verse 8:0:0\n"
                            "TRACK 2 name=\"Vox\"\n"
                            "  AUDIO \"vocals take 3.wav\" 16:0:0 stretch=1.0 pitch=0 reverse=no\n"
                            "\n"
                            "[MIXER]\n"
                            "INSERT 1 name=\"Master\"\n"
                            "INSERT 2 name=\"Lead\" gain=0.9 pan=-0.1\n"
                            "  SLOT 1 Reverb mix=0.35 room=0.8 damp=0.5\n"
                            "  SLOT 2 EQ low=0 mid=2.5 high=-1\n"
                            "  SEND 1 insert.1 level=0.3 pre=no\n"
                            "ROUTE insert.2 -> insert.1\n"
                            "\n"
                            "[MARKERS]\n"
                            "0:0:0 \"Intro\"\n"
                            "32:0:0 \"Drop\"\n";

} // namespace

TEST_CASE("a v2 file parses into the model the spec describes", "[format][parser]") {
    Loaded loaded;
    loadInto(loaded, kSample);
    reportErrors(loaded.diagnostics);
    REQUIRE_FALSE(loaded.diagnostics.hasErrors());

    const Project& project = loaded.project;
    CHECK(project.meta.title == "Suffocation");
    CHECK(project.meta.version == 2);
    CHECK(project.meta.loopEnabled);
    CHECK(project.meta.loopEnd.value == adx::core::kPpq * 4 * 8);

    REQUIRE(project.tempo.tempoEvents().size() == 2);
    CHECK(project.tempo.tempoEvents()[1].ramp);
    CHECK(project.tempo.bpmAt(adx::core::Ticks{0}) == 140.0);

    REQUIRE(project.channels.size() == 2);
    const auto& lead = project.channels.front();
    CHECK(lead.name == "Lead");
    CHECK(lead.maxPolyphony == 8);
    CHECK(lead.output.value == 2);
    REQUIRE(lead.instrument.params.size() == 2);
    CHECK(lead.instrument.params[0].name == "env.attack");
    CHECK(lead.instrument.params[0].hasCurve);
    CHECK(lead.instrument.params[0].curve.kind == adx::core::CurveKind::Bezier);
    CHECK(lead.arp.mode == adx::project::ArpMode::Up);
    CHECK(lead.arp.rate == adx::core::Rational::make(1, 16));

    REQUIRE(project.patterns.size() == 1);
    const auto& pattern = project.patterns.front();
    CHECK(pattern.length.value == adx::core::kPpq * 4 * 8);
    REQUIRE(pattern.noteClips.size() == 1);
    REQUIRE(pattern.noteClips.front().notes.size() == 2);
    CHECK(pattern.noteClips.front().notes[0].pitch == 78);
    CHECK(pattern.noteClips.front().notes[1].pan == 0.25F);
    REQUIRE(pattern.mini.size() == 1);
    CHECK(pattern.mini.front().lines.front() == "bd*4, [~ sn]*2, hh(5,8)");
    REQUIRE(pattern.autoClips.size() == 1);
    CHECK(pattern.autoClips.front().target.valid());
    CHECK(pattern.autoClips.front().points.size() == 2);

    REQUIRE(project.playlist.tracks.size() == 2);
    CHECK(project.playlist.tracks[0].items.size() == 2);
    CHECK(project.playlist.tracks[1].items.size() == 1);
    REQUIRE(project.resources.samples.size() == 1);
    CHECK(project.resources.samples.front().path == "vocals take 3.wav");

    REQUIRE(project.mixer.inserts.size() == 2);
    CHECK(project.mixer.master.value == 1);
    CHECK(project.mixer.inserts[1].slots.size() == 2);
    CHECK(project.mixer.inserts[1].sends.size() == 1);
    CHECK(project.mixer.routes.size() == 1);

    CHECK(project.markers.size() == 2);

    DiagnosticList invariants;
    const bool valid = adx::project::validate(project, invariants);
    reportErrors(invariants);
    CHECK(valid);
}

TEST_CASE("sections may appear in any order", "[format][parser]") {
    // The parser processes by dependency, not by file position: a channel may name an
    // insert declared further down, and a playlist may place a pattern declared after
    // it.
    const std::string reordered =
        "[MARKERS]\n0:0:0 \"Intro\"\n"
        "\n[PLAYLIST]\nTRACK 1\n  PATTERN Verse 0:0:0\n"
        "\n[PATTERN Verse]\nLENGTH=4:0:0\nNOTES Lead\n  C4 0:0:0 0:1:0 96\n"
        "\n[CHANNEL Lead]\nOUTPUT=insert.7\n"
        "\n[MIXER]\nINSERT 7 name=\"Only\"\n"
        "\n[PROJECT]\nADX_VERSION=2\n"
        "\n[TEMPO]\n0:0:0 100\n";

    Loaded loaded;
    loadInto(loaded, reordered);
    reportErrors(loaded.diagnostics);
    CHECK_FALSE(loaded.diagnostics.hasErrors());
    REQUIRE(loaded.project.channels.size() == 1);
    CHECK(loaded.project.channels.front().output.value == 7);
    CHECK(loaded.project.playlist.tracks.front().items.size() == 1);
}

TEST_CASE("a file with no mixer still gets a master", "[format][parser]") {
    Loaded loaded;
    loadInto(loaded, "[PROJECT]\nADX_VERSION=2\n\n[CHANNEL Lead]\nVOLUME=1\n");
    CHECK_FALSE(loaded.diagnostics.hasErrors());
    REQUIRE(loaded.project.mixer.inserts.size() == 1);
    CHECK(loaded.project.mixer.inserts.front().name == "Master");
    CHECK(loaded.project.channels.front().output == loaded.project.mixer.master);
}

TEST_CASE("unknown keys and sections become residue", "[format][parser]") {
    Loaded loaded;
    loadInto(loaded, "[PROJECT]\nADX_VERSION=2\nFUTURE_KEY=1\n\n[FUTURE_SECTION]\nsomething\n");
    CHECK_FALSE(loaded.diagnostics.hasErrors());

    bool sawKey = false;
    bool sawSection = false;
    for (const auto& item : loaded.diagnostics.all()) {
        sawKey = sawKey || item.code == adx::format::code::kUnknownKey;
        sawSection = sawSection || item.code == adx::format::code::kUnknownSection;
    }
    CHECK(sawKey);
    CHECK(sawSection);

    const auto* projectResidue = loaded.project.residue.forSection("PROJECT", "");
    REQUIRE(projectResidue != nullptr);
    REQUIRE(projectResidue->lines.size() == 1);
    CHECK(projectResidue->lines.front().raw == "FUTURE_KEY=1");
}

TEST_CASE("parser_uses_commands", "[format][parser]") {
    // Loading a file and then undoing every command yields an empty project. This is
    // what makes FINAL_PLAN §4's one-command-set guarantee real: anything the parser
    // did outside a command would survive the undo and show up here.
    Loaded loaded;
    loadInto(loaded, kSample, /*group=*/false);
    REQUIRE_FALSE(loaded.diagnostics.hasErrors());
    REQUIRE(loaded.stack.undoDepth() > 0);

    // Residue and the base directory are set by the loader outside the command
    // stream, and are not model state - they are facts about the file. Clearing them
    // is what makes "empty" comparable.
    loaded.project.residue = {};
    loaded.project.resources.baseDirectory.clear();

    while (loaded.stack.undo(loaded.project)) {
        // Unwind the whole load.
    }

    const Project empty;
    CHECK(loaded.project == empty);
}

TEST_CASE("a grouped load is one undo step", "[format][parser]") {
    Loaded loaded;
    loadInto(loaded, kSample, /*group=*/true);
    CHECK(loaded.stack.undoDepth() == 1);
    CHECK(loaded.stack.history().size() == 1);
    CHECK(loaded.stack.history().front().label == "Load project");

    loaded.project.residue = {};
    loaded.project.resources.baseDirectory.clear();
    CHECK(loaded.stack.undo(loaded.project));
    CHECK(loaded.project == Project{});
}

TEST_CASE("an automation path that names nothing is an error with a span", "[format][parser]") {
    Loaded loaded;
    loadInto(loaded, "[PROJECT]\nADX_VERSION=2\n\n[CHANNEL Lead]\nVOLUME=1\n\n"
                     "[PATTERN P]\nAUTOMATION channel.Lead.filter.cutoff\n  0:0:0 100\n");
    // The channel does not declare filter.cutoff, so the lane cannot resolve. Adding
    // it silently would write a PARAM line the author never typed.
    CHECK(loaded.diagnostics.hasErrors());
    bool reported = false;
    for (const auto& item : loaded.diagnostics.all()) {
        reported = reported || item.code == adx::format::code::kUnresolvedParamPath;
    }
    CHECK(reported);
}

TEST_CASE("a channel name with spaces works everywhere it appears", "[format][parser]") {
    Loaded loaded;
    loadInto(loaded, "[PROJECT]\nADX_VERSION=2\n\n[CHANNEL Hardstyle Kick]\nPARAM drive=6\n\n"
                     "[PATTERN P]\nNOTES \"Hardstyle Kick\"\n  C2 0:0:0 0:1:0 100\n"
                     "AUTOMATION channel.\"Hardstyle Kick\".drive\n  0:0:0 6\n");
    reportErrors(loaded.diagnostics);
    CHECK_FALSE(loaded.diagnostics.hasErrors());
    REQUIRE(loaded.project.channels.size() == 1);
    CHECK(loaded.project.channels.front().name == "Hardstyle Kick");
    REQUIRE(loaded.project.patterns.size() == 1);
    CHECK(loaded.project.patterns.front().noteClips.size() == 1);
    REQUIRE(loaded.project.patterns.front().autoClips.size() == 1);
    CHECK(loaded.project.patterns.front().autoClips.front().target.valid());
}
