// The command stack: coalescing, grouping, dirty masks, and the id discipline that
// makes undo-then-redo produce the same bytes.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

#include "engine/project/Project.h"
#include "engine/project/commands/ChannelCommands.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/MixerCommands.h"
#include "engine/project/commands/NoteCommands.h"
#include "engine/project/commands/PatternCommands.h"
#include "engine/project/commands/PlaylistCommands.h"
#include "engine/project/commands/ProjectCommands.h"

using namespace adx::project;
using adx::core::Ticks;

namespace {

using Clock = CommandStack::Clock;

struct Fixture {
    Project project;
    CommandStack stack;
    Clock::time_point now;

    void at(std::chrono::milliseconds offset, std::unique_ptr<Command> command) {
        stack.executeAt(std::move(command), project, now + offset);
    }
    void run(std::unique_ptr<Command> command) {
        stack.execute(std::move(command), project);
    }
};

/// A project with one channel, one pattern and two notes in it.
struct Scene : Fixture {
    adx::core::ChannelId channel;
    adx::core::PatternId pattern;
    std::vector<adx::core::NoteId> notes;

    Scene() {
        Channel prototype;
        prototype.name = "Lead";
        auto add = std::make_unique<AddChannel>(std::move(prototype));
        channel = add->created();
        run(std::move(add));
        channel = project.channels.front().id;

        Pattern patternPrototype;
        patternPrototype.name = "Verse";
        auto addPattern = std::make_unique<AddPattern>(std::move(patternPrototype));
        const AddPattern* rawPattern = addPattern.get();
        run(std::move(addPattern));
        pattern = rawPattern->created();

        std::vector<Note> newNotes(2);
        newNotes[0].start = Ticks{0};
        newNotes[0].length = Ticks{adx::core::kPpq};
        newNotes[0].pitch = 60;
        newNotes[1].start = Ticks{adx::core::kPpq};
        newNotes[1].length = Ticks{adx::core::kPpq};
        newNotes[1].pitch = 64;
        auto addNotes = std::make_unique<AddNotes>(pattern, channel, std::move(newNotes));
        const AddNotes* rawNotes = addNotes.get();
        run(std::move(addNotes));
        notes = rawNotes->created();
    }
};

} // namespace

TEST_CASE("command_coalescing_window", "[commands]") {
    Scene scene;
    const std::size_t before = scene.stack.undoDepth();

    // Two drags 100 ms apart become one history entry.
    scene.at(std::chrono::milliseconds{1000},
             std::make_unique<MoveNotes>(scene.pattern, scene.channel, scene.notes, Ticks{120}, 0));
    scene.at(std::chrono::milliseconds{1100},
             std::make_unique<MoveNotes>(scene.pattern, scene.channel, scene.notes, Ticks{120}, 0));
    CHECK(scene.stack.undoDepth() == before + 1);

    // 600 ms apart, they do not.
    scene.at(std::chrono::milliseconds{1700},
             std::make_unique<MoveNotes>(scene.pattern, scene.channel, scene.notes, Ticks{120}, 0));
    CHECK(scene.stack.undoDepth() == before + 2);

    // And one undo of the merged pair puts both moves back.
    const auto& clip = scene.project.patterns.front().noteClips.front();
    CHECK(clip.notes[0].start.value == 360);
    CHECK(scene.stack.undo(scene.project));
    CHECK(clip.notes[0].start.value == 240);
    CHECK(scene.stack.undo(scene.project));
    CHECK(clip.notes[0].start.value == 0);
}

TEST_CASE("a group boundary prevents coalescing", "[commands]") {
    Scene scene;
    const std::size_t before = scene.stack.undoDepth();

    scene.at(std::chrono::milliseconds{0},
             std::make_unique<MoveNotes>(scene.pattern, scene.channel, scene.notes, Ticks{10}, 0));
    scene.stack.beginGroup("Quantize");
    scene.at(std::chrono::milliseconds{10},
             std::make_unique<MoveNotes>(scene.pattern, scene.channel, scene.notes, Ticks{10}, 0));
    scene.stack.endGroup();
    scene.at(std::chrono::milliseconds{20},
             std::make_unique<MoveNotes>(scene.pattern, scene.channel, scene.notes, Ticks{10}, 0));

    // Three entries: the first move, the group, and the move after it.
    CHECK(scene.stack.undoDepth() == before + 3);
}

