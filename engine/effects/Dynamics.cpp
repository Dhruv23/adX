#include "engine/effects/Dynamics.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"

namespace adx::effects {
namespace {

template<class E> [[nodiscard]] constexpr std::uint32_t at(E param) noexcept {
    return static_cast<std::uint32_t>(param);
}

/// A one-pole smoothing coefficient for a time constant in milliseconds.
[[nodiscard]] float timeCoefficient(float milliseconds, std::uint32_t rate) noexcept {
    const double samples = static_cast<double>(std::max(milliseconds, 0.001F)) * 0.001 * rate;
    return static_cast<float>(dsp::exp(-1.0 / samples));
}

[[nodiscard]] float levelDb(float magnitude) noexcept {
    return static_cast<float>(dsp::gainToDb(static_cast<double>(magnitude), -200.0));
}

} // namespace

// --- Compressor -----------------------------------------------------------------

void Compressor::prepareEffect(const graph::PrepareInfo& info) {
    prepareLookahead(info.sampleRate);
    m_rmsCoefficient = static_cast<float>(1.0 - dsp::exp(-1.0 / (0.010 * info.sampleRate)));
}

void Compressor::resetEffect() noexcept {
    m_line.reset();
    m_envelopeDb = 0.0F;
    m_gain = 1.0F;
    m_meanSquare = 0.0F;
}

void Compressor::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                            std::span<float> outLeft, std::span<float> outRight,
                            const EffectContext& context) noexcept {
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            m_attack = timeCoefficient(context.paramAt(at(CompressorParam::Attack), i),
                                       context.sampleRate);
            m_release = timeCoefficient(context.paramAt(at(CompressorParam::Release), i),
                                        context.sampleRate);
            m_ratio = std::max(1.0F, context.paramAt(at(CompressorParam::Ratio), i));
            m_knee = std::max(0.0F, context.paramAt(at(CompressorParam::Knee), i));
            m_rms = context.paramAt(at(CompressorParam::Detector), i) >= 0.5F;
            m_external = context.paramAt(at(CompressorParam::KeyInput), i) >= 0.5F;
            m_linear = context.paramAt(at(CompressorParam::Smoothing), i) >= 0.5F;
        }
        const float keyLeft = m_external ? context.sideLeft[i] : inLeft[i];
        const float keyRight = m_external ? context.sideRight[i] : inRight[i];
        float level = 0.0F;
        if (m_rms) {
            const float square = 0.5F * ((keyLeft * keyLeft) + (keyRight * keyRight));
            m_meanSquare += m_rmsCoefficient * (square - m_meanSquare);
            level = levelDb(std::sqrt(m_meanSquare));
        } else {
            level = levelDb(std::max(std::abs(keyLeft), std::abs(keyRight)));
        }

        // The gain computer, with a quadratic soft knee (Giannoulis et al.).
        const float over = level - context.paramAt(at(CompressorParam::Threshold), i);
        const float slope = (1.0F / m_ratio) - 1.0F;
        float target = 0.0F;
        if (2.0F * over <= -m_knee) {
            target = 0.0F;
        } else if (m_knee > 0.0F && 2.0F * std::abs(over) <= m_knee) {
            const float x = over + (m_knee * 0.5F);
            target = slope * (x * x) / (2.0F * m_knee);
        } else {
            target = slope * over;
        }
        // Attack while the reduction deepens, release while it recovers.
        const float makeup = dsp::dbToGainF(context.paramAt(at(CompressorParam::Makeup), i));
        float gain = 1.0F;
        if (m_linear) {
            const float targetGain = dsp::dbToGainF(target);
            const float coefficient = targetGain < m_gain ? m_attack : m_release;
            m_gain = targetGain + ((m_gain - targetGain) * coefficient);
            gain = m_gain * makeup;
        } else {
            const float coefficient = target < m_envelopeDb ? m_attack : m_release;
            m_envelopeDb = target + ((m_envelopeDb - target) * coefficient);
            gain = dsp::dbToGainF(m_envelopeDb) * makeup;
        }

        float left = 0.0F;
        float right = 0.0F;
        m_line.push(inLeft[i], inRight[i], left, right);
        outLeft[i] = left * gain;
        outRight[i] = right * gain;
    }
}

