#include "engine/effects/Ported.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"
#include "engine/dsp/Saturate.h"

namespace adx::effects {
namespace {

template<class E> [[nodiscard]] constexpr std::uint32_t at(E param) noexcept {
    return static_cast<std::uint32_t>(param);
}

// Freeverb's tunings, in samples at 44.1 kHz.
constexpr std::array<std::size_t, 8> kCombTunings{1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr std::array<std::size_t, 4> kAllpassTunings{556, 441, 341, 225};
constexpr std::size_t kStereoSpread = 23;
constexpr float kFixedGain = 0.015F;
constexpr float kAllpassFeedback = 0.5F;
constexpr float kWetScale = 3.0F;

[[nodiscard]] std::size_t scaled(std::size_t samples, std::uint32_t rate) noexcept {
    return std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(
                                        (static_cast<double>(samples) * rate / 44100.0) + 0.5)));
}

} // namespace

// --- Reverb -----------------------------------------------------------------

void Reverb::prepareEffect(const graph::PrepareInfo& info) {
    for (std::size_t i = 0; i < kCombs; ++i) {
        m_combLeft[i].buffer.allocate(scaled(kCombTunings[i], info.sampleRate));
        m_combRight[i].buffer.allocate(scaled(kCombTunings[i] + kStereoSpread, info.sampleRate));
    }
    for (std::size_t i = 0; i < kAllpasses; ++i) {
        m_allpassLeft[i].buffer.allocate(scaled(kAllpassTunings[i], info.sampleRate));
        m_allpassRight[i].buffer.allocate(
            scaled(kAllpassTunings[i] + kStereoSpread, info.sampleRate));
    }
}

void Reverb::resetEffect() noexcept {
    for (auto* lines : {&m_combLeft, &m_combRight}) {
        for (Line& line : *lines) {
            std::ranges::fill(line.buffer.view(), 0.0F);
            line.index = 0;
            line.store = 0.0F;
        }
    }
    for (auto* lines : {&m_allpassLeft, &m_allpassRight}) {
        for (Line& line : *lines) {
            std::ranges::fill(line.buffer.view(), 0.0F);
            line.index = 0;
        }
    }
}

void Reverb::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                        std::span<float> outLeft, std::span<float> outRight,
                        const EffectContext& context) noexcept {
    const auto comb = [](Line& c, float input, float feedback, float damp) {
        const std::span<float> buffer = c.buffer.view();
        const float output = buffer[c.index];
        c.store = (output * (1.0F - damp)) + (c.store * damp);
        buffer[c.index] = input + (c.store * feedback);
        if (++c.index >= buffer.size()) {
            c.index = 0;
        }
        return output;
    };
    const auto allpass = [](Line& a, float input) {
        const std::span<float> buffer = a.buffer.view();
        const float buffered = buffer[a.index];
        const float output = buffered - input;
        buffer[a.index] = input + (buffered * kAllpassFeedback);
        if (++a.index >= buffer.size()) {
            a.index = 0;
        }
        return output;
    };

    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            m_feedback =
                (std::clamp(context.paramAt(at(ReverbParam::Room), i), 0.0F, 1.0F) * 0.28F) + 0.7F;
            m_damp = std::clamp(context.paramAt(at(ReverbParam::Damp), i), 0.0F, 1.0F) * 0.4F;
            // Freeverb's width: at 1 each side is its own tail, as v1 always had.
            const float width = std::clamp(context.paramAt(at(ReverbParam::Width), i), 0.0F, 1.0F);
            m_wet1 = (width * 0.5F) + 0.5F;
            m_wet2 = (1.0F - width) * 0.5F;
        }
        const float feedback = m_feedback;
        const float damp = m_damp;
        const float wet1 = m_wet1;
        const float wet2 = m_wet2;
        const float input = (inLeft[i] + inRight[i]) * kFixedGain;
        float left = 0.0F;
        float right = 0.0F;
        for (std::size_t c = 0; c < kCombs; ++c) {
            left += comb(m_combLeft[c], input, feedback, damp);
            right += comb(m_combRight[c], input, feedback, damp);
        }
        for (std::size_t a = 0; a < kAllpasses; ++a) {
            left = allpass(m_allpassLeft[a], left);
            right = allpass(m_allpassRight[a], right);
        }
        left *= kWetScale;
        right *= kWetScale;
        outLeft[i] = (left * wet1) + (right * wet2);
        outRight[i] = (right * wet1) + (left * wet2);
    }
}

// --- Distortion ---------------------------------------------------------------

void Distortion::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                            std::span<float> outLeft, std::span<float> outRight,
                            const EffectContext& context) noexcept {
    float drive = 0.0F;
    float norm = 1.0F;
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        const float target = std::max(1.0F, context.paramAt(at(DistortionParam::Drive), i));
        if (i == 0 || target != drive) {
            drive = target;
            norm = dsp::driveNorm(drive);
        }
        outLeft[i] = dsp::driveTanh(inLeft[i], drive, norm);
        outRight[i] = dsp::driveTanh(inRight[i], drive, norm);
    }
}

// --- Bitcrush -----------------------------------------------------------------

void Bitcrush::resetEffect() noexcept {
    m_heldLeft = 0.0F;
    m_heldRight = 0.0F;
    m_phase = 1.0F;
}

