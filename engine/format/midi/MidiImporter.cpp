// adx-thread: main
#include "engine/format/midi/MidiImporter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

#include "engine/core/Time.h"
#include "engine/project/ParamRegistry.h"
#include "engine/project/Project.h"
#include "engine/project/commands/AutomationCommands.h"
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

/// A channel's pitch-bend history and the controller state that sets its range.
struct ChannelBend {
    /// (adX tick, cents) at each bend message, in file order.
    std::vector<std::pair<std::int64_t, double>> events;
    double semitones{2.0};
    /// The RPN selected by CC 101/100; 0x7F7F when none.
    int rpn{0x3FFF};
    bool rangeSet{false};
};

/// The bend in effect at `at`, from the history (0 before the first message).
double bendAt(const ChannelBend& bend, std::int64_t at) {
    double value = 0.0;
    for (const auto& [tick, cents] : bend.events) {
        if (tick > at) {
            break;
        }
        value = cents;
    }
    return value;
}

/// The note's bend points: the value at its start, then every change before its end,
/// thinned where a straight line through the neighbours is within a cent.
std::vector<MidiBend> notesBend(const ChannelBend& bend, std::int64_t start, std::int64_t stop) {
    std::vector<MidiBend> points;
    points.push_back(
        MidiBend{.at = 0, .cents = static_cast<std::int16_t>(std::lround(bendAt(bend, start)))});
    for (const auto& [tick, cents] : bend.events) {
        if (tick <= start || tick >= stop) {
            continue;
        }
        const auto value = static_cast<std::int16_t>(std::lround(cents));
        if (points.back().at == tick - start) {
            points.back().cents = value;
        } else if (points.back().cents != value) {
            points.push_back(MidiBend{.at = tick - start, .cents = value});
        }
    }
    std::vector<MidiBend> kept;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (kept.empty() || i + 1 == points.size()) {
            kept.push_back(points[i]);
            continue;
        }
        const MidiBend& a = kept.back();
        const MidiBend& b = points[i + 1];
        const double t =
            static_cast<double>(points[i].at - a.at) / static_cast<double>(b.at - a.at);
        const double line = a.cents + ((b.cents - a.cents) * t);
        if (std::abs(line - points[i].cents) > 1.0) {
            kept.push_back(points[i]);
        }
    }
    if (std::ranges::all_of(kept, [](const MidiBend& p) { return p.cents == 0; })) {
        kept.clear();
    }
    return kept;
}

/// A pitch-bend or controller message on `channel`: the bend history, and the RPN
/// writes that set its range (0) or configure an MPE zone (6).
void bendMessage(std::array<ChannelBend, 16>& bends, bool& mpe, int channel, std::uint8_t kind,
                 std::uint8_t d1, std::uint8_t d2, std::int64_t at) {
    ChannelBend& bend = bends.at(static_cast<std::size_t>(channel));
    if (kind == 0xE0) {
        const int value = d1 | (d2 << 7);
        bend.events.emplace_back(at, (value - 8192) / 8192.0 * bend.semitones * 100.0);
        return;
    }
    if (d1 == 101) {
        bend.rpn = (d2 << 7) | (bend.rpn & 0x7F);
    } else if (d1 == 100) {
        bend.rpn = (bend.rpn & ~0x7F) | d2;
    } else if (d1 == 6 && bend.rpn == 0) {
        bend.semitones = d2;
        bend.rangeSet = true;
    } else if (d1 == 38 && bend.rpn == 0) {
        bend.semitones = std::floor(bend.semitones) + (d2 / 100.0);
    } else if (d1 == 6 && bend.rpn == 6 && (channel == 0 || channel == 15) && d2 > 0) {
        // MPE: member channels default to 48 semitones of bend.
        mpe = true;
        for (ChannelBend& member : bends) {
            if (!member.rangeSet) {
                member.semitones = kMpeBendSemitones;
            }
        }
    }
}