// --- Ducker ---------------------------------------------------------------------

void Ducker::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                        std::span<float> outLeft, std::span<float> outRight,
                        const EffectContext& context) noexcept {
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            const float release =
                std::clamp(context.paramAt(at(DuckerParam::ReleaseMs), i), 10.0F, 1000.0F);
            m_releaseCoefficient = timeCoefficient(release, context.sampleRate);
            m_enabled = context.paramAt(at(DuckerParam::Enabled), i) >= 0.5F;
        }
        // v1's follower exactly: instant attack, one-pole release, on the key's peak.
        const float key = std::max(std::abs(context.sideLeft[i]), std::abs(context.sideRight[i]));
        m_envelope = std::max(key, m_envelope * m_releaseCoefficient);
        float duck = 1.0F;
        if (m_enabled) {
            const float amount =
                std::clamp(context.paramAt(at(DuckerParam::Amount), i), 0.0F, 1.0F);
            duck = std::max(0.0F, 1.0F - (amount * std::min(1.0F, m_envelope)));
        }
        outLeft[i] = inLeft[i] * duck;
        outRight[i] = inRight[i] * duck;
    }
}

// --- Limiter --------------------------------------------------------------------

void Limiter::prepareEffect(const graph::PrepareInfo& info) {
    prepareLookahead(info.sampleRate);
    const std::size_t lookahead = std::max<std::size_t>(m_latency, 1);
    // The audio waits for the true-peak estimate as well as for the lookahead, so the
    // estimate of every sample is in the window before that sample leaves.
    m_latency = static_cast<std::uint32_t>(lookahead + dsp::TruePeak::kDelay);
    m_line.allocate(m_latency);
    m_required.allocate(lookahead + 1);
    m_queue.allocate(lookahead + 1);
    m_box.allocate(lookahead);
}

void Limiter::resetEffect() noexcept {
    m_line.reset();
    std::ranges::fill(m_required.view(), 1.0F);
    m_queueHead = 0;
    m_queueSize = 0;
    std::ranges::fill(m_box.view(), 1.0F);
    m_boxSum = static_cast<double>(m_box.size());
    m_boxIndex = 0;
    m_envelope = 1.0F;
    m_index = 0;
    m_peakLeft.reset();
    m_peakRight.reset();
    m_samplePeaks.fill(0.0F);
    m_samplePeakIndex = 0;
}

// NOLINTNEXTLINE(readability-function-size)
void Limiter::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                         std::span<float> outLeft, std::span<float> outRight,
                         const EffectContext& context) noexcept {
    // The gain is: the lowest gain any sample in the next `lookahead` needs (a sliding
    // minimum), released by a one-pole, then box-averaged over the lookahead - which
    // brings it down smoothly *before* the peak arrives and provably to at most what
    // the peak needs when it does. A hard clip at the ceiling catches float rounding.
    const std::span<float> required = m_required.view();
    const std::span<std::uint64_t> queue = m_queue.view();
    const std::span<float> box = m_box.view();
    const std::size_t window = required.size(); // lookahead + 1
    const std::size_t boxLength = box.size();

    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            m_ceiling =
                dsp::dbToGainF(std::min(0.0F, context.paramAt(at(LimiterParam::Ceiling), i)));
            const float release = std::max(1.0F, context.paramAt(at(LimiterParam::Release), i));
            m_releaseCoefficient = 1.0F - timeCoefficient(release, context.sampleRate);
            m_truePeak = context.paramAt(at(LimiterParam::TruePeak), i) >= 0.5F;
        }
        // Both detectors run every sample, so switching true peak on does not start from
        // a stale history. The sample peak is delayed by the same kDelay the true-peak
        // estimate comes out late by, so both describe the same sample - the one the
        // audio line will release `lookahead` samples from now.
        const float truePeak = std::max(m_peakLeft.push(inLeft[i]), m_peakRight.push(inRight[i]));
        m_samplePeaks[m_samplePeakIndex] = std::max(std::abs(inLeft[i]), std::abs(inRight[i]));
        m_samplePeakIndex = (m_samplePeakIndex + 1) % m_samplePeaks.size();
        const float samplePeak = m_samplePeaks[m_samplePeakIndex];
        const float peak = m_truePeak ? std::max(truePeak, samplePeak) : samplePeak;
        const float need = peak > m_ceiling ? m_ceiling / peak : 1.0F;

        // Sliding minimum over the last `window` needs: a monotonic queue of indices.
        required[m_index % window] = need;
        while (m_queueSize > 0 &&
               required[queue[(m_queueHead + m_queueSize - 1) % window] % window] >= need) {
            --m_queueSize;
        }
        queue[(m_queueHead + m_queueSize) % window] = m_index;
        ++m_queueSize;
        while (queue[m_queueHead] + window <= m_index) {
            m_queueHead = (m_queueHead + 1) % window;
            --m_queueSize;
        }
        const float minimum = required[queue[m_queueHead] % window];

        m_envelope = minimum < m_envelope
                         ? minimum
                         : m_envelope + ((minimum - m_envelope) * m_releaseCoefficient);
        m_boxSum += static_cast<double>(m_envelope) - static_cast<double>(box[m_boxIndex]);
        box[m_boxIndex] = m_envelope;
        m_boxIndex = (m_boxIndex + 1) % boxLength;
        const auto gain = static_cast<float>(m_boxSum / static_cast<double>(boxLength));
        ++m_index;

        float left = 0.0F;
        float right = 0.0F;
        m_line.push(inLeft[i], inRight[i], left, right);
        outLeft[i] = std::clamp(left * gain, -m_ceiling, m_ceiling);
        outRight[i] = std::clamp(right * gain, -m_ceiling, m_ceiling);
    }
}

