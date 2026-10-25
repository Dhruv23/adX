// Patch: a re-parsed file turned into commands (phase_7.md §4.2, §6).
//
// The correctness property is that a patched project *is* the target - compared on
// the written text, which is what a person and git see - and that undoing the patch
// gives back exactly what was there. The performance property is narrowness: a
// catalogue of single edits, each with the number of commands it is documented to
// cost, so a regression shows up as a count rather than as a feeling.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "engine/format/adx/Parser.h"
#include "engine/format/adx/Patch.h"
#include "engine/format/adx/Writer.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/NoteCommands.h"
#include "tests/cpp/Corpus.h"
#include "tests/cpp/Env.h"
#include "tests/cpp/project/RandomCommands.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::project::CommandStack;
using adx::project::Project;

namespace {

struct Doc {
    Project project;
    CommandStack stack;
    adx::format::DiagnosticList diagnostics;
};

std::unique_ptr<Doc> parse(std::string_view text) {
    auto doc = std::make_unique<Doc>();
    adx::format::load(text, doc->project, doc->stack, doc->diagnostics);
    doc->stack.clear();
    return doc;
}

/// Patches `from` to `to`; checks the result writes as `to` does and that one undo
/// writes as `from` did. Returns the command count.
std::size_t patchAndCheck(std::string_view from, std::string_view to) {
    auto current = parse(from);
    const std::string before = adx::format::write(current->project);
    auto target = parse(to);
    const std::size_t history = current->stack.history().size();
    const auto result = adx::format::applyPatch(current->project, current->stack, target->project);
    CHECK(adx::format::write(current->project) == adx::format::write(target->project));
    CHECK(adx::format::sameContent(current->project,
                                   adx::format::remapInto(current->project, target->project)));
    if (result.commands == 0) {
        CHECK(current->stack.history().size() == history);
    } else {
        CHECK(current->stack.history().size() == history + 1);
        REQUIRE(current->stack.undo(current->project));
        CHECK(adx::format::write(current->project) == before);
    }
    return result.commands;
}

std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const std::size_t at = text.find(from);
    REQUIRE(at != std::string::npos);
    REQUIRE(text.find(from, at + 1) == std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

constexpr std::string_view kFixture = R"([PROJECT]
ADX_VERSION=2
TITLE="Patch catalogue"

[TEMPO]
0:0:0 120

[CHANNEL Lead]
OUTPUT=insert.2
VOLUME=0.8
PARAM filter.cutoff=1200
PARAM env.release=0.3

[CHANNEL Drums]
OUTPUT=insert.2

[PATTERN Verse]
LENGTH=1:0:0
NOTES Lead
  C4 0:0:0 0:1:0 100
  E4 0:1:0 0:1:0 90
  G4 0:2:0 0:1:0 80
MINI Drums
  bd sn
AUTOMATION channel.Lead.filter.cutoff
  0:0:0 400
  0:2:0 3200

[PATTERN Chorus]
LENGTH=1:0:0
NOTES Lead
  A4 0:0:0 0:2:0 100

[PLAYLIST]
TRACK 1
  PATTERN Verse 0:0:0
  PATTERN Chorus 1:0:0

[MIXER]
INSERT 1 name="Master"
INSERT 2 name="Bus" gain=0.9
  SLOT 1 Reverb mix=0.3 room=0.5
  SEND 1 insert.3 level=0.25
INSERT 3 name="Aux"
ROUTE insert.2 -> insert.1
ROUTE insert.3 -> insert.1

[MARKERS]
0:0:0 "Intro"
)";

struct Edit {
    std::string_view what;
    std::string_view from;
    std::string_view to;
    std::size_t commands;
};

} // namespace

TEST_CASE("patch_fixture_is_clean", "[sync][patch]") {
    auto doc = parse(kFixture);
    for (const auto& d : doc->diagnostics.all()) {
        INFO(d.codeString() << " line " << d.span.line << ": " << d.message);
        CHECK(d.severity != adx::format::Severity::Error);
    }
}

