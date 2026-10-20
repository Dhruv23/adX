// Phase 6's commands and the model they change: slot order, placement edits (slip and
// stretch), track solo and groups, arrangement snapshots and controllers - each undoable
// byte for byte, each round-tripping through the format.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "engine/format/adx/Parser.h"
#include "engine/format/adx/Writer.h"
#include "engine/project/ControllerCompile.h"
#include "engine/project/Diff.h"
#include "engine/project/Project.h"
#include "engine/project/commands/ArrangeCommands.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/MixerCommands.h"
#include "engine/project/commands/PlaylistCommands.h"

using namespace adx::project;
using adx::core::kPpq;
using adx::core::PlaylistTrackId;
using adx::core::SampleId;
using adx::core::SlotId;
using adx::core::Ticks;

namespace {

constexpr const char* kScene = R"([PROJECT]
ADX_VERSION=2

[CHANNEL Lead]
OUTPUT=insert.1
PARAM filter.cutoff=2000

[CHANNEL Kick]
INSTRUMENT=drumsynth
OUTPUT=insert.2

[PATTERN Verse]
LENGTH=4:0:0
NOTES Lead
  60 0:0:0 0:1:0 100
NOTES Kick
  36 0:0:0 0:0:1920 100
  36 1:0:0 0:0:1920 100

[PLAYLIST]
TRACK 1 name="Lead"
  PATTERN Verse 0:0:0
  PATTERN Verse 4:0:0
TRACK 2 name="Drums"

[MIXER]
INSERT 1 name="Master"
  SLOT 1 EQ
  SLOT 2 Delay
  SLOT 3 Reverb
INSERT 2 name="Kick"
ROUTE insert.2 -> insert.1
)";

struct Scene {
    Project project;
    CommandStack stack;

    Scene() {
        adx::format::DiagnosticList diagnostics;
        adx::format::load(kScene, project, stack, diagnostics);
        REQUIRE_FALSE(diagnostics.hasErrors());
        stack.clear();
    }

    [[nodiscard]] std::string text() const {
        return adx::format::write(project);
    }

    void run(std::unique_ptr<Command> command) {
        stack.execute(std::move(command), project);
    }

    /// Parses the written text into a fresh project and writes it again.
    [[nodiscard]] std::string reloaded() const {
        Project again;
        CommandStack stack2;
        adx::format::DiagnosticList diagnostics;
        adx::format::load(text(), again, stack2, diagnostics);
        REQUIRE_FALSE(diagnostics.hasErrors());
        return adx::format::write(again);
    }
};

} // namespace

TEST_CASE("move_slot_keeps_ids_and_undoes", "[commands][phase6]") {
    Scene scene;
    const std::string before = scene.text();
    const Insert& master = *scene.project.master();
    const SlotId reverb = master.slots[2].id;

    scene.run(std::make_unique<MoveSlot>(reverb, 0));
    const Insert& after = *scene.project.master();
    REQUIRE(after.slots.size() == 3);
    CHECK(after.slots[0].type == "Reverb");
    CHECK(after.slots[0].id == reverb);
    CHECK(after.slots[1].type == "EQ");
    // The order is the list's, and it is written that way and survives a reload.
    CHECK(scene.text().find("SLOT 3 Reverb") < scene.text().find("SLOT 1 EQ"));
    CHECK(scene.reloaded() == scene.text());

    REQUIRE(scene.stack.undo(scene.project));
    CHECK(scene.text() == before);
}

TEST_CASE("playlist_slip_and_stretch", "[commands][phase6]") {
    // Slip changes sourceOffset only; stretch changes the length and the stretch factor
    // (phase_6.md §5).
    Scene scene;
    const std::string before = scene.text();
    const PlaylistTrack& lead = scene.project.playlist.tracks.front();
    const PlaylistItem original = lead.items.front();

    PlaylistItem slipped = original;
    slipped.sourceOffset = Ticks{kPpq};
    scene.run(std::make_unique<SetPlaylistItem>(lead.id, original.id, slipped, lead.id));
    const PlaylistItem& afterSlip = scene.project.playlist.tracks.front().items.front();
    CHECK(afterSlip.sourceOffset == Ticks{kPpq});
    CHECK(afterSlip.start == original.start);
    CHECK(afterSlip.length == original.length);
    CHECK(afterSlip.content == original.content);

    PlaylistItem audio = original;
    audio.content = AudioClipRef{.sample = SampleId{1}, .stretch = 2.0F};
    audio.length = Ticks{kPpq * 8};
    scene.run(std::make_unique<SetPlaylistItem>(lead.id, original.id, audio, lead.id));
    const PlaylistItem& stretched = scene.project.playlist.tracks.front().items.front();
    CHECK(stretched.length == Ticks{kPpq * 8});
    CHECK(std::get<AudioClipRef>(stretched.content).stretch == 2.0F);
    CHECK(stretched.id == original.id);

    while (scene.stack.undo(scene.project)) {
    }
    CHECK(scene.text() == before);
}

