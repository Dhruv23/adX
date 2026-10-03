// The invariant checker.
//
// Two callers depend on it: the loader, so a malformed file is reported rather than
// half-loaded, and Phase 3, which asserts a project is valid before snapshotting so
// the render path never has to defend against a model that cannot happen.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>

#include "engine/format/adx/Diagnostics.h"
#include "engine/project/Project.h"
#include "engine/project/Validate.h"

using namespace adx::project;
using adx::core::Ticks;
using adx::format::DiagnosticList;

namespace {

/// A minimal valid project: one master insert, one channel feeding it.
Project minimalProject() {
    Project project;
    Insert master;
    master.id = project.newInsertId();
    master.name = "Master";
    project.mixer.master = master.id;
    project.mixer.inserts.push_back(master);

    Channel channel;
    channel.id = project.newChannelId();
    channel.name = "Lead";
    channel.output = master.id;
    project.channels.push_back(channel);
    return project;
}

[[nodiscard]] bool emitted(const DiagnosticList& diagnostics, std::uint16_t code) {
    return std::ranges::any_of(diagnostics.all(),
                               [code](const auto& item) { return item.code == code; });
}

} // namespace

TEST_CASE("a minimal project is valid", "[validate]") {
    DiagnosticList diagnostics;
    const Project project = minimalProject();
    CHECK(validate(project, diagnostics));
    CHECK(diagnostics.empty());
}

TEST_CASE("validate_catches_invariants", "[validate]") {
    using namespace adx::format::code;

    SECTION("a dangling insert reference on a channel") {
        Project project = minimalProject();
        project.channels.front().output = adx::core::InsertId{99};
        DiagnosticList diagnostics;
        CHECK_FALSE(validate(project, diagnostics));
        CHECK(emitted(diagnostics, kUnknownInsertRef));
    }

    SECTION("a send to an insert that does not exist") {
        Project project = minimalProject();
        project.mixer.inserts.front().sends.push_back(
            Send{.id = project.newSendId(), .target = adx::core::InsertId{42}});
        DiagnosticList diagnostics;
        CHECK_FALSE(validate(project, diagnostics));
        CHECK(emitted(diagnostics, kUnknownInsertRef));
    }

    SECTION("a note outside the pattern's length") {
        Project project = minimalProject();
        Pattern pattern;
        pattern.id = project.newPatternId();
        pattern.name = "P";
        pattern.length = Ticks{adx::core::kPpq * 4};
        NoteClip clip;
        clip.channel = project.channels.front().id;
        Note note;
        note.id = project.newNoteId();
        note.start = Ticks{adx::core::kPpq * 100};
        note.length = Ticks{adx::core::kPpq};
        clip.notes.push_back(note);
        pattern.noteClips.push_back(std::move(clip));
        project.patterns.push_back(std::move(pattern));

        DiagnosticList diagnostics;
        // A warning, not an error: shortening a pattern must not silently delete work.
        CHECK(validate(project, diagnostics));
        CHECK(emitted(diagnostics, kNoteOutsidePattern));
    }

    SECTION("a duplicate id") {
        Project project = minimalProject();
        Channel twin = project.channels.front();
        twin.name = "Other";
        project.channels.push_back(twin);
        DiagnosticList diagnostics;
        CHECK_FALSE(validate(project, diagnostics));
        CHECK(emitted(diagnostics, kDuplicateId));
    }

    SECTION("a duplicate name") {
        Project project = minimalProject();
        Channel twin;
        twin.id = project.newChannelId();
        twin.name = "Lead";
        twin.output = project.mixer.master;
        project.channels.push_back(twin);
        DiagnosticList diagnostics;
        CHECK_FALSE(validate(project, diagnostics));
        CHECK(emitted(diagnostics, kDuplicateName));
    }

    SECTION("a name containing a character references cannot survive") {
        Project project = minimalProject();
        project.channels.front().name = "Bad.Name";
        DiagnosticList diagnostics;
        CHECK_FALSE(validate(project, diagnostics));
        CHECK(emitted(diagnostics, kBadNameCharacter));
    }

    SECTION("the null id") {
        Project project = minimalProject();
        project.channels.front().id = adx::core::ChannelId{};
        DiagnosticList diagnostics;
        CHECK_FALSE(validate(project, diagnostics));
        CHECK(emitted(diagnostics, kZeroId));
    }

    SECTION("no master at all") {
        Project project = minimalProject();
        project.mixer.master = adx::core::InsertId{};
        DiagnosticList diagnostics;
        CHECK_FALSE(validate(project, diagnostics));
        CHECK(emitted(diagnostics, kMissingRequiredKey));
    }

    SECTION("two note clips for one channel in one pattern") {
        Project project = minimalProject();
        Pattern pattern;
        pattern.id = project.newPatternId();
        pattern.name = "P";
        pattern.noteClips.push_back(
            NoteClip{.channel = project.channels.front().id, .notes = {}, .extras = {}});
        pattern.noteClips.push_back(
            NoteClip{.channel = project.channels.front().id, .notes = {}, .extras = {}});
        project.patterns.push_back(std::move(pattern));
        DiagnosticList diagnostics;
        CHECK_FALSE(validate(project, diagnostics));
        CHECK(emitted(diagnostics, kDuplicateNoteClip));
    }

    SECTION("a playlist item placing a pattern that is gone") {
        Project project = minimalProject();
        PlaylistTrack track;
        track.id = project.newPlaylistTrackId();
        PlaylistItem item;
        item.id = project.newItemId();
        item.content = PatternRef{.pattern = adx::core::PatternId{7}};
        track.items.push_back(item);
        project.playlist.tracks.push_back(std::move(track));
        DiagnosticList diagnostics;
        CHECK_FALSE(validate(project, diagnostics));
        CHECK(emitted(diagnostics, kUnknownPatternRef));
    }

    SECTION("an automation lane whose target does not resolve") {
        Project project = minimalProject();
        Pattern pattern;
        pattern.id = project.newPatternId();
        pattern.name = "P";
        AutomationClip clip;
        clip.id = project.newAutomationClipId();
        clip.targetPath = "channel.Ghost.volume";
        pattern.autoClips.push_back(std::move(clip));
        project.patterns.push_back(std::move(pattern));
        DiagnosticList diagnostics;
        CHECK_FALSE(validate(project, diagnostics));
        CHECK(emitted(diagnostics, kUnresolvedParamPath));
    }

    SECTION("a negative duration") {
        Project project = minimalProject();
        Pattern pattern;
        pattern.id = project.newPatternId();
        pattern.name = "P";
        pattern.length = Ticks{-1};
        project.patterns.push_back(std::move(pattern));
        DiagnosticList diagnostics;
        CHECK_FALSE(validate(project, diagnostics));
        CHECK(emitted(diagnostics, kNegativeDuration));
    }
}

