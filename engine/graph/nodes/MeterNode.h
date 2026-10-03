// Peak, RMS and loudness of a strip, one reading per process() call, into a ring the UI
// polls. The master's meter adds true peak (phase_4.md §4.10).
//
// Computed here, where the samples already are, so the UI's 60 Hz timer reads a few
// dozen floats in one call rather than shipping every sample across the FFI boundary
// to be squared in Python (FINAL_PLAN §2.2 Rule 2). The ring is the Phase 1
// OverwriteRing: the audio thread never waits for a UI that is busy, and a UI that
// misses a frame has lost nothing worth having.
#pragma once

#include "engine/dsp/Loudness.h"
#include "engine/dsp/TruePeak.h"
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

    /// Main thread, before prepare(). The master's meter measures true peak too.
    void setTruePeak(bool enabled) noexcept {
        m_truePeakEnabled = enabled;
    }
    [[nodiscard]] bool truePeak() const noexcept {
        return m_truePeakEnabled;
    }

private:
    Ring m_ring;
    dsp::LoudnessMeter m_loudness;
    dsp::TruePeak m_peakLeft;
    dsp::TruePeak m_peakRight;
    bool m_truePeakEnabled{false};
};

} // namespace adx::graph
