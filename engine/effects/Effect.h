// The effect base: a slot node that owns wet/dry, bypass and latency, once.
//
// phase_4.md §4.2. An effect implements processWet - its fully wet output - and
// everything around that is here, the same for every effect:
//
//   wet/dry   an equal-power crossfade, per frame, so the slot's `mix` is automatable
//             and a sweep is a sweep;
//   bypass    ramped over 5 ms rather than switched, so toggling it is not a click;
//   latency   an effect that declares latency (a lookahead, an FFT block) gets its
//             dry path delayed by the same amount here, so wet and dry stay aligned
//             and a bypassed lookahead effect still has the latency PDC planned for.
//
// Two disciplines ported from _archive/src-cpp/include/AudioEffect.h, which got them
// right: a clone is a fresh instance with pristine DSP state, for an offline render
// that must never touch the instance the audio thread is running (effects::clone in
// Factory.h, main thread, because it allocates); isEquivalent() lets a hot reload keep
// the instance - and its reverb tail - when nothing about it changed.
// What changed: parameters are not std::atomic<float> members polled by the audio
// thread; they arrive in the ProcessContext like every other node's (§4.2).
#pragma once

#include <cstdint>
#include <span>

#include "engine/graph/nodes/SlotNode.h"
#include "engine/project/ParamDescriptor.h"
#include "engine/rt/BlockArena.h"
#include "engine/rt/OwnedArray.h"
#include "engine/transport/TimeSource.h"

namespace adx::effects {

/// How an effect's wet and dry are mixed.
enum class MixLaw : std::uint8_t {
    /// dry = cos(mix pi/2), wet = sin(mix pi/2): constant energy for a wet signal
    /// uncorrelated with the dry (a reverb tail, a delay). The default.
    EqualPower,
    /// dry = 1 - mix, wet = mix: iteration one's law for every effect it had. The
    /// ported effects keep it, because every v1 file's `mix` was chosen by ear against
    /// it, and for a wet signal correlated with the dry (an EQ, a chorus, a crusher)
    /// equal power would make the same `mix` up to 3 dB louder (phase_4.md §11).
    Linear,
};

/// Bypass ramp length.
inline constexpr double kBypassSeconds = 0.005;

/// What processWet is handed: the effect's own parameters (after the slot's mix and
/// bypass), the sidechain input, and the effect's sample clock.
struct EffectContext {
    std::uint32_t sampleRate{48000};
    std::uint32_t frames{0};
    std::span<const float> params;
    std::span<const float* const> automation;
    const transport::TimeSource* time{nullptr};
    rt::BlockArena* arena{nullptr};
    /// The sidechain input, `frames` long; silence when nothing is routed to it.
    std::span<const float> sideLeft;
    std::span<const float> sideRight;
    /// Frames this effect has processed before this call: its own clock, for the
    /// control grid. Never a block boundary - so the output does not depend on the
    /// block size.
    std::uint64_t clock{0};

    [[nodiscard]] float paramAt(std::uint32_t index, std::uint32_t frame) const noexcept {
        if (index < automation.size() && automation[index] != nullptr) {
            return automation[index][frame];
        }
        return index < params.size() ? params[index] : 0.0F;
    }
    [[nodiscard]] float param(std::uint32_t index) const noexcept {
        return paramAt(index, 0);
    }
    /// True on the frames of this call that fall on the control grid.
    [[nodiscard]] bool control(std::uint32_t frame) const noexcept {
        return ((clock + frame) % project::kControlFrames) == 0;
    }
};

class Effect : public graph::SlotNode {
public:
    void prepare(const graph::PrepareInfo& info) final;
    void process(graph::ProcessContext& context) noexcept final;
    void reset() noexcept final;
    [[nodiscard]] graph::PortSpec ports() const noexcept final;
    [[nodiscard]] std::uint32_t latencySamples() const noexcept final;

    /// True when `other` is the same effect with the same configuration, so a reload
    /// may keep this instance. Parameters are not part of it: they live in the
    /// snapshot, not in the node.
    [[nodiscard]] virtual bool isEquivalent(const Effect& other) const noexcept {
        return typeName() == other.typeName();
    }

    /// Main thread: copies whatever configuration lives in the node rather than in the
    /// snapshot's parameters - a convolution's impulse response - into a fresh instance
    /// of the same type. effects::clone calls it. Most effects have none.
    virtual void copyConfigTo(Effect& /*fresh*/) const {}

protected:
    /// Main thread. Allocate here.
    virtual void prepareEffect(const graph::PrepareInfo& info) = 0;
    /// Clears DSP state: delay lines, envelopes, filter memories.
    virtual void resetEffect() noexcept = 0;
    // SlotNode's overload is the one Effect::process replaces; this is the one an
    // effect implements.
    using graph::SlotNode::processWet;
    /// Writes the fully wet output for `in` into `out`. `out` never aliases `in`.
    virtual void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                            std::span<float> outLeft, std::span<float> outRight,
                            const EffectContext& context) noexcept = 0;
    /// Latency the effect introduces, in samples, at the prepared rate.
    [[nodiscard]] virtual std::uint32_t effectLatency() const noexcept {
        return 0;
    }
    /// The effect's wet/dry law. Fixed per type.
    [[nodiscard]] virtual MixLaw mixLaw() const noexcept {
        return MixLaw::EqualPower;
    }
    /// True for an effect with a sidechain input port (Compressor, Ducker, Gate).
    [[nodiscard]] virtual bool usesSidechain() const noexcept {
        return false;
    }

    [[nodiscard]] std::uint32_t preparedRate() const noexcept {
        return m_sampleRate;
    }

private:
    std::uint32_t m_sampleRate{48000};
    std::uint32_t m_latency{0};
    /// The dry signal, delayed by the effect's latency: a ring of latency + 1 frames.
    rt::OwnedArray<float> m_dryLeft;
    rt::OwnedArray<float> m_dryRight;
    std::size_t m_dryIndex{0};
    /// 0 = processing, 1 = bypassed; moves linearly over kBypassSeconds.
    float m_bypass{0.0F};
    float m_bypassStep{1.0F};
    bool m_primed{false};
    std::uint64_t m_clock{0};
};

/// Equal-power gains for a wet/dry mix in [0, 1]: dry = cos(mix pi/2), wet = sin.
/// Exact at the ends, so mix 0 is the dry signal bit for bit and mix 1 the wet.
struct MixGains {
    float dry{1.0F};
    float wet{0.0F};
};
[[nodiscard]] MixGains equalPowerMix(float mix) noexcept;
[[nodiscard]] MixGains mixGains(MixLaw law, float mix) noexcept;

} // namespace adx::effects
