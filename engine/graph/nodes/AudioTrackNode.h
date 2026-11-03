// One playlist track's audio clips, played into the insert the track feeds.
//
// Stateless by design: the output at arrangement sample p is a function of p and the
// regions alone (AudioRegion.h). A seek, a loop wrap or a different block size cannot
// make it disagree with itself, which is the property offline export and the golden
// corpus rely on. The price is that a stop cuts rather than fades, as audio clips do in
// most DAWs.
#pragma once

#include "engine/dsp/Interpolate.h"
#include "engine/graph/Node.h"

namespace adx::graph {

class AudioTrackNode final : public Node {
public:
    /// `frozenChannel`: this plays a frozen channel's render in the channel's place
    /// (phase_8.md §3.4), with the channel's parameter slice - of which it honours only
    /// Audible, so mute and solo still work. Volume and pan are in the render.
    explicit AudioTrackNode(bool frozenChannel = false) noexcept : m_frozenChannel(frozenChannel) {}

    void prepare(const PrepareInfo& info) override;
    void process(ProcessContext& context) noexcept override;
    void reset() noexcept override;
    [[nodiscard]] PortSpec ports() const noexcept override;

    /// Adds `region`'s samples for arrangement positions [from, to) into `left` and
    /// `right`, whose element 0 is position `blockStart`. Exposed for tests.
    static void mix(const AudioRegion& region, const dsp::SincTable& sinc, std::int64_t blockStart,
                    std::int64_t from, std::int64_t to, float* left, float* right) noexcept;

private:
    const dsp::SincTable* m_sinc{nullptr};
    bool m_frozenChannel{false};
};

} // namespace adx::graph
