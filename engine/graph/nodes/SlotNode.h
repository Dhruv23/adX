// One effect slot: an effect, its wet/dry mix, and its bypass.
//
// Phase 3 has no effects - their DSP is Phase 4's - so the effect here is the
// identity, and a slot passes its input through bit for bit. What this phase owns is
// the slot's *place*: in the insert's chain, in the topological order, and in PDC,
// with its parameters resolved. A Phase 4 effect derives from this class, overrides
// processWet and latencySamples, and inherits the mix, the bypass and the wiring.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "engine/graph/Handover.h"
#include "engine/graph/Node.h"

namespace adx::graph {

enum class SlotParam : std::uint32_t {
    Mix,
    /// 1 when bypassed.
    Bypass,
    Count,
};

/// The slot's own parameters come first; the effect's follow, in the order its
/// descriptor table names them.
inline constexpr std::uint32_t kSlotParamCount = static_cast<std::uint32_t>(SlotParam::Count);

class SlotNode : public Node {
public:
    void prepare(const PrepareInfo& info) override;
    /// Final: every slot remembers its parameters and finishes a handover here, whatever
    /// its effect does. An effect implements processSlot (Effect does) or processWet.
    void process(ProcessContext& context) noexcept final;

    /// The effect type this node implements, as the catalog names it; empty for the
    /// identity. What the node store compares when a slot's type changes.
    [[nodiscard]] virtual std::string_view typeName() const noexcept {
        return {};
    }
    void reset() noexcept override;
    [[nodiscard]] PortSpec ports() const noexcept override;

    /// Main thread, before this node is first published: it replaces `old` (a slot
    /// whose effect type changed), and crossfades from it over `frames` rather than
    /// cutting its tail (Handover.h).
    void fadeFrom(SlotNode& old, std::uint32_t frames) noexcept {
        m_handover.begin(old, frames);
    }
    /// True once the old node will not be touched again. Any thread.
    [[nodiscard]] bool handoverDone() const noexcept {
        return m_handover.done.load(std::memory_order_acquire);
    }

protected:
    /// Writes the effect's fully wet output. Returns false for an effect that is the
    /// identity, which lets the slot skip the mix arithmetic and pass its input
    /// through exactly. `effectParams` is the slice after the slot's own two.
    virtual bool processWet(std::span<const float> inLeft, std::span<const float> inRight,
                            std::span<float> outLeft, std::span<float> outRight,
                            std::span<const float> effectParams, ProcessContext& context) noexcept;

    /// The slot's own block: the effect, its mix and its bypass. The default runs
    /// processWet through the slot's mix and bypass.
    virtual void processSlot(ProcessContext& context) noexcept;

private:
    void crossfadeFromOld(ProcessContext& context) noexcept;

    RememberedParams m_last;
    HandoverState<SlotNode> m_handover;
};

} // namespace adx::graph