/// A parsed track's notes into the song: each lyric event onto the note it starts, then
/// an MPE zone as one track, or one track per channel used.
void finishTrack(std::array<std::vector<MidiNote>, 16>& notes,
                 std::array<std::vector<MidiControl>, 16>& controls,
                 const std::vector<std::pair<std::int64_t, std::string>>& lyrics, bool mpe,
                 const std::string& name, MidiSong& song) {
    for (const auto& [at, text] : lyrics) {
        for (std::vector<MidiNote>& mine : notes) {
            const auto found = std::ranges::find_if(
                mine, [&](const MidiNote& n) { return n.start == at && n.lyric.empty(); });
            if (found != mine.end()) {
                found->lyric = text;
                break;
            }
        }
    }
    if (mpe) {
        // A zone is one instrument: its notes, from every member channel, are one track.
        std::vector<MidiNote> all;
        for (std::vector<MidiNote>& mine : notes) {
            all.insert(all.end(), mine.begin(), mine.end());
            mine.clear();
        }
        if (!all.empty()) {
            sortNotes(all);
            MidiTrack track;
            track.name = name.empty() ? "MPE" : name;
            track.notes = std::move(all);
            track.mpe = true;
            song.tracks.push_back(std::move(track));
        }
        return;
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
        track.controls = std::move(controls.at(channel));
        song.tracks.push_back(std::move(track));
    }
}

/// Whether `control` is one a General MIDI part keeps.
[[nodiscard]] bool partControl(std::uint8_t control) noexcept {
    return control <= 1 || control == 7 || control == 10 || control == 11 || control == 64;
}

/// One MTrk chunk: its notes per channel, and the meta events the song keeps.
// NOLINTNEXTLINE(readability-function-size) - one status byte's dispatch, read in order.
void parseTrack(ByteReader& r, std::size_t end, std::uint16_t division, MidiSong& song) {
    std::string name;
    std::array<std::vector<MidiNote>, 16> notes;
    std::array<std::array<std::vector<Pending>, 128>, 16> pending;
    std::array<ChannelBend, 16> bends{};
    std::array<std::vector<MidiControl>, 16> controls;
    /// Lyric meta events, (adX tick, text): each names the note that starts there.
    std::vector<std::pair<std::int64_t, std::string>> lyrics;
    /// An MPE zone configured on this track (RPN 6 on channel 1 or 16).
    bool mpe = false;
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
            .push_back(MidiNote{
                .start = started.start,
                .length = std::max<std::int64_t>(stop - started.start, 1),
                .pitch = static_cast<std::uint8_t>(pitch),
                .velocity = started.velocity,
                .bend = notesBend(bends.at(static_cast<std::size_t>(channel)), started.start, stop),
                .lyric = {}});
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
            } else if (type == 0x05) {
                lyrics.emplace_back(
                    at, std::string(reinterpret_cast<const char*>(bytes.data() + data), length));
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
            } else if (kind == 0xE0 || kind == 0xB0) {
                bendMessage(bends, mpe, channel, kind, d1, d2, toAdx(tick, division));
                if (kind == 0xB0 && partControl(d1)) {
                    controls.at(static_cast<std::size_t>(channel))
                        .push_back(
                            MidiControl{.at = toAdx(tick, division), .control = d1, .value = d2});
                }
            } else if (kind == 0xC0) {
                controls.at(static_cast<std::size_t>(channel))
                    .push_back(MidiControl{.at = toAdx(tick, division),
                                           .control = MidiControl::kMidiProgram,
                                           .value = d1});
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
    finishTrack(notes, controls, lyrics, mpe, name, song);
}

/// One GM part's parameter as the file moves it: its name, the controller, its reset
/// value, and how a controller value becomes the parameter's.
struct PartLane {
    const char* name;
    std::uint8_t control;
    double reset;
    double (*value)(std::uint8_t);
};

double direct(std::uint8_t v) {
    return v;
}
double panOf(std::uint8_t v) {
    return std::clamp((static_cast<double>(v) - 64.0) / 63.0, -1.0, 1.0);
}

constexpr std::array<PartLane, 6> kPartLanes{{
    {.name = "program", .control = MidiControl::kMidiProgram, .reset = 0.0, .value = direct},
    {.name = "bankMsb", .control = 0, .reset = 0.0, .value = direct},
    {.name = "volume", .control = 7, .reset = 100.0, .value = direct},
    {.name = "expression", .control = 11, .reset = 127.0, .value = direct},
    {.name = "pan", .control = 10, .reset = 0.0, .value = panOf},
    {.name = "modWheel", .control = 1, .reset = 0.0, .value = direct},
}};

