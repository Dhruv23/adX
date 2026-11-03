// adx-thread: main
#include "engine/format/midi/MidiExporter.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <tuple>
#include <variant>

#include "engine/format/mininotation/PatternCompiler.h"
#include "engine/project/Project.h"

namespace adx::format::midi {

namespace {

constexpr std::uint8_t kPercussion = 9;

/// One event on a track, before delta-encoding.
struct Event {
    std::int64_t at{0};
    /// Sort key at one tick: note-offs (0) before note-ons (1), so a note ending where
    /// the next begins frees its pitch first; among ons, longest first, so a reader
    /// that pairs same-pitch notes last-in-first-out - as the importer does - gives the
    /// shortest its earliest note-off.
    int order{0};
    std::int64_t length{0};
    std::vector<std::uint8_t> bytes;
};

void appendTrack(ByteWriter& out, std::vector<Event>& events) {
    std::ranges::stable_sort(events, [](const Event& a, const Event& b) {
        return std::tie(a.at, a.order, b.length) < std::tie(b.at, b.order, a.length);
    });
    ByteWriter body;
    std::int64_t now = 0;
    for (const Event& event : events) {
        body.vlq(static_cast<std::uint32_t>(std::max<std::int64_t>(event.at - now, 0)));
        body.append(event.bytes);
        now = std::max(now, event.at);
    }
    body.vlq(0);
    body.append(std::vector<std::uint8_t>{0xFF, 0x2F, 0x00});
    out.text("MTrk");
    out.u32(static_cast<std::uint32_t>(body.bytes().size()));
    out.append(body.bytes());
}

Event meta(std::int64_t at, std::uint8_t type, std::span<const std::uint8_t> data) {
    ByteWriter bytes;
    bytes.u8(0xFF);
    bytes.u8(type);
    bytes.vlq(static_cast<std::uint32_t>(data.size()));
    bytes.append(data);
    return Event{.at = at, .order = 0, .length = 0, .bytes = bytes.bytes()};
}

Event metaText(std::int64_t at, std::uint8_t type, std::string_view text) {
    const std::vector<std::uint8_t> data(text.begin(), text.end());
    return meta(at, type, data);
}

/// Every note a pattern placement plays, in arrangement ticks: the snapshot builder's
/// window rule - notes that start inside it, cut at its end.
void placeNotes(const project::Project& project, const project::PlaylistItem& item,
                const project::Pattern& pattern, std::vector<std::vector<MidiNote>>& byChannel) {
    const std::int64_t length = item.length.value > 0 ? item.length.value : pattern.length.value;
    const std::int64_t from = item.sourceOffset.value;
    const std::int64_t to = from + std::max<std::int64_t>(length, 0);
    const auto ordinal = [&project](core::ChannelId id) -> std::size_t {
        const auto found = std::ranges::find(project.channels, id, &project::Channel::id);
        return static_cast<std::size_t>(found - project.channels.begin());
    };
    for (const project::NoteClip& clip : pattern.noteClips) {
        const std::size_t c = ordinal(clip.channel);
        if (c >= byChannel.size()) {
            continue;
        }
        for (const project::Note& note : clip.notes) {
            if (note.muted || note.start.value < from || note.start.value >= to) {
                continue;
            }
            const std::int64_t end = std::min(note.end().value, to);
            byChannel[c].push_back(
                MidiNote{.start = item.start.value + (note.start.value - from),
                         .length = std::max<std::int64_t>(end - note.start.value, 1),
                         .pitch = note.pitch,
                         .velocity = std::max<std::uint8_t>(note.velocity, 1)});
        }
    }
    const core::Ticks cycle = mini::cycleLength(project.tempo);
    for (const project::MiniNotationSource& source : pattern.mini) {
        const std::size_t c = ordinal(source.channel);
        if (c >= byChannel.size()) {
            continue;
        }
        const std::int64_t placedEnd = item.start.value + (to - from);
        for (const mini::RealizedNote& realized : mini::realize(
                 mini::joinLines(source.lines), item.start, core::Ticks{placedEnd}, cycle)) {
            const std::int64_t on = realized.note.start.value;
            const std::int64_t off = std::min(on + realized.note.length.value, placedEnd);
            if (off > on) {
                byChannel[c].push_back(
                    MidiNote{.start = on,
                             .length = off - on,
                             .pitch = realized.note.pitch,
                             .velocity = std::max<std::uint8_t>(realized.note.velocity, 1)});
            }
        }
    }
}

} // namespace

MidiSong songOf(const project::Project& project) {
    MidiSong song;
    song.division = kExportDivision;
    const core::TempoView tempo = project.tempo.view();
    for (std::size_t i = 0; i < tempo.events.size(); ++i) {
        const core::TempoEvent& event = tempo.events[i];
        song.tempos.push_back(MidiTempo{.at = event.at.value, .bpm = event.bpm});
        // A ramp: one step per beat, at the tempo the ramp has reached there.
        if (event.ramp && i + 1 < tempo.events.size()) {
            const core::TempoEvent& next = tempo.events[i + 1];
            for (std::int64_t at = event.at.value + core::kPpq; at < next.at.value;
                 at += core::kPpq) {
                song.tempos.push_back(
                    MidiTempo{.at = at, .bpm = project.tempo.bpmAt(core::Ticks{at})});
            }
        }
    }
    for (const core::MeterEvent& meter : project.tempo.meterEvents()) {
        song.meters.push_back(
            MidiMeter{.at = meter.at.value,
                      .numerator = static_cast<std::uint8_t>(meter.numerator),
                      .denominator = static_cast<std::uint8_t>(meter.denominator)});
    }
    for (const project::Marker& marker : project.markers) {
        song.markers.push_back(MidiMarker{.at = marker.at.value, .name = marker.name});
    }
    std::ranges::stable_sort(song.markers, {}, &MidiMarker::at);

    std::vector<std::vector<MidiNote>> byChannel(project.channels.size());
    for (const project::PlaylistTrack& track : project.playlist.tracks) {
        if (!project.playlist.audible(track)) {
            continue;
        }
        for (const project::PlaylistItem& item : track.items) {
            const auto* ref = std::get_if<project::PatternRef>(&item.content);
            const project::Pattern* pattern = ref != nullptr ? project.find(ref->pattern) : nullptr;
            if (!item.muted && pattern != nullptr) {
                placeNotes(project, item, *pattern, byChannel);
            }
        }
    }
    std::uint8_t next = 0;
    for (std::size_t c = 0; c < project.channels.size(); ++c) {
        if (next == kPercussion) {
            ++next;
        }
        MidiTrack track{.name = project.channels[c].name, .channel = next, .notes = {}};
        next = static_cast<std::uint8_t>((next + 1) % 16);
        sortNotes(byChannel[c]);
        track.notes = std::move(byChannel[c]);
        song.tracks.push_back(std::move(track));
    }
    return song;
}

std::vector<std::uint8_t> writeMidi(const MidiSong& song) {
    ByteWriter out;
    out.text("MThd");
    out.u32(6);
    out.u16(1);
    out.u16(static_cast<std::uint16_t>(song.tracks.size() + 1));
    out.u16(kExportDivision);

    std::vector<Event> conductor;
    for (const MidiTempo& tempo : song.tempos) {
        const auto us =
            static_cast<std::uint32_t>(std::llround(60'000'000.0 / std::max(tempo.bpm, 1.0)));
        const std::vector<std::uint8_t> data{static_cast<std::uint8_t>(us >> 16U),
                                             static_cast<std::uint8_t>(us >> 8U),
                                             static_cast<std::uint8_t>(us)};
        conductor.push_back(meta(tempo.at, 0x51, data));
    }
    for (const MidiMeter& meter : song.meters) {
        std::uint8_t power = 0;
        while ((1U << power) < meter.denominator && power < 6) {
            ++power;
        }
        // 24 clocks per metronome click, 8 thirty-seconds per quarter: the defaults.
        const std::vector<std::uint8_t> data{meter.numerator, power, 24, 8};
        conductor.push_back(meta(meter.at, 0x58, data));
    }
    for (const MidiMarker& marker : song.markers) {
        conductor.push_back(metaText(marker.at, 0x06, marker.name));
    }
    appendTrack(out, conductor);

    for (const MidiTrack& track : song.tracks) {
        std::vector<Event> events;
        events.push_back(metaText(0, 0x03, track.name));
        for (const MidiNote& note : track.notes) {
            const auto channel = static_cast<std::uint8_t>(track.channel & 0x0FU);
            events.push_back(Event{
                .at = note.start,
                .order = 1,
                .length = note.length,
                .bytes = {static_cast<std::uint8_t>(0x90U | channel), note.pitch, note.velocity}});
            events.push_back(
                Event{.at = note.start + note.length,
                      .order = 0,
                      .length = 0,
                      .bytes = {static_cast<std::uint8_t>(0x80U | channel), note.pitch, 64}});
        }
        appendTrack(out, events);
    }
    return out.bytes();
}

bool exportMidiFile(const project::Project& project, const std::filesystem::path& path,
                    std::string& error) {
    const std::vector<std::uint8_t> bytes = writeMidi(songOf(project));
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        error = "could not write " + path.string();
        return false;
    }
    return true;
}

} // namespace adx::format::midi
