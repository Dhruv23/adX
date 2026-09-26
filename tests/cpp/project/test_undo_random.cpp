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

using namespace adx::project;
using adx::core::Ticks;

namespace {

/// Picks random *valid* targets out of the project as it currently stands.
///
/// "Valid" matters: a generator that mostly names entities that do not exist would
/// spend its ten thousand commands doing nothing, and the test would pass without
/// having exercised anything.
class Generator {
public:
    Generator(std::mt19937& rng, const Project& project) : m_rng(rng), m_project(project) {}

    [[nodiscard]] std::unique_ptr<Command> next();

    /// Metadata, tempo, markers and channels.
    [[nodiscard]] std::unique_ptr<Command> nextSimple(int kind);
    /// Patterns, notes, the playlist and the mixer.
    [[nodiscard]] std::unique_ptr<Command> nextStructural(int kind);

private:
    template<class Container> [[nodiscard]] const auto* pick(const Container& items) {
        if (items.empty()) {
            return static_cast<const typename Container::value_type*>(nullptr);
        }
        std::uniform_int_distribution<std::size_t> index(0, items.size() - 1);
        return &items[index(m_rng)];
    }

    [[nodiscard]] int roll(int high) {
        return std::uniform_int_distribution<int>(0, high)(m_rng);
    }
    [[nodiscard]] double unit() {
        return std::uniform_real_distribution<double>(0.0, 1.0)(m_rng);
    }
    [[nodiscard]] Ticks someTick() {
        return Ticks{std::uniform_int_distribution<std::int64_t>(0, adx::core::kPpq * 64)(m_rng)};
    }
    [[nodiscard]] std::string someName() {
        return "gen" + std::to_string(std::uniform_int_distribution<int>(0, 1000000)(m_rng));
    }