TEST_CASE("set_playlist_item_drag_across_tracks_is_one_step", "[commands][phase6]") {
    Scene scene;
    const std::string before = scene.text();
    const PlaylistTrackId lead = scene.project.playlist.tracks[0].id;
    const PlaylistTrackId drums = scene.project.playlist.tracks[1].id;
    PlaylistItem item = scene.project.playlist.tracks[0].items[1];
    const std::size_t depth = scene.stack.undoDepth();

    // A drag: three moves, the middle one onto the other track, coalesced into one.
    for (int step = 1; step <= 3; ++step) {
        item.start = Ticks{(kPpq * 16) + (kPpq * step)};
        const PlaylistTrackId from = step <= 2 ? lead : drums;
        const PlaylistTrackId to = step == 1 ? lead : drums;
        scene.run(std::make_unique<SetPlaylistItem>(from, item.id, item, to));
    }
    CHECK(scene.stack.undoDepth() == depth + 1);
    CHECK(scene.project.playlist.tracks[1].items.size() == 1);
    CHECK(scene.project.playlist.tracks[1].items[0].start == Ticks{kPpq * 19});
    REQUIRE(scene.stack.undo(scene.project));
    CHECK(scene.text() == before);
    REQUIRE(scene.stack.redo(scene.project));
    CHECK(scene.project.playlist.tracks[1].items.size() == 1);
}

TEST_CASE("track_solo_and_groups_decide_audibility", "[commands][phase6]") {
    Scene scene;
    Playlist& playlist = scene.project.playlist;
    const PlaylistTrackId lead = playlist.tracks[0].id;
    const PlaylistTrackId drums = playlist.tracks[1].id;
    CHECK(playlist.audible(playlist.tracks[0]));
    CHECK(playlist.audible(playlist.tracks[1]));

    // Lead joins the Drums group; muting the group mutes it.
    PlaylistTrack value = *playlist.find(lead);
    value.group = drums;
    scene.run(std::make_unique<SetPlaylistTrack>(lead, value));
    PlaylistTrack group = *playlist.find(drums);
    group.muted = true;
    scene.run(std::make_unique<SetPlaylistTrack>(drums, group));
    CHECK_FALSE(playlist.audible(*playlist.find(lead)));

    // Unmute; solo the member: the group lane plays too, nothing else would.
    group.muted = false;
    scene.run(std::make_unique<SetPlaylistTrack>(drums, group));
    value.soloed = true;
    scene.run(std::make_unique<SetPlaylistTrack>(lead, value));
    CHECK(playlist.audible(*playlist.find(lead)));
    CHECK(playlist.audible(*playlist.find(drums)));

    // A track cannot group itself, nor join a member.
    PlaylistTrack self = *playlist.find(drums);
    self.group = drums;
    scene.run(std::make_unique<SetPlaylistTrack>(drums, self));
    CHECK_FALSE(playlist.find(drums)->group.valid());

    const std::string text = scene.text();
    CHECK(text.find("TRACK 1 name=\"Lead\" solo=yes group=2") != std::string::npos);
    CHECK(scene.reloaded() == text);
}

TEST_CASE("arrangements_switch_by_swap", "[commands][phase6]") {
    Scene scene;
    const std::string before = scene.text();
    scene.run(std::make_unique<RenameArrangement>(std::nullopt, "Main"));
    auto add = std::make_unique<AddArrangement>("Alt", true);
    const AddArrangement* raw = add.get();
    scene.run(std::move(add));
    const std::size_t alt = raw->index();
    scene.run(std::make_unique<SwitchArrangement>(alt));
    CHECK(scene.project.playlist.arrangementName == "Alt");

    // Edit the live (Alt) arrangement; Main is untouched.
    const PlaylistTrack& track = scene.project.playlist.tracks.front();
    scene.run(std::make_unique<RemovePlaylistItem>(track.id, track.items.front().id));
    CHECK(scene.project.playlist.tracks.front().items.size() == 1);
    CHECK(scene.project.playlist.arrangements[alt].tracks.front().items.size() == 2);

    const std::string text = scene.text();
    CHECK(text.find("[ARRANGEMENT Main]") != std::string::npos);
    CHECK(text.find("NAME=Alt") != std::string::npos);
    CHECK(scene.reloaded() == text);

    scene.run(std::make_unique<SwitchArrangement>(alt));
    CHECK(scene.project.playlist.arrangementName == "Main");
    CHECK(scene.project.playlist.tracks.front().items.size() == 2);

    while (scene.stack.undo(scene.project)) {
    }
    CHECK(scene.text() == before);
}

