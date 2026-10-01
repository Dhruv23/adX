#include "engine/graph/EngineCore.h"

#include "engine/rt/Violation.h"

namespace adx::graph {

bool EngineCore::post(const EngineMessage& message) noexcept {
    if (!m_queue.tryPush(message)) {
        ADX_RECORD_VIOLATION(rt::ViolationKind::Unbounded, 0);
        return false;
    }
    return true;
}

void EngineCore::install(project::Snapshot* snapshot) noexcept {
    if (snapshot == nullptr) {
        return;
    }
    if (m_current != nullptr) {
        // The retired snapshot goes to the reaper, never to delete: this is the audio
        // thread. If the reaper is full the snapshot leaks, loudly - Reaper::retire
        // records the violation.
        static_cast<void>(m_reaper.retire(m_current->lifetime));
    }
    m_current = snapshot;
    // The arrangement follows the snapshot's tempo map. When the map really changed,
    // the source keeps its musical position and invalidates the cursors itself.
    m_transport.arrangement().bindTempo(snapshot->tempo, snapshot->sampleRate);
    m_renderedRevision.store(snapshot->revision, std::memory_order_release);
    m_swaps.fetch_add(1, std::memory_order_release);
}

void EngineCore::drain() noexcept {
    // Bounded by the ring's capacity: the loop cannot run longer than kQueueCapacity
    // iterations per block, however fast the main thread posts.
    EngineMessage message;
    for (std::size_t i = 0; i < kQueueCapacity && m_queue.tryPop(message); ++i) {
        switch (message.kind) {
        case MessageKind::Snapshot:
            install(message.snapshot);
            break;
        case MessageKind::Param:
            if (m_current != nullptr) {
                const std::uint32_t index = m_current->findParam(message.param);
                if (index < m_current->params.size()) {
                    m_current->params[index] = message.value;
                }
            }
            break;
        case MessageKind::Seek:
            m_transport.get(transport::TimeSourceId{message.timeSource})
                .requestSeek(core::Ticks{message.tickA});
            break;
        case MessageKind::State:
            m_transport.get(transport::TimeSourceId{message.timeSource})
                .requestState(message.state);
            break;
        case MessageKind::Loop:
            m_transport.get(transport::TimeSourceId{message.timeSource})
                .requestLoop(transport::LoopRegion{.start = core::Ticks{message.tickA},
                                                   .end = core::Ticks{message.tickB},
                                                   .enabled = message.flag});
            break;
        case MessageKind::Rate:
            m_transport.get(transport::TimeSourceId{message.timeSource}).requestRate(message.rate);
            break;
        }
    }
}

void EngineCore::process(float* out, std::uint32_t frames, std::uint32_t channels,
                         rt::BlockArena& arena) noexcept {
    drain();
    if (m_current == nullptr) {
        // No project yet: silence, but the transport still answers requests and
        // publishes, so a UI polling the playhead is not left looking at stale state.
        for (std::uint32_t s = 0; s < transport::kMaxTimeSources; ++s) {
            const transport::TimeSourceId id{s};
            if (m_transport.inUse(id)) {
                static_cast<void>(m_transport.get(id).beginBlock());
                m_transport.get(id).publish();
            }
        }
        return;
    }
    m_scheduler.render(*m_current, m_transport, arena, out, frames, channels);
}

void EngineCore::processStep(void* user, float* out, const float* /*in*/, std::uint32_t frames,
                             std::uint32_t channels, const audio::StreamTime& /*time*/,
                             rt::BlockArena& arena) noexcept {
    static_cast<EngineCore*>(user)->process(out, frames, channels, arena);
}

project::Snapshot* EngineCore::detach() noexcept {
    EngineMessage message;
    while (m_queue.tryPop(message)) {
        if (message.kind == MessageKind::Snapshot && message.snapshot != nullptr) {
            // Published but never rendered. Retire it the same way a swapped-out one
            // is, so the owner's teardown has exactly one path to follow.
            static_cast<void>(m_reaper.retire(message.snapshot->lifetime));
        }
    }
    project::Snapshot* current = m_current;
    m_current = nullptr;
    return current;
}

} // namespace adx::graph
