#include "engine/effects/Modulation.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"

namespace adx::effects {
namespace {

/// A triangle in [-1, 1] at `phase` turns.
[[nodiscard]] float triangle(double phase) noexcept {
    const double p = phase - std::floor(phase);
    return static_cast<float>(p < 0.5 ? (4.0 * p) - 1.0 : 3.0 - (4.0 * p));
}

[[nodiscard]] double wrap(double phase) noexcept {
    return phase - std::floor(phase);
}

} // namespace

// --- ModDelay -------------------------------------------------------------------------

void ModDelay::allocate(std::size_t frames) {
    std::size_t size = 1;
    while (size < frames + 4) {
        size <<= 1U;
    }
    m_buffer.allocate(size);
    m_mask = size - 1;
    m_write = 0;
}

void ModDelay::reset() noexcept {
    std::ranges::fill(m_buffer.view(), 0.0F);
    m_write = 0;
}

void ModDelay::write(float x) noexcept {
    m_write = (m_write + 1) & m_mask;
    m_buffer.view()[m_write] = x;
}

float ModDelay::read(float delay) const noexcept {
    const std::span<const float> buffer = m_buffer.view();
    const auto whole = static_cast<std::size_t>(delay);
    const float fraction = delay - static_cast<float>(whole);
    const float a = buffer[(m_write - whole) & m_mask];
    const float b = buffer[(m_write - whole - 1) & m_mask];
    return a + ((b - a) * fraction);
}

// --- Flanger ------------------------------------------------------------------------

void Flanger::prepareEffect(const graph::PrepareInfo& info) {
    // Delay plus depth, at most 20 ms.
    const auto frames = static_cast<std::size_t>(0.021 * info.sampleRate) + 2;
    for (ModDelay& line : m_delay) {
        line.allocate(frames);
    }
}

void Flanger::resetEffect() noexcept {
    for (ModDelay& line : m_delay) {
        line.reset();
    }
    m_feedback = {};
    m_phase = 0.0;
}

void Flanger::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                         std::span<float> outLeft, std::span<float> outRight,
                         const EffectContext& context) noexcept {
    const float msToFrames = static_cast<float>(context.sampleRate) / 1000.0F;
    const std::array<std::span<const float>, 2> in{inLeft, inRight};
    const std::array<std::span<float>, 2> out{outLeft, outRight};
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            m_increment = static_cast<double>(context.paramAt(idx(FlangerParam::Rate), i)) /
                          context.sampleRate;
            m_stereo = std::clamp(context.paramAt(idx(FlangerParam::Stereo), i), 0.0F, 0.5F);
        }
        const double increment = m_increment;
        const float stereo = m_stereo;
        const float depth = std::clamp(context.paramAt(idx(FlangerParam::Depth), i), 0.0F, 10.0F);
        const float base = std::clamp(context.paramAt(idx(FlangerParam::Delay), i), 0.1F, 10.0F);
        const float feedback =
            std::clamp(context.paramAt(idx(FlangerParam::Feedback), i), -0.95F, 0.95F);
        for (std::size_t c = 0; c < 2; ++c) {
            // The LFO sweeps the delay from `delay` up to `delay + depth`.
            const float lfo = triangle(m_phase + (c == 1 ? stereo : 0.0F));
            const float delayMs = base + (depth * 0.5F * (lfo + 1.0F));
            // Write, then read: a delay of d frames reads exactly d frames back.
            m_delay[c].write(in[c][i] + (feedback * m_feedback[c]));
            const float delayed = m_delay[c].read(std::max(1.0F, delayMs * msToFrames));
            m_feedback[c] = delayed;
            out[c][i] = 0.5F * (in[c][i] + delayed);
        }
        m_phase = wrap(m_phase + increment);
    }
}

// --- Phaser -------------------------------------------------------------------------

void Phaser::resetEffect() noexcept {
    for (auto& channel : m_state) {
        channel.fill(0.0F);
    }
    m_feedback = {};
    m_phase = 0.0;
}

