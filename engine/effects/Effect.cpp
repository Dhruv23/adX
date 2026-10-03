#include "engine/effects/Effect.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/dsp/Math.h"
#include "engine/rt/RtConfig.h"

namespace adx::effects {
namespace {

/// What an unconnected sidechain reads.
constexpr std::array<float, rt::kMaxBlockFrames> kSilence{};

} // namespace

MixGains equalPowerMix(float mix) noexcept {
    if (mix <= 0.0F) {
        return MixGains{.dry = 1.0F, .wet = 0.0F};
    }
    if (mix >= 1.0F) {
        return MixGains{.dry = 0.0F, .wet = 1.0F};
    }
    return MixGains{.dry = dsp::cosTurnsF(mix * 0.25F), .wet = dsp::sinTurnsF(mix * 0.25F)};
}

MixGains mixGains(MixLaw law, float mix) noexcept {
    if (law == MixLaw::EqualPower) {
        return equalPowerMix(mix);
    }
    const float m = std::clamp(mix, 0.0F, 1.0F);
    return MixGains{.dry = 1.0F - m, .wet = m};
}

void Effect::prepare(const graph::PrepareInfo& info) {
    m_sampleRate = info.sampleRate;
    prepareEffect(info);
    m_latency = effectLatency();
    m_dryLeft.allocate(static_cast<std::size_t>(m_latency) + 1);
    m_dryRight.allocate(static_cast<std::size_t>(m_latency) + 1);
    const double rampSamples = kBypassSeconds * static_cast<double>(info.sampleRate);
    m_bypassStep = rampSamples < 1.0 ? 1.0F : static_cast<float>(1.0 / rampSamples);
    reset();
}

void Effect::reset() noexcept {
    resetEffect();
    std::ranges::fill(m_dryLeft.view(), 0.0F);
    std::ranges::fill(m_dryRight.view(), 0.0F);
    m_dryIndex = 0;
    m_primed = false;
    m_clock = 0;
}

graph::PortSpec Effect::ports() const noexcept {
    return graph::PortSpec{.inputs = static_cast<std::uint8_t>(usesSidechain() ? 2 : 1),
                           .outputs = 1,
                           .acceptsEvents = false};
}

std::uint32_t Effect::latencySamples() const noexcept {
    return m_latency;
}

void Effect::process(graph::ProcessContext& context) noexcept {
    const std::uint32_t frames = context.frames;
    const std::span<const float> inLeft = context.inputs[0];
    const std::span<const float> inRight = context.inputs[1];
    const std::span<float> outLeft = context.outputs[0];
    const std::span<float> outRight = context.outputs[1];

    constexpr auto kMix = static_cast<std::uint32_t>(graph::SlotParam::Mix);
    constexpr auto kBypass = static_cast<std::uint32_t>(graph::SlotParam::Bypass);
    const float bypassTarget = graph::paramAt(context, kBypass, 0) >= 0.5F ? 1.0F : 0.0F;
    if (!m_primed) {
        // A slot that starts bypassed starts bypassed: no 5 ms fade in from silence.
        m_bypass = bypassTarget;
        m_primed = true;
    }
    // Fully bypassed, an effect's wet path is skipped - unless it has latency. A
    // lookahead line that stopped at the moment of bypass would, on un-bypass, play the
    // audio it held from then, and then jump to the present as that drained: a click
    // whatever the crossfade (effect_bypass_is_click_free found it on the Limiter). So a
    // latent effect keeps running while bypassed, its output discarded.
    const bool silentWet = m_bypass >= 1.0F && bypassTarget >= 1.0F && m_latency == 0;
    const MixLaw law = mixLaw();

    if (!silentWet) {
        const bool hasSide = context.inputs.size() >= 4;
        const std::span<const float> silence{kSilence.data(), frames};
        const EffectContext effect{
            .sampleRate = context.sampleRate,
            .frames = frames,
            .params = context.params.size() > graph::kSlotParamCount
                          ? context.params.subspan(graph::kSlotParamCount)
                          : std::span<const float>{},
            .automation = context.automation.size() > graph::kSlotParamCount
                              ? context.automation.subspan(graph::kSlotParamCount)
                              : std::span<const float* const>{},
            .time = &context.time,
            .arena = &context.arena,
            .sideLeft = hasSide ? context.inputs[2] : silence,
            .sideRight = hasSide ? context.inputs[3] : silence,
            .clock = m_clock,
        };
        processWet(inLeft, inRight, outLeft, outRight, effect);
    }

    // Dry, delayed by the effect's latency so it lines up with the wet; then the
    // equal-power mix; then the bypass ramp between that and the dry.
    const std::span<float> dryLeft = m_dryLeft.view();
    const std::span<float> dryRight = m_dryRight.view();
    const std::size_t ring = dryLeft.size();
    for (std::uint32_t i = 0; i < frames; ++i) {
        dryLeft[m_dryIndex] = inLeft[i];
        dryRight[m_dryIndex] = inRight[i];
        const std::size_t read = (m_dryIndex + 1) % ring; // oldest: latency frames ago
        const float dl = m_latency == 0 ? inLeft[i] : dryLeft[read];
        const float dr = m_latency == 0 ? inRight[i] : dryRight[read];
        m_dryIndex = read;

        if (silentWet) {
            outLeft[i] = dl;
            outRight[i] = dr;
            continue;
        }
        const MixGains gains = mixGains(law, graph::paramAt(context, kMix, i));
        float left = (dl * gains.dry) + (outLeft[i] * gains.wet);
        float right = (dr * gains.dry) + (outRight[i] * gains.wet);
        if (m_bypass > 0.0F || bypassTarget > 0.0F) {
            left += (dl - left) * m_bypass;
            right += (dr - right) * m_bypass;
            m_bypass = bypassTarget > m_bypass ? std::min(bypassTarget, m_bypass + m_bypassStep)
                                               : std::max(bypassTarget, m_bypass - m_bypassStep);
        }
        outLeft[i] = left;
        outRight[i] = right;
    }
    m_clock += frames;
}

} // namespace adx::effects
