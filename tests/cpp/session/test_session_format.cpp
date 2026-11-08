// [SESSION] in the text (phase_11.md §4.1, §5 session_serializes): parsed into the model
// as one command, written back losslessly through the Phase 2 writer.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "engine/format/adx/Parser.h"
#include "engine/format/adx/Writer.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace {

const char* const kSet = R"([PROJECT]
ADX_VERSION=2

[TEMPO]
0:0:0 120

[CHANNEL Lead]
INSTRUMENT=testtone
OUTPUT=insert.1

[CHANNEL Bass]
INSTRUMENT=testtone
OUTPUT=insert.1

[PATTERN A]
LENGTH=1:0:0
NOTES Lead
  C4 0:0:0 0:1:0 100

[PATTERN B]
LENGTH=2:0:0
NOTES Bass
  C2 0:0:0 0:2:0 100

[PLAYLIST]
TRACK 1 name="Lead"
TRACK 2 name="Bass"

[MIXER]
INSERT 1 name="Master"

[SESSION]
QUANTIZE=2bar
SCENE "Intro" tempo=128
SCENE "Drop"
SCENE "Out"
TRACK Lead linked=1
  1 A follow=next repeats=2
  3 A length=0:2:0 loop=no quantize=instant legato=yes gain=0.5
TRACK Bass linked=2 stop_on_empty=no
  2 B quantize=1/4 follow=random
)";

/// A cell that the test has just required to exist (and fails on, rather than read, if not).
const adx::project::ClipSlot& cellOf(const adx::project::SessionTrack& track, std::size_t row) {
    static const adx::project::ClipSlot kNone{};
    if (row >= track.slots.size() || !track.slots[row].has_value()) {
        FAIL("no clip in row " << row);
        return kNone;
    }
    return *track.slots[row];
}

} // namespace

TEST_CASE("session_serializes", "[session][format]") {
    const auto loaded = adx::tests::loadText(kSet);
    const adx::project::SessionModel& model = loaded->project.session;
    REQUIRE(model.scenes.size() == 3);
    CHECK(model.quantize == adx::session::LaunchQuantize::TwoBars);
    CHECK(model.scenes[0].name == "Intro");
    REQUIRE(model.scenes[0].tempo.has_value());
    CHECK(*model.scenes[0].tempo == 128.0);
    CHECK_FALSE(model.scenes[1].tempo.has_value());
    REQUIRE(model.tracks.size() == 2);

    const adx::project::SessionTrack& lead = model.tracks[0];
    CHECK(lead.channel == loaded->project.findChannelByName("Lead")->id);
    CHECK(lead.linked.value == 1);
    CHECK(lead.stopOnEmptyScene);
    REQUIRE(lead.slots.size() == 3); // padded to the scene count
    REQUIRE(lead.slots[0].has_value());
    CHECK(cellOf(lead, 0).follow == adx::session::FollowAction::Next);
    CHECK(cellOf(lead, 0).followRepeats == 2);
    CHECK(cellOf(lead, 0).loop);
    CHECK_FALSE(lead.slots[1].has_value());
    REQUIRE(lead.slots[2].has_value());
    CHECK(cellOf(lead, 2).length.value == adx::core::kPpq * 2);
    CHECK_FALSE(cellOf(lead, 2).loop);
    CHECK(cellOf(lead, 2).quantize == adx::session::LaunchQuantize::Instant);
    CHECK(cellOf(lead, 2).legato);
    CHECK(cellOf(lead, 2).gain == 0.5F);
    const adx::project::SessionTrack& bass = model.tracks[1];
    CHECK_FALSE(bass.stopOnEmptyScene);
    REQUIRE(model.slot(1, 1) != nullptr);
    CHECK(model.slot(1, 1)->follow == adx::session::FollowAction::Random);
    CHECK(model.slot(1, 0) == nullptr);

    // Written and read again: the same model, and the same text.
    const std::string written = adx::format::write(loaded->project);
    CHECK(written.find("[SESSION]") != std::string::npos);
    const auto again = adx::tests::loadText(written);
    CHECK(again->project.session == model);
    CHECK(adx::format::write(again->project) == written);

    // The whole session is one undo step.
    REQUIRE(loaded->stack.undo(loaded->project));
    CHECK(loaded->project.session.empty());
}

TEST_CASE("session_parse_reports_bad_cells", "[session][format]") {
    std::string text = kSet;
    text += "TRACK Nobody\n  1 A\n";
    adx::project::Project project;
    adx::project::CommandStack stack;
    adx::format::DiagnosticList diagnostics;
    adx::format::load(text, project, stack, diagnostics);
    bool unknownChannel = false;
    for (const adx::format::Diagnostic& d : diagnostics.all()) {
        unknownChannel =
            unknownChannel || d.message.find("no channel named 'Nobody'") != std::string::npos;
    }
    CHECK(unknownChannel);
    CHECK(project.session.tracks.size() == 2); // the good tracks still load
}
