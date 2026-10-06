// EditNotes and note mute (phase_5.md §4.7): every piano-roll gesture is one command,
// and undoing it restores the pattern byte for byte.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "engine/format/adx/Writer.h"
#include "engine/project/commands/EditNotes.h"
#include "engine/project/commands/NoteCommands.h"
#include "tests/cpp/geometry/GeometryFixtures.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::core::kPpq;
using adx::core::Ticks;
using adx::project::EditNotes;
using adx::project::Note;

namespace {

std::string text(const adx::project::Project& project) {
    return adx::format::write(project, adx::format::WriteOptions{});
}

} // namespace

TEST_CASE("edit_notes_is_one_undo_step", "[commands]") {
    adx::test::RollScene scene(200, 8, 4);
    const std::string before = text(scene.project);
    const std::size_t depth = scene.stack.undoDepth();
    const auto& notes = scene.clip().notes;

    // A slice-shaped edit: shorten one, delete two, change one, add two.
    Note shortened = notes[0];
    shortened.length = Ticks{std::max<std::int64_t>(1, shortened.length.value / 2)};
    Note changed = notes[3];
    changed.velocity = 7;
    changed.muted = true;
    std::vector<Note> added(2);
    added[0].start = Ticks{kPpq};
    added[0].length = Ticks{kPpq};
    added[0].pitch = 70;
    added[1] = added[0];
    added[1].pitch = 72;
    const std::size_t count = notes.size();
    auto command = std::make_unique<EditNotes>(scene.pattern, scene.lead,
                                               std::vector{notes[1].id, notes[2].id},
                                               std::vector{shortened, changed}, added, "Slice");
    const EditNotes* raw = command.get();
    scene.run(std::move(command));

    REQUIRE(scene.stack.undoDepth() == depth + 1);
    REQUIRE(scene.stack.history().back().label == "Slice");
    REQUIRE(scene.clip().notes.size() == count - 2 + 2);
    REQUIRE(raw->created().size() == 2);
    REQUIRE(text(scene.project) != before);

    REQUIRE(scene.stack.undo(scene.project));
    REQUIRE(text(scene.project) == before);
    REQUIRE(scene.stack.redo(scene.project));
    REQUIRE(scene.clip().notes.size() == count);
    REQUIRE(scene.stack.undo(scene.project));
    REQUIRE(text(scene.project) == before);
}

TEST_CASE("edit_notes_emptying_a_clip_removes_it", "[commands]") {
    adx::test::RollScene scene(5, 2, 2);
    const std::string before = text(scene.project);
    std::vector<adx::core::NoteId> all;
    for (const Note& note : scene.clip().notes) {
        all.push_back(note.id);
    }
    scene.run(std::make_unique<EditNotes>(scene.pattern, scene.lead, all, std::vector<Note>{},
                                          std::vector<Note>{}));
    REQUIRE(scene.project.find(scene.pattern)->clipFor(scene.lead) == nullptr);
    REQUIRE(scene.stack.undo(scene.project));
    REQUIRE(text(scene.project) == before);
}

TEST_CASE("note_mute_round_trips", "[format]") {
    auto loaded = adx::tests::loadText(R"([PROJECT]
ADX_VERSION=2

[CHANNEL Tone]
OUTPUT=insert.1

[PATTERN P]
NOTES Tone
  A4 0:0:0 0:1:0 100 mute=yes
  B4 0:1:0 0:1:0 100

[MIXER]
INSERT 1 name="Master"
)");
    const auto& notes = loaded->project.patterns.front().noteClips.front().notes;
    REQUIRE(notes[0].muted);
    REQUIRE_FALSE(notes[1].muted);
    const std::string written = text(loaded->project);
    REQUIRE(written.find("A4 0:0:0 0:1:0 100 mute=yes") != std::string::npos);
    REQUIRE(written.find("B4 0:1:0 0:1:0 100\n") != std::string::npos);
}

TEST_CASE("muted_note_is_silent", "[render]") {
    adx::project::Project project = adx::tests::toneProject({{.start = 0, .length = kPpq}});
    adx::render::RenderStats stats;
    const auto loud = adx::tests::renderFrames(project, 256, 24000, stats);
    REQUIRE(adx::tests::firstSoundFrom(loud, 0) < 24000);

    project.patterns.front().noteClips.front().notes.front().muted = true;
    const auto quiet = adx::tests::renderFrames(project, 256, 24000, stats);
    REQUIRE(adx::tests::firstSoundFrom(quiet, 0) == 24000);
}
