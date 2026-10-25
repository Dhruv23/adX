#include "tests/cpp/project/RandomCommands.h"

#include <utility>
#include <variant>
#include <vector>

#include "engine/project/commands/AutomationCommands.h"
#include "engine/project/commands/ChannelCommands.h"
#include "engine/project/commands/MixerCommands.h"
#include "engine/project/commands/NoteCommands.h"
#include "engine/project/commands/PatternCommands.h"
#include "engine/project/commands/PlaylistCommands.h"
#include "engine/project/commands/ProjectCommands.h"

namespace adx::tests {

using namespace adx::project;
using adx::core::Ticks;

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

} // namespace adx::tests
