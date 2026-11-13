#include "engine/project/SnapshotBuilder.h"
#include "engine/midi/DeviceNames.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <variant>

#include "engine/clip/ClipRenderCache.h"
#include "engine/core/Config.h"
#include "engine/dsp/Math.h"
#include "engine/format/audio/SamplePool.h"
#include "engine/format/mininotation/PatternCompiler.h"
#include "engine/project/ControllerCompile.h"
#include "engine/project/EventCompile.h"
#include "engine/project/MacroCompile.h"
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
    std::vector<std::uint32_t> trackChannels;
    std::vector<float> params;
    std::vector<float> paramBase;
    std::vector<AutomationLane> lanes;
    std::vector<AutomationPoint> points;
    std::vector<Knot> knots;
    std::vector<std::uint32_t> paramLaneStart;
    std::vector<std::uint32_t> paramLanes;
    std::vector<midi::MidiRoute> midiRoutes;
    std::vector<midi::CompiledBinding> midiBindings;
    clip::CompiledAudio audio;
    std::vector<SessionClip> sessionClips;
    std::vector<double> sceneTempo;
    std::vector<std::uint8_t> trackStopOnEmpty;
    std::vector<std::uint32_t> sessionChannels;
};

/// The project's [MIDI] section as the audio thread reads it.
void compileMidi(const Project& project, SnapshotStorage& storage) {
    for (const MidiRouteSetting& route : project.midi.routes) {
        storage.midiRoutes.push_back(midi::MidiRoute{.device = midi::deviceSlot(route.device),
                                                     .channels = route.channels,
                                                     .keyLow = route.keyLow,
                                                     .keyHigh = route.keyHigh,
                                                     .velocityLow = route.velocityLow,
                                                     .velocityHigh = route.velocityHigh,
                                                     .transpose = route.transpose,
                                                     .curve = route.curve,
                                                     .target = route.target.value});
    }
    for (const MidiBinding& binding : project.midi.bindings) {
        if (!binding.target.valid()) {
            continue; // kept in the file, inert until its target exists again
        }
        storage.midiBindings.push_back(
            midi::CompiledBinding{.device = midi::deviceSlot(binding.device),
                                  .channel = binding.channel,
                                  .kind = binding.kind,
                                  .number = binding.number,
                                  .target = binding.target,
                                  .minimum = binding.minimum,
                                  .maximum = binding.maximum,
                                  .toggle = binding.toggle});
    }
}

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

/// Appends one placement's mini-notation notes. The cycles are the song's, not the
/// pattern's: they are realised over the arrangement range the placement covers, so a
/// pattern placed at bar 9 plays cycle 8 - the same cycle in an export as in playback.
/// Voice ids sit above kMiniNoteIdBit and are (cycle, index): stable across an edit
/// that does not change this source, so such an edit never restarts its voices.
void appendMiniEvents(std::vector<ScheduledEvent>& out, const MiniNotationSource& source,
                      const Window& window, std::uint32_t instance, core::Ticks cycle, float gain) {
    const std::int64_t from = window.placedAt;
    const std::int64_t to = window.place(window.localEnd);
    const std::string text = mini::joinLines(source.lines);
    for (const mini::RealizedNote& realized :
         mini::realize(text, core::Ticks{from}, core::Ticks{to}, cycle)) {
        const std::int64_t on = realized.note.start.value;
        const std::int64_t off = std::min(on + realized.note.length.value, to);
        if (off <= on) {
            continue;
        }
        const std::uint32_t noteId =
            kMiniNoteIdBit | ((static_cast<std::uint32_t>(realized.cycle) & 0x3FFFU) << 16U) |
            (realized.index & 0xFFFFU);
        out.push_back(ScheduledEvent{.tick = on,
                                     .endTick = off,
                                     .noteId = noteId,
                                     .instance = instance,
                                     .kind = EventKind::NoteOn,
                                     .pitch = realized.note.pitch,
                                     .velocity = scaledVelocity(realized.note.velocity, gain)});
        out.push_back(ScheduledEvent{.tick = off,
                                     .endTick = off,
                                     .noteId = noteId,
                                     .instance = instance,
                                     .kind = EventKind::NoteOff,
                                     .pitch = realized.note.pitch,
                                     .velocity = 0});
    }
}

