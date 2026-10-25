// The coordinator (phase_7.md §4.1, §6): loops terminate, conflicts are raised rather
// than resolved by guessing, and text with errors is never applied.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <random>
#include <string>

#include "engine/format/adx/Parser.h"
#include "engine/format/adx/Writer.h"
#include "engine/project/Project.h"
#include "engine/project/commands/ChannelCommands.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/NoteCommands.h"
#include "engine/sync/TextSync.h"
#include "tests/cpp/project/RandomCommands.h"

using adx::sync::ApplyStatus;
using adx::sync::TextSync;

namespace {

const std::string kText = R"([PROJECT]
ADX_VERSION=2

[CHANNEL Lead]
OUTPUT=insert.1
VOLUME=0.8

[PATTERN Verse]
LENGTH=1:0:0
NOTES Lead
  C4 0:0:0 0:1:0 100
  E4 0:1:0 0:1:0 90

[PLAYLIST]
TRACK 1
  PATTERN Verse 0:0:0

[MIXER]
INSERT 1 name="Master"
)";

struct Open {
    adx::project::Project project;
    adx::project::CommandStack stack;
    std::unique_ptr<TextSync> sync;

    explicit Open(const std::string& text) {
        adx::format::DiagnosticList diagnostics;
        adx::format::load(text, project, stack, diagnostics);
        stack.clear();
        sync = std::make_unique<TextSync>(project, stack, adx::format::WriteOptions{}, "");
        sync->markSynced(text);
    }
};

/// What an external editor does: parse the file, change something, write it back.
std::string externallyEdited(const std::string& file, std::mt19937& rng) {
    adx::project::Project project;
    adx::project::CommandStack stack;
    adx::format::DiagnosticList diagnostics;
    adx::format::load(file, project, stack, diagnostics);
    adx::tests::Generator generator(rng, project);
    for (int tries = 0; tries < 8; ++tries) {
        if (auto command = generator.next()) {
            stack.execute(std::move(command), project);
            break;
        }
    }
    return adx::format::write(project);
}

} // namespace

TEST_CASE("sync_no_loop", "[sync]") {
    // 1000 alternating GUI and external-editor edits with Live sync, simulated exactly
    // as the app wires it: a GUI edit makes adX write the file, which the watcher hears;
    // an external edit is reloaded, which must not make adX write. Each round is run to
    // quiescence, and must get there in a bounded number of steps.
    Open open(kText);
    open.sync->setMode(adx::sync::SyncMode::Live);
    std::string file = kText;
    int writes = 0;
    const auto liveWrite = [&] {
        if (open.sync->modelDirty()) {
            file = open.sync->renderText();
            open.sync->noteWrite(file);
            open.sync->markSynced(file);
            ++writes;
            return true;
        }
        return false;
    };
    std::mt19937 rng(7U);
    adx::tests::Generator generator(rng, open.project);
    int guiEdits = 0;
    for (int i = 0; i < 1000; ++i) {
        if (i % 2 == 0) {
            auto command = generator.next();
            if (command == nullptr) {
                continue;
            }
            open.stack.execute(std::move(command), open.project);
            ++guiEdits;
        } else {
            file = externallyEdited(file, rng);
            const ApplyStatus status = open.sync->applyExternal(file).status;
            REQUIRE(status != ApplyStatus::Conflict);
            if (status == ApplyStatus::Invalid) {
                // The editor saved something with an error (a routing cycle, say): the
                // model kept what it had, and the person puts the file right.
                file = open.sync->syncedText();
            }
        }
        // Run to quiescence: write if the model is ahead, let the watcher hear it.
        int steps = 0;
        while (liveWrite()) {
            REQUIRE(open.sync->applyExternal(file).status == ApplyStatus::SelfWrite);
            REQUIRE(++steps <= 1);
        }
    }
    INFO(writes << " writes for " << guiEdits << " GUI edits");
    CHECK(writes <= guiEdits);
    // Both sides converged.
    adx::project::Project fromFile;
    adx::project::CommandStack stack;
    adx::format::DiagnosticList diagnostics;
    adx::format::load(file, fromFile, stack, diagnostics);
    CHECK(adx::format::write(fromFile) == adx::format::write(open.project));
}

