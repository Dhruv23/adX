#include "engine/project/SnapshotBuilder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <variant>

#include "engine/core/Config.h"
#include "engine/project/EventCompile.h"
#include "engine/project/Project.h"

namespace adx::project {

struct SnapshotBuilder::TempoBlock {
    std::vector<core::TempoEvent> events;
    std::vector<double> cumSeconds;
};

struct SnapshotBuilder::ChannelEvents {
    std::vector<ScheduledEvent> events;
    /// The text the Lyric events index, and views of it for the snapshot. Two vectors
    /// because a view into a string the vector might still move would dangle.
    std::vector<std::string> lyricText;
    std::vector<std::string_view> lyrics;
};

namespace {

/// Everything one snapshot's spans point into. What `Snapshot::lifetime` destroys.
///
/// The shared parts are held as shared_ptr<const void>: the storage does not need to
/// know what they are, only to keep them alive until the audio thread is done.
std::atomic<std::int64_t>& liveCounter() noexcept {
    static std::atomic<std::int64_t> live{0};
    return live;
}

struct SnapshotStorage {
    SnapshotStorage() noexcept {
        liveCounter().fetch_add(1, std::memory_order_relaxed);
    }
    ~SnapshotStorage() {
        liveCounter().fetch_sub(1, std::memory_order_relaxed);
    }
    SnapshotStorage(const SnapshotStorage&) = delete;
    SnapshotStorage& operator=(const SnapshotStorage&) = delete;
    SnapshotStorage(SnapshotStorage&&) = delete;
    SnapshotStorage& operator=(SnapshotStorage&&) = delete;

    Snapshot snapshot;
    std::vector<std::shared_ptr<const void>> keepAlive;
    std::shared_ptr<graph::CompiledGraph> graph;
    std::vector<EventTrack> tracks;
    std::vector<EventCursor> cursors;
    std::vector<float> params;
    std::vector<float> paramBase;
    std::vector<AutomationLane> lanes;
    std::vector<AutomationPoint> points;
    std::vector<Knot> knots;
    std::vector<std::uint32_t> paramLaneStart;
    std::vector<std::uint32_t> paramLanes;
};

/// The window a placement plays from its source, in the source's own ticks.
struct Window {
    std::int64_t localStart{0};
    std::int64_t localEnd{0};
    std::int64_t placedAt{0};