TEST_CASE("a group is one undo step and reverts in reverse order", "[commands]") {
    Fixture fixture;
    fixture.stack.beginGroup("Build a mixer");
    Insert master;
    master.name = "Master";
    fixture.run(std::make_unique<AddInsert>(std::move(master)));
    Insert strip;
    strip.name = "Lead";
    fixture.run(std::make_unique<AddInsert>(std::move(strip)));
    fixture.run(std::make_unique<AddRoute>(adx::core::InsertId{2}, adx::core::InsertId{1}));
    fixture.stack.endGroup();

    CHECK(fixture.stack.undoDepth() == 1);
    CHECK(fixture.project.mixer.inserts.size() == 2);
    CHECK(fixture.project.mixer.routes.size() == 1);

    CHECK(fixture.stack.undo(fixture.project));
    CHECK(fixture.project.mixer.inserts.empty());
    CHECK(fixture.project.mixer.routes.empty());
    CHECK(fixture.project == Project{});

    CHECK(fixture.stack.redo(fixture.project));
    CHECK(fixture.project.mixer.inserts.size() == 2);
    CHECK(fixture.project.mixer.routes.size() == 1);
}

TEST_CASE("redo hands out the same ids apply did", "[commands]") {
    // The reason every create command saves and restores Project::idMarks: without
    // it, undo-then-redo produces a structurally identical project that writes
    // different `insert.N` references.
    Fixture fixture;
    Insert first;
    first.name = "One";
    auto command = std::make_unique<AddInsert>(std::move(first));
    const AddInsert* raw = command.get();
    fixture.run(std::move(command));
    const adx::core::InsertId original = raw->created();

    CHECK(fixture.stack.undo(fixture.project));
    CHECK(fixture.stack.redo(fixture.project));
    CHECK(raw->created() == original);
    CHECK(fixture.project.mixer.inserts.front().id == original);

    // And a *new* insert after that gets the next id, not a reused one.
    Insert second;
    second.name = "Two";
    auto next = std::make_unique<AddInsert>(std::move(second));
    const AddInsert* nextRaw = next.get();
    fixture.run(std::move(next));
    CHECK(nextRaw->created().value == original.value + 1);
}

TEST_CASE("command_dirty_mask", "[commands]") {
    // Table-driven: each command reports exactly the subsystems it touches, which is
    // what lets Phase 3 rebuild part of a snapshot instead of all of it.
    const Scene scene;

    struct Row {
        std::unique_ptr<Command> command;
        DirtyMask expected;
    };

    // NOLINTBEGIN(modernize-use-designated-initializers) - a table reads as a table:
    // one row per line, columns aligned by position. Designating every field would
    // triple its width and bury the values the table exists to show.
    std::vector<Row> rows;
    rows.push_back({std::make_unique<SetMeta>(ProjectMeta{}), dirty::kMeta});
    rows.push_back({std::make_unique<SetTempoEvent>(Ticks{0}, 100.0, false), dirty::kTempo});
    rows.push_back({std::make_unique<SetMeterEvent>(Ticks{0}, 3, 4), dirty::kTempo});
    rows.push_back({std::make_unique<AddMarker>(Ticks{0}, "x"), dirty::kMarkers});
    rows.push_back({std::make_unique<AddChannel>(Channel{}), dirty::kChannels});
    rows.push_back(
        {std::make_unique<RemoveChannel>(scene.channel), dirty::kChannels | dirty::kPatterns});
    rows.push_back({std::make_unique<SetChannelValue>(scene.channel, ChannelField::Volume, 0.5),
                    dirty::kChannels});
    rows.push_back({std::make_unique<SetChannelOutput>(scene.channel, adx::core::InsertId{1}),
                    dirty::kChannels | dirty::kRouting});
    rows.push_back({std::make_unique<AddPattern>(Pattern{}), dirty::kPatterns});
    rows.push_back(
        {std::make_unique<RemovePattern>(scene.pattern), dirty::kPatterns | dirty::kPlaylist});
    rows.push_back({std::make_unique<AddNotes>(scene.pattern, scene.channel, std::vector<Note>{}),
                    dirty::kPatterns});
    rows.push_back(
        {std::make_unique<MoveNotes>(scene.pattern, scene.channel, scene.notes, Ticks{0}, 0),
         dirty::kPatterns});
    rows.push_back({std::make_unique<AddPlaylistTrack>(PlaylistTrack{}), dirty::kPlaylist});
    rows.push_back({std::make_unique<AddInsert>(Insert{}), dirty::kMixer});
    rows.push_back({std::make_unique<RemoveInsert>(adx::core::InsertId{1}),
                    dirty::kMixer | dirty::kRouting | dirty::kChannels});
    rows.push_back({std::make_unique<AddRoute>(adx::core::InsertId{1}, adx::core::InsertId{2}),
                    dirty::kRouting});
    rows.push_back({std::make_unique<AddSample>("x.wav"), dirty::kResources});

    for (const Row& row : rows) {
        // NOLINTEND(modernize-use-designated-initializers)
        INFO(row.command->name());
        CHECK(row.command->dirty() == row.expected);
    }
}