void Bitcrush::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                          std::span<float> outLeft, std::span<float> outRight,
                          const EffectContext& context) noexcept {
    const auto rate = static_cast<float>(context.sampleRate);
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            const float hold =
                std::clamp(context.paramAt(at(BitcrushParam::Rate), i), 100.0F, rate);
            // Stepping a fractional phase rather than counting samples keeps a
            // non-integer ratio like 48000 / 14000 from drifting (v1's comment).
            m_step = hold / rate;
            // v1 clamped to 16 bits; the range is wider now, the arithmetic the same.
            const float bits = std::clamp(context.paramAt(at(BitcrushParam::Bits), i), 1.0F, 24.0F);
            m_levels = dsp::exp2F(bits);
        }
        const float levels = m_levels;
        m_phase += m_step;
        if (m_phase >= 1.0F) {
            m_phase -= 1.0F;
            m_heldLeft = inLeft[i];
            m_heldRight = inRight[i];
        }
        outLeft[i] = std::round(m_heldLeft * levels) / levels;
        outRight[i] = std::round(m_heldRight * levels) / levels;
    }
}

// --- Chorus ----------------------------------------------------------------------

void Chorus::prepareEffect(const graph::PrepareInfo& info) {
    // 50 ms, comfortably more than the 13 ms deepest read.
    const std::size_t size = static_cast<std::size_t>(0.05 * info.sampleRate) + 4;
    m_left.allocate(size);
    m_right.allocate(size);
}

void Chorus::resetEffect() noexcept {
    std::ranges::fill(m_left.view(), 0.0F);
    std::ranges::fill(m_right.view(), 0.0F);
    m_write = 0;
    m_lfoPhase = 0.0F;
}

void Chorus::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                        std::span<float> outLeft, std::span<float> outRight,
                        const EffectContext& context) noexcept {
    const std::span<float> left = m_left.view();
    const std::span<float> right = m_right.view();
    const std::size_t size = left.size();
    const auto rate = static_cast<float>(context.sampleRate);
    constexpr float kBaseDelayMs = 7.0F;
    constexpr float kModDepthMs = 6.0F;

    const auto read = [&](std::span<const float> buffer, float lfo, float depth) {
        const float delayMs = kBaseDelayMs + (lfo * kModDepthMs * depth);
        const float delay =
            std::clamp((delayMs / 1000.0F) * rate, 1.0F, static_cast<float>(size - 2));
        float position = static_cast<float>(m_write) - delay;
        if (position < 0.0F) {
            position += static_cast<float>(size);
            // A position a hair below zero rounds, in float, to exactly `size` once the
            // buffer length is added - one past the end. (v1's chorus had this too.)
            if (position >= static_cast<float>(size)) {
                position -= static_cast<float>(size);
            }
        }
        const auto i0 = static_cast<std::size_t>(position);
        const std::size_t i1 = (i0 + 1) % size;
        const float fraction = position - static_cast<float>(i0);
        return (buffer[i0] * (1.0F - fraction)) + (buffer[i1] * fraction);
    };

    for (std::uint32_t i = 0; i < context.frames; ++i) {
        const float lfoRate = std::max(0.01F, context.paramAt(at(ChorusParam::Rate), i));
        const float depth = std::clamp(context.paramAt(at(ChorusParam::Depth), i), 0.0F, 1.0F);
        left[m_write] = inLeft[i];
        right[m_write] = inRight[i];
        outLeft[i] = read(left, dsp::sinTurnsF(m_lfoPhase), depth);
        outRight[i] = read(right, dsp::sinTurnsF(m_lfoPhase + 0.25F), depth);
        if (++m_write >= size) {
            m_write = 0;
        }
        m_lfoPhase += lfoRate / rate;
        if (m_lfoPhase >= 1.0F) {
            m_lfoPhase -= 1.0F;
        }
    }
}

// --- EQ ----------------------------------------------------------------------------

void Eq3::resetEffect() noexcept {
    for (dsp::Biquad& b : m_left) {
        b.reset();
    }
    for (dsp::Biquad& b : m_right) {
        b.reset();
    }
}

void Eq3::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                     std::span<float> outLeft, std::span<float> outRight,
                     const EffectContext& context) noexcept {
    constexpr double kLowHz = 250.0;
    constexpr double kMidHz = 1200.0;
    constexpr double kMidQ = 0.7;
    constexpr double kHighHz = 4000.0;
    const double rate = context.sampleRate;
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            // Per control frame rather than v1's per sample: the same gains, and the
            // trig is no longer the most expensive thing in the mix.
            m_coefficients[0] = dsp::biquadCoefficients(dsp::BiquadKind::LowShelf, kLowHz, 1.0,
                                                        context.paramAt(at(EqParam::Low), i), rate);
            m_coefficients[1] = dsp::biquadCoefficients(dsp::BiquadKind::Peaking, kMidHz, kMidQ,
                                                        context.paramAt(at(EqParam::Mid), i), rate);
            m_coefficients[2] =
                dsp::biquadCoefficients(dsp::BiquadKind::HighShelf, kHighHz, 1.0,
                                        context.paramAt(at(EqParam::High), i), rate);
        }
        float left = inLeft[i];
        float right = inRight[i];
        for (std::size_t b = 0; b < 3; ++b) {
            left = m_left[b].process(left, m_coefficients[b]);
            right = m_right[b].process(right, m_coefficients[b]);
        }
        outLeft[i] = left;
        outRight[i] = right;
    }
}

} // namespace adx::effects