/// A part's (tick, value) history for one lane: the value in effect when its first note
/// starts, at tick 0, then every change after it. One point means it never changes.
std::vector<std::pair<std::int64_t, double>> laneOf(const std::vector<MidiControl>& controls,
                                                    const PartLane& lane, std::int64_t first) {
    std::vector<std::pair<std::int64_t, double>> points{{0, lane.reset}};
    for (const MidiControl& c : controls) {
        if (c.control != lane.control) {
            continue;
        }
        const double v = lane.value(c.value);
        if (c.at <= first) {
            points.front().second = v;
        } else if (points.back().first == c.at) {
            points.back().second = v;
        } else if (points.back().second != v) {
            points.emplace_back(c.at, v);
        }
    }
    return points;
}

/// Each note released while the part's sustain pedal (CC64) is down sounds until the
/// pedal lifts - or until the same key is struck again, which a GM player retriggers.
void applySustain(std::vector<project::Note>& notes, const std::vector<MidiControl>& controls,
                  std::int64_t songEnd) {
    std::vector<std::pair<std::int64_t, std::int64_t>> held; // [down, up)
    std::int64_t down = -1;
    for (const MidiControl& c : controls) {
        if (c.control != 64) {
            continue;
        }
        if (c.value >= 64 && down < 0) {
            down = c.at;
        } else if (c.value < 64 && down >= 0) {
            held.emplace_back(down, c.at);
            down = -1;
        }
    }
    if (down >= 0) {
        held.emplace_back(down, songEnd);
    }
    if (held.empty()) {
        return;
    }
    for (project::Note& note : notes) {
        const std::int64_t end = note.end().value;
        for (const auto& [from, to] : held) {
            if (end < from || end >= to) {
                continue;
            }
            std::int64_t stop = to;
            for (const project::Note& other : notes) {
                if (other.pitch == note.pitch && other.start.value > note.start.value &&
                    other.start.value < stop) {
                    stop = other.start.value;
                }
            }
            note.length = core::Ticks{std::max(note.length.value, stop - note.start.value)};
            break;
        }
    }
}

project::ParamValue paramValue(std::string name, double value) {
    project::ParamValue out;
    out.name = std::move(name);
    out.value = value;
    return out;
}

