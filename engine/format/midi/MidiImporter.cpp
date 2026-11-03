// adx-thread: main
#include "engine/format/midi/MidiImporter.h"

#include <array>
#include <cmath>
#include <fstream>
#include <iterator>

#include "engine/core/Time.h"
#include "engine/project/Project.h"
#include "engine/project/commands/ChannelCommands.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/NoteCommands.h"
#include "engine/project/commands/PatternCommands.h"
#include "engine/project/commands/PlaylistCommands.h"
#include "engine/project/commands/ProjectCommands.h"

namespace adx::format::midi {

namespace {

constexpr std::uint32_t kHeader = 0x4D546864; // MThd
constexpr std::uint32_t kTrack = 0x4D54726B;  // MTrk

/// A sounding note awaiting its note-off.
struct Pending {
    std::int64_t start{0};
    std::uint8_t velocity{0};
};

/// File ticks to adX ticks: exact when the division divides kPpq, nearest otherwise.
std::int64_t toAdx(std::uint64_t ticks, std::uint16_t division) {
    const auto scaled = ticks * static_cast<std::uint64_t>(core::kPpq);
    return static_cast<std::int64_t>((scaled + (division / 2)) / division);
}

/// One MTrk chunk: its notes per channel, and the meta events the song keeps.
void parseTrack(ByteReader& r, std::size_t end, std::uint16_t division, MidiSong& song) {
    std::string name;
    std::array<std::vector<MidiNote>, 16> notes;
    std::array<std::array<std::vector<Pending>, 128>, 16> pending;
    std::uint64_t tick = 0;
    std::uint8_t running = 0;
    const std::span<const std::uint8_t> bytes = r.bytes();

    // Same-pitch overlaps pair last-in-first-out, the way most sequencers pair them;
    // either pairing covers the same time on the roll.
    const auto close = [&](int channel, int pitch, std::uint64_t at) {
        auto& stack =
            pending.at(static_cast<std::size_t>(channel)).at(static_cast<std::size_t>(pitch));
        if (stack.empty()) {
            return; // an unmatched note-off
        }
        const Pending started = stack.back();
        stack.pop_back();
        const std::int64_t stop = toAdx(at, division);
        notes.at(static_cast<std::size_t>(channel))
            .push_back(MidiNote{.start = started.start,
                                .length = std::max<std::int64_t>(stop - started.start, 1),
                                .pitch = static_cast<std::uint8_t>(pitch),
                                .velocity = started.velocity});
    };

    bool ended = false;
    while (r.position() < end && !ended) {
        std::uint32_t delta = 0;
        if (!r.vlq(delta) || r.position() >= end) {
            break;
        }
        tick += delta;
        std::uint8_t status = r.peek();
        if ((status & 0x80U) != 0) {
            r.u8();
            if (status < 0xF0) {
                running = status;
            }
        } else {
            if (running == 0) {
                break; // a data byte with no status to run on: the rest is unreadable
            }
            status = running;
        }

        if (status == 0xFF) {
            if (!r.has(1)) {
                break;
            }
            const std::uint8_t type = r.u8();
            std::uint32_t length = 0;
            if (!r.vlq(length) || !r.has(length)) {
                break;
            }
            const std::size_t data = r.position();
            const std::int64_t at = toAdx(tick, division);
            if (type == 0x2F) {
                ended = true;
            } else if (type == 0x51 && length >= 3) {
                const std::uint32_t usPerQuarter =
                    (static_cast<std::uint32_t>(bytes[data]) << 16U) |
                    (static_cast<std::uint32_t>(bytes[data + 1]) << 8U) | bytes[data + 2];
                if (usPerQuarter > 0) {
                    song.tempos.push_back(MidiTempo{.at = at, .bpm = 60'000'000.0 / usPerQuarter});
                }
            } else if (type == 0x58 && length >= 2) {
                song.meters.push_back(MidiMeter{
                    .at = at,
                    .numerator = bytes[data],
                    .denominator =
                        static_cast<std::uint8_t>(1U << std::min<unsigned>(bytes[data + 1], 6U))});
            } else if (type == 0x03 && name.empty()) {
                name.assign(reinterpret_cast<const char*>(bytes.data() + data), length);
            } else if (type == 0x06) {
                song.markers.push_back(MidiMarker{
                    .at = at,
                    .name =
                        std::string(reinterpret_cast<const char*>(bytes.data() + data), length)});
            }
            r.seek(data + length);
        } else if (status == 0xF0 || status == 0xF7) {
            std::uint32_t length = 0;
            if (!r.vlq(length) || !r.has(length)) {
                break;
            }
            r.seek(r.position() + length);
        } else if (status >= 0x80 && status < 0xF0) {
            const std::uint8_t kind = status & 0xF0U;
            const int channel = status & 0x0F;
            const int count = (kind == 0xC0 || kind == 0xD0) ? 1 : 2;
            if (!r.has(static_cast<std::size_t>(count))) {
                break;
            }
            const std::uint8_t d1 = r.u8() & 0x7FU;
            const std::uint8_t d2 = count == 2 ? static_cast<std::uint8_t>(r.u8() & 0x7FU) : 0;
            if (kind == 0x90 && d2 > 0) {
                pending.at(static_cast<std::size_t>(channel))
                    .at(d1)
                    .push_back(Pending{.start = toAdx(tick, division), .velocity = d2});
            } else if (kind == 0x80 || (kind == 0x90 && d2 == 0)) {
                close(channel, d1, tick);
            }
        } else {
            // System common: essentially never in files. 0xF2 carries two data bytes,
            // 0xF1 and 0xF3 one, the rest none.
            std::size_t count = 0;
            if (status == 0xF2) {
                count = 2;
            } else if (status == 0xF1 || status == 0xF3) {
                count = 1;
            }
            r.seek(r.position() + count);
        }
    }
    // Anything still sounding at the end of the track ends there - loop exports often
    // omit their last note-offs.
    for (int channel = 0; channel < 16; ++channel) {
        for (int pitch = 0; pitch < 128; ++pitch) {
            while (!pending.at(static_cast<std::size_t>(channel))
                        .at(static_cast<std::size_t>(pitch))
                        .empty()) {
                close(channel, pitch, tick);
            }
        }
    }
    const auto used = std::ranges::count_if(notes, [](const auto& n) { return !n.empty(); });
    for (std::uint8_t channel = 0; channel < 16; ++channel) {
        std::vector<MidiNote>& mine = notes.at(channel);
        if (mine.empty()) {
            continue;
        }
        MidiTrack track;
        track.channel = channel;
        track.name = name.empty() ? "MIDI Ch " + std::to_string(channel + 1) : name;
        if (!name.empty() && used > 1) {
            track.name += " (Ch " + std::to_string(channel + 1) + ")";
        }
        sortNotes(mine);
        track.notes = std::move(mine);
        song.tracks.push_back(std::move(track));
    }
}

} // namespace

std::optional<MidiSong> parseMidi(std::span<const std::uint8_t> bytes, std::string& error) {
    ByteReader r(bytes);
    if (!r.has(14) || r.u32() != kHeader) {
        error = "not a MIDI file (no MThd header)";
        return std::nullopt;
    }
    const std::uint32_t headerLength = r.u32();
    if (headerLength < 6 || !r.has(headerLength)) {
        error = "malformed MThd header";
        return std::nullopt;
    }
    static_cast<void>(r.u16()); // format 0, 1 or 2: every MTrk is read the same way
    const std::uint16_t trackCount = r.u16();
    const std::uint16_t division = r.u16();
    r.seek(r.position() + headerLength - 6);
    if ((division & 0x8000U) != 0) {
        error = "SMPTE time division is not supported";
        return std::nullopt;
    }
    if (division == 0) {
        error = "time division is zero";
        return std::nullopt;
    }
    MidiSong song;
    song.division = division;
    for (std::uint16_t read = 0; read < trackCount && r.has(8);) {
        const std::uint32_t type = r.u32();
        const std::uint32_t length = r.u32();
        if (!r.has(length)) {
            break; // truncated: keep the tracks already read
        }
        const std::size_t end = r.position() + length;
        if (type == kTrack) {
            parseTrack(r, end, division, song);
            ++read;
        }
        // An alien chunk is skipped and does not count against the header's tracks.
        r.seek(end);
    }
    std::ranges::stable_sort(song.tempos, {}, &MidiTempo::at);
    std::ranges::stable_sort(song.meters, {}, &MidiMeter::at);
    std::ranges::stable_sort(song.markers, {}, &MidiMarker::at);
    return song;
}

std::optional<MidiSong> readMidiFile(const std::filesystem::path& path, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "could not open " + path.string();
        return std::nullopt;
    }
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),
                                          std::istreambuf_iterator<char>()};
    return parseMidi(bytes, error);
}

