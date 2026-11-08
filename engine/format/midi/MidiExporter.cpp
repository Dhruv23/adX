// adx-thread: main
#include "engine/format/midi/MidiExporter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <tuple>
#include <utility>
#include <variant>

#include "engine/format/mininotation/PatternCompiler.h"
#include "engine/project/CurveCompile.h"
#include "engine/project/EventCompile.h"
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

/// A pitch-bend message: `cents` within +-range semitones, 14 bits, centre 8192.
std::vector<std::uint8_t> bendBytes(std::uint8_t channel, double cents, int semitones) {
    const double unit = 8192.0 / (static_cast<double>(semitones) * 100.0);
    const auto value =
        static_cast<int>(std::clamp<long long>(std::llround(8192.0 + (cents * unit)), 0, 16383));
    return {static_cast<std::uint8_t>(0xE0U | channel), static_cast<std::uint8_t>(value & 0x7F),
            static_cast<std::uint8_t>((value >> 7) & 0x7F)};
}

/// The four controller messages of one RPN write: select, data entry MSB and LSB.
void rpn(std::vector<Event>& events, std::uint8_t channel, std::uint8_t number, std::uint8_t msb,
         std::uint8_t lsb) {
    const auto cc = static_cast<std::uint8_t>(0xB0U | channel);
    for (const auto& [control, value] : std::array<std::pair<std::uint8_t, std::uint8_t>, 4>{
             {{101, 0}, {100, number}, {6, msb}, {38, lsb}}}) {
        events.push_back(Event{.at = 0, .order = -1, .length = 0, .bytes = {cc, control, value}});
    }
}

/// A note's pitch offset as bend points: every corner of its chain, and a point every
/// kBendStep ticks along each straight segment, so a receiver stepping between them is
/// within a few cents of the line.
constexpr std::int64_t kBendStep = 60;

std::vector<MidiBend> bendOf(const project::NoteExtras& extras, std::int64_t length) {
    std::vector<MidiBend> out;
    const std::vector<project::Knot> chain = project::pitchOffsetChain(extras);
    if (chain.empty()) {
        return out;
    }
    const auto add = [&](std::int64_t at, float cents) {
        if (at < 0 || at >= length) {
            return;
        }
        const auto value = static_cast<std::int16_t>(std::lround(cents));
        if (!out.empty() && out.back().at == at) {
            out.back().cents = value;
            return;
        }
        if (!out.empty() && out.back().cents == value) {
            return;
        }
        out.push_back(MidiBend{.at = at, .cents = value});
    };
    add(0, project::knotValueAt(chain.data(), chain.size(), 0));
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        const project::Knot& a = chain[i];
        const project::Knot& b = chain[i + 1];
        for (std::int64_t at = std::max<std::int64_t>(a.tick, 0); at < b.tick; at += kBendStep) {
            const double t =
                static_cast<double>(at - a.tick) / static_cast<double>(b.tick - a.tick);
            add(at, static_cast<float>(a.value + ((b.value - a.value) * t)));
        }
        add(b.tick, b.value);
    }
    if (std::ranges::all_of(out, [](const MidiBend& p) { return p.cents == 0; })) {
        out.clear();
    }
    return out;
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
            MidiNote out{.start = item.start.value + (note.start.value - from),
                         .length = std::max<std::int64_t>(end - note.start.value, 1),
                         .pitch = note.pitch,
                         .velocity = std::max<std::uint8_t>(note.velocity, 1),
                         .bend = {},
                         .lyric = {}};
            if (const project::NoteExtras* extras = clip.extrasFor(note.id)) {
                out.bend = bendOf(*extras, out.length);
                out.lyric = extras->lyric;
            }
            byChannel[c].push_back(std::move(out));
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
                             .velocity = std::max<std::uint8_t>(realized.note.velocity, 1),
                             .bend = {},
                             .lyric = {}});
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
        track.mpe =
            std::ranges::any_of(track.notes, [](const MidiNote& n) { return !n.bend.empty(); });
        if (track.mpe) {
            song.warnings.push_back(
                "\"" + track.name +
                "\" bends, so it is written as an MPE zone on all 16 MIDI channels; "
                "play it on a port of its own");
        }
        song.tracks.push_back(std::move(track));
    }
    return song;
}

namespace {

/// An MPE lower zone (MPE 1.0 §2): the manager is MIDI channel 1 (0 here), members 2-16
/// (1-15), each set to kMpeBendSemitones. Each note takes the member channel that has
/// been free longest, and its bend goes on that channel alone. More than fifteen notes
/// at once share channels, and their bends with them: the one lossy case.
void appendMpe(const MidiTrack& track, std::vector<Event>& events) {
    constexpr std::uint8_t kManager = 0;
    constexpr std::uint8_t kMembers = 15;
    rpn(events, kManager, 6, kMembers, 0); // the MPE configuration message
    for (std::uint8_t member = 1; member <= kMembers; ++member) {
        rpn(events, member, 0, kMpeBendSemitones, 0);
    }
    std::array<std::int64_t, kMembers + 1> freeAt{};
    std::array<std::int64_t, kMembers + 1> usedAt{};
    for (const MidiNote& note : track.notes) {
        std::uint8_t channel = 1;
        bool free = false;
        for (std::uint8_t m = 1; m <= kMembers; ++m) {
            if (freeAt.at(m) <= note.start && (!free || usedAt.at(m) < usedAt.at(channel))) {
                channel = m;
                free = true;
            }
        }
        if (!free) {
            for (std::uint8_t m = 1; m <= kMembers; ++m) {
                if (freeAt.at(m) < freeAt.at(channel)) {
                    channel = m;
                }
            }
        }
        freeAt.at(channel) = std::max(freeAt.at(channel), note.start + note.length);
        usedAt.at(channel) = note.start;
        if (!note.lyric.empty()) {
            events.push_back(metaText(note.start, 0x05, note.lyric));
        }
        // The bend the note starts with goes before its note-on; the rest during it.
        const double first =
            note.bend.empty() || note.bend.front().at > 0 ? 0.0 : note.bend.front().cents;
        events.push_back(Event{.at = note.start,
                               .order = 1,
                               .length = note.length,
                               .bytes = bendBytes(channel, first, kMpeBendSemitones)});
        events.push_back(Event{
            .at = note.start,
            .order = 2,
            .length = note.length,
            .bytes = {static_cast<std::uint8_t>(0x90U | channel), note.pitch, note.velocity}});
        for (const MidiBend& point : note.bend) {
            if (point.at > 0) {
                events.push_back(
                    Event{.at = note.start + point.at,
                          .order = 3,
                          .length = 0,
                          .bytes = bendBytes(channel, point.cents, kMpeBendSemitones)});
            }
        }
        events.push_back(
            Event{.at = note.start + note.length,
                  .order = 0,
                  .length = 0,
                  .bytes = {static_cast<std::uint8_t>(0x80U | channel), note.pitch, 64}});
    }
}

} // namespace

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
        if (track.mpe) {
            appendMpe(track, events);
            appendTrack(out, events);
            continue;
        }
        for (const MidiNote& note : track.notes) {
            const auto channel = static_cast<std::uint8_t>(track.channel & 0x0FU);
            if (!note.lyric.empty()) {
                events.push_back(metaText(note.start, 0x05, note.lyric));
            }
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
