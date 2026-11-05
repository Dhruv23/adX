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
#include "engine/audio/StreamConfig.h"
#include "engine/graph/EngineCore.h"
#include "engine/project/ParamRef.h"
#include "engine/project/SnapshotBuilder.h"
#include "engine/render/RenderWorkers.h"
#include "engine/rt/OverwriteRing.h"
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
    /// Recording (phase_8.md §3.1): the device to listen to and how many of its
    /// channels. None by default - a stream that cannot record opens no input.
    std::uint32_t inputDeviceId{audio::kNoDevice};
    std::uint32_t inputChannels{0};
    /// Pulled by an offline render rather than a device clock: nothing is late, so a
    /// sandboxed plugin is waited for rather than given a realtime deadline
    /// (phase_9.md §4.3).
    bool offline{false};
    /// Helper threads a block's graph is spread over (RenderWorkers.h); negative leaves
    /// it to defaultRenderWorkers(). 0 renders on the callback's thread alone. The
    /// output is the same either way, bit for bit.
    std::int32_t renderWorkers{-1};
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
    /// rebuilt == false - unless an audio clip was playing a stand-in that is now
    /// ready (SnapshotBuilder::audioReady), so a caller that commits at UI rate hears a
    /// stretched clip as soon as it has rendered.
    CommitResult commit(const project::Project& project, const project::CommandStack& stack);

    /// A knob turn. Executes the matching value command, so the edit is in the
    /// project and in undo, and posts the new value straight to the audio thread,
    /// so it is heard next block *without* a rebuild (phase_3.md §4.2). Returns false
    /// for a ref that names nothing turnable.
    bool setParam(project::Project& project, project::CommandStack& stack, project::ParamRef ref,
                  float value);

    /// Software monitoring: the input plays through `track`'s insert while it is set
    /// (invalid turns it off). Rebuilds, because the track may need a node.
    CommitResult setMonitor(const project::Project& project, core::PlaylistTrackId track);

    // --- the arrangement's transport ---

    void play();
    void stopPlayback();
    void seek(core::Ticks at);
    void setLoop(transport::LoopRegion loop);
    void setRate(double rate);

    // --- outside the arrangement ---

    /// Plays one note through `channel`'s instrument for `seconds`, now, without a
    /// snapshot rebuild and whether or not the transport rolls (P5-1).
    void audition(core::ChannelId channel, std::uint8_t pitch, std::uint8_t velocity,
                  double seconds);
    /// Releases every audition note.
    void stopAudition();
    /// Puts every value a learned MIDI control wrote since the last call into the
    /// project, through the value commands, so the knob follows and the save has it.
    /// Returns how many. Main thread, at UI rate.
    std::size_t applyLearned(project::Project& project, project::CommandStack& stack);

    /// Live MIDI input (EngineCore::setMidiInput) plays `channel` from the next block; an
    /// invalid id silences it. Notes already held release where they started.
    void setMidiTarget(core::ChannelId channel);
    /// The click: on every `beat` ticks while the arrangement rolls, the first of every
    /// `beatsPerBar` higher. Realtime only - nothing renders it offline (P5-3).
    void setMetronome(bool enabled, core::Ticks beat, std::uint32_t beatsPerBar);

    /// One sample of `level` at arrangement sample `at`, straight to the output, while
    /// the transport rolls past it. Loopback calibration (RecordSession.h).
    void ping(std::int64_t at, float level);

    /// Where the audio thread says the arrangement is, as of its last block. O(1).
    [[nodiscard]] core::Ticks positionTicks() const noexcept;
    [[nodiscard]] std::int64_t positionSamples() const noexcept;
    [[nodiscard]] transport::PlayState state() const noexcept;

    // --- metering ---

    struct StripLevel {
        core::InsertId insert;
        rt::LevelFrame frame;
    };
    /// Every insert's latest meter reading, in insert-id order: one call per UI frame,
    /// whatever the number of strips (P3-7; phase_5.md §4.9 counts it). Main thread.
    /// Reuses `out`'s storage, so a steady caller does not allocate.
    void levels(std::vector<StripLevel>& out);

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

    /// An audio clip still plays a silent stand-in: its file is decoding or its
    /// processed buffer rendering. commit() swaps the real one in once it is ready. A
    /// device-clocked engine never notices; a test that renders a take as fast as it
    /// can must wait for this to clear first.
    [[nodiscard]] bool audioPending() const noexcept {
        return m_builder.audioPending();
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
    /// After the core and the builder, so its threads are joined first - though by then
    /// the stream has stopped and nothing hands them work.
    RenderWorkers m_workers;

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
