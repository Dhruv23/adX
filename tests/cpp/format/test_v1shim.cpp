// The v1 compatibility shim.
//
// FINAL_PLAN §6 rule 6: every v1 file loads, including suffocation.adx, whose 551
// lines exercise nearly the whole extended grammar. The counts below were derived
// by hand from the file and are frozen as a fixture - if the shim's output changes,
// either the shim is wrong or the change was deliberate and these numbers move with
// a reason written next to them.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <variant>

#include "engine/format/adx/Parser.h"
#include "engine/project/Project.h"
#include "engine/project/Validate.h"
#include "engine/project/commands/CommandStack.h"
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

void loadInto(Loaded& out, const std::string& text) {
    adx::format::load(text, out.project, out.stack, out.diagnostics);
}

[[nodiscard]] std::size_t noteCount(const Project& project) {
    std::size_t total = 0;
    for (const auto& pattern : project.patterns) {
        for (const auto& clip : pattern.noteClips) {
            total += clip.notes.size();
        }
    }
    return total;
}

[[nodiscard]] std::size_t slotCount(const Project& project) {
    std::size_t total = 0;
    for (const auto& insert : project.mixer.inserts) {
        total += insert.slots.size();
    }
    return total;
}

void reportErrors(const DiagnosticList& diagnostics) {
    for (const auto& item : diagnostics.all()) {
        if (item.severity != adx::format::Severity::Error) {
            continue;
        }
        UNSCOPED_INFO(item.codeString() << " at line " << item.span.line << ':' << item.span.column
                                        << " - " << item.message);
    }
}

} // namespace

TEST_CASE("v1_shim_loads_all_examples", "[format][v1]") {
    for (const auto& path : adx::tests::corpusPaths()) {
        INFO(path.string());
        Loaded loaded;
        loadInto(loaded, adx::tests::readFile(path));
        reportErrors(loaded.diagnostics);
        CHECK_FALSE(loaded.diagnostics.hasErrors());

        // And what came out is a project the rest of the engine can rely on.
        DiagnosticList invariants;
        const bool valid = adx::project::validate(loaded.project, invariants);
        reportErrors(invariants);
        CHECK(valid);
    }
}

TEST_CASE("v1_shim_suffocation_fidelity", "[format][v1]") {
    Loaded loaded;
    loadInto(loaded, adx::tests::readFile(adx::tests::corpusPaths().back()));
    const Project& project = loaded.project;

    // Seven [TRACK]s, each expanding to a channel, a pattern and a playlist track.
    CHECK(project.channels.size() == 7);
    CHECK(project.patterns.size() == 7);
    // Seven track lanes plus the one the migrated automation lives on.
    CHECK(project.playlist.tracks.size() == 8);
    // Seven track inserts, the master, the two aux buses the SEND= lines created, and
    // "Master FX", which carries v1's master delay and reverb ahead of the master.
    CHECK(project.mixer.inserts.size() == 11);

    CHECK(noteCount(project) == 349);
    // Seventeen per-track EFFECT lines; the master's delay, reverb, ducker and drive;
    // the Compressor and Limiter standing in for v1's peak compressor and clamp; and
    // each aux bus's own Delay or Reverb.
    CHECK(slotCount(project) == 25);
    CHECK(project.markers.size() == 6);
    CHECK(project.playlist.autoClips.size() == 6);

    std::size_t breakpoints = 0;
    for (const auto& clip : project.playlist.autoClips) {
        breakpoints += clip.points.size();
        INFO("lane " << clip.targetPath);
        CHECK(clip.target.valid());
    }
    CHECK(breakpoints == 25);

    CHECK(project.tempo.bpmAt(adx::core::Ticks{0}) == 120.0);
    CHECK(project.meta.tuning == 440.0);
    // The file said it was v1, and loading it must not quietly claim otherwise.
    CHECK(project.meta.version == 1);

    const adx::project::Insert* master = project.master();
    REQUIRE(master != nullptr);
    CHECK(master->name == "Master");
    CHECK(master->gain == 0.8F);
}

