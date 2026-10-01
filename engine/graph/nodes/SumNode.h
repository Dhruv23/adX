// An N-in, 1-out mixing point.
//
// Summing itself happens at every node's main input: the scheduler adds each incoming
// edge into the input accumulator in a fixed order, so every convergence point in the
// graph is already a sum (phase_3.md §4.4 asks for a SumNode "at every convergence
// point"; §10 of the plan records why it is the port rather than a node). This node is
// the named convergence point for a graph that needs one without processing - a test's
// diamond, or a Phase 11 clip-group bus - and it is the identity on its summed input.
#pragma once

#include "engine/graph/Node.h"

namespace adx::graph {

class SumNode final : public Node {
public:
    void prepare(const PrepareInfo& info) override;
    void process(ProcessContext& context) noexcept override;
    void reset() noexcept override;
    [[nodiscard]] PortSpec ports() const noexcept override;
};

} // namespace adx::graph
