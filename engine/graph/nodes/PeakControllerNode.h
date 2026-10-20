// A peak controller: follows one insert's level and drives one parameter with it
// (FINAL_PLAN §5.1, phase_6.md §4.5).
//
// The one controller that cannot be compiled ahead of time - it follows a signal - so it
// is a node: the source insert's post-fader output arrives on its main input like a
// sidechain, which puts it in the topological order after its source, and the scheduler
// writes its output into the target parameter after it runs (NodeStep::modulates). A
// target node later in the order hears this block's value; an earlier one, the last
// block's. Either way the value is a function of the audio alone, so an offline render
// and a realtime one at the same block size agree.
#pragma once

#include <cstdint>

#include "engine/graph/Node.h"

namespace adx::graph {

/// What a peak controller is built from. Fixed for the node's life: a change is a new
/// node (NodeStore compares these), because the audio thread reads it unlocked.
struct PeakConfig {
    float attackMs{5.0F};
    float releaseMs{100.0F};
    /// Linear gain applied to the level before it is shaped.
    float gain{1.0F};
    float depth{0.5F};
    float center{0.5F};
    /// The target's range, and whether it is logarithmic.
    float minimum{0.0F};
    float maximum{1.0F};
    bool logarithmic{false};

    [[nodiscard]] friend bool operator==(const PeakConfig&, const PeakConfig&) noexcept = default;
};

class PeakControllerNode final : public Node {
public:
    explicit PeakControllerNode(PeakConfig config) noexcept : m_config(config) {}

    void prepare(const PrepareInfo& info) override;
    void process(ProcessContext& context) noexcept override;
    void reset() noexcept override;
    [[nodiscard]] PortSpec ports() const noexcept override;

    /// The value to write into the target after this block: true when there is one.
    [[nodiscard]] bool modulation(float& out) const noexcept override;

    [[nodiscard]] const PeakConfig& config() const noexcept {
        return m_config;
    }
    /// The follower's level, 0..1 after gain. For tests.
    [[nodiscard]] float level() const noexcept {
        return m_level;
    }

private:
    PeakConfig m_config;
    float m_attack{0.0F};
    float m_release{0.0F};
    double m_logRatio{0.0};
    float m_level{0.0F};
    float m_output{0.0F};
};

} // namespace adx::graph
