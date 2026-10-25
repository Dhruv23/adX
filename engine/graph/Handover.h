// adx-thread: realtime
//
// What a node replaced by a reload leaves behind (phase_7.md §4.4).
//
// NodeStore keys nodes by entity, so an edit that keeps an entity's configuration
// keeps its node, and nothing is disturbed. Two edits cannot: a slot whose effect
// *type* changed, and a channel whose instrument changed, get new nodes. Before Phase
// 7 the old node was simply dropped - a reverb's tail cut mid-ring, a pad's voices
// cut mid-note. Now the new node is handed the old one and keeps it sounding for a
// moment: an effect crossfades from the old instance to the new over
// kEffectSwapFadeMs, and an instrument's old voices are released and allowed to ring
// out (for at most kInstrumentReleaseCapMs) while new notes play on the new one.
//
// The old node is run with the parameter values it last ran with - remembered by the
// node itself every block, because the new snapshot's parameter slots belong to the
// new node and mean something else.
//
// Ownership: the old node is kept alive by NodeStore (main thread) until the new one
// says the handover is done; the audio thread only ever holds a raw pointer, and
// clears it itself before it says so.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "engine/graph/Node.h"

namespace adx::graph {

/// How long an effect replaced by a different type takes to fade out.
inline constexpr std::uint32_t kEffectSwapFadeMs = 20;
/// The longest an old instrument's released voices may keep sounding.
inline constexpr std::uint32_t kInstrumentReleaseCapMs = 4000;

/// The parameter values a node last processed with. Fixed-size, so remembering them
/// every block costs a copy and never an allocation; a node with more parameters
/// than this simply does not hand over (and is cut, as before Phase 7).
class RememberedParams {
public:
    static constexpr std::size_t kCapacity = 256;

    void capture(const ProcessContext& context) noexcept {
        if (context.params.size() > kCapacity) {
            m_count = 0;
            return;
        }
        const std::uint32_t last = context.frames > 0 ? context.frames - 1 : 0;
        m_count = static_cast<std::uint32_t>(context.params.size());
        for (std::uint32_t i = 0; i < m_count; ++i) {
            m_values[i] = paramAt(context, i, last);
        }
    }

    /// False until the node has processed a block (or when it has too many).
    [[nodiscard]] bool valid() const noexcept {
        return m_count > 0;
    }

    [[nodiscard]] std::span<const float> view() const noexcept {
        return std::span<const float>(m_values).first(m_count);
    }

private:
    std::array<float, kCapacity> m_values{};
    std::uint32_t m_count{0};
};

/// The new node's side of a handover: who it is taking over from, and whether it has
/// finished. `done` starts true - a node nobody handed anything to has nothing to do.
template<class NodeType> struct HandoverState {
    NodeType* from{nullptr};
    std::uint32_t remaining{0};
    std::uint32_t total{0};
    bool started{false};
    std::atomic<bool> done{true};

    /// Main thread, before the node is first published.
    void begin(NodeType& old, std::uint32_t frames) noexcept {
        from = &old;
        remaining = frames;
        total = frames;
        started = false;
        done.store(false, std::memory_order_release);
    }

    /// Audio thread: the old node will not be touched again.
    void finish() noexcept {
        from = nullptr;
        remaining = 0;
        done.store(true, std::memory_order_release);
    }
};

} // namespace adx::graph