TEST_CASE("revision advances on every mutation, including undo", "[commands]") {
    Scene scene;
    const std::uint64_t before = scene.stack.revision();

    scene.run(std::make_unique<SetChannelValue>(scene.channel, ChannelField::Volume, 0.5));
    const std::uint64_t afterEdit = scene.stack.revision();
    CHECK(afterEdit > before);

    CHECK(scene.stack.undo(scene.project));
    CHECK(scene.stack.revision() > afterEdit);

    // dirtySince tells a poller what changed without it having to diff anything.
    CHECK((scene.stack.dirtySince(before) & dirty::kChannels) != 0U);
    CHECK(scene.stack.dirtySince(scene.stack.revision()) == dirty::kNone);
}

TEST_CASE("a new edit clears the redo stack", "[commands]") {
    Scene scene;
    scene.run(std::make_unique<SetChannelValue>(scene.channel, ChannelField::Volume, 0.5));
    CHECK(scene.stack.undo(scene.project));
    CHECK(scene.stack.canRedo());

    scene.run(std::make_unique<SetChannelValue>(scene.channel, ChannelField::Pan, 0.25));
    CHECK_FALSE(scene.stack.canRedo());
}

TEST_CASE("removing a channel takes its notes with it and puts them back", "[commands]") {
    Scene scene;
    CHECK(scene.project.patterns.front().noteClips.size() == 1);

    scene.run(std::make_unique<RemoveChannel>(scene.channel));
    CHECK(scene.project.channels.empty());
    CHECK(scene.project.patterns.front().noteClips.empty());

    CHECK(scene.stack.undo(scene.project));
    CHECK(scene.project.channels.size() == 1);
    REQUIRE(scene.project.patterns.front().noteClips.size() == 1);
    CHECK(scene.project.patterns.front().noteClips.front().notes.size() == 2);
}

TEST_CASE("removing an insert reroutes what fed it", "[commands]") {
    Fixture fixture;
    Insert master;
    master.name = "Master";
    fixture.run(std::make_unique<AddInsert>(std::move(master)));
    Insert strip;
    strip.name = "Strip";
    fixture.run(std::make_unique<AddInsert>(std::move(strip)));

    Channel channel;
    channel.name = "Lead";
    fixture.run(std::make_unique<AddChannel>(std::move(channel)));
    const adx::core::ChannelId id = fixture.project.channels.front().id;
    fixture.run(std::make_unique<SetChannelOutput>(id, adx::core::InsertId{2}));
    fixture.run(std::make_unique<AddRoute>(adx::core::InsertId{2}, adx::core::InsertId{1}));

    fixture.run(std::make_unique<RemoveInsert>(adx::core::InsertId{2}));
    CHECK(fixture.project.mixer.routes.empty());
    CHECK(fixture.project.find(id)->output == fixture.project.mixer.master);

    CHECK(fixture.stack.undo(fixture.project));
    CHECK(fixture.project.mixer.routes.size() == 1);
    CHECK(fixture.project.find(id)->output.value == 2);
}

TEST_CASE("moves are clamped, and the clamp is what undo reverses", "[commands]") {
    Scene scene;
    // Dragging left past the start of the pattern: the notes stop at zero, and
    // undoing has to return them to where they were rather than to minus ten bars.
    scene.run(
        std::make_unique<MoveNotes>(scene.pattern, scene.channel, scene.notes, Ticks{-1000000}, 0));
    const auto& clip = scene.project.patterns.front().noteClips.front();
    CHECK(clip.notes[0].start.value == 0);
    CHECK(clip.notes[1].start.value == 0);

    CHECK(scene.stack.undo(scene.project));
    CHECK(clip.notes[0].start.value == 0);
    CHECK(clip.notes[1].start.value == adx::core::kPpq);
}
