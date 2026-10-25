// The gate.
//
// FINAL_PLAN §9: "The command system has a randomized-sequence test: apply N random
// commands, undo all N, assert the project is byte-identical to its initial state."
//
// Byte-identical is the operative word. Comparing the model would miss the failure
// this is really guarding against - a create command that allocates a fresh id on
// redo, producing a structurally identical project that writes different
// `insert.N` references. So the comparison is on the written text as well as on
// the model.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <random>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "engine/format/adx/Parser.h"
#include "engine/format/adx/Writer.h"
#include "engine/project/Project.h"
#include "engine/project/commands/AutomationCommands.h"
#include "engine/project/commands/ChannelCommands.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/MixerCommands.h"
#include "engine/project/commands/NoteCommands.h"
#include "engine/project/commands/PatternCommands.h"
#include "engine/project/commands/PlaylistCommands.h"
#include "engine/project/commands/ProjectCommands.h"
#include "tests/cpp/Corpus.h"
#include "tests/cpp/project/RandomCommands.h"

using namespace adx::project;

namespace {

using adx::tests::Generator;

struct Loaded {
    Project project;
    CommandStack stack;
    adx::format::DiagnosticList diagnostics;
};

void loadSuffocation(Loaded& out) {
    adx::format::load(adx::tests::readFile(adx::tests::corpusPaths().back()), out.project,
                      out.stack, out.diagnostics);
    out.stack.clear();
}

/// Applies `count` random commands, undoes all of them, and checks that both the
/// model and its written form came back exactly.
[[nodiscard]] bool oneSeed(std::uint32_t seed, int count) {
    Loaded loaded;
    loadSuffocation(loaded);

    const Project before = loaded.project;
    const std::string beforeText = adx::format::write(loaded.project);

    std::mt19937 rng(seed);
    Generator generator(rng, loaded.project);
    int applied = 0;
    for (int i = 0; i < count; ++i) {
        std::unique_ptr<Command> command = generator.next();
        if (command == nullptr) {
            continue;
        }
        // Each command lands a full coalescing window after the last, so none of
        // them merge. Coalescing is tested on its own, and merging here would hide a
        // command whose revert is wrong behind a neighbour whose revert is right.
        loaded.stack.executeAt(std::move(command), loaded.project,
                               CommandStack::Clock::time_point{} +
                                   (kCoalesceWindow * (applied + 1)));
        ++applied;
    }
    if (applied == 0) {
        return false;
    }

    while (loaded.stack.undo(loaded.project)) {
        // Unwind everything.
    }

    return loaded.project == before && adx::format::write(loaded.project) == beforeText;
}

} // namespace

TEST_CASE("undo_to_empty_random", "[commands][undo]") {
    // Two seeds in the fast loop; the full hundred are in the [.slow] case below and
    // run by default through ctest.
    for (std::uint32_t seed = 1; seed <= 2; ++seed) {
        INFO("seed " << seed);
        CHECK(oneSeed(seed, 10000));
    }
}

TEST_CASE("undo_to_empty_random 100 seeds", "[commands][undo][.slow]") {
    for (std::uint32_t seed = 1; seed <= 100; ++seed) {
        INFO("seed " << seed);
        REQUIRE(oneSeed(seed, 10000));
    }
}

TEST_CASE("undo_redo_symmetry", "[commands][undo]") {
    // After N undos and N redos the output is byte-identical to what it was before
    // the undos - which is where a command that reallocates ids on redo shows up.
    Loaded loaded;
    loadSuffocation(loaded);

    std::mt19937 rng(777U);
    Generator generator(rng, loaded.project);
    int applied = 0;
    while (applied < 500) {
        std::unique_ptr<Command> command = generator.next();
        if (command == nullptr) {
            continue;
        }
        loaded.stack.executeAt(std::move(command), loaded.project,
                               CommandStack::Clock::time_point{} +
                                   (kCoalesceWindow * (applied + 1)));
        ++applied;
    }

    const Project afterEdits = loaded.project;
    const std::string afterText = adx::format::write(loaded.project);

    std::size_t undone = 0;
    while (loaded.stack.undo(loaded.project)) {
        ++undone;
    }
    CHECK(std::cmp_equal(undone, applied));

    std::size_t redone = 0;
    while (loaded.stack.redo(loaded.project)) {
        ++redone;
    }
    CHECK(redone == undone);

    CHECK(loaded.project == afterEdits);
    CHECK(adx::format::write(loaded.project) == afterText);
}
