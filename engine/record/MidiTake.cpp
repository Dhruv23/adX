// adx-thread: main
#include "engine/record/MidiTake.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "engine/core/Time.h"
#include "engine/format/midi/MidiImporter.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/NoteCommands.h"
#include "engine/project/commands/PatternCommands.h"
#include "engine/project/commands/PlaylistCommands.h"

namespace adx::record {

namespace {

/// The log as one SMF track at adX's own division, so the importer reads it tick for tick.
std::vector<std::uint8_t> smfOf(std::span<const midi::TakeEvent> events, std::int64_t origin,
                                std::int64_t stop) {
    using format::midi::ByteWriter;
    std::vector<midi::TakeEvent> sorted(events.begin(), events.end());
    std::ranges::stable_sort(sorted, {}, &midi::TakeEvent::tick);
    ByteWriter body;
    std::int64_t now = 0;
    for (const midi::TakeEvent& event : sorted) {
        const std::uint8_t kind = event.status & 0xF0U;
        if (event.status >= 0xF0 || kind == 0xC0 || kind == 0xD0) {
            continue; // nothing a note keeps
        }
        const std::int64_t at = std::max<std::int64_t>(event.tick - origin, 0);
        body.vlq(static_cast<std::uint32_t>(at - now));
        now = at;
        body.u8(event.status);
        body.u8(event.data1 & 0x7FU);
        body.u8(event.data2 & 0x7FU);
    }
    // End of track at the stop: a note still held when recording stopped ends there.
    body.vlq(static_cast<std::uint32_t>(std::max<std::int64_t>(stop - origin - now, 0)));
    body.u8(0xFF);
    body.u8(0x2F);
    body.u8(0x00);

    ByteWriter file;
    file.text("MThd");
    file.u32(6);
    file.u16(0);
    file.u16(1);
    file.u16(format::midi::kExportDivision);
    file.text("MTrk");
    file.u32(static_cast<std::uint32_t>(body.bytes().size()));
    file.append(body.bytes());
    return file.bytes();
}

/// The start of the bar `tick` is in, by the meter in force there.
std::int64_t barFloor(const project::Project& project, std::int64_t tick) {
    std::int64_t from = 0;
    std::int64_t bar = core::kPpq * 4;
    for (const core::MeterEvent& meter : project.tempo.meterEvents()) {
        if (meter.at.value > tick) {
            break;
        }
        from = meter.at.value;
        bar = std::max<std::int64_t>(1, core::kPpq * 4 * meter.numerator /
                                            std::max(1, static_cast<int>(meter.denominator)));
    }
    return from + (((tick - from) / bar) * bar);
}

} // namespace

std::optional<format::midi::MidiSong> takeSong(std::span<const midi::TakeEvent> events,
                                               std::int64_t origin, std::int64_t stop) {
    std::string error;
    std::optional<format::midi::MidiSong> song =
        format::midi::parseMidi(smfOf(events, origin, std::max(stop, origin)), error);
    if (!song || std::ranges::all_of(song->tracks, [](const auto& t) { return t.notes.empty(); })) {
        return std::nullopt;
    }
    return song;
}

std::string commitTake(std::span<const midi::TakeEvent> events, std::int64_t origin,
                       std::int64_t stop, core::ChannelId channel, project::Project& project,
                       project::CommandStack& stack) {
    const std::int64_t start = barFloor(project, std::max<std::int64_t>(origin, 0));
    const std::optional<format::midi::MidiSong> song = takeSong(events, start, stop);
    if (!song || project.find(channel) == nullptr) {
        return {};
    }
    std::vector<project::Note> notes;
    std::vector<project::NoteExtrasAt> extras;
    std::int64_t end = 0;
    for (const format::midi::MidiTrack& track : song->tracks) {
        for (const format::midi::MidiNote& note : track.notes) {
            if (!note.bend.empty()) {
                project::NoteExtras bent;
                for (const format::midi::MidiBend& point : note.bend) {
                    bent.pitchCurve.push_back(project::PitchPoint{
                        .at = core::Ticks{point.at}, .cents = point.cents, .curve = {}});
                }
                extras.push_back(project::NoteExtrasAt{.index = notes.size(), .extras = bent});
            }
            project::Note value;
            value.start = core::Ticks{note.start};
            value.length = core::Ticks{note.length};
            value.pitch = note.pitch;
            value.velocity = note.velocity;
            notes.push_back(value);
            end = std::max(end, note.start + note.length);
        }
    }
    project::Pattern prototype;
    for (std::size_t n = 1;; ++n) {
        prototype.name = "Take " + std::to_string(n);
        if (project.findPatternByName(prototype.name) == nullptr) {
            break;
        }
    }
    const std::int64_t bar = core::kPpq * 4;
    prototype.length = core::Ticks{std::max<std::int64_t>(((end + bar - 1) / bar) * bar, bar)};

    stack.beginGroup("Record MIDI");
    auto pattern = std::make_unique<project::AddPattern>(prototype);
    const project::AddPattern* patternRaw = pattern.get();
    stack.execute(std::move(pattern), project);
    stack.execute(std::make_unique<project::AddNotes>(patternRaw->created(), channel,
                                                      std::move(notes), std::move(extras)),
                  project);
    project::PlaylistTrack row;
    row.name = prototype.name;
    auto addTrack = std::make_unique<project::AddPlaylistTrack>(row);
    const project::AddPlaylistTrack* trackRaw = addTrack.get();
    stack.execute(std::move(addTrack), project);
    project::PlaylistItem item;
    item.start = core::Ticks{start};
    item.content = project::PatternRef{.pattern = patternRaw->created()};
    stack.execute(std::make_unique<project::AddPlaylistItem>(trackRaw->created(), item), project);
    stack.endGroup();
    return prototype.name;
}

} // namespace adx::record