TEST_CASE("v1_shim_track_expands_to_four", "[format][v1]") {
    const std::string text = "[GLOBAL]\nBPM=120\n\n"
                             "[PATCH Bass]\nENVELOPE=0.01, 0.1, 0.5, 0.2\n\n"
                             "[TRACK Bass]\nMIX=0.8, -0.25\nC2 0.0 1.0 0.5\n";

    Loaded loaded;
    loadInto(loaded, text);
    const Project& project = loaded.project;
    CHECK_FALSE(loaded.diagnostics.hasErrors());

    REQUIRE(project.channels.size() == 1);
    REQUIRE(project.patterns.size() == 1);
    REQUIRE(project.playlist.tracks.size() == 1);
    // The track's own insert, plus the master.
    REQUIRE(project.mixer.inserts.size() == 2);

    const auto& channel = project.channels.front();
    const auto& pattern = project.patterns.front();
    const auto& lane = project.playlist.tracks.front();

    CHECK(channel.name == "Bass");
    CHECK(pattern.name == "Bass");
    CHECK(lane.name == "Bass");

    // All four cross-referenced correctly, which is the half that makes the expansion
    // worth anything.
    const adx::project::Insert* insert = project.mixer.find(channel.output);
    REQUIRE(insert != nullptr);
    CHECK(insert->name == "Bass");
    CHECK(insert->gain == 0.8F);
    CHECK(insert->pan == -0.25F);

    REQUIRE(pattern.noteClips.size() == 1);
    CHECK(pattern.noteClips.front().channel == channel.id);

    REQUIRE(lane.items.size() == 1);
    const auto* placed = std::get_if<adx::project::PatternRef>(&lane.items.front().content);
    REQUIRE(placed != nullptr);
    CHECK(placed->pattern == pattern.id);

    // The insert reaches the master, rather than being an island.
    const bool routed = std::ranges::any_of(project.mixer.routes, [&](const auto& route) {
        return route.from == insert->id && route.to == project.mixer.master;
    });
    CHECK(routed);
}

TEST_CASE("v1_shim_marker_first_comma", "[format][v1]") {
    // v1 split MARKER= on the first comma only, so a marker name may contain commas.
    // Preserved exactly, because changing it would rename somebody's sections.
    const std::string text = "[GLOBAL]\nBPM=120\nMARKER=16,Drop, part 2\n";
    Loaded loaded;
    loadInto(loaded, text);

    REQUIRE(loaded.project.markers.size() == 1);
    CHECK(loaded.project.markers.front().name == "Drop, part 2");
    CHECK(loaded.project.markers.front().at.value == 16 * adx::core::kPpq);
}

TEST_CASE("v1_shim_send_creates_bus", "[format][v1]") {
    const std::string text = "[GLOBAL]\nBPM=120\n\n"
                             "[TRACK One]\nSEND=Reverb, 0.3\nC2 0.0 1.0 0.5\n\n"
                             "[TRACK Two]\nSEND=Reverb, 0.5\nC2 0.0 1.0 0.5\n";

    Loaded loaded;
    loadInto(loaded, text);
    const Project& project = loaded.project;

    // Master, two track inserts, and exactly one reverb bus - the second track's send
    // reuses the first one's rather than creating another.
    CHECK(project.mixer.inserts.size() == 4);
    const auto bus =
        std::ranges::find(project.mixer.inserts, "Reverb Bus", &adx::project::Insert::name);
    REQUIRE(bus != project.mixer.inserts.end());

    std::size_t sends = 0;
    for (const auto& insert : project.mixer.inserts) {
        for (const auto& send : insert.sends) {
            CHECK(send.target == bus->id);
            ++sends;
        }
    }
    CHECK(sends == 2);

    const bool routed = std::ranges::any_of(project.mixer.routes, [&](const auto& route) {
        return route.from == bus->id && route.to == project.mixer.master;
    });
    CHECK(routed);
}

TEST_CASE("v1 velocity is rescaled and reported", "[format][v1]") {
    const std::string text = "[GLOBAL]\nBPM=120\n\n[TRACK One]\nC4 0.0 1.0 1.0\nE4 1.0 1.0 0.5\n";
    Loaded loaded;
    loadInto(loaded, text);

    REQUIRE(loaded.project.patterns.size() == 1);
    const auto& notes = loaded.project.patterns.front().noteClips.front().notes;
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].velocity == 127);
    CHECK(notes[1].velocity == 64);

    const bool reported = std::ranges::any_of(loaded.diagnostics.all(), [](const auto& item) {
        return item.code == adx::format::code::kV1VelocityRescaled;
    });
    CHECK(reported);
}

TEST_CASE("an unresolvable v1 automation target is preserved, not dropped", "[format][v1]") {
    const std::string text = "[GLOBAL]\nBPM=120\n\n[TRACK One]\nC4 0.0 1.0 1.0\n\n"
                             "[AUTOMATION One patch.somethingNobodyEverAdded]\n0 1 lin\n";

    Loaded loaded;
    loadInto(loaded, text);
    CHECK(loaded.project.playlist.autoClips.empty());

    const bool reported = std::ranges::any_of(loaded.diagnostics.all(), [](const auto& item) {
        return item.code == adx::format::code::kV1UnresolvedAutomation;
    });
    CHECK(reported);

    // Preserved as residue rather than silently discarded.
    bool kept = false;
    for (const auto& block : loaded.project.residue.blocks) {
        kept = kept || (block.wholeSection && block.sectionType == "AUTOMATION");
    }
    CHECK(kept);
}
