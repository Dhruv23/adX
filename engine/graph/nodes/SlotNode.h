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
    void process(ProcessContext& context) noexcept override;

    /// The effect type this node implements, as the catalog names it; empty for the
    /// identity. What the node store compares when a slot's type changes.
    [[nodiscard]] virtual std::string_view typeName() const noexcept {
        return {};
    }
    void reset() noexcept override;
    [[nodiscard]] PortSpec ports() const noexcept override;

protected:
    /// Writes the effect's fully wet output. Returns false for an effect that is the
    /// identity, which lets the slot skip the mix arithmetic and pass its input
    /// through exactly. `effectParams` is the slice after the slot's own two.
    virtual bool processWet(std::span<const float> inLeft, std::span<const float> inRight,
                            std::span<float> outLeft, std::span<float> outRight,
                            std::span<const float> effectParams, ProcessContext& context) noexcept;
};

} // namespace adx::graph
