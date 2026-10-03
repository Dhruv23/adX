#include "engine/dsp/Envelope.h"

#include <algorithm>
#include <cmath>

namespace adx::dsp {
namespace {

[[nodiscard]] std::uint32_t samplesFor(float seconds, std::uint32_t sampleRate) noexcept {
    if (!(seconds > 0.0F)) {
        return 0;
    }
    const double samples = static_cast<double>(seconds) * static_cast<double>(sampleRate);
    return static_cast<std::uint32_t>(std::floor(samples + 0.5));
}

} // namespace

void Envelope::enter(EnvStage stage, float from, float to, float seconds, const core::Curve& curve,
                     std::uint32_t sampleRate) noexcept {
    m_stage = stage;
    m_from = from;
    m_to = to;
    m_curve = curve;
    m_stageFrame = 0;
    m_length = samplesFor(seconds, sampleRate);
    m_strideFrom = 0.0F;
    m_strideTo = m_curve.evaluate(m_length == 0 ? 1.0F
                                                : std::min(1.0F, static_cast<float>(kCurveStride) /
                                                                     static_cast<float>(m_length)));
}

void Envelope::trigger(const EnvelopeShape& shape, std::uint32_t sampleRate) noexcept {
    m_sampleRate = sampleRate;
    enter(EnvStage::Attack, m_level, 1.0F, shape.attackSeconds, shape.attackCurve, sampleRate);
}

void Envelope::release(const EnvelopeShape& shape, std::uint32_t sampleRate) noexcept {
    if (m_stage == EnvStage::Release || m_stage == EnvStage::Idle) {
        return;
    }
    m_sampleRate = sampleRate;
    enter(EnvStage::Release, m_level, 0.0F, shape.releaseSeconds, shape.releaseCurve, sampleRate);
}

void Envelope::reset() noexcept {
    m_stage = EnvStage::Idle;
    m_level = 0.0F;
    m_stageFrame = 0;
    m_length = 0;
}

float Envelope::shaped(std::uint32_t position) const noexcept {
    if (m_curve.kind == core::CurveKind::Linear && m_curve.tension == 0.0F) {
        return static_cast<float>(position) / static_cast<float>(m_length);
    }
    // Between stride points, linearly: the curve is evaluated once per kCurveStride.
    const std::uint32_t inStride = position % kCurveStride;
    const std::uint32_t strideStart = position - inStride;
    const std::uint32_t strideEnd = std::min(strideStart + kCurveStride, m_length);
    if (strideEnd == strideStart) {
        return m_strideTo;
    }
    const float fraction =
        static_cast<float>(position - strideStart) / static_cast<float>(strideEnd - strideStart);
    return m_strideFrom + ((m_strideTo - m_strideFrom) * fraction);
}

float Envelope::next(const EnvelopeShape& shape) noexcept {
    // A stage with no length is passed straight through, possibly several in one call:
    // a zero attack and zero decay land on the sustain level on the first sample.
    for (int guard = 0; guard < 4; ++guard) {
        switch (m_stage) {
        case EnvStage::Idle:
            m_level = 0.0F;
            return 0.0F;
        case EnvStage::Sustain:
            m_level = shape.sustain;
            return m_level;
        case EnvStage::Attack:
        case EnvStage::Decay:
        case EnvStage::Release:
            break;
        }
        if (m_stageFrame >= m_length) {
            m_level = m_to;
            if (m_stage == EnvStage::Attack) {
                enter(EnvStage::Decay, 1.0F, shape.sustain, shape.decaySeconds, shape.decayCurve,
                      m_sampleRate);
            } else if (m_stage == EnvStage::Decay) {
                m_stage = EnvStage::Sustain;
            } else {
                m_stage = EnvStage::Idle;
                m_level = 0.0F;
                return 0.0F;
            }
            continue;
        }
        ++m_stageFrame;
        if (m_stageFrame % kCurveStride == 0 && m_stageFrame < m_length) {
            // Crossed into the next stride: its two ends.
            m_strideFrom = m_strideTo;
            const std::uint32_t end = std::min(m_stageFrame + kCurveStride, m_length);
            m_strideTo = m_curve.evaluate(static_cast<float>(end) / static_cast<float>(m_length));
        }
        // The stage's last sample is its target exactly, not from + (to - from) * 1
        // rounded: a decay must land on the sustain level, not a ulp beside it.
        m_level =
            m_stageFrame == m_length ? m_to : m_from + ((m_to - m_from) * shaped(m_stageFrame));
        return m_level;
    }
    return m_level;
}

} // namespace adx::dsp