/// The project's A4 as a pitch offset from 440 Hz, in cents (P4-3). Exactly zero at
/// 440, so every project at standard pitch renders bit for bit as before.
float tuningCents(double tuning) noexcept {
    if (tuning == 440.0 || !(tuning > 0.0)) {
        return 0.0F;
    }
    return static_cast<float>(1200.0 * dsp::log2(tuning / 440.0));
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
// NOLINTNEXTLINE(misc-no-recursion) - a macro's lane becomes its targets' lanes, once.
bool addLane(const Project& project, ParamRef target, const std::vector<Breakpoint>& points,
             const Window& window, std::int64_t endTick, std::int64_t lastTick,
             const graph::ParamLayout& layout, SnapshotStorage& storage,
             std::vector<LaneRange>& ranges, const MacroOffsets& macros) {
    if (!target.valid() || points.empty()) {
        return false;
    }
    if (target.kind == ParamKind::Macro) {
        // A lane drawn on a macro: the same lane on each of its targets, every point
        // put through that target's range and curve.
        const Macro* macro = project.find(core::MacroId{target.owner});
        bool any = false;
        for (const MacroTarget& each :
             macro != nullptr ? macro->targets : std::vector<MacroTarget>{}) {
            if (each.target.kind == ParamKind::Macro) {
                continue;
            }
            std::vector<Breakpoint> mapped = points;
            for (Breakpoint& point : mapped) {
                point.value = macroTargetValue(each, point.value);
            }
            any = addLane(project, each.target, mapped, window, endTick, lastTick, layout, storage,
                          ranges, {}) ||
                  any;
        }
        return any;
    }
    const std::uint32_t index = paramIndexOf(layout, target);
    if (index == graph::kNone) {
        return false;
    }
    const auto offset = macros.find(index);
    LaneRange range{.points = storage.points.size(), .knots = storage.knots.size()};
    for (const Breakpoint& point : points) {
        const std::int64_t tick = window.place(point.at.value);
        if (tick > lastTick) {
            break;
        }
        storage.points.push_back(AutomationPoint{
            .tick = tick,
            .value = offset != macros.end() ? offset->second.apply(point.value) : point.value,
            .curve = point.curve});
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

/// Appends the lanes of the LFO and envelope controllers. Last, so a controller wins
/// over drawn automation on the parameter it drives - it is the more deliberate act.
/// `eventsOf` gives a channel's flattened events, for an envelope's triggers.
template<class EventsOf>
void addControllerLanes(const Project& project, const graph::ParamLayout& layout,
                        SnapshotStorage& storage, std::vector<LaneRange>& ranges,
                        EventsOf eventsOf) {
    // Far enough past the end that playing on does not run off the chain; it holds
    // its last value beyond.
    const std::int64_t end =
        std::max(project.contentLength().value,
                 project.meta.loopEnabled ? project.meta.loopEnd.value : std::int64_t{0}) +
        (core::kPpq * 4 * 64);
    for (const Controller& controller : project.controllers) {
        if (controller.kind == ControllerKind::Peak || !controller.target.valid()) {
            continue;
        }
        const std::uint32_t index = paramIndexOf(layout, controller.target);
        if (index == graph::kNone) {
            continue;
        }
        const ControllerRange range =
            ControllerRange::of(ParamRegistry::descriptorFor(controller.target, project));
        LaneRange lane{.points = storage.points.size(), .knots = storage.knots.size()};
        if (controller.kind == ControllerKind::Lfo) {
            compileLfo(storage.knots, controller, range, end);
        } else {
            compileEnvelope(storage.knots, controller, range, eventsOf(controller.sourceChannel),
                            project.tempo);
        }
        lane.knotCount = storage.knots.size() - lane.knots;
        if (lane.knotCount == 0) {
            continue;
        }
        storage.lanes.push_back(AutomationLane{.paramIndex = index,
                                               .startTick = 0,
                                               .endTick = std::numeric_limits<std::int64_t>::max(),
                                               .points = {},
                                               .knots = {}});
        ranges.push_back(lane);
    }
}

template<class EventsOf>
void buildAutomation(const Project& project, const graph::ParamLayout& layout,
                     SnapshotStorage& storage, const MacroOffsets& macros, EventsOf eventsOf) {
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
                                  storage, ranges, macros));
    };
    // Clip envelopes go last, so where one overlaps a pattern or playlist lane on the
    // same parameter the most specific automation wins.
    std::vector<std::pair<const PlaylistItem*, Window>> enveloped;

    for (const PlaylistTrack& track : project.playlist.tracks) {
        if (!project.playlist.audible(track)) {
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
                                          end, layout, storage, ranges, macros));
            }
        }
    }
    addControllerLanes(project, layout, storage, ranges, eventsOf);

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