TEST_CASE("controllers_roundtrip_and_undo", "[commands][phase6]") {
    Scene scene;
    const std::string before = scene.text();

    Controller lfo;
    lfo.name = "Wobble";
    lfo.kind = ControllerKind::Lfo;
    lfo.targetPath = "channel.Lead.filter.cutoff";
    lfo.target = ParamRegistry::resolve(lfo.targetPath, scene.project).ref;
    lfo.shape = LfoShape::Triangle;
    lfo.period = Ticks{kPpq * 2};
    lfo.depth = 0.25F;
    scene.run(std::make_unique<AddController>(lfo));

    Controller envelope;
    envelope.name = "Pump";
    envelope.kind = ControllerKind::Envelope;
    envelope.targetPath = "insert.1.gain";
    envelope.target = ParamRegistry::resolve(envelope.targetPath, scene.project).ref;
    envelope.sourceChannel = scene.project.findChannelByName("Kick")->id;
    envelope.decayMs = 120.0F;
    scene.run(std::make_unique<AddController>(envelope));

    Controller peak;
    peak.name = "Follow";
    peak.kind = ControllerKind::Peak;
    peak.targetPath = "insert.1.slot.2.mix";
    peak.target = ParamRegistry::resolve(peak.targetPath, scene.project).ref;
    peak.sourceInsert = adx::core::InsertId{2};
    peak.gainDb = 6.0F;
    scene.run(std::make_unique<AddController>(peak));

    REQUIRE(lfo.target.valid());
    REQUIRE(envelope.target.valid());
    REQUIRE(peak.target.valid());
    const std::string text = scene.text();
    CHECK(text.find("[CONTROLLER Wobble]\nTYPE=lfo\nTARGET=channel.Lead.filter.cutoff\n"
                    "SHAPE=triangle\nPERIOD=0:2:0\nDEPTH=0.25") != std::string::npos);
    CHECK(text.find("SOURCE=channel.Kick") != std::string::npos);
    CHECK(text.find("SOURCE=insert.2") != std::string::npos);
    CHECK(scene.reloaded() == text);

    // An edit is one command; a semantic diff names it.
    Controller edited = *scene.project.findControllerByName("Wobble");
    const Project snapshot = scene.project;
    edited.depth = 0.5F;
    scene.run(std::make_unique<SetController>(edited.id, edited));
    const auto changes = diff(snapshot, scene.project);
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].subject == "controller Wobble");

    while (scene.stack.undo(scene.project)) {
    }
    CHECK(scene.text() == before);
}

TEST_CASE("lfo_compiles_to_its_shape", "[controllers][phase6]") {
    Controller lfo;
    lfo.kind = ControllerKind::Lfo;
    lfo.period = Ticks{kPpq};
    lfo.depth = 0.5F;
    lfo.center = 0.5F;
    const ControllerRange range{.minimum = 0.0F, .maximum = 100.0F, .logarithmic = false};
    for (const LfoShape shape :
         {LfoShape::Sine, LfoShape::Triangle, LfoShape::Saw, LfoShape::Square}) {
        lfo.shape = shape;
        std::vector<Knot> knots;
        compileLfo(knots, lfo, range, kPpq * 8);
        INFO("shape " << toString(shape));
        REQUIRE(knots.size() > 4);
        // Every knot lies on the shape, sampled at its own tick (a jump's two knots share
        // a tick; the later is the value from there on, so only the later is checked).
        for (std::size_t i = 0; i + 1 < knots.size(); ++i) {
            if (knots[i + 1].tick == knots[i].tick) {
                continue;
            }
            const double turns = static_cast<double>(knots[i].tick) / kPpq;
            const float expected = range.map(0.5F + (0.5F * lfoShapeAt(shape, turns)));
            CHECK(std::abs(knots[i].value - expected) < 0.01F);
        }
        CHECK(knots.back().tick >= kPpq * 8);
    }
}

TEST_CASE("controller_range_maps_logarithmically", "[controllers][phase6]") {
    const ControllerRange range{.minimum = 20.0F, .maximum = 20000.0F, .logarithmic = true};
    CHECK(std::abs(range.map(0.0F) - 20.0F) < 1e-3F);
    CHECK(std::abs(range.map(1.0F) - 20000.0F) < 0.5F);
    // Half way on a log scale is the geometric mean.
    CHECK(std::abs(range.map(0.5F) - 632.456F) < 0.05F);
}