    std::mt19937& m_rng;
    const Project& m_project;
};

std::unique_ptr<Command> Generator::next() {
    const int kind = roll(29);
    return kind < 14 ? nextSimple(kind) : nextStructural(kind);
}

std::unique_ptr<Command> Generator::nextSimple(int kind) {
    switch (kind) {
    case 0: {
        ProjectMeta meta = m_project.meta;
        meta.title = someName();
        meta.tuning = 400.0 + (unit() * 80.0);
        return std::make_unique<SetMeta>(std::move(meta));
    }
    case 1:
        return std::make_unique<SetTempoEvent>(someTick(), 40.0 + (unit() * 200.0), roll(1) == 0);
    case 2:
        return std::make_unique<RemoveTempoEvent>(someTick());
    case 3:
        return std::make_unique<SetMeterEvent>(someTick(), static_cast<std::uint16_t>(1 + roll(11)),
                                               static_cast<std::uint16_t>(1U << roll(4)));
    case 4:
        return std::make_unique<RemoveMeterEvent>(someTick());
    case 5:
        return std::make_unique<AddMarker>(someTick(), someName());
    case 6:
        if (const auto* marker = pick(m_project.markers)) {
            return std::make_unique<RemoveMarker>(marker->id);
        }
        return nullptr;
    case 7:
        if (const auto* marker = pick(m_project.markers)) {
            return std::make_unique<MoveMarker>(marker->id, someTick(), someName());
        }
        return nullptr;
    case 8: {
        Channel channel;
        channel.name = someName();
        channel.output = m_project.mixer.master;
        return std::make_unique<AddChannel>(std::move(channel));
    }
    case 9:
        if (const auto* channel = pick(m_project.channels)) {
            return std::make_unique<RemoveChannel>(channel->id);
        }
        return nullptr;
    case 10:
        if (const auto* channel = pick(m_project.channels)) {
            return std::make_unique<SetChannelValue>(
                channel->id, static_cast<ChannelField>(roll(6)), unit() * 16.0);
        }
        return nullptr;
    case 11:
        if (const auto* channel = pick(m_project.channels)) {
            return std::make_unique<RenameChannel>(channel->id, someName());
        }
        return nullptr;
    case 12:
        if (const auto* channel = pick(m_project.channels)) {
            return std::make_unique<SetChannelParam>(channel->id, "drive", unit() * 12.0);
        }
        return nullptr;
    case 13:
        if (const auto* channel = pick(m_project.channels)) {
            ArpSettings arp;
            arp.mode = static_cast<ArpMode>(roll(6));
            arp.gate = static_cast<float>(unit());
            return std::make_unique<SetChannelArp>(channel->id, arp);
        }
        return nullptr;
    default:
        return nullptr;
    }
}

std::unique_ptr<Command> Generator::nextStructural(int kind) {
    switch (kind) {
    case 14: {
        Pattern pattern;
        pattern.name = someName();
        return std::make_unique<AddPattern>(std::move(pattern));
    }
    case 15:
        if (const auto* pattern = pick(m_project.patterns)) {
            return std::make_unique<RemovePattern>(pattern->id);
        }
        return nullptr;
    case 16:
        if (const auto* pattern = pick(m_project.patterns)) {
            return std::make_unique<SetPatternLength>(pattern->id, someTick());
        }
        return nullptr;
    case 17:
        if (const auto* pattern = pick(m_project.patterns)) {
            if (const auto* channel = pick(m_project.channels)) {
                std::vector<Note> notes(static_cast<std::size_t>(1 + roll(7)));
                for (Note& note : notes) {
                    note.start = someTick();
                    note.length = Ticks{adx::core::kPpq};
                    note.pitch = static_cast<std::uint8_t>(roll(127));
                }
                return std::make_unique<AddNotes>(pattern->id, channel->id, std::move(notes));
            }
        }
        return nullptr;
    case 18:
    case 19:
    case 20:
        if (const auto* pattern = pick(m_project.patterns)) {
            if (const auto* clip = pick(pattern->noteClips)) {
                std::vector<adx::core::NoteId> ids;
                for (const Note& note : clip->notes) {
                    if (roll(3) == 0) {
                        ids.push_back(note.id);
                    }
                }
                if (ids.empty()) {
                    return nullptr;
                }
                if (roll(2) == 0) {
                    return std::make_unique<RemoveNotes>(pattern->id, clip->channel,
                                                         std::move(ids));
                }
                if (roll(1) == 0) {
                    return std::make_unique<MoveNotes>(pattern->id, clip->channel, std::move(ids),
                                                       Ticks{roll(2000) - 1000}, roll(24) - 12);
                }
                return std::make_unique<SetNoteValue>(pattern->id, clip->channel, std::move(ids),
                                                      static_cast<NoteField>(roll(8)),
                                                      unit() * 120.0);
            }
        }
        return nullptr;
    case 21: {
        PlaylistTrack track;
        track.name = someName();
        return std::make_unique<AddPlaylistTrack>(std::move(track));
    }
    case 22:
        if (const auto* track = pick(m_project.playlist.tracks)) {
            return std::make_unique<RemovePlaylistTrack>(track->id);
        }
        return nullptr;
    case 23:
        if (const auto* track = pick(m_project.playlist.tracks)) {
            if (const auto* pattern = pick(m_project.patterns)) {
                PlaylistItem item;
                item.start = someTick();
                item.content = PatternRef{.pattern = pattern->id};
                return std::make_unique<AddPlaylistItem>(track->id, item);
            }
        }
        return nullptr;
    case 24:
        if (const auto* track = pick(m_project.playlist.tracks)) {
            if (const auto* item = pick(track->items)) {
                if (roll(1) == 0) {
                    return std::make_unique<RemovePlaylistItem>(track->id, item->id);
                }
                return std::make_unique<MovePlaylistItem>(track->id, item->id, someTick(),
                                                          adx::core::PlaylistTrackId{});
            }
        }
        return nullptr;
    case 25: {
        Insert insert;
        insert.name = someName();
        return std::make_unique<AddInsert>(std::move(insert));
    }
    case 26:
        if (const auto* insert = pick(m_project.mixer.inserts)) {
            switch (roll(4)) {
            case 0:
                return std::make_unique<RemoveInsert>(insert->id);
            case 1:
                return std::make_unique<RenameInsert>(insert->id, someName());
            case 2:
                return std::make_unique<SetInsertValue>(
                    insert->id, static_cast<InsertField>(roll(5)), unit() * 2.0);
            case 3: {
                Slot slot;
                slot.type = "Reverb";
                slot.params.push_back(SlotParam{.name = "room", .value = unit()});
                return std::make_unique<AddSlot>(insert->id, std::move(slot));
            }
            default:
                return std::make_unique<SetMaster>(insert->id);
            }
        }
        return nullptr;
    case 27:
        if (const auto* insert = pick(m_project.mixer.inserts)) {
            if (const auto* slot = pick(insert->slots)) {
                if (roll(1) == 0) {
                    return std::make_unique<RemoveSlot>(slot->id);
                }
                return std::make_unique<SetSlotValue>(slot->id, "room", unit());
            }
            if (const auto* target = pick(m_project.mixer.inserts)) {
                return std::make_unique<AddSend>(insert->id, target->id, static_cast<float>(unit()),
                                                 roll(1) == 0);
            }
        }
        return nullptr;
    case 28:
        if (const auto* from = pick(m_project.mixer.inserts)) {
            if (const auto* to = pick(m_project.mixer.inserts)) {
                return std::make_unique<AddRoute>(from->id, to->id);
            }
        }
        return nullptr;
    default:
        if (const auto* route = pick(m_project.mixer.routes)) {
            return std::make_unique<RemoveRoute>(route->id);
        }
        return nullptr;
    }
}

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