    /// Source-local tick to arrangement tick.
    [[nodiscard]] std::int64_t place(std::int64_t local) const noexcept {
        return placedAt + (local - localStart);
    }
};

Window windowOf(const PlaylistItem& item, core::Ticks naturalLength) {
    const std::int64_t length = item.length.value > 0 ? item.length.value : naturalLength.value;
    return Window{.localStart = item.sourceOffset.value,
                  .localEnd = item.sourceOffset.value + std::max<std::int64_t>(length, 0),
                  .placedAt = item.start.value};
}

std::uint32_t paramIndexOf(const graph::ParamLayout& layout, ParamRef ref) {
    const auto position = std::ranges::lower_bound(
        layout.index, ref, [](const ParamRef& a, const ParamRef& b) { return paramRefLess(a, b); },
        &ParamSlot::ref);
    return position != layout.index.end() && position->ref == ref ? position->index : graph::kNone;
}

/// The tolerance a lane on `ref` is compiled to: 0.1 % of the parameter's range
/// (phase_4.md §4.0), or of the lane's own span when the parameter has no descriptor.
float toleranceFor(const Project& project, ParamRef ref, const std::vector<Breakpoint>& points) {
    const ParamDescriptor* descriptor = ParamRegistry::descriptorFor(ref, project);
    if (descriptor != nullptr && descriptor->maximum > descriptor->minimum) {
        return automationTolerance(descriptor->minimum, descriptor->maximum);
    }
    float low = points.front().value;
    float high = low;
    for (const Breakpoint& point : points) {
        low = std::min(low, point.value);
        high = std::max(high, point.value);
    }
    return automationTolerance(low, high);
}

/// Where one lane's points and knots sit in the storage vectors, until every lane is
/// in and spans into them can no longer dangle.
struct LaneRange {
    std::size_t points{0};
    std::size_t pointCount{0};
    std::size_t knots{0};
    std::size_t knotCount{0};
};

/// Appends one lane's points, its compiled knots and the lane itself. `endTick` is
/// where the lane stops applying; `lastTick` is where its points are cut, for a clip
/// envelope trimmed by its item (phase_4.md §4.0: trimming the tail drops points).
bool addLane(const Project& project, ParamRef target, const std::vector<Breakpoint>& points,
             const Window& window, std::int64_t endTick, std::int64_t lastTick,
             const graph::ParamLayout& layout, SnapshotStorage& storage,
             std::vector<LaneRange>& ranges) {
    if (!target.valid() || points.empty()) {
        return false;
    }
    const std::uint32_t index = paramIndexOf(layout, target);
    if (index == graph::kNone) {
        return false;
    }
    LaneRange range{.points = storage.points.size(), .knots = storage.knots.size()};
    for (const Breakpoint& point : points) {
        const std::int64_t tick = window.place(point.at.value);
        if (tick > lastTick) {
            break;
        }
        storage.points.push_back(
            AutomationPoint{.tick = tick, .value = point.value, .curve = point.curve});
    }
    range.pointCount = storage.points.size() - range.points;
    if (range.pointCount == 0) {
        return false;
    }
    compileBreakpoints(
        storage.knots,
        std::span<const AutomationPoint>{storage.points}.subspan(range.points, range.pointCount),
        toleranceFor(project, target, points));
    range.knotCount = storage.knots.size() - range.knots;
    storage.lanes.push_back(AutomationLane{.paramIndex = index,
                                           .startTick = window.placedAt,
                                           .endTick = endTick,
                                           .points = {},
                                           .knots = {}});
    ranges.push_back(range);
    return true;
}

void buildAutomation(const Project& project, const graph::ParamLayout& layout,
                     SnapshotStorage& storage) {
    // Two passes: collect every lane's points into one vector, then point each lane at
    // its slice - a span taken during the first pass would dangle the moment the
    // vector grew.
    std::vector<LaneRange> ranges;
    constexpr std::int64_t kForever = std::numeric_limits<std::int64_t>::max();
    const auto add = [&](const AutomationClip& clip, const Window& window, bool bounded) {
        // An unbounded placement holds its last value, the way a lane with no end does
        // in every DAW.
        const std::int64_t end = bounded ? window.place(window.localEnd) : kForever;
        static_cast<void>(addLane(project, clip.target, clip.points, window, end, kForever, layout,
                                  storage, ranges));
    };
    // Clip envelopes go last, so where one overlaps a pattern or playlist lane on the
    // same parameter the most specific automation wins.
    std::vector<std::pair<const PlaylistItem*, Window>> enveloped;

    for (const PlaylistTrack& track : project.playlist.tracks) {
        if (track.muted) {
            continue;
        }
        for (const PlaylistItem& item : track.items) {
            if (item.muted) {
                continue;
            }
            if (const auto* ref = std::get_if<PatternRef>(&item.content)) {
                const Pattern* pattern = project.find(ref->pattern);
                if (pattern == nullptr) {
                    continue;
                }
                const Window window = windowOf(item, pattern->length);
                for (const AutomationClip& clip : pattern->autoClips) {
                    add(clip, window, true);
                }
                if (!item.envelopes.empty()) {
                    enveloped.emplace_back(&item, window);
                }
            } else if (const auto* autoRef = std::get_if<AutomationRef>(&item.content)) {
                const AutomationClip* clip = project.findAutomationClip(autoRef->clip);
                if (clip != nullptr) {
                    add(*clip, windowOf(item, core::Ticks{0}), item.length.value > 0);
                }
            }
        }
    }
    for (const auto& [item, placed] : enveloped) {
        // Envelope times are item-relative: 0 is item.start, whatever the source offset.
        const Window window{.localStart = 0,
                            .localEnd = placed.localEnd - placed.localStart,
                            .placedAt = placed.placedAt};
        const std::int64_t end = window.place(window.localEnd);
        for (const ClipEnvelope& envelope : item->envelopes) {
            if (envelope.local == ClipTarget::Param) {
                static_cast<void>(addLane(project, envelope.target, envelope.points, window, end,
                                          end, layout, storage, ranges));
            }
        }
    }

    for (std::size_t i = 0; i < storage.lanes.size(); ++i) {
        storage.lanes[i].points = std::span<const AutomationPoint>{storage.points}.subspan(
            ranges[i].points, ranges[i].pointCount);
        storage.lanes[i].knots =
            std::span<const Knot>{storage.knots}.subspan(ranges[i].knots, ranges[i].knotCount);
    }

    // The parameter -> lanes index, compressed. Lanes keep their order within a
    // parameter, which is their priority.
    if (storage.lanes.empty()) {
        return;
    }
    storage.paramLaneStart.assign(storage.params.size() + 1, 0);
    for (const AutomationLane& lane : storage.lanes) {
        ++storage.paramLaneStart[lane.paramIndex + 1];
    }
    for (std::size_t p = 1; p < storage.paramLaneStart.size(); ++p) {
        storage.paramLaneStart[p] += storage.paramLaneStart[p - 1];
    }
    storage.paramLanes.assign(storage.lanes.size(), 0);
    std::vector<std::uint32_t> fill(storage.paramLaneStart.begin(),
                                    storage.paramLaneStart.end() - 1);
    for (std::size_t l = 0; l < storage.lanes.size(); ++l) {
        storage.paramLanes[fill[storage.lanes[l].paramIndex]++] = static_cast<std::uint32_t>(l);
    }
}

} // namespace

std::int64_t liveSnapshotCount() noexcept {
    return liveCounter().load(std::memory_order_relaxed);
}

void destroySnapshot(Snapshot* snapshot) noexcept {
    if (snapshot != nullptr && snapshot->lifetime.destroy != nullptr) {
        snapshot->lifetime.destroy(snapshot->lifetime.ptr);
    }
}

SnapshotBuilder::SnapshotBuilder(std::uint32_t sampleRate, std::uint32_t maxBlockFrames)
    : m_sampleRate(sampleRate),
      m_nodes(graph::PrepareInfo{.sampleRate = sampleRate, .maxBlockFrames = maxBlockFrames}) {}

SnapshotBuilder::~SnapshotBuilder() = default;

void SnapshotBuilder::forgetCaches() {
    m_tempo.reset();
    m_graphBuild.reset();
    m_graph.reset();
    m_clips.clear();
    m_channels.clear();
    m_primed = false;
}

std::uint32_t SnapshotBuilder::totalLatency() const noexcept {
    return m_graph ? m_graph->view().totalLatency : 0;
}

void SnapshotBuilder::refreshClipCache(const Project& project) {
    for (auto& entry : m_clips) {
        entry.second.seen = false;
    }
    for (const Pattern& pattern : project.patterns) {
        for (const NoteClip& clip : pattern.noteClips) {
            ClipCacheEntry& entry = m_clips[{pattern.id.value, clip.channel.value}];
            // A compare, not a hash: equal-length vectors of trivially comparable
            // notes, which is memcmp-speed for the untouched clips that are nearly all
            // of them - and exact, where a hash would only be probable.
            if (entry.version == 0 || entry.notes != clip.notes || entry.extras != clip.extras) {
                entry.notes = clip.notes;
                entry.extras = clip.extras;
                entry.version = ++m_clipVersion;
            }
            entry.seen = true;
        }
    }
    std::erase_if(m_clips, [](const auto& item) { return !item.second.seen; });
}

SnapshotBuilder::PlacementsByChannel
SnapshotBuilder::collectPlacements(const Project& project) const {
    PlacementsByChannel byChannel;
    for (const PlaylistTrack& track : project.playlist.tracks) {
        if (track.muted) {
            continue;
        }
        for (const PlaylistItem& item : track.items) {
            const auto* ref = std::get_if<PatternRef>(&item.content);
            if (item.muted || ref == nullptr) {
                continue;
            }
            const Pattern* pattern = project.find(ref->pattern);
            if (pattern == nullptr) {
                continue;
            }
            for (const NoteClip& clip : pattern->noteClips) {
                const auto cached = m_clips.find({pattern->id.value, clip.channel.value});
                const std::uint64_t version = cached != m_clips.end() ? cached->second.version : 0;
                byChannel[clip.channel.value].push_back(Placement{
                    .item = &item, .pattern = pattern, .clip = &clip, .clipVersion = version});
            }
        }
    }
    return byChannel;
}

std::vector<std::int64_t> SnapshotBuilder::signatureOf(const std::vector<Placement>& placements,
                                                       const Channel* channel) {
    std::vector<std::int64_t> signature;
    signature.reserve((placements.size() * 6) + 5);
    // The arpeggiator rewrites the events, so its settings are part of what they are a
    // function of.
    if (channel != nullptr && channel->arp.mode != ArpMode::Off) {
        signature.insert(
            signature.end(),
            {static_cast<std::int64_t>(channel->arp.mode), channel->arp.rate.numerator,
             channel->arp.rate.denominator, channel->arp.octaves,
             static_cast<std::int64_t>(std::bit_cast<std::uint32_t>(channel->arp.gate))});
    }
    for (const Placement& placement : placements) {
        const PlaylistItem& item = *placement.item;
        signature.insert(signature.end(), {static_cast<std::int64_t>(item.id.value),
                                           item.start.value, item.length.value,
                                           item.sourceOffset.value, placement.pattern->length.value,
                                           static_cast<std::int64_t>(placement.clipVersion)});
    }
    return signature;
}

std::shared_ptr<const SnapshotBuilder::ChannelEvents>
SnapshotBuilder::flatten(const std::vector<Placement>& placements, const Channel* channel) {
    auto events = std::make_shared<ChannelEvents>();
    for (const Placement& placement : placements) {
        const PlaylistItem& item = *placement.item;
        const Window window = windowOf(item, placement.pattern->length);
        for (const Note& note : placement.clip->notes) {
            // A placement plays the notes that *start* inside its window, and cuts any
            // that run past its end - trimming a placement shortens its notes.
            if (note.muted || note.start.value < window.localStart ||
                note.start.value >= window.localEnd) {
                continue;
            }
            const std::int64_t localEnd = std::min(note.end().value, window.localEnd);
            if (localEnd <= note.start.value) {
                continue;
            }
            const std::int64_t on = window.place(note.start.value);
            const std::int64_t off = window.place(localEnd);
            events->events.push_back(ScheduledEvent{.tick = on,
                                                    .endTick = off,
                                                    .noteId = note.id.value,
                                                    .instance = item.id.value,
                                                    .kind = EventKind::NoteOn,
                                                    .pitch = note.pitch,
                                                    .velocity = note.velocity});
            events->events.push_back(ScheduledEvent{.tick = off,
                                                    .endTick = off,
                                                    .noteId = note.id.value,
                                                    .instance = item.id.value,
                                                    .kind = EventKind::NoteOff,
                                                    .pitch = note.pitch,
                                                    .velocity = 0});
            if (const NoteExtras* extras = placement.clip->extrasFor(note.id)) {
                appendNoteExtras(events->events, events->lyricText, *extras, on, off, note.id.value,
                                 item.id.value);
            }
        }
    }
    std::ranges::sort(events->events, eventLess);
    if (channel != nullptr) {
        arpeggiate(events->events, channel->arp);
    }
    events->lyrics.assign(events->lyricText.begin(), events->lyricText.end());
    return events;
}

// NOLINTNEXTLINE(readability-function-size)
SnapshotBuildResult SnapshotBuilder::build(const Project& project, std::uint64_t revision,
                                           DirtyMask dirty) {
    const auto startedAt = std::chrono::steady_clock::now();
    m_stats = SnapshotBuildStats{};
    if (!m_primed) {
        dirty = dirty::kAll;
    }
    const std::uint64_t createdBefore = m_nodes.created();

    // --- tempo ---
    if (!m_tempo || (dirty & dirty::kTempo) != 0) {
        auto block = std::make_shared<TempoBlock>();
        const core::TempoView view = project.tempo.view();
        block->events.assign(view.events.begin(), view.events.end());
        block->cumSeconds.assign(view.cumSeconds.begin(), view.cumSeconds.end());
        m_tempo = std::move(block);
    } else {
        m_stats.tempoReused = true;
    }

    // --- graph ---
    if (!m_graphBuild || (dirty & (dirty::kChannels | dirty::kMixer | dirty::kRouting)) != 0) {
        auto candidate = std::make_shared<graph::GraphBuild>(graph::buildGraph(project, m_nodes));
        if (!candidate->ok) {
            return SnapshotBuildResult{.snapshot = nullptr, .error = candidate->error};
        }
        if (m_graphBuild && m_graph && candidate->sameStructure(*m_graphBuild)) {
            m_stats.graphReused = true;
        } else {
            m_graph = std::make_shared<graph::CompiledGraph>(candidate->graph, candidate->order,
                                                             candidate->pdc);
            m_graphBuild = std::move(candidate);
        }
        m_nodes.pruneUnused();
    } else {
        m_stats.graphReused = true;
    }

    // --- events ---
    const bool eventsDirty =
        (dirty & (dirty::kPatterns | dirty::kPlaylist | dirty::kChannels)) != 0;
    if (eventsDirty) {
        refreshClipCache(project);
        const PlacementsByChannel placements = collectPlacements(project);
        static const std::vector<Placement> kNone;
        for (auto& entry : m_channels) {
            entry.second.seen = false;
        }
        for (const core::ChannelId channel : m_graphBuild->trackChannels) {
            ChannelCacheEntry& entry = m_channels[channel.value];
            const auto found = placements.find(channel.value);
            const std::vector<Placement>& mine = found != placements.end() ? found->second : kNone;
            const Channel* model = project.find(channel);
            std::vector<std::int64_t> signature = signatureOf(mine, model);
            if (entry.events && entry.signature == signature) {
                ++m_stats.tracksReused;
            } else {
                entry.events = flatten(mine, model);
                entry.signature = std::move(signature);
                ++m_stats.tracksRebuilt;
            }
            entry.seen = true;
        }
        std::erase_if(m_channels, [](const auto& item) { return !item.second.seen; });
    } else {
        m_stats.tracksReused = static_cast<std::uint32_t>(m_graphBuild->trackChannels.size());
    }

    // --- assemble ---
    auto storage = std::make_unique<SnapshotStorage>();
    storage->keepAlive.push_back(m_tempo);
    storage->keepAlive.push_back(m_graphBuild);
    storage->graph = m_graph;

    static const ChannelEvents kNoEvents{};
    storage->tracks.reserve(m_graphBuild->trackChannels.size());
    for (const core::ChannelId channel : m_graphBuild->trackChannels) {
        const auto found = m_channels.find(channel.value);
        if (found != m_channels.end() && found->second.events) {
            storage->keepAlive.push_back(found->second.events);
            storage->tracks.push_back(EventTrack{.events = found->second.events->events,
                                                 .lyrics = found->second.events->lyrics});
        } else {
            storage->tracks.push_back(EventTrack{.events = kNoEvents.events, .lyrics = {}});
        }
    }
    storage->cursors.assign(storage->tracks.size(), EventCursor{});
    graph::fillParams(project, m_graphBuild->params, storage->params);
    storage->paramBase = storage->params;
    buildAutomation(project, m_graphBuild->params, *storage);

    Snapshot& snapshot = storage->snapshot;
    snapshot.revision = revision;
    snapshot.sampleRate = m_sampleRate;
    snapshot.tempo = core::TempoView{.events = m_tempo->events, .cumSeconds = m_tempo->cumSeconds};
    snapshot.eventTracks = storage->tracks;
    snapshot.cursors = storage->cursors;
    snapshot.automation = storage->lanes;
    snapshot.graph = m_graph->view();
    snapshot.params = storage->params;
    snapshot.paramBase = storage->paramBase;
    snapshot.paramIndex = m_graphBuild->params.index;
    snapshot.paramLaneStart = storage->paramLaneStart;
    snapshot.paramLanes = storage->paramLanes;

    SnapshotStorage* owned = storage.release();
    owned->snapshot.lifetime = rt::retireOf(owned);

    m_primed = true;
    m_stats.nodesCreated = static_cast<std::uint32_t>(m_nodes.created() - createdBefore);
    m_stats.milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startedAt)
            .count();
    return SnapshotBuildResult{.snapshot = &owned->snapshot, .error = {}};
}

} // namespace adx::project
