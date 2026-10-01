// The audio thread's half of the render engine: the commit protocol's receiving end.
//
// FINAL_PLAN §4: a command mutates the main-thread project, then publishes an
// immutable render snapshot to the audio thread via the lock-free queue; the audio
// thread swaps the pointer between blocks and hands the retired snapshot to the
// reaper. This class is that audio-thread side - and nothing else in the engine is
// allowed to be.
//
// One queue, not one per message kind. A knob turn and the snapshot a structural
// edit produces must arrive in the order they were sent: a ParamChange posted after a
// snapshot was built has to land on *that* snapshot, not on the one it replaces and
// then be lost at the swap. Two queues cannot promise that; one can
// (phase_3.md §4.2, and §10 of that plan).
#pragma once

#include <atomic>
#include <cstdint>

#include "engine/audio/AudioBackend.h"
#include "engine/graph/Scheduler.h"
#include "engine/project/ParamRef.h"
#include "engine/project/Snapshot.h"
#include "engine/rt/BlockArena.h"
#include "engine/rt/Reaper.h"
#include "engine/rt/SpscRing.h"
#include "engine/transport/PlayState.h"
#include "engine/transport/TransportSet.h"

namespace adx::graph {

enum class MessageKind : std::uint8_t {
    /// Swap in `snapshot` at the next block boundary. Ownership passes with it.
    Snapshot,
    /// A knob turn: write `value` to `param` in the current snapshot. No rebuild.
    Param,
    Seek,
    State,
    Loop,
    Rate,
};

/// Everything that crosses from the main thread to the audio thread. Trivially
/// copyable, because the ring moves it by value.
struct EngineMessage {
    project::Snapshot* snapshot{nullptr};
    project::ParamRef param;
    float value{0.0F};
    std::uint32_t timeSource{0};
    /// Seek: the tick. Loop: the start tick.
    std::int64_t tickA{0};
    /// Loop: the end tick.
    std::int64_t tickB{0};
    double rate{1.0};
    MessageKind kind{MessageKind::Param};
    transport::PlayState state{transport::PlayState::Stopped};
    /// Loop: enabled.
    bool flag{false};
};

class EngineCore {
public:
    static constexpr std::size_t kQueueCapacity = 1024;

    EngineCore(transport::TransportSet& transport, rt::Reaper& reaper) noexcept
        : m_transport(transport), m_reaper(reaper) {}

    /// Main thread. False when the queue is full - the audio thread has stopped
    /// draining, or the main thread is outrunning it. Recorded as an Unbounded
    /// violation rather than left to each caller to notice (Phase 1 open issue P1-4);
    /// the caller keeps whatever it could not send and tries again.
    [[nodiscard]] bool post(const EngineMessage& message) noexcept;

    /// Audio thread. The per-block entry point: drain the queue, render, publish.
    void process(float* out, std::uint32_t frames, std::uint32_t channels,
                 rt::BlockArena& arena) noexcept;

    /// The AudioThread::ProcessStep trampoline. `user` is the EngineCore.
    static void processStep(void* user, float* out, const float* in, std::uint32_t frames,
                            std::uint32_t channels, const audio::StreamTime& time,
                            rt::BlockArena& arena) noexcept;

    /// Main thread, with no audio thread running this core: takes back the snapshot
    /// currently installed so its owner can destroy it. Drains any snapshots still
    /// queued into the reaper first, so nothing published is lost.
    [[nodiscard]] project::Snapshot* detach() noexcept;

    /// The revision of the snapshot being rendered. Any thread.
    [[nodiscard]] std::uint64_t renderedRevision() const noexcept {
        return m_renderedRevision.load(std::memory_order_acquire);
    }

    /// Snapshots swapped in since construction. Any thread. What
    /// snapshot_swap_between_blocks and param_change_without_rebuild count.
    [[nodiscard]] std::uint64_t swapCount() const noexcept {
        return m_swaps.load(std::memory_order_acquire);
    }

    [[nodiscard]] Scheduler& scheduler() noexcept {
        return m_scheduler;
    }

private:
    void drain() noexcept;
    void install(project::Snapshot* snapshot) noexcept;

    transport::TransportSet& m_transport;
    rt::Reaper& m_reaper;
    rt::SpscRing<EngineMessage, kQueueCapacity> m_queue;
    project::Snapshot* m_current{nullptr};
    Scheduler m_scheduler;
    std::atomic<std::uint64_t> m_renderedRevision{0};
    std::atomic<std::uint64_t> m_swaps{0};
};

} // namespace adx::graph