TEST_CASE("sync_conflict_detected", "[sync]") {
    // The model has an edit the file has not seen, and the file changes underneath it.
    Open open(kText);
    const auto& clip = open.project.patterns.front().noteClips.front();
    open.stack.execute(std::make_unique<adx::project::SetNoteValue>(
                           open.project.patterns.front().id, clip.channel,
                           std::vector<adx::core::NoteId>{clip.notes[0].id},
                           adx::project::NoteField::Velocity, 50.0),
                       open.project);
    const std::string mine = adx::format::write(open.project);
    std::string theirs = kText;
    theirs.replace(theirs.find("VOLUME=0.8"), 10, "VOLUME=0.3");

    const auto result = open.sync->applyExternal(theirs);
    CHECK(result.status == ApplyStatus::Conflict);
    // Neither side was dropped: the model still has its edit, and nothing was applied.
    CHECK(adx::format::write(open.project) == mine);
    CHECK(open.sync->modelDirty());

    SECTION("keep mine") {
        // The caller writes the model over the file.
        const std::string text = open.sync->renderText();
        open.sync->noteWrite(text);
        open.sync->markSynced(text);
        CHECK(text.find("VOLUME=0.8") != std::string::npos);
        CHECK(text.find(" 50") != std::string::npos);
        CHECK_FALSE(open.sync->modelDirty());
    }
    SECTION("take theirs") {
        CHECK(open.sync->applyForced(theirs).status == ApplyStatus::Applied);
        CHECK(adx::format::write(open.project).find("VOLUME=0.3") != std::string::npos);
        // The discarded edit is still one undo away.
        REQUIRE(open.stack.undo(open.project));
        CHECK(adx::format::write(open.project) == mine);
    }
    SECTION("the same edit on both sides is not a conflict") {
        CHECK(open.sync->applyExternal(mine).status == ApplyStatus::Unchanged);
        CHECK_FALSE(open.sync->modelDirty());
    }
}

TEST_CASE("sync_invalid_text_is_not_applied", "[sync]") {
    // A half-typed line must not delete the channel it is in.
    Open const open(kText);
    std::string broken = kText;
    broken.replace(broken.find("[CHANNEL Lead]"), 14, "[CHANNEL Lead");
    const auto result = open.sync->applyText(broken);
    CHECK(result.status == ApplyStatus::Invalid);
    CHECK(result.diagnostics.hasErrors());
    CHECK(open.stack.history().empty());
    CHECK(open.project.findChannelByName("Lead") != nullptr);
}

TEST_CASE("sync_self_write_ignored", "[sync]") {
    Open open(kText);
    open.stack.execute(
        std::make_unique<adx::project::SetChannelValue>(open.project.channels.front().id,
                                                        adx::project::ChannelField::Volume, 0.5),
        open.project);
    const std::string written = open.sync->renderText();
    open.sync->noteWrite(written);
    open.sync->markSynced(written);
    const std::size_t history = open.stack.history().size();
    CHECK(open.sync->applyExternal(written).status == ApplyStatus::SelfWrite);
    CHECK(open.stack.history().size() == history);
}

TEST_CASE("sync_noop_edit_is_not_a_conflict", "[sync]") {
    // An edit that changed nothing - a value set to what it was - moves the revision but
    // not the content. A reload then is not a conflict: there is nothing of the model's
    // to lose. (The bidirectional gate found a Live write overwriting an external save
    // after exactly such an edit.)
    Open open(kText);
    open.stack.execute(std::make_unique<adx::project::SetChannelValue>(
                           open.project.channels.front().id, adx::project::ChannelField::Volume,
                           static_cast<double>(open.project.channels.front().volume)),
                       open.project);
    REQUIRE(open.sync->modelDirty());
    CHECK_FALSE(open.sync->modelDiffers());
    std::string theirs = kText;
    theirs.replace(theirs.find("VOLUME=0.8"), 10, "VOLUME=0.3");
    CHECK(open.sync->applyExternal(theirs).status == ApplyStatus::Applied);
}

TEST_CASE("sync_stale_event_is_not_a_conflict", "[sync]") {
    // The watcher can report a save after it was already applied. If the model has been
    // edited since, re-reading the same text must not be a conflict: it is not new.
    Open open(kText);
    std::string theirs = kText;
    theirs.replace(theirs.find("VOLUME=0.8"), 10, "VOLUME=0.3");
    REQUIRE(open.sync->applyExternal(theirs).status == ApplyStatus::Applied);
    open.stack.execute(std::make_unique<adx::project::SetChannelValue>(
                           open.project.channels.front().id, adx::project::ChannelField::Pan, 0.5),
                       open.project);
    REQUIRE(open.sync->modelDiffers());
    CHECK(open.sync->applyExternal(theirs).status == ApplyStatus::Unchanged);
    CHECK(adx::format::write(open.project).find("PAN=0.5") != std::string::npos);
}
