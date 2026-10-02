// Project + DirtyMask -> Snapshot, reusing whatever did not change.
//
// Dragging one note must not re-flatten the mixer graph (phase_3.md §4.2). So a
// snapshot is assembled from independently reusable parts, each held by shared
// ownership on the main thread:
//
//   tempo      rebuilt on kTempo
//   graph      rebuilt when the graph's *structure* changes - compared, not assumed
//              from the dirty mask, so a fader move that arrives as a command still
//              reuses every node, buffer and delay line
//   per-channel event tracks
//              each channel's track is rebuilt only when a note in a pattern it
//              plays, or a placement of one, actually changed
//   parameters, cursors, automation lanes
//              small; rebuilt every time
//
// Main thread only. Shared ownership is only ever touched here and in the reaper's
// drain, both on the main thread, so the refcounts need no atomic ordering the audio
// thread could observe.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "engine/graph/CompiledGraph.h"
#include "engine/graph/GraphBuilder.h"
#include "engine/project/Note.h"
#include "engine/project/Pattern.h"
#include "engine/project/Playlist.h"
#include "engine/project/Snapshot.h"
#include "engine/project/commands/Command.h"

namespace adx::project {

class Project;

/// What the last build did, for the incremental-budget test and for anyone asking why
/// a build was slow.
struct SnapshotBuildStats {
    bool graphReused{false};
    bool tempoReused{false};
    std::uint32_t tracksRebuilt{0};
    std::uint32_t tracksReused{0};
    std::uint32_t nodesCreated{0};
    double milliseconds{0.0};
};

struct SnapshotBuildResult {
    /// Owned by its own `lifetime`: hand it to the engine, or destroy it with
    /// destroySnapshot. Null when the build failed.
    Snapshot* snapshot{nullptr};
    /// A routing cycle, by name. The previous snapshot stays in force.
    std::string error;
};

/// Destroys a snapshot this builder produced. Main thread. What the reaper's drain
/// does, for the snapshots that never reached the audio thread.
void destroySnapshot(Snapshot* snapshot) noexcept;

/// Snapshots built and not yet destroyed, process-wide. After a stream has stopped and
/// its reaper has drained, this is the number the engine still holds - one - and
/// reaper_reclaims_snapshots checks that thousands of publishes leave it there.
[[nodiscard]] std::int64_t liveSnapshotCount() noexcept;

class SnapshotBuilder {
public:
    SnapshotBuilder(std::uint32_t sampleRate, std::uint32_t maxBlockFrames);
    ~SnapshotBuilder();

    SnapshotBuilder(const SnapshotBuilder&) = delete;
    SnapshotBuilder& operator=(const SnapshotBuilder&) = delete;
    SnapshotBuilder(SnapshotBuilder&&) = delete;
    SnapshotBuilder& operator=(SnapshotBuilder&&) = delete;

    /// Builds a snapshot of `project` at `revision`. `dirty` is what changed since the
    /// previous build; pass dirty::kAll the first time, or to force a full rebuild.
    [[nodiscard]] SnapshotBuildResult build(const Project& project, std::uint64_t revision,
                                            DirtyMask dirty);

    [[nodiscard]] const SnapshotBuildStats& lastStats() const noexcept {
        return m_stats;
    }

    [[nodiscard]] graph::NodeStore& nodes() noexcept {
        return m_nodes;
    }

    /// The compiled graph of the last successful build. For tests and diagnostics.
    [[nodiscard]] const graph::CompiledGraph* compiledGraph() const noexcept {
        return m_graph.get();
    }
    [[nodiscard]] const graph::GraphBuild* graphBuild() const noexcept {
        return m_graphBuild.get();
    }

    /// Latency along the slowest path of the last successful build, in samples.
    [[nodiscard]] std::uint32_t totalLatency() const noexcept;

    /// Clears every cache, so the next build starts from nothing. For the debug mode
    /// phase_3.md §10 asks for: build incrementally, build from scratch, compare.
    void forgetCaches();

private:
    struct TempoBlock;
    struct ChannelEvents;
    struct ClipCacheEntry {
        std::vector<Note> notes;
        std::uint64_t version{0};
        bool seen{false};
    };
    struct ChannelCacheEntry {
        std::vector<std::int64_t> signature;
        std::shared_ptr<const ChannelEvents> events;
        bool seen{false};
    };

    /// One placement of one note clip: which item, which pattern, which clip.
    struct Placement {
        const PlaylistItem* item{nullptr};
        const Pattern* pattern{nullptr};
        const NoteClip* clip{nullptr};
        std::uint64_t clipVersion{0};
    };
    using PlacementsByChannel = std::map<std::uint32_t, std::vector<Placement>>;

    void refreshClipCache(const Project& project);
    /// Every audible placement of every note clip, grouped by the channel it plays
    /// through. One pass over the playlist, so rebuilding a channel's track costs its
    /// own placements and not every placement in the project.
    [[nodiscard]] PlacementsByChannel collectPlacements(const Project& project) const;
    [[nodiscard]] static std::vector<std::int64_t>
    signatureOf(const std::vector<Placement>& placements);
    [[nodiscard]] static std::shared_ptr<const ChannelEvents>
    flatten(const std::vector<Placement>& placements);

    std::uint32_t m_sampleRate;
    graph::NodeStore m_nodes;

    std::shared_ptr<const TempoBlock> m_tempo;
    std::shared_ptr<graph::GraphBuild> m_graphBuild;
    std::shared_ptr<graph::CompiledGraph> m_graph;
    std::map<std::pair<std::uint32_t, std::uint32_t>, ClipCacheEntry> m_clips;
    std::map<std::uint32_t, ChannelCacheEntry> m_channels;
    std::uint64_t m_clipVersion{0};
    bool m_primed{false};

    SnapshotBuildStats m_stats;
};

} // namespace adx::project