TEST_CASE("patch_minimal_catalogue", "[sync][patch]") {
    // The documented cost of each single edit (phase_7.md §6). Narrow edits are one
    // command of the matching kind; structural ones are one ReplaceProject; edits that
    // do not change the project are none.
    const std::vector<Edit> catalogue{
        {.what = "note velocity",
         .from = "E4 0:1:0 0:1:0 90",
         .to = "E4 0:1:0 0:1:0 91",
         .commands = 1},
        {.what = "note length",
         .from = "E4 0:1:0 0:1:0 90",
         .to = "E4 0:1:0 0:2:0 90",
         .commands = 1},
        {.what = "note pitch",
         .from = "E4 0:1:0 0:1:0 90",
         .to = "F4 0:1:0 0:1:0 90",
         .commands = 1},
        {.what = "note start",
         .from = "E4 0:1:0 0:1:0 90",
         .to = "E4 0:1:960 0:1:0 90",
         .commands = 1},
        {.what = "note added",
         .from = "  G4 0:2:0 0:1:0 80\n",
         .to = "  G4 0:2:0 0:1:0 80\n  B4 0:3:0 0:1:0 70\n",
         .commands = 1},
        {.what = "note removed", .from = "  G4 0:2:0 0:1:0 80\n", .to = "", .commands = 1},
        {.what = "two notes in one clip",
         .from = "  C4 0:0:0 0:1:0 100\n  E4 0:1:0 0:1:0 90",
         .to = "  C4 0:0:0 0:1:0 101\n  E4 0:1:0 0:1:0 91",
         .commands = 1},
        {.what = "channel volume", .from = "VOLUME=0.8", .to = "VOLUME=0.7", .commands = 1},
        {.what = "channel pan added",
         .from = "VOLUME=0.8",
         .to = "VOLUME=0.8\nPAN=-0.5",
         .commands = 1},
        {.what = "channel param",
         .from = "PARAM filter.cutoff=1200",
         .to = "PARAM filter.cutoff=1800",
         .commands = 1},
        {.what = "two channel params",
         .from = "PARAM filter.cutoff=1200\nPARAM env.release=0.3",
         .to = "PARAM filter.cutoff=1800\nPARAM env.release=0.4",
         .commands = 2},
        {.what = "insert gain",
         .from = "INSERT 2 name=\"Bus\" gain=0.9",
         .to = "INSERT 2 name=\"Bus\" gain=0.8",
         .commands = 1},
        {.what = "slot parameter",
         .from = "SLOT 1 Reverb mix=0.3 room=0.5",
         .to = "SLOT 1 Reverb mix=0.3 room=0.6",
         .commands = 1},
        {.what = "slot mix",
         .from = "SLOT 1 Reverb mix=0.3 room=0.5",
         .to = "SLOT 1 Reverb mix=0.4 room=0.5",
         .commands = 1},
        {.what = "send level", .from = "level=0.25", .to = "level=0.5", .commands = 1},
        {.what = "pattern length",
         .from = "[PATTERN Chorus]\nLENGTH=1:0:0",
         .to = "[PATTERN Chorus]\nLENGTH=2:0:0",
         .commands = 1},
        {.what = "mini-notation", .from = "  bd sn\n", .to = "  bd*2 sn\n", .commands = 1},
        {.what = "automation point", .from = "0:2:0 3200", .to = "0:2:0 6400", .commands = 1},
        {.what = "tempo", .from = "0:0:0 120", .to = "0:0:0 128", .commands = 1},
        {.what = "marker renamed",
         .from = "0:0:0 \"Intro\"",
         .to = "0:0:0 \"Start\"",
         .commands = 1},
        {.what = "title",
         .from = "TITLE=\"Patch catalogue\"",
         .to = "TITLE=\"Renamed\"",
         .commands = 1},
        {.what = "playlist item moved",
         .from = "PATTERN Chorus 1:0:0",
         .to = "PATTERN Chorus 2:0:0",
         .commands = 1},
        {.what = "channel added",
         .from = "[CHANNEL Drums]",
         .to = "[CHANNEL Pad]\nOUTPUT=insert.1\n\n[CHANNEL Drums]",
         .commands = 1},
        {.what = "insert added",
         .from = "INSERT 3 name=\"Aux\"",
         .to = "INSERT 3 name=\"Aux\"\nINSERT 4 name=\"Fx\"",
         .commands = 1},
        {.what = "route removed", .from = "ROUTE insert.3 -> insert.1\n", .to = "", .commands = 1},
        {.what = "comment only",
         .from = "[CHANNEL Drums]",
         .to = "# the kit\n[CHANNEL Drums]",
         .commands = 0},
        {.what = "blank lines only",
         .from = "[PATTERN Chorus]",
         .to = "\n\n[PATTERN Chorus]",
         .commands = 0},
        {.what = "spelling only", .from = "VOLUME=0.8", .to = "VOLUME=0.800", .commands = 0},
    };
    for (const Edit& edit : catalogue) {
        INFO(edit.what);
        const std::string after = replaced(std::string(kFixture), edit.from, edit.to);
        CHECK(patchAndCheck(kFixture, after) == edit.commands);
    }
}

