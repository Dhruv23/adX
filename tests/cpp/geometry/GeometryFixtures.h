// A project sized for the piano roll's budgets: one pattern, a lead channel holding
// `noteCount` seeded random notes, and a second channel whose notes are ghosts.
#pragma once

#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "engine/project/Project.h"
#include "engine/project/commands/ChannelCommands.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/NoteCommands.h"
#include "engine/project/commands/PatternCommands.h"

namespace adx::test {

struct RollScene {
    project::Project project;
    project::CommandStack stack;
    core::ChannelId lead;
    core::ChannelId ghost;
    core::PatternId pattern;

    void run(std::unique_ptr<project::Command> command) {
        stack.execute(std::move(command), project);
    }

    core::ChannelId addChannel(const char* name) {
        project::Channel prototype;
        prototype.name = name;
        run(std::make_unique<project::AddChannel>(std::move(prototype)));
        return project.channels.back().id;
    }

    /// Notes spread over `bars` bars of 4/4, sixteenth-aligned, pitches 36..95.
    static std::vector<project::Note> randomNotes(std::size_t count, std::int64_t bars,
                                                  std::uint32_t seed) {
        std::mt19937 rng(seed);
        const std::int64_t sixteenth = core::kPpq / 4;
        std::uniform_int_distribution<std::int64_t> start(0, bars * 16 - 1);
        std::uniform_int_distribution<std::int64_t> length(1, 8);
        std::uniform_int_distribution<int> pitch(36, 95);
        std::uniform_int_distribution<int> velocity(1, 127);
        std::vector<project::Note> notes(count);
        for (project::Note& note : notes) {
            note.start = core::Ticks{start(rng) * sixteenth};
            note.length = core::Ticks{length(rng) * sixteenth};
            note.pitch = static_cast<std::uint8_t>(pitch(rng));
            note.velocity = static_cast<std::uint8_t>(velocity(rng));
        }
        return notes;
    }

    explicit RollScene(std::size_t noteCount = 10000, std::int64_t bars = 256,
                       std::uint32_t seed = 1) {
        lead = addChannel("Lead");
        ghost = addChannel("Pad");
        project::Pattern prototype;
        prototype.name = "Roll";
        prototype.length = core::Ticks{bars * 4 * core::kPpq};
        auto add = std::make_unique<project::AddPattern>(std::move(prototype));
        const project::AddPattern* raw = add.get();
        run(std::move(add));
        pattern = raw->created();
        run(std::make_unique<project::AddNotes>(pattern, lead, randomNotes(noteCount, bars, seed)));
        run(std::make_unique<project::AddNotes>(pattern, ghost,
                                                randomNotes(noteCount / 10, bars, seed + 1)));
    }

    [[nodiscard]] const project::NoteClip& clip() const {
        return *project.find(pattern)->clipFor(lead);
    }
};

} // namespace adx::test
