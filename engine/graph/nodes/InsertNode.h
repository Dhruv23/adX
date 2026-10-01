// A mixer strip's fader stage: gain, pan, width, polarity, mute and solo.
//
// An insert in the model becomes a short chain in the graph - its slots in order, then
// this node, then a meter - rather than one node that runs its slots internally. The
// chain is what lets PDC see each slot's latency separately and lets a sidechain edge
// land on the slot that wants it (phase_3.md §4.4).
//
// Two outputs. Post-fader is the strip's result; pre-fader is the same signal before
// gain, pan, width, polarity and mute, for pre-fader sends - which by definition must
// not follow the fader (sends_pre_post_fader).
#pragma once

#include <cstdint>

#include "engine/graph/Node.h"

namespace adx::graph {

enum class InsertParam : std::uint32_t {
    Gain,
    Pan,
    Width,
    /// 1 when audible, 0 when muted by its own mute or by another strip's solo.
    Audible,
    /// -1 when the polarity is inverted, else 1.
    Polarity,
    Count,
};

inline constexpr std::uint32_t kInsertParamCount = static_cast<std::uint32_t>(InsertParam::Count);

class InsertNode final : public Node {
public:
    void prepare(const PrepareInfo& info) override;
    void process(ProcessContext& context) noexcept override;
    void reset() noexcept override;
    [[nodiscard]] PortSpec ports() const noexcept override;
};

} // namespace adx::graph