SnapshotBuilder::SnapshotBuilder(std::uint32_t sampleRate, std::uint32_t maxBlockFrames,
                                 bool offline)
    : m_sampleRate(sampleRate),
      m_nodes(graph::PrepareInfo{
          .sampleRate = sampleRate, .maxBlockFrames = maxBlockFrames, .offline = offline}),
      m_stemNodes(graph::PrepareInfo{
          .sampleRate = sampleRate, .maxBlockFrames = maxBlockFrames, .offline = offline}) {}

void SnapshotBuilder::setMonitor(core::PlaylistTrackId track) {
    if (m_nodes.monitorTrack() != track) {
        m_nodes.setMonitorTrack(track);
        // A track with no clips has a node only while it is monitored: a new graph.
        m_graphBuild.reset();
        m_graph.reset();
    }
}

void SnapshotBuilder::setStem(const graph::StemSpec* stem) {
    if (stem != nullptr) {
        m_stem = *stem;
    } else {
        m_stem.reset();
    }
    m_graphBuild.reset();
    m_graph.reset();
}

SnapshotBuilder::~SnapshotBuilder() = default;

void SnapshotBuilder::forgetCaches() {
    m_tempo.reset();
    m_graphBuild.reset();
    m_graph.reset();
    m_clips.clear();
    m_channels.clear();
    m_primed = false;
}

bool SnapshotBuilder::audioReady() const noexcept {
    // What changed since the build: the rendered buffer finished, or the file it stood
    // in with finished decoding.
    return std::ranges::any_of(m_audioWaiting, [](const auto& waiting) {
        const auto& [entry, sourceWasReady] = waiting;
        return entry->handle().state() != format::SampleState::Loading ||
               (!sourceWasReady &&
                entry->source()->handle().state() != format::SampleState::Loading);
    });
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
        if (!project.playlist.audible(track)) {
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
            for (const MiniNotationSource& source : pattern->mini) {
                byChannel[source.channel.value].push_back(
                    Placement{.item = &item,
                              .pattern = pattern,
                              .mini = &source,
                              .miniHash = mini::fnv1a(mini::joinLines(source.lines))});
            }
        }
    }
    return byChannel;
}

std::vector<std::int64_t> SnapshotBuilder::signatureOf(const std::vector<Placement>& placements,
                                                       const Channel* channel,
                                                       core::Ticks miniCycle) {
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
    if (channel != nullptr && !channel->noteFilter.all()) {
        const NoteFilter& f = channel->noteFilter;
        signature.push_back((std::int64_t{1} << 40) | (std::int64_t{f.keyLow} << 24) |
                            (std::int64_t{f.keyHigh} << 16) | (std::int64_t{f.velocityLow} << 8) |
                            std::int64_t{f.velocityHigh});
    }
    for (const Placement& placement : placements) {
        const PlaylistItem& item = *placement.item;
        signature.insert(signature.end(), {static_cast<std::int64_t>(item.id.value),
                                           item.start.value, item.length.value,
                                           item.sourceOffset.value, placement.pattern->length.value,
                                           static_cast<std::int64_t>(placement.clipVersion)});
        if (placement.mini != nullptr) {
            // Its notes depend on where the cycles fall, so on the cycle length too.
            signature.insert(signature.end(),
                             {static_cast<std::int64_t>(placement.miniHash), miniCycle.value});
        }
        if (const auto* ref = std::get_if<PatternRef>(&item.content);
            (ref != nullptr) && ref->gain != 1.0F) {
            signature.push_back(static_cast<std::int64_t>(std::bit_cast<std::uint32_t>(ref->gain)));
        }
    }
    return signature;
}

