// adx-thread: main
//
// The helper threads behind an engine's Crew (engine/graph/Crew.h): their storage,
// their priority, and their shutdown. The audio thread never sees this class, only the
// Crew it owns.
//
// Helpers of a realtime engine join MMCSS's "Pro Audio" task at high priority, as the
// device's own thread does (WasapiExclusiveBackend): a helper preempted by the UI
// while it holds a node would make the audio thread wait for it. An offline engine's
// helpers keep normal priority - nothing is late there, and an export should not
// starve the interface.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "engine/graph/Crew.h"
#include "engine/rt/BlockArena.h"

namespace adx::render {

/// The helpers an engine starts when its options leave it to the engine: the
/// ADX_RENDER_WORKERS environment variable when it is set (0 turns them off), or one
/// fewer than the machine's hardware threads, at most Crew::kMaxLanes - 1.
[[nodiscard]] std::uint32_t defaultRenderWorkers();

class RenderWorkers {
public:
    /// Starts `helpers` threads (clamped to Crew::kMaxLanes - 1). `realtime` raises
    /// their priority as described above.
    RenderWorkers(std::uint32_t helpers, bool realtime);
    ~RenderWorkers();

    RenderWorkers(const RenderWorkers&) = delete;
    RenderWorkers& operator=(const RenderWorkers&) = delete;
    RenderWorkers(RenderWorkers&&) = delete;
    RenderWorkers& operator=(RenderWorkers&&) = delete;

    /// The crew to hand a scheduler, or null when there are no helpers.
    [[nodiscard]] graph::Crew* crew() noexcept {
        return m_threads.empty() ? nullptr : &m_crew;
    }
    [[nodiscard]] std::uint32_t helpers() const noexcept {
        return static_cast<std::uint32_t>(m_threads.size());
    }

private:
    void work(std::uint32_t lane, bool realtime);

    graph::Crew m_crew;
    std::vector<float> m_scratch;
    std::vector<std::byte> m_arenaStorage;
    std::vector<rt::BlockArena> m_arenas;
    std::atomic<bool> m_stopping{false};
    std::vector<std::thread> m_threads;
};

} // namespace adx::render
