// The Phase 4 A0 note and clip extensions: slide, bend, lyric, clip envelopes.
//
// phase_4.md §4.0: each is additive. A file that uses none of them must write exactly
// what Phase 2's writer wrote, and a file that does must round-trip them.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "engine/format/adx/Parser.h"
#include "engine/format/adx/Writer.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/NoteCommands.h"
#include "engine/project/commands/PlaylistCommands.h"
#include "tests/cpp/Corpus.h"

using adx::format::DiagnosticList;
using adx::project::CommandStack;
using adx::project::Project;

namespace {

struct Loaded {
    Project project;
    CommandStack stack;
    DiagnosticList diagnostics;
};

void load(const std::string& text, Loaded& out) {
    adx::format::load(text, out.project, out.stack, out.diagnostics);
}

const std::string kExtended =
    "[PROJECT]\nADX_VERSION=2\nTUNING=440\n"
    "\n[TEMPO]\n0:0:0 120\n"
    "\n[METER]\n0:0:0 4/4\n"
    "\n[CHANNEL Voice]\nINSTRUMENT=additive\nOUTPUT=insert.1\nPOLYPHONY=16\nVOLUME=1\nPAN=0\n"
    "\n[PATTERN P]\nLENGTH=1:0:0\nNOTES Voice\n"
    "  A4 0:0:0 0:1:0 100 slide=3st@0:0:1920+0:0:960~smooth lyric=\"ka\"\n"
    "  C5 0:1:0 0:1:0 100 bend=0st@0:0:0|-50c@0:0:1920~exponential(0.5)|0st@0:1:0\n"
    "  E5 0:2:0 0:1:0 100 lyric=\"a i\"\n"
    "\n[PLAYLIST]\nTRACK 1\n  PATTERN P 0:0:0\n"
    "    ENVELOPE channel.Voice.volume\n      0:0:0 0.5\n      0:2:0 1 smooth\n"
    "    ENVELOPE gain\n      0:0:0 1\n"
    "\n[MIXER]\nINSERT 1 name=\"Master\"\n";

} // namespace

TEST_CASE("new_note_fields_absent_roundtrip_unchanged", "[format][a0]") {
    // tests/data/canonical/ holds what Phase 3's writer produced for every corpus and
    // golden file, captured before A0 touched the model. The extensions are absent
    // from all of them, so the writer must reproduce those bytes exactly.
    const std::filesystem::path canonical = adx::tests::repoRoot() / "tests" / "data" / "canonical";
    std::size_t checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(canonical)) {
        if (entry.path().extension() != ".adx") {
            continue;
        }
        INFO(entry.path().string());
        const std::string expected = adx::tests::readFile(entry.path());
        Loaded loaded;
        load(expected, loaded);
        CHECK(adx::format::write(loaded.project) == expected);
        ++checked;
    }
    CHECK(checked >= 9);
}

TEST_CASE("note_extensions_roundtrip", "[format][a0]") {
    Loaded loaded;
    load(kExtended, loaded);
    INFO(loaded.diagnostics.all().size());
    REQUIRE_FALSE(loaded.diagnostics.hasErrors());
    CHECK(adx::format::write(loaded.project) == kExtended);

    const adx::project::NoteClip& clip = loaded.project.patterns.front().noteClips.front();
    REQUIRE(clip.extras.size() == 3);
    const adx::project::NoteExtras* first = clip.extrasFor(clip.notes[0].id);
    REQUIRE(first != nullptr);
    REQUIRE(first->slide.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): checked by the REQUIRE above.
    const adx::project::NoteSlide& slide = *first->slide;
    CHECK(slide.targetCents == 300);
    CHECK(slide.start.value == 1920);
    CHECK(slide.length.value == 960);
    CHECK(slide.curve.kind == adx::core::CurveKind::Smooth);
    CHECK(first->lyric == "ka");

    const adx::project::NoteExtras* second = clip.extrasFor(clip.notes[1].id);
    REQUIRE(second != nullptr);
    REQUIRE(second->pitchCurve.size() == 3);
    CHECK(second->pitchCurve[1].cents == -50);
    CHECK(second->pitchCurve[1].curve.kind == adx::core::CurveKind::Exponential);

    const adx::project::PlaylistItem& item = loaded.project.playlist.tracks.front().items.front();
    REQUIRE(item.envelopes.size() == 2);
    CHECK(item.envelopes[0].local == adx::project::ClipTarget::Param);
    CHECK(item.envelopes[0].target.valid());
    CHECK(item.envelopes[1].local == adx::project::ClipTarget::Gain);
}

TEST_CASE("note_extension_commands_undo", "[format][a0]") {
    Loaded loaded;
    load(kExtended, loaded);
    Project& project = loaded.project;
    const std::string before = adx::format::write(project);
    const adx::project::Pattern& pattern = project.patterns.front();
    const adx::project::NoteClip& clip = pattern.noteClips.front();
    const adx::core::NoteId third = clip.notes[2].id;

    loaded.stack.execute(std::make_unique<adx::project::SetNoteSlide>(
                             pattern.id, clip.channel, third,
                             adx::project::NoteSlide{.targetCents = -1200,
                                                     .start = adx::core::Ticks{0},
                                                     .length = adx::core::Ticks{480},
                                                     .curve = {}}),
                         project);
    loaded.stack.execute(
        std::make_unique<adx::project::SetLyric>(pattern.id, clip.channel, third, std::string{}),
        project);
    const std::string edited = adx::format::write(project);
    CHECK(edited != before);
    CHECK(edited.find("slide=-12st@0:0:0+0:0:480") != std::string::npos);
    CHECK(edited.find("lyric=\"a i\"") == std::string::npos);

    // A deleted note takes its extras with it, and undo brings both back.
    loaded.stack.execute(std::make_unique<adx::project::RemoveNotes>(
                             pattern.id, clip.channel, std::vector<adx::core::NoteId>{third}),
                         project);
    CHECK(project.patterns.front().noteClips.front().extrasFor(third) == nullptr);
    CHECK(loaded.stack.undo(project));
    CHECK(adx::format::write(project) == edited);

    CHECK(loaded.stack.undo(project));
    CHECK(loaded.stack.undo(project));
    CHECK(adx::format::write(project) == before);
}

TEST_CASE("note_extension_malformed_is_a_diagnostic", "[format][a0]") {
    const std::string bad = std::string{kExtended}.replace(kExtended.find("3st@"), 4, "3@");
    Loaded loaded;
    load(bad, loaded);
    bool found = false;
    for (const auto& diagnostic : loaded.diagnostics.all()) {
        found = found || diagnostic.code == adx::format::code::kMalformedKeyValue;
    }
    CHECK(found);
}