void Phaser::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                        std::span<float> outLeft, std::span<float> outRight,
                        const EffectContext& context) noexcept {
    const double nyquistGuard = 0.45 * context.sampleRate;
    const std::array<std::span<const float>, 2> in{inLeft, inRight};
    const std::array<std::span<float>, 2> out{outLeft, outRight};
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            m_increment = static_cast<double>(context.paramAt(idx(PhaserParam::Rate), i)) /
                          context.sampleRate;
            m_stages = 2 * static_cast<std::size_t>(
                               std::clamp(static_cast<int>(std::floor(
                                              context.paramAt(idx(PhaserParam::Stages), i) + 0.5F)),
                                          1, 6));
            m_stereo = std::clamp(context.paramAt(idx(PhaserParam::Stereo), i), 0.0F, 0.5F);
        }
        const double increment = m_increment;
        const std::size_t stages = m_stages;
        const float stereo = m_stereo;
        const float depth = std::clamp(context.paramAt(idx(PhaserParam::Depth), i), 0.0F, 1.0F);
        const float centre = context.paramAt(idx(PhaserParam::Centre), i);
        const float feedback =
            std::clamp(context.paramAt(idx(PhaserParam::Feedback), i), -0.95F, 0.95F);
        for (std::size_t c = 0; c < 2; ++c) {
            const double lfo = dsp::sinTurns(m_phase + (c == 1 ? stereo : 0.0F));
            const double hz = std::clamp(static_cast<double>(centre) * dsp::exp2(3.0 * depth * lfo),
                                         10.0, nyquistGuard);
            // First-order allpass: a = (t - 1) / (t + 1), t = tan(pi f / fs).
            const double t = dsp::tanTurns(0.5 * hz / context.sampleRate);
            const auto a = static_cast<float>((t - 1.0) / (t + 1.0));
            float x = in[c][i] + (feedback * m_feedback[c]);
            for (std::size_t s = 0; s < stages; ++s) {
                const float y = (a * x) + m_state[c][s];
                m_state[c][s] = x - (a * y);
                x = y;
            }
            m_feedback[c] = x;
            out[c][i] = 0.5F * (in[c][i] + x);
        }
        m_phase = wrap(m_phase + increment);
    }
}

// --- Tremolo ------------------------------------------------------------------------

void Tremolo::resetEffect() noexcept {
    m_lfo = {dsp::Lfo{0x7E401U}, dsp::Lfo{0x7E402U}};
    m_started = false;
}

void Tremolo::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                         std::span<float> outLeft, std::span<float> outRight,
                         const EffectContext& context) noexcept {
    if (!m_started) {
        // The right LFO leads by `stereo` of a cycle, set once from the first call.
        m_lfo[0].reset(0.0F);
        m_lfo[1].reset(std::clamp(context.param(idx(TremoloParam::Stereo)), 0.0F, 0.5F));
        m_started = true;
    }
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            m_increment = context.paramAt(idx(TremoloParam::Rate), i) /
                          static_cast<float>(context.sampleRate);
            m_shape = static_cast<dsp::LfoShape>(std::clamp(
                static_cast<int>(std::floor(context.paramAt(idx(TremoloParam::Shape), i) + 0.5F)),
                0, dsp::kLfoShapeCount - 1));
        }
        const float increment = m_increment;
        const dsp::LfoShape shape = m_shape;
        const float depth = std::clamp(context.paramAt(idx(TremoloParam::Depth), i), 0.0F, 1.0F);
        // From 1 at the LFO's top to 1 - depth at its bottom.
        const float left = 1.0F - (depth * 0.5F * (1.0F - m_lfo[0].next(shape, increment)));
        const float right = 1.0F - (depth * 0.5F * (1.0F - m_lfo[1].next(shape, increment)));
        outLeft[i] = inLeft[i] * left;
        outRight[i] = inRight[i] * right;
    }
}

// --- RingMod ------------------------------------------------------------------------

void RingMod::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                         std::span<float> outLeft, std::span<float> outRight,
                         const EffectContext& context) noexcept {
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            m_external = context.paramAt(idx(RingModParam::Carrier), i) >= 0.5F;
        }
        const bool external = m_external;
        const float bias = std::clamp(context.paramAt(idx(RingModParam::Bias), i), 0.0F, 1.0F);
        float carrierLeft = 0.0F;
        float carrierRight = 0.0F;
        if (external) {
            carrierLeft = context.sideLeft[i];
            carrierRight = context.sideRight[i];
        } else {
            carrierLeft = static_cast<float>(dsp::sinTurns(m_phase));
            carrierRight = carrierLeft;
            m_phase = wrap(m_phase +
                           (static_cast<double>(context.paramAt(idx(RingModParam::Frequency), i)) /
                            context.sampleRate));
        }
        // Bias 1: (1 + c) / 2, amplitude modulation; bias 0: c, ring modulation.
        const float left = (bias * 0.5F * (1.0F + carrierLeft)) + ((1.0F - bias) * carrierLeft);
        const float right = (bias * 0.5F * (1.0F + carrierRight)) + ((1.0F - bias) * carrierRight);
        outLeft[i] = inLeft[i] * left;
        outRight[i] = inRight[i] * right;
    }
}

