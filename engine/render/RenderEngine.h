// The main thread's half of the render engine: the commit protocol's sending end.
//
// Owns the stream, the transport, the snapshot builder and the engine core, and is the
// only thing that talks to the audio thread - by posting messages, never by touching
// its state. Realtime playback and offline render both go through one of these,
// differing only in the backend they were constructed with, which is how the two stay
// the same code path (phase_3.md §4.10 condition 1).
//
// Main thread only.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/audio/AudioThread.h"
#include "engine/graph/EngineCore.h"
#include "engine/project/ParamRef.h"
#include "engine/project/SnapshotBuilder.h"
#include "engine/transport/LoopRegion.h"
#include "engine/transport/TransportSet.h"

namespace adx::project {
class Project;
class CommandStack;
} // namespace adx::project

namespace adx::render {

/// Snapshots allowed between being posted and being swapped in. Beyond this the newest
/// waits on the main thread - and replaces any older one waiting there - so a burst of
/// edits costs the audio thread one swap, not hundreds, and the reaper cannot be
/// flooded faster than the main thread drains it.
inline constexpr std::uint64_t kMaxSnapshotsInFlight = 4;

struct EngineOptions {
    std::uint32_t sampleRate{48000};
    std::uint32_t blockFrames{256};
    std::uint32_t outputChannels{2};
};

struct CommitResult {
    /// A new snapshot was built and handed to the audio thread (or queued for it).
    bool rebuilt{false};
    /// Why not, when the project could not be rendered: a routing cycle, by name. The
    /// previous snapshot keeps playing.
    std::string error;
};

class RenderEngine {
public:
    RenderEngine(std::unique_ptr<audio::AudioBackend> backend, EngineOptions options);
    ~RenderEngine();

    RenderEngine(const RenderEngine&) = delete;
    RenderEngine& operator=(const RenderEngine&) = delete;
    RenderEngine(RenderEngine&&) = delete;
    RenderEngine& operator=(RenderEngine&&) = delete;

    /// Opens the stream without starting it. Messages posted before start() are
    /// applied by the first callback, in order - which is what lets a render begin
    /// from a known state on a known block.
    audio::Error open();
    audio::Error start();
    void stop() noexcept;

    // --- the project ---

    /// Builds from scratch and publishes. `revision` is recorded as the synced one.
    CommitResult setProject(const project::Project& project, std::uint64_t revision);

    /// Brings the audio thread up to `stack.revision()`, rebuilding only what the
    /// dirty mask since the last sync says changed. Nothing to do returns
    /// rebuilt == false.
    CommitResult commit(const project::Project& project, const project::CommandStack& stack);

    /// A knob turn. Executes the matching value command, so the edit is in the
    /// project and in undo, and posts the new value straight to the audio thread,
    /// so it is heard next block *without* a rebuild (phase_3.md §4.2). Returns false
    /// for a ref that names nothing turnable.
    bool setParam(project::Project& project, project::CommandStack& stack, project::ParamRef ref,
                  float value);

    // --- the arrangement's transport ---

    void play();
    void stopPlayback();
    void seek(core::Ticks at);
    void setLoop(transport::LoopRegion loop);
    void setRate(double rate);

    /// Where the audio thread says the arrangement is, as of its last block. O(1).
    [[nodiscard]] core::Ticks positionTicks() const noexcept;
    [[nodiscard]] std::int64_t positionSamples() const noexcept;
    [[nodiscard]] transport::PlayState state() const noexcept;

    // --- housekeeping ---

    /// Main-thread timer work: destroy what the audio thread retired, and retry
    /// anything the queue refused. Call it at UI rate.
    void pump();

    /// Snapshots built since construction. What param_change_without_rebuild counts.
    [[nodiscard]] std::uint64_t snapshotsBuilt() const noexcept {
        return m_snapshotsBuilt;
    }
    /// Messages waiting because the queue was full.
    [[nodiscard]] std::size_t backlog() const noexcept {
        return m_backlog.size();
    }
    /// Latency along the slowest path, in samples. The UI's latency readout.
    [[nodiscard]] std::uint32_t latencySamples() const noexcept {
        return m_builder.totalLatency();
    }

    [[nodiscard]] audio::AudioThread& audio() noexcept {
        return m_audio;
    }
    [[nodiscard]] graph::EngineCore& core() noexcept {
        return m_core;
    }
    [[nodiscard]] project::SnapshotBuilder& builder() noexcept {
        return m_builder;
    }
    [[nodiscard]] transport::TransportSet& transport() noexcept {
        return m_transport;
    }
    [[nodiscard]] const EngineOptions& options() const noexcept {
        return m_options;
    }

private:
    CommitResult publish(const project::Project& project, std::uint64_t revision,
                         project::DirtyMask dirty);
    void post(const graph::EngineMessage& message);
    void flushBacklog();
    [[nodiscard]] bool snapshotThrottled() const noexcept;
    /// Hands one message to the queue, or reports that it could not.
    [[nodiscard]] bool send(const graph::EngineMessage& message);

    EngineOptions m_options;
    transport::TransportSet m_transport;
    audio::AudioThread m_audio;
    graph::EngineCore m_core;
    project::SnapshotBuilder m_builder;

    /// Messages the queue refused, oldest first. Anything posted while this is
    /// non-empty joins the back, so order is kept.
    std::vector<graph::EngineMessage> m_backlog;

    bool m_synced{false};
    std::uint64_t m_syncedRevision{0};
    std::uint64_t m_snapshotsBuilt{0};
    std::uint64_t m_snapshotsPosted{0};
    bool m_open{false};
};

} // namespace adx::render