std::string importSong(const MidiSong& song, project::Project& project,
                       project::CommandStack& stack, const std::string& patternName) {
    stack.beginGroup("Import MIDI");
    for (const MidiTempo& tempo : song.tempos) {
        stack.execute(
            std::make_unique<project::SetTempoEvent>(core::Ticks{tempo.at}, tempo.bpm, false),
            project);
    }
    for (const MidiMeter& meter : song.meters) {
        stack.execute(std::make_unique<project::SetMeterEvent>(core::Ticks{meter.at},
                                                               meter.numerator, meter.denominator),
                      project);
    }
    std::int64_t end = 0;
    for (const MidiTrack& track : song.tracks) {
        for (const MidiNote& note : track.notes) {
            end = std::max(end, note.start + note.length);
        }
    }
    project::Pattern prototype;
    prototype.name = project.findPatternByName(patternName) == nullptr
                         ? patternName
                         : patternName + " " + std::to_string(project.patterns.size() + 1);
    // Whole bars, so the pattern loops on the grid.
    const std::int64_t bar = core::kPpq * 4;
    prototype.length = core::Ticks{std::max<std::int64_t>(((end + bar - 1) / bar) * bar, bar)};
    auto pattern = std::make_unique<project::AddPattern>(prototype);
    const project::AddPattern* patternRaw = pattern.get();
    stack.execute(std::move(pattern), project);

    for (const MidiTrack& track : song.tracks) {
        project::Channel channel;
        channel.name = track.name;
        channel.output = project.mixer.master;
        auto add = std::make_unique<project::AddChannel>(channel);
        const project::AddChannel* raw = add.get();
        stack.execute(std::move(add), project);
        std::vector<project::Note> notes;
        notes.reserve(track.notes.size());
        for (const MidiNote& note : track.notes) {
            project::Note value;
            value.start = core::Ticks{note.start};
            value.length = core::Ticks{note.length};
            value.pitch = note.pitch;
            value.velocity = note.velocity;
            notes.push_back(value);
        }
        stack.execute(std::make_unique<project::AddNotes>(patternRaw->created(), raw->created(),
                                                          std::move(notes)),
                      project);
    }

    project::PlaylistTrack row;
    row.name = prototype.name;
    auto addTrack = std::make_unique<project::AddPlaylistTrack>(row);
    const project::AddPlaylistTrack* trackRaw = addTrack.get();
    stack.execute(std::move(addTrack), project);
    project::PlaylistItem item;
    item.content = project::PatternRef{.pattern = patternRaw->created()};
    stack.execute(std::make_unique<project::AddPlaylistItem>(trackRaw->created(), item), project);

    for (const MidiMarker& marker : song.markers) {
        stack.execute(std::make_unique<project::AddMarker>(core::Ticks{marker.at}, marker.name),
                      project);
    }
    stack.endGroup();
    return prototype.name;
}

} // namespace adx::format::midi