// --- FrequencyShifter ----------------------------------------------------------------

namespace {

// Olli Niemitalo's 90-degree phase-difference network: two chains of four
// second-order allpasses (each y = a^2 (x + y[-2]) - x[-2]), path A delayed a sample.
// Within +-0.7 degrees from 15 Hz to 20 kHz at 44.1 kHz; better at 48.
// NOLINTNEXTLINE(modernize-use-std-numbers) - Niemitalo's coefficient, not ln 2.
constexpr std::array<float, 4> kPathA{0.6923878F, 0.9360654322959F, 0.9882295226860F,
                                      0.9987488452737F};
constexpr std::array<float, 4> kPathB{0.4021921162426F, 0.8561710882420F, 0.9722909545651F,
                                      0.9952884791278F};

} // namespace

void HilbertPair::reset() noexcept {
    m_a = {};
    m_b = {};
    m_delayed = 0.0F;
}

void HilbertPair::process(float x, float& real, float& imaginary) noexcept {
    const auto chain = [](std::array<Stage, 4>& stages, const std::array<float, 4>& coeffs,
                          float input) {
        for (std::size_t s = 0; s < 4; ++s) {
            Stage& st = stages[s];
            const float a2 = coeffs[s] * coeffs[s];
            const float y = (a2 * (input + st.out2)) - st.in2;
            st.in2 = st.in1;
            st.in1 = input;
            st.out2 = st.out1;
            st.out1 = y;
            input = y;
        }
        return input;
    };
    const float a = chain(m_a, kPathA, x);
    real = m_delayed;
    m_delayed = a;
    imaginary = chain(m_b, kPathB, x);
}

void FrequencyShifter::resetEffect() noexcept {
    for (HilbertPair& pair : m_hilbert) {
        pair.reset();
    }
    m_phase = {};
}

void FrequencyShifter::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                                  std::span<float> outLeft, std::span<float> outRight,
                                  const EffectContext& context) noexcept {
    const std::array<std::span<const float>, 2> in{inLeft, inRight};
    const std::array<std::span<float>, 2> out{outLeft, outRight};
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        const double shift = context.paramAt(idx(FrequencyShifterParam::Shift), i);
        const double spread = context.paramAt(idx(FrequencyShifterParam::Spread), i);
        for (std::size_t c = 0; c < 2; ++c) {
            float real = 0.0F;
            float imaginary = 0.0F;
            m_hilbert[c].process(in[c][i], real, imaginary);
            // Single sideband, every partial up by w. The network's quadrature output
            // lags the in-phase one, so the upper sideband takes the sum.
            const auto cosine = static_cast<float>(dsp::cosTurns(m_phase[c]));
            const auto sine = static_cast<float>(dsp::sinTurns(m_phase[c]));
            out[c][i] = (real * cosine) + (imaginary * sine);
            const double hz = shift + (c == 0 ? -spread : spread);
            m_phase[c] = wrap(m_phase[c] + (hz / context.sampleRate));
        }
    }
}

// --- StereoImager --------------------------------------------------------------------

void StereoImager::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                              std::span<float> outLeft, std::span<float> outRight,
                              const EffectContext& context) noexcept {
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            const double crossover = std::clamp(
                static_cast<double>(context.paramAt(idx(StereoImagerParam::Crossover), i)), 20.0,
                0.45 * context.sampleRate);
            m_coefficients = dsp::svfCoefficients(crossover, 0.7071, context.sampleRate);
        }
        const float width = std::max(0.0F, context.paramAt(idx(StereoImagerParam::Width), i));
        const float lowWidth = std::max(0.0F, context.paramAt(idx(StereoImagerParam::LowWidth), i));
        const float mid = 0.5F * (inLeft[i] + inRight[i]);
        const float side = 0.5F * (inLeft[i] - inRight[i]);
        // Low and high parts of the side add back to the side exactly.
        const float low = m_split.process(side, m_coefficients, dsp::SvfMode::LowPass);
        const float high = side - low;
        const float wide = (low * lowWidth) + (high * width);
        outLeft[i] = mid + wide;
        outRight[i] = mid - wide;
    }
}

} // namespace adx::effects