// --- Gate -----------------------------------------------------------------------

void Gate::prepareEffect(const graph::PrepareInfo& info) {
    prepareLookahead(info.sampleRate);
}

void Gate::resetEffect() noexcept {
    m_line.reset();
    m_open = false;
    m_holdLeft = 0;
    m_gain = 0.0F;
}

void Gate::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                      std::span<float> outLeft, std::span<float> outRight,
                      const EffectContext& context) noexcept {
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            m_attack = 1.0F - timeCoefficient(context.paramAt(at(GateParam::Attack), i),
                                              context.sampleRate);
            m_release = 1.0F - timeCoefficient(context.paramAt(at(GateParam::Release), i),
                                               context.sampleRate);
            m_hysteresis = std::max(0.0F, context.paramAt(at(GateParam::Hysteresis), i));
            m_range = std::min(0.0F, context.paramAt(at(GateParam::Range), i));
            m_rangeGain = dsp::dbToGainF(m_range);
            m_ratio = std::max(1.0F, context.paramAt(at(GateParam::Ratio), i));
            m_hold =
                static_cast<std::uint32_t>(std::max(0.0F, context.paramAt(at(GateParam::Hold), i)) *
                                           0.001F * static_cast<float>(context.sampleRate));
            m_external = context.paramAt(at(GateParam::KeyInput), i) >= 0.5F;
        }
        const float keyLeft = m_external ? context.sideLeft[i] : inLeft[i];
        const float keyRight = m_external ? context.sideRight[i] : inRight[i];
        const float level = levelDb(std::max(std::abs(keyLeft), std::abs(keyRight)));
        const float threshold = context.paramAt(at(GateParam::Threshold), i);

        // Hysteresis: opens above the threshold, closes only once the level has fallen
        // `hysteresis` below it and the hold has run out - no chatter on a level that
        // hovers at the threshold.
        if (level > threshold) {
            m_open = true;
            m_holdLeft = m_hold;
        } else if (m_open && level < threshold - m_hysteresis) {
            if (m_holdLeft > 0) {
                --m_holdLeft;
            } else {
                m_open = false;
            }
        }
        float target = 1.0F;
        if (!m_open) {
            if (m_ratio >= 99.0F) {
                target = m_rangeGain;
            } else {
                // Downward expansion below the threshold, floored at the range.
                const float db = std::max(m_range, (level - threshold) * (m_ratio - 1.0F));
                target = dsp::dbToGainF(db);
            }
        }
        m_gain += (target - m_gain) * (target > m_gain ? m_attack : m_release);

        float left = 0.0F;
        float right = 0.0F;
        m_line.push(inLeft[i], inRight[i], left, right);
        outLeft[i] = left * m_gain;
        outRight[i] = right * m_gain;
    }
}

} // namespace adx::effects
