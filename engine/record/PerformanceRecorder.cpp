// adx-thread: main
#include "engine/record/PerformanceRecorder.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/PatternCommands.h"
#include "engine/project/commands/PlaylistCommands.h"
#include "engine/project/commands/ProjectCommands.h"
#include "engine/project/commands/SessionCommands.h"

namespace adx::record {

namespace {

using session::PerformanceEvent;

struct Pass {
    std::uint32_t row{0};
    std::int64_t tick{0};
    std::int64_t phase{0};
    std::int64_t length{0};
    float gain{1.0F};
};

struct Placed {
    std::uint32_t track{0};
    std::uint32_t row{0};
    std::int64_t start{0};
    std::int64_t length{0};
    std::int64_t offset{0};
    float gain{1.0F};
};

/// Every pass as a placement: from its tick to the next event on its track (a stop, or
/// the next pass), never past its own end.
std::vector<Placed> placementsOf(std::span<const PerformanceEvent> log, std::int64_t end) {
    std::vector<PerformanceEvent> events(log.begin(), log.end());
    std::ranges::stable_sort(events, {}, &PerformanceEvent::tick);
    std::map<std::uint32_t, Pass> open;
    std::vector<Placed> out;
    const auto close = [&](std::uint32_t track, std::int64_t at) {
        const auto found = open.find(track);
        if (found == open.end()) {
            return;
        }
        const Pass& pass = found->second;
        const std::int64_t full = pass.length - pass.phase;
        const std::int64_t length = at < 0 ? full : std::min(at - pass.tick, full);
        if (length > 0) {
            out.push_back(Placed{.track = track,
                                 .row = pass.row,
                                 .start = pass.tick,
                                 .length = length,
                                 .offset = pass.phase,
                                 .gain = pass.gain});
        }
        open.erase(found);
    };
    for (const PerformanceEvent& event : events) {
        if (event.kind == PerformanceEvent::Kind::Stop) {
            close(event.track, event.tick);
        } else if (event.kind == PerformanceEvent::Kind::Pass) {
            close(event.track, event.tick);
            open[event.track] = Pass{.row = event.row,
                                     .tick = event.tick,
                                     .phase = event.phase,
                                     .length = event.length,
                                     .gain = event.gain};
        }
    }
    std::vector<std::uint32_t> still;
    still.reserve(open.size());
    for (const auto& [track, pass] : open) {
        still.push_back(track);
    }
    for (const std::uint32_t track : still) {
        close(track, end);
    }
    std::ranges::stable_sort(out, {}, &Placed::start);
    return out;
}

/// The tempo events the rate changes make: from each change on, every tempo in the map
/// times that rate. The map's own events inside a changed stretch are rewritten too.
std::vector<core::TempoEvent> tempoOf(std::span<const PerformanceEvent> log,
                                      const core::TempoMap& map) {
    std::vector<std::pair<std::int64_t, double>> rates;
    for (const PerformanceEvent& event : log) {
        if (event.kind == PerformanceEvent::Kind::Rate) {
            rates.emplace_back(event.tick, event.rate);
        }
    }
    if (rates.empty()) {
        return {};
    }
    std::ranges::stable_sort(rates, {}, &std::pair<std::int64_t, double>::first);
    const auto rateAt = [&](std::int64_t tick) {
        double rate = 1.0;
        for (const auto& [at, value] : rates) {
            if (at > tick) {
                break;
            }
            rate = value;
        }
        return rate;
    };
    std::vector<core::TempoEvent> out;
    for (const auto& [at, rate] : rates) {
        const core::Ticks tick{std::max<std::int64_t>(at, 0)};
        const core::TempoView view = map.view();
        const bool ramp = view.events[view.segmentAt(tick)].ramp;
        out.push_back(core::TempoEvent{.at = tick, .bpm = map.bpmAt(tick) * rate, .ramp = ramp});
    }
    for (const core::TempoEvent& event : map.view().events) {
        const double rate = rateAt(event.at.value);
        if (rate != 1.0) {
            out.push_back(
                core::TempoEvent{.at = event.at, .bpm = event.bpm * rate, .ramp = event.ramp});
        }
    }
    std::ranges::stable_sort(out, {}, [](const core::TempoEvent& e) { return e.at.value; });
    // One per tick: the rate change wins over the map's own event at the same tick.
    std::vector<core::TempoEvent> unique;
    for (const core::TempoEvent& event : out) {
        if (unique.empty() || unique.back().at != event.at) {
            unique.push_back(event);
        }
    }
    for (core::TempoEvent& event : unique) {
        event.bpm = std::clamp(event.bpm, core::kMinBpm, core::kMaxBpm);
    }
    return unique;
}

/// Whether `pattern` holds anything for a channel other than `channel`.
bool sharedPattern(const project::Pattern& pattern, core::ChannelId channel) {
    const bool notes = std::ranges::any_of(
        pattern.noteClips, [&](const project::NoteClip& c) { return c.channel != channel; });
    const bool mini = std::ranges::any_of(
        pattern.mini, [&](const project::MiniNotationSource& m) { return m.channel != channel; });
    return notes || mini || !pattern.autoClips.empty();
}

std::string freeName(const project::Project& project, const std::string& base) {
    if (project.findPatternByName(base) == nullptr) {
        return base;
    }
    for (int n = 2;; ++n) {
        std::string name = base + " " + std::to_string(n);
        if (project.findPatternByName(name) == nullptr) {
            return name;
        }
    }
}

} // namespace

PerformanceSummary PerformanceRecorder::commit(project::Project& project,
                                               project::CommandStack& stack,
                                               std::int64_t end) const {
    PerformanceSummary summary;
    const std::vector<Placed> placed = placementsOf(m_events, end);
    const std::vector<core::TempoEvent> tempo = tempoOf(m_events, project.tempo);
    if (placed.empty() && tempo.empty()) {
        return summary;
    }
    stack.beginGroup("Record performance");

    // The linked playlist tracks: made, and the session told of them, where missing.
    project::SessionModel session = project.session;
    bool relinked = false;
    std::map<std::uint32_t, core::PlaylistTrackId> rows;
    for (const Placed& p : placed) {
        if (rows.contains(p.track) || p.track >= session.tracks.size()) {
            continue;
        }
        project::SessionTrack& track = session.tracks[p.track];
        const bool exists = std::ranges::any_of(
            project.playlist.tracks, [&](const auto& t) { return t.id == track.linked; });
        if (!exists) {
            project::PlaylistTrack prototype;
            const project::Channel* channel = project.find(track.channel);
            prototype.name = channel != nullptr ? channel->name : "Performance";
            auto add = std::make_unique<project::AddPlaylistTrack>(prototype);
            const project::AddPlaylistTrack* raw = add.get();
            stack.execute(std::move(add), project);
            track.linked = raw->created();
            relinked = true;
            ++summary.tracksMade;
        }
        rows[p.track] = track.linked;
    }
    if (relinked) {
        stack.execute(std::make_unique<project::SetSession>(session), project);
    }

    // The pattern each cell's items place: its own, or its channel's share of it.
    std::map<std::pair<std::uint32_t, std::uint32_t>, core::PatternId> own;
    std::map<std::pair<std::uint32_t, std::uint32_t>, core::PatternId> made;
    for (const Placed& p : placed) {
        const auto key = std::pair{p.track, p.row};
        if (own.contains(key)) {
            continue;
        }
        const project::ClipSlot* slot = session.slot(p.track, p.row);
        const project::Pattern* pattern = slot != nullptr ? project.find(slot->pattern) : nullptr;
        if (pattern == nullptr) {
            own[key] = core::PatternId{};
            continue;
        }
        const core::ChannelId channel = session.tracks[p.track].channel;
        if (!sharedPattern(*pattern, channel)) {
            own[key] = pattern->id;
            continue;
        }
        const auto share = std::pair{pattern->id.value, channel.value};
        if (const auto found = made.find(share); found != made.end()) {
            own[key] = found->second;
            continue;
        }
        project::Pattern prototype;
        const project::Channel* named = project.find(channel);
        prototype.name =
            freeName(project, pattern->name + " " + (named != nullptr ? named->name : "clip"));
        prototype.color = pattern->color;
        prototype.length = pattern->length;
        if (const project::NoteClip* clip = pattern->clipFor(channel)) {
            prototype.noteClips.push_back(*clip);
        }
        for (const project::MiniNotationSource& source : pattern->mini) {
            if (source.channel == channel) {
                prototype.mini.push_back(source);
            }
        }
        auto add = std::make_unique<project::AddPattern>(std::move(prototype));
        const project::AddPattern* raw = add.get();
        stack.execute(std::move(add), project);
        made[share] = raw->created();
        own[key] = raw->created();
        ++summary.patternsMade;
    }

    for (const Placed& p : placed) {
        const core::PatternId pattern = own[std::pair{p.track, p.row}];
        const auto row = rows.find(p.track);
        if (!pattern.valid() || row == rows.end()) {
            continue;
        }
        project::PlaylistItem item;
        item.start = core::Ticks{p.start};
        item.length = core::Ticks{p.length};
        item.sourceOffset = core::Ticks{p.offset};
        item.content = project::PatternRef{.pattern = pattern, .gain = p.gain};
        stack.execute(std::make_unique<project::AddPlaylistItem>(row->second, item), project);
        ++summary.items;
    }
    for (const core::TempoEvent& event : tempo) {
        stack.execute(std::make_unique<project::SetTempoEvent>(event.at, event.bpm, event.ramp),
                      project);
        ++summary.tempoEvents;
    }
    stack.endGroup();
    return summary;
}

} // namespace adx::record