/// The GmParts layout: one 16-part `gm` channel holding every track's notes.
// NOLINTNEXTLINE(readability-function-size) - gather, lay out the parts, add, automate.
void importGmParts(const MidiSong& song, project::Project& project, project::CommandStack& stack,
                   core::PatternId pattern, std::int64_t songEnd) {
    std::array<std::vector<project::Note>, 16> notes;
    std::array<std::vector<project::NoteExtras>, 16> extras;
    std::array<std::vector<MidiControl>, 16> controls;
    for (const MidiTrack& track : song.tracks) {
        const auto part = static_cast<std::uint8_t>(track.mpe ? 0U : (track.channel & 0x0FU));
        for (const MidiNote& note : track.notes) {
            project::NoteExtras extra;
            for (const MidiBend& point : note.bend) {
                extra.pitchCurve.push_back(project::PitchPoint{
                    .at = core::Ticks{point.at}, .cents = point.cents, .curve = {}});
            }
            extra.lyric = note.lyric;
            project::Note value;
            value.start = core::Ticks{note.start};
            value.length = core::Ticks{note.length};
            value.pitch = note.pitch;
            value.velocity = note.velocity;
            value.part = part;
            notes.at(part).push_back(value);
            extras.at(part).push_back(std::move(extra));
        }
        controls.at(part).insert(controls.at(part).end(), track.controls.begin(),
                                 track.controls.end());
    }

    project::Channel channel;
    channel.name = "GM";
    channel.output = project.mixer.master;
    channel.maxPolyphony = 64;
    channel.instrument.type = "gm";
    channel.instrument.params.push_back(paramValue("part", 1.0));
    // Lanes that change are drawn once the channel exists, so their paths resolve.
    std::vector<std::pair<std::string, std::vector<std::pair<std::int64_t, double>>>> moving;
    for (std::size_t part = 0; part < 16; ++part) {
        if (notes.at(part).empty()) {
            continue;
        }
        std::ranges::stable_sort(controls.at(part), {}, &MidiControl::at);
        std::int64_t first = std::numeric_limits<std::int64_t>::max();
        for (const project::Note& note : notes.at(part)) {
            first = std::min(first, note.start.value);
        }
        const std::string prefix = "p" + std::to_string(part + 1) + ".";
        for (const PartLane& lane : kPartLanes) {
            auto points = laneOf(controls.at(part), lane, first);
            if (points.size() > 1) {
                // Listed with its first value, so the lane's path resolves to it.
                channel.instrument.params.push_back(
                    paramValue(prefix + lane.name, points.front().second));
                moving.emplace_back(prefix + lane.name, std::move(points));
            } else if (points.front().second != lane.reset) {
                channel.instrument.params.push_back(
                    paramValue(prefix + lane.name, points.front().second));
            }
        }
        applySustain(notes.at(part), controls.at(part), songEnd);
    }
    auto add = std::make_unique<project::AddChannel>(channel);
    const project::AddChannel* raw = add.get();
    stack.execute(std::move(add), project);
    const core::ChannelId id = raw->created();

    std::vector<project::Note> all;
    std::vector<project::NoteExtrasAt> allExtras;
    for (std::size_t part = 0; part < 16; ++part) {
        for (std::size_t i = 0; i < notes.at(part).size(); ++i) {
            if (!extras.at(part).at(i).empty()) {
                allExtras.push_back(
                    project::NoteExtrasAt{.index = all.size(), .extras = extras.at(part).at(i)});
            }
            all.push_back(notes.at(part).at(i));
        }
    }
    stack.execute(
        std::make_unique<project::AddNotes>(pattern, id, std::move(all), std::move(allExtras)),
        project);

    const project::Channel* made = project.find(id);
    for (const auto& [param, points] : moving) {
        project::AutomationClip clip;
        clip.targetPath = "channel." + (made == nullptr ? channel.name : made->name) + "." + param;
        const project::ParamResolution resolved =
            project::ParamRegistry::resolve(clip.targetPath, project);
        if (resolved.ok()) {
            clip.target = resolved.ref;
        }
        core::Curve step;
        step.kind = core::CurveKind::Step;
        for (const auto& [at, value] : points) {
            clip.points.push_back(project::Breakpoint{
                .at = core::Ticks{at}, .value = static_cast<float>(value), .curve = step});
        }
        stack.execute(std::make_unique<project::AddAutomationClip>(pattern, std::move(clip)),
                      project);
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
                       project::CommandStack& stack, const std::string& patternName,
                       ImportLayout layout) {
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

    if (layout == ImportLayout::GmParts) {
        importGmParts(song, project, stack, patternRaw->created(), prototype.length.value);
    }
    for (const MidiTrack& track : song.tracks) {
        if (layout == ImportLayout::GmParts) {
            break;
        }
        project::Channel channel;
        channel.name = track.name;
        channel.output = project.mixer.master;
        auto add = std::make_unique<project::AddChannel>(channel);
        const project::AddChannel* raw = add.get();
        stack.execute(std::move(add), project);
        std::vector<project::Note> notes;
        std::vector<project::NoteExtrasAt> extras;
        notes.reserve(track.notes.size());
        for (const MidiNote& note : track.notes) {
            if (!note.bend.empty() || !note.lyric.empty()) {
                project::NoteExtras bent;
                for (const MidiBend& point : note.bend) {
                    bent.pitchCurve.push_back(project::PitchPoint{
                        .at = core::Ticks{point.at}, .cents = point.cents, .curve = {}});
                }
                bent.lyric = note.lyric;
                extras.push_back(project::NoteExtrasAt{.index = notes.size(), .extras = bent});
            }
            project::Note value;
            value.start = core::Ticks{note.start};
            value.length = core::Ticks{note.length};
            value.pitch = note.pitch;
            value.velocity = note.velocity;
            notes.push_back(value);
        }
        stack.execute(std::make_unique<project::AddNotes>(patternRaw->created(), raw->created(),
                                                          std::move(notes), std::move(extras)),
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