TEST_CASE("route_cycle_detection", "[validate]") {
    Project project = minimalProject();
    // Three inserts in a ring. The master is insert 1, so this is a genuine cycle
    // that does not involve it.
    std::array<adx::core::InsertId, 3> ring{};
    for (std::size_t i = 0; i < ring.size(); ++i) {
        Insert insert;
        insert.id = project.newInsertId();
        insert.name = "Strip" + std::to_string(i + 1);
        ring.at(i) = insert.id;
        project.mixer.inserts.push_back(std::move(insert));
    }
    for (std::size_t i = 0; i < ring.size(); ++i) {
        project.mixer.routes.push_back(Route{
            .id = project.newRouteId(), .from = ring.at(i), .to = ring.at((i + 1) % ring.size())});
    }

    DiagnosticList diagnostics;
    CHECK_FALSE(validate(project, diagnostics));

    const auto cycle = std::ranges::find(diagnostics.all(), adx::format::code::kRoutingCycle,
                                         &adx::format::Diagnostic::code);
    REQUIRE(cycle != diagnostics.all().end());
    // The path, not just the fact: "there is a cycle" is not actionable on a
    // forty-strip mixer.
    INFO(cycle->message);
    CHECK(cycle->message.find("Strip1") != std::string::npos);
    CHECK(cycle->message.find("Strip2") != std::string::npos);
    CHECK(cycle->message.find("Strip3") != std::string::npos);
    CHECK(cycle->message.find("->") != std::string::npos);
}

TEST_CASE("a send closing a loop is a cycle too", "[validate]") {
    // A send from A to B and a route from B to A is a feedback loop just as surely as
    // two routes are, and treating sends as non-edges is how one gets through.
    Project project = minimalProject();
    Insert aux;
    aux.id = project.newInsertId();
    aux.name = "Aux";
    project.mixer.inserts.push_back(aux);

    project.mixer.inserts.front().sends.push_back(
        Send{.id = project.newSendId(), .target = aux.id});
    project.mixer.routes.push_back(
        Route{.id = project.newRouteId(), .from = aux.id, .to = project.mixer.master});

    DiagnosticList diagnostics;
    CHECK_FALSE(validate(project, diagnostics));
    CHECK(std::ranges::any_of(diagnostics.all(), [](const auto& item) {
        return item.code == adx::format::code::kRoutingCycle;
    }));
}

TEST_CASE("entity names are checked and can be repaired", "[validate]") {
    CHECK(isValidEntityName("Lead"));
    CHECK(isValidEntityName("Hardstyle Kick"));
    CHECK_FALSE(isValidEntityName(""));
    CHECK_FALSE(isValidEntityName("   "));
    CHECK_FALSE(isValidEntityName("a.b"));
    CHECK_FALSE(isValidEntityName("a\"b"));
    CHECK_FALSE(isValidEntityName("a]b"));

    CHECK(sanitizeEntityName("a.b", "x") == "a_b");
    CHECK(sanitizeEntityName("", "Fallback") == "Fallback");
    CHECK(sanitizeEntityName("  padded  ", "x") == "padded");
}