std::shared_ptr<const SnapshotBuilder::ChannelEvents>
SnapshotBuilder::flatten(const std::vector<Placement>& placements, const Channel* channel,
                         core::Ticks miniCycle) {
    auto events = std::make_shared<ChannelEvents>();
    for (const Placement& placement : placements) {
        const PlaylistItem& item = *placement.item;
        const Window window = windowOf(item, placement.pattern->length);
        const auto* ref = std::get_if<PatternRef>(&item.content);
        const float gain = ref != nullptr ? ref->gain : 1.0F;
        if (placement.mini != nullptr) {
            appendMiniEvents(events->events, *placement.mini, window, item.id.value, miniCycle,
                             gain);
            continue;
        }
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
                                                    .velocity = scaledVelocity(note.velocity, gain),
                                                    .part = note.part});
            events->events.push_back(ScheduledEvent{.tick = off,
                                                    .endTick = off,
                                                    .noteId = note.id.value,
                                                    .instance = item.id.value,
                                                    .kind = EventKind::NoteOff,
                                                    .pitch = note.pitch,
                                                    .velocity = 0,
                                                    .part = note.part});
            if (const NoteExtras* extras = placement.clip->extrasFor(note.id)) {
                appendNoteExtras(events->events, events->lyricText, *extras, on, off, note.id.value,
                                 item.id.value);
            }
        }
    }
    if (channel != nullptr && !channel->noteFilter.all()) {
        // A rack layer's key and velocity range: the notes outside it, and everything
        // that belongs to them, never reach the channel.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> dropped;
        for (const ScheduledEvent& event : events->events) {
            if (event.kind == EventKind::NoteOn &&
                !channel->noteFilter.accepts(event.pitch, event.velocity)) {
                dropped.emplace_back(event.noteId, event.instance);
            }
        }
        std::ranges::sort(dropped);
        std::erase_if(events->events, [&dropped](const ScheduledEvent& event) {
            return event.kind != EventKind::Param &&
                   std::ranges::binary_search(dropped, std::pair{event.noteId, event.instance});
        });
    }
    std::ranges::sort(events->events, eventLess);
    if (channel != nullptr) {
        arpeggiate(events->events, channel->arp);
    }
    events->lyrics.assign(events->lyricText.begin(), events->lyricText.end());
    return events;
}

namespace {

/// The launch grid's bar: the meter at tick 0.
std::int64_t sessionBar(const Project& project) {
    for (const core::MeterEvent& meter : project.tempo.meterEvents()) {
        if (meter.at.value <= 0) {
            return std::max<std::int64_t>(1, core::kPpq * 4 * meter.numerator /
                                                 std::max(1, static_cast<int>(meter.denominator)));
        }
    }
    return core::kPpq * 4;
}

} // namespace

