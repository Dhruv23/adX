#include "engine/effects/Delay.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"

namespace adx::effects {
namespace {

template<class E> [[nodiscard]] constexpr std::uint32_t at(E param) noexcept {
    return static_cast<std::uint32_t>(param);
}

[[nodiscard]] float onePoleCoefficient(float cutoff, std::uint32_t rate) noexcept {
    return static_cast<float>(1.0 - dsp::exp(-dsp::kTwoPi * static_cast<double>(cutoff) / rate));
}

} // namespace

void Delay::prepareEffect(const graph::PrepareInfo& info) {
    const auto size = static_cast<std::size_t>(kMaxSeconds * info.sampleRate) + 4;
    m_left.allocate(size);
    m_right.allocate(size);
    // A changed delay time glides with a 50 ms time constant: fast enough to follow a
    // tempo change, slow enough that the pitch bend it causes is a swoop, not a click.
    m_glide = static_cast<float>(1.0 - dsp::exp(-1.0 / (0.05 * info.sampleRate)));
}

void Delay::resetEffect() noexcept {
    std::ranges::fill(m_left.view(), 0.0F);
    std::ranges::fill(m_right.view(), 0.0F);
    m_write = 0;
    m_primed = false;
    m_lowState.fill(0.0F);
    m_highState.fill(0.0F);
}

void Delay::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                       std::span<float> outLeft, std::span<float> outRight,
                       const EffectContext& context) noexcept {
    const std::span<float> left = m_left.view();
    const std::span<float> right = m_right.view();
    const std::size_t size = left.size();
    const auto maxDelay = static_cast<float>(size - 3);

    const auto read = [&](std::span<const float> buffer) {
        float position = static_cast<float>(m_write) - m_delay;
        if (position < 0.0F) {
            position += static_cast<float>(size);
            // A position a hair below zero rounds, in float, to exactly `size` once the
            // buffer length is added - one past the end. (v1's chorus had this too.)
            if (position >= static_cast<float>(size)) {
                position -= static_cast<float>(size);
            }
        }
        const auto i0 = static_cast<std::size_t>(position);
        const float fraction = position - static_cast<float>(i0);
        const std::size_t i1 = i0 + 1 >= size ? 0 : i0 + 1;
        return buffer[i0] + ((buffer[i1] - buffer[i0]) * fraction);
    };

    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            const auto sync = static_cast<std::size_t>(
                std::clamp(static_cast<int>(std::lround(context.paramAt(at(DelayParam::Sync), i))),
                           0, static_cast<int>(kDelaySyncWholes.size()) - 1));
            double seconds =
                static_cast<double>(context.paramAt(at(DelayParam::TimeMs), i)) * 0.001;
            if (sync > 0 && context.time != nullptr) {
                // The source's own tempo at its own position: two sources at two tempos
                // each get their own echo spacing (phase_3.md §2).
                const double bpm = context.time->tempo().bpmAt(context.time->positionTicks());
                seconds = kDelaySyncWholes[sync] * 4.0 * 60.0 / std::max(bpm, 1.0);
            }
            // Whole samples, as v1 truncated them: a static delay is an exact one.
            m_target = std::clamp(std::floor(static_cast<float>(seconds * context.sampleRate)),
                                  1.0F, maxDelay);
            if (!m_primed) {
                m_delay = m_target;
                m_primed = true;
            }
            m_pingPong = context.paramAt(at(DelayParam::PingPong), i) >= 0.5F;
            const float lowCut = context.paramAt(at(DelayParam::LowCut), i);
            const float highCut = context.paramAt(at(DelayParam::HighCut), i);
            m_filtered = lowCut > 20.0F || highCut < 20000.0F;
            m_lowCoefficient =
                onePoleCoefficient(std::clamp(lowCut, 20.0F, 2000.0F), context.sampleRate);
            m_highCoefficient =
                onePoleCoefficient(std::clamp(highCut, 500.0F, 20000.0F), context.sampleRate);
        }
        if (m_delay != m_target) {
            m_delay += (m_target - m_delay) * m_glide;
            if (std::abs(m_target - m_delay) < 1e-3F) {
                m_delay = m_target;
            }
        }

        const float delayedLeft = read(left);
        const float delayedRight = read(right);
        const float feedback =
            std::clamp(context.paramAt(at(DelayParam::Feedback), i), 0.0F, 0.95F);

        float returnLeft = m_pingPong ? delayedRight : delayedLeft;
        float returnRight = m_pingPong ? delayedLeft : delayedRight;
        if (m_filtered) {
            // Highcut is a one-pole lowpass; lowcut subtracts a one-pole lowpass.
            m_highState[0] += m_highCoefficient * (returnLeft - m_highState[0]);
            m_highState[1] += m_highCoefficient * (returnRight - m_highState[1]);
            returnLeft = m_highState[0];
            returnRight = m_highState[1];
            m_lowState[0] += m_lowCoefficient * (returnLeft - m_lowState[0]);
            m_lowState[1] += m_lowCoefficient * (returnRight - m_lowState[1]);
            returnLeft -= m_lowState[0];
            returnRight -= m_lowState[1];
        }
        left[m_write] = inLeft[i] + (returnLeft * feedback);
        right[m_write] = inRight[i] + (returnRight * feedback);
        if (++m_write >= size) {
            m_write = 0;
        }
        const float dry = context.paramAt(at(DelayParam::Dry), i);
        const float level = context.paramAt(at(DelayParam::Level), i);
        outLeft[i] = (dry * inLeft[i]) + (level * delayedLeft);
        outRight[i] = (dry * inRight[i]) + (level * delayedRight);
    }
}

} // namespace adx::effects