TEST_CASE("patch_minimal_single_note", "[sync][patch]") {
    // Editing one note's velocity in suffocation.adx yields exactly one command - a
    // SetNoteValue - and the note keeps its id, so a voice playing it is not restarted.
    // The file in docs/examples is v1; the text an editor and adX share is the v2 text
    // adX writes, so that is the one edited here (patching the v1 file into v2 text
    // also changes ADX_VERSION, which is a second, honest command).
    const std::string text = adx::tests::readFile(adx::tests::repoRoot() / "tests" / "data" /
                                                  "canonical" / "docs_examples_suffocation.adx");
    auto current = parse(text);
    Project edited = current->project;
    CommandStack scratch;
    adx::project::Pattern* pattern = nullptr;
    for (auto& p : edited.patterns) {
        if (!p.noteClips.empty() && p.noteClips.front().notes.size() > 3) {
            pattern = &p;
            break;
        }
    }
    REQUIRE(pattern != nullptr);
    const adx::project::NoteClip& clip = pattern->noteClips.front();
    const adx::core::NoteId id = clip.notes[2].id;
    const int velocity = clip.notes[2].velocity;
    scratch.execute(std::make_unique<adx::project::SetNoteValue>(
                        pattern->id, clip.channel, std::vector<adx::core::NoteId>{id},
                        adx::project::NoteField::Velocity,
                        velocity == 127 ? 126.0 : velocity + 1.0),
                    edited);
    auto target = parse(adx::format::write(edited));

    const auto result = adx::format::applyPatch(current->project, current->stack, target->project);
    CHECK(result.commands == 1);
    CHECK_FALSE(result.structural);
    CHECK(adx::format::write(current->project) == adx::format::write(edited));
    REQUIRE(current->stack.history().size() == 1);
    // The note that changed is the note that was there.
    const adx::project::Pattern* after = current->project.find(pattern->id);
    REQUIRE(after != nullptr);
    const auto& notes = after->noteClips.front().notes;
    CHECK(std::ranges::find(notes, id, &adx::project::Note::id) != notes.end());
}

TEST_CASE("patch_section_move_is_noop", "[sync][patch]") {
    // Moving whole sections around is formatting, not music: zero commands.
    std::string moved = replaced(std::string(kFixture), "[CHANNEL Drums]\nOUTPUT=insert.2\n\n", "");
    moved += "\n[CHANNEL Drums]\nOUTPUT=insert.2\n";
    moved = replaced(moved, "[MARKERS]\n0:0:0 \"Intro\"\n", "");
    moved = "[MARKERS]\n0:0:0 \"Intro\"\n\n" + moved;
    auto current = parse(kFixture);
    auto target = parse(moved);
    REQUIRE_FALSE(target->diagnostics.hasErrors());
    const auto result = adx::format::applyPatch(current->project, current->stack, target->project);
    CHECK(result.commands == 0);
    CHECK(current->stack.history().empty());
}

TEST_CASE("patch_is_one_undo_group", "[sync][patch]") {
    // An external reload is one history entry however large: every corpus file patched
    // into every other.
    const auto paths = adx::tests::corpusPaths();
    for (const auto& a : paths) {
        for (const auto& b : paths) {
            INFO(a.filename().string() << " -> " << b.filename().string());
            const std::string from = adx::tests::readFile(a);
            const std::string to = adx::tests::readFile(b);
            static_cast<void>(patchAndCheck(from, to));
        }
    }
}

TEST_CASE("patch_apply_equals_target", "[sync][patch]") {
    // apply(computePatch(a, b)) == b over random but realistic edits: each pair is a
    // corpus file and the same file after a few random commands (the generator behind
    // undo_to_empty_random), so every kind of command the GUI can issue gets diffed.
    const int pairs = adx::tests::optimisedBuild() ? 10000 : 1000;
    const std::string base =
        adx::format::write(parse(adx::tests::readFile(adx::tests::repoRoot() / "tests" / "golden" /
                                                      "routing_sends.adx"))
                               ->project);
    auto original = parse(base);
    // ADX_PATCH_SEED explores further seeds by hand; CI keeps the fixed one.
    const auto seedText = adx::tests::environment("ADX_PATCH_SEED");
    const std::uint32_t seed =
        seedText ? static_cast<std::uint32_t>(std::stoul(*seedText)) : 20261021U;
    INFO("seed " << seed);
    std::mt19937 rng(seed);
    int structural = 0;
    for (int pair = 0; pair < pairs; ++pair) {
        Project edited = original->project;
        CommandStack scratch;
        adx::tests::Generator generator(rng, edited);
        const int edits = 1 + static_cast<int>(rng() % 4);
        for (int i = 0; i < edits; ++i) {
            if (auto command = generator.next()) {
                scratch.execute(std::move(command), edited);
            }
        }
        const std::string to = adx::format::write(edited);
        Project current = original->project;
        CommandStack stack;
        auto target = parse(to);
        const auto result = adx::format::applyPatch(current, stack, target->project);
        structural += result.structural ? 1 : 0;
        const std::string written = adx::format::write(current);
        if (written != adx::format::write(target->project)) {
            INFO("pair " << pair << "\n--- target\n" << to << "\n--- patched\n" << written);
            REQUIRE(written == adx::format::write(target->project));
        }
        if (result.commands > 0) {
            REQUIRE(stack.undo(current));
            REQUIRE(adx::format::write(current) == base);
        }
    }
    INFO(structural << " of " << pairs << " pairs needed a structural replace");
    CHECK(structural < pairs);
}