void SnapshotBuilder::compileSession(const Project& project, core::Ticks miniCycle,
                                     SessionOut& storage) {
    const SessionModel& model = project.session;
    for (const SessionScene& scene : model.scenes) {
        storage.sceneTempo.push_back(scene.tempo.value_or(0.0));
    }
    for (std::size_t t = 0; t < model.tracks.size(); ++t) {
        const SessionTrack& track = model.tracks[t];
        storage.trackStopOnEmpty.push_back(track.stopOnEmptyScene ? 1 : 0);
        storage.sessionChannels.push_back(track.channel.value);
        const Channel* channel = project.find(track.channel);
        for (std::size_t row = 0; row < track.slots.size(); ++row) {
            if (!track.slots[row].has_value()) {
                continue;
            }
            const ClipSlot& slot = *track.slots[row];
            const Pattern* pattern = project.find(slot.pattern);
            if (pattern == nullptr || channel == nullptr) {
                continue;
            }
            const std::int64_t length =
                slot.length.value > 0 ? slot.length.value : pattern->length.value;
            // A placement at tick 0 for the clip's length: the pattern's notes in its own
            // ticks. Its instance id keeps the clip's voices apart from the arrangement's
            // on the same channel (a voice is keyed by note id and instance).
            PlaylistItem item;
            item.id = core::ItemId{0x40000000U | (static_cast<std::uint32_t>(t) << 12U) |
                                   static_cast<std::uint32_t>(row)};
            item.start = core::Ticks{0};
            item.length = core::Ticks{length};
            item.content = PatternRef{.pattern = pattern->id};
            const Placement placement{
                .item = &item, .pattern = pattern, .clip = pattern->clipFor(channel->id)};
            std::vector<Placement> placements;
            if (placement.clip != nullptr) {
                placements.push_back(placement);
            }
            // A launchable mini-notation clip (phase_11.md §4.4): the pattern's sources
            // for this channel, realised from the clip's own tick 0, so `<a b>` and
            // `every 4` count the clip's cycles, not the song's.
            for (const MiniNotationSource& source : pattern->mini) {
                if (source.channel == channel->id) {
                    placements.push_back(
                        Placement{.item = &item,
                                  .pattern = pattern,
                                  .mini = &source,
                                  .miniHash = mini::fnv1a(mini::joinLines(source.lines))});
                }
            }
            auto events = flatten(placements, channel, miniCycle);
            const session::LaunchQuantize quantize =
                slot.quantize == session::LaunchQuantize::Global ? model.quantize : slot.quantize;
            storage.sessionClips.push_back(SessionClip{
                .track = static_cast<std::uint32_t>(t),
                .row = static_cast<std::uint32_t>(row),
                .channel = channel->id.value,
                .events = EventTrack{.events = events->events, .lyrics = events->lyrics},
                .length = std::max<std::int64_t>(length, 1),
                .loop = slot.loop,
                .legato = slot.legato,
                .quantize = static_cast<std::uint8_t>(quantize),
                .follow = static_cast<std::uint8_t>(slot.follow),
                .followRepeats = slot.followRepeats,
                .gain = slot.gain});
            storage.keepAlive.push_back(std::move(events));
        }
    }
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
    // kMeta too: a Voice channel bakes the project's tuning into its rendered notes, so
    // a new tuning has to reach its node's configuration check.
    if (!m_graphBuild ||
        (dirty & (dirty::kChannels | dirty::kMixer | dirty::kRouting | dirty::kMeta)) != 0) {
        m_stemNodes.setSamplePool(m_nodes.samplePool());
        auto candidate = std::make_shared<graph::GraphBuild>(graph::buildGraph(
            project, m_nodes, m_stem ? &*m_stem : nullptr, m_stem ? &m_stemNodes : nullptr));
        if (!candidate->ok) {
            return SnapshotBuildResult{.snapshot = nullptr, .error = candidate->error};
        }
        if (m_graphBuild && m_graph && candidate->sameStructure(*m_graphBuild)) {
            m_stats.graphReused = true;
        } else {
            m_graph = std::make_shared<graph::CompiledGraph>(candidate->graph, candidate->order,
                                                             candidate->pdc, m_parallel);
            m_graphBuild = std::move(candidate);
        }
        m_nodes.pruneUnused();
        m_stemNodes.pruneUnused();
    } else {
        m_stats.graphReused = true;
    }

    // --- events ---
    const bool eventsDirty =
        (dirty & (dirty::kPatterns | dirty::kPlaylist | dirty::kChannels)) != 0;
    if (eventsDirty) {
        refreshClipCache(project);
        const PlacementsByChannel placements = collectPlacements(project);
        const core::Ticks miniCycle = mini::cycleLength(project.tempo);
        static const std::vector<Placement> kNone;
        for (auto& entry : m_channels) {
            entry.second.seen = false;
        }
        for (const core::ChannelId channel : m_graphBuild->trackChannels) {
            ChannelCacheEntry& entry = m_channels[channel.value];
            const auto found = placements.find(channel.value);
            const std::vector<Placement>& mine = found != placements.end() ? found->second : kNone;
            const Channel* model = project.find(channel);
            std::vector<std::int64_t> signature = signatureOf(mine, model, miniCycle);
            if (entry.events && entry.signature == signature) {
                ++m_stats.tracksReused;
            } else {
                entry.events = flatten(mine, model, miniCycle);
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
    const bool anyFiltered = std::ranges::any_of(m_graphBuild->trackPatterns,
                                                 [](core::PatternId p) { return p.valid(); });
    const PlacementsByChannel stemPlacements =
        anyFiltered ? collectPlacements(project) : PlacementsByChannel{};
    for (std::size_t t = 0; t < m_graphBuild->trackChannels.size(); ++t) {
        const core::ChannelId channel = m_graphBuild->trackChannels[t];
        const core::PatternId only = t < m_graphBuild->trackPatterns.size()
                                         ? m_graphBuild->trackPatterns[t]
                                         : core::PatternId{};
        if (only.valid()) {
            // A pattern stem's copy of a channel: that pattern's placements only. Not
            // cached - a stem's builder renders once.
            std::vector<Placement> mine;
            if (const auto found = stemPlacements.find(channel.value);
                found != stemPlacements.end()) {
                std::ranges::copy_if(found->second, std::back_inserter(mine),
                                     [only](const Placement& p) { return p.pattern->id == only; });
            }
            auto events = flatten(mine, project.find(channel), mini::cycleLength(project.tempo));
            storage->tracks.push_back(
                EventTrack{.events = events->events, .lyrics = events->lyrics});
            storage->keepAlive.push_back(std::move(events));
            continue;
        }
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
    storage->trackChannels.reserve(m_graphBuild->trackChannels.size());
    for (const core::ChannelId channel : m_graphBuild->trackChannels) {
        storage->trackChannels.push_back(channel.value);
    }
    graph::fillParams(project, m_graphBuild->params, storage->params);
    const MacroOffsets macros = foldMacros(
        project,
        [this](ParamRef ref) -> std::optional<std::uint32_t> {
            const std::uint32_t index = paramIndexOf(m_graphBuild->params, ref);
            return index == graph::kNone ? std::nullopt : std::optional<std::uint32_t>{index};
        },
        storage->params);
    storage->paramBase = storage->params;
    buildAutomation(project, m_graphBuild->params, *storage, macros,
                    [this](core::ChannelId channel) -> std::span<const ScheduledEvent> {
                        const auto found = m_channels.find(channel.value);
                        if (found == m_channels.end() || !found->second.events) {
                            return {};
                        }
                        return found->second.events->events;
                    });

    // --- audio clips ---
    // Rebuilt every time: a track's regions are a handful of structs, and their sample
    // positions move with the tempo map.
    format::SamplePool& pool =
        m_nodes.samplePool() != nullptr ? *m_nodes.samplePool() : format::SamplePool::global();
    clip::compileAudio(project, m_graphBuild->audioSources, m_sampleRate, pool,
                       clip::ClipRenderCache::global(), storage->audio);
    m_audioWaiting.clear();
    for (const auto& entry : storage->audio.waiting) {
        m_audioWaiting.emplace_back(entry, entry->source()->handle().ready());
    }

    Snapshot& snapshot = storage->snapshot;
    snapshot.revision = revision;
    snapshot.sampleRate = m_sampleRate;
    snapshot.tuningCents = tuningCents(project.meta.tuning);
    snapshot.tempo = core::TempoView{.events = m_tempo->events, .cumSeconds = m_tempo->cumSeconds};
    snapshot.eventTracks = storage->tracks;
    snapshot.cursors = storage->cursors;
    snapshot.trackChannels = storage->trackChannels;
    snapshot.automation = storage->lanes;
    snapshot.graph = m_graph->view();
    snapshot.params = storage->params;
    snapshot.paramBase = storage->paramBase;
    snapshot.paramIndex = m_graphBuild->params.index;
    snapshot.paramLaneStart = storage->paramLaneStart;
    snapshot.paramLanes = storage->paramLanes;
    snapshot.audioTracks = storage->audio.tables;
    compileMidi(project, *storage);
    {
        SessionOut out{.sessionClips = storage->sessionClips,
                       .sceneTempo = storage->sceneTempo,
                       .trackStopOnEmpty = storage->trackStopOnEmpty,
                       .sessionChannels = storage->sessionChannels,
                       .keepAlive = storage->keepAlive};
        compileSession(project, mini::cycleLength(project.tempo), out);
    }
    snapshot.midiRoutes = storage->midiRoutes;
    snapshot.midiBindings = storage->midiBindings;
    snapshot.sessionClips = storage->sessionClips;
    snapshot.sessionTracks = static_cast<std::uint32_t>(project.session.tracks.size());
    snapshot.sessionRows = static_cast<std::uint32_t>(project.session.scenes.size());
    snapshot.sceneTempo = storage->sceneTempo;
    snapshot.trackStopOnEmpty = storage->trackStopOnEmpty;
    snapshot.sessionChannels = storage->sessionChannels;
    snapshot.sessionBarTicks = sessionBar(project);
    // No project seed exists yet; a fixed one keeps Random follow reproducible.
    snapshot.sessionSeed = 0x5EED5EEDU;
    snapshot.sessionQuantize = static_cast<std::uint8_t>(
        project.session.quantize == session::LaunchQuantize::Global ? session::LaunchQuantize::Bar
                                                                    : project.session.quantize);
    for (std::size_t i = 0; i < m_graphBuild->audioSources.size(); ++i) {
        if (m_nodes.monitorTrack().valid() &&
            m_graphBuild->audioSources[i].track == m_nodes.monitorTrack()) {
            snapshot.monitorTable = static_cast<std::uint32_t>(i);
        }
    }

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
