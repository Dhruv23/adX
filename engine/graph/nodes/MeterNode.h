// Peak and RMS of a strip, one reading per process() call, into a ring the UI polls.
//
// Computed here, where the samples already are, so the UI's 60 Hz timer reads a few
// dozen floats in one call rather than shipping every sample across the FFI boundary
// to be squared in Python (FINAL_PLAN §2.2 Rule 2). The ring is the Phase 1
// OverwriteRing: the audio thread never waits for a UI that is busy, and a UI that
// misses a frame has lost nothing worth having.
#pragma once

#include "engine/graph/Node.h"
#include "engine/rt/OverwriteRing.h"

namespace adx::graph {

class MeterNode final : public Node {
public:
    using Ring = rt::OverwriteRing<rt::LevelFrame, 256>;

    void prepare(const PrepareInfo& info) override;
    void process(ProcessContext& context) noexcept override;
    void reset() noexcept override;
    [[nodiscard]] PortSpec ports() const noexcept override;

    /// Any thread may read it.
    [[nodiscard]] const Ring& ring() const noexcept {
        return m_ring;
    }

private:
    Ring m_ring;
};

} // namespace adx::graph
