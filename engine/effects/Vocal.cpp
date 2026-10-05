#include "engine/effects/Vocal.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"

namespace adx::effects {
namespace {

constexpr double kLowest = 100.0;
constexpr double kHighest = 8000.0;
constexpr double kSibilanceHz = 5000.0;
/// Restores the level a bank of narrow bands loses: two narrow band-passes in series
/// pass a sliver of each band's energy.
constexpr float kMakeup = 6.0F;

[[nodiscard]] int stepped(const EffectContext& context, std::uint32_t index,
                          std::uint32_t frame) noexcept {
    return static_cast<int>(std::floor(context.paramAt(index, frame) + 0.5F));
}

[[nodiscard]] float pole(float ms, std::uint32_t rate) noexcept {
    const double samples = std::max(1.0, static_cast<double>(ms) * 0.001 * rate);
    return static_cast<float>(dsp::exp(-1.0 / samples));
}

} // namespace

// --- Vocoder -------------------------------------------------------------------------

void Vocoder::prepareEffect(const graph::PrepareInfo& info) {
    dsp::Oscillator::prepareTables();
    m_sibilanceCoefficients = dsp::svfCoefficients(std::min(kSibilanceHz, 0.45 * info.sampleRate),
                                                   0.7071, info.sampleRate);
}

void Vocoder::resetEffect() noexcept {
    for (dsp::SvFilter& f : m_analysis) {
        f.reset();
    }
    for (auto& channel : m_synthesis) {
        for (dsp::SvFilter& f : channel) {
            f.reset();
        }
    }
    m_envelope = {};
    m_sibilanceFilter.reset();
    m_saw.reset(0.0F);
    m_noise.reseed(0x70C0DEU);
}

void Vocoder::refresh(const EffectContext& context, std::uint32_t frame) noexcept {
    m_bands = static_cast<std::size_t>(std::clamp(stepped(context, idx(VocoderParam::Bands), frame),
                                                  16, static_cast<int>(kVocoderMaxBands)));
    m_carrier = static_cast<VocoderCarrier>(
        std::clamp(stepped(context, idx(VocoderParam::Carrier), frame), 0, 3));
    m_increment = context.paramAt(idx(VocoderParam::CarrierPitch), frame) /
                  static_cast<float>(context.sampleRate);
    m_attack = pole(context.paramAt(idx(VocoderParam::Attack), frame), context.sampleRate);
    m_release = pole(context.paramAt(idx(VocoderParam::Release), frame), context.sampleRate);
    m_freeze = context.paramAt(idx(VocoderParam::Freeze), frame) >= 0.5F;
    // Q from the spacing: bands an octave-fraction apart, widened by `bandwidth`.
    const double octaves = dsp::log2(kHighest / kLowest);
    const double spacing = octaves / static_cast<double>(m_bands - 1);
    const double width =
        spacing *
        std::clamp(static_cast<double>(context.paramAt(idx(VocoderParam::Bandwidth), frame)), 0.25,
                   4.0);
    // Q of a band-pass `width` octaves wide: 2^(w/2) / (2^w - 1).
    const double q = dsp::exp2(width * 0.5) / (dsp::exp2(width) - 1.0);
    const double shift = dsp::exp2(
        static_cast<double>(context.paramAt(idx(VocoderParam::FormantShift), frame)) / 12.0);
    const double limit = 0.45 * context.sampleRate;
    for (std::size_t b = 0; b < m_bands; ++b) {
        const double hz = kLowest * dsp::exp2(spacing * static_cast<double>(b));
        m_analysisCoefficients[b] =
            dsp::svfCoefficients(std::min(hz, limit), q, context.sampleRate);
        m_synthesisCoefficients[b] =
            dsp::svfCoefficients(std::clamp(hz * shift, 20.0, limit), q, context.sampleRate);
    }
}

// NOLINTNEXTLINE(readability-function-size) - analysis, carrier, synthesis, sibilance.
void Vocoder::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                         std::span<float> outLeft, std::span<float> outRight,
                         const EffectContext& context) noexcept {
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            refresh(context, i);
        }
        const float modulator = 0.5F * (inLeft[i] + inRight[i]);

        // The carrier, per side.
        float carrierLeft = 0.0F;
        float carrierRight = 0.0F;
        switch (m_carrier) {
        case VocoderCarrier::Sidechain:
            carrierLeft = context.sideLeft[i];
            carrierRight = context.sideRight[i];
            break;
        case VocoderCarrier::Saw:
            carrierLeft = m_saw.nextBandLimited(dsp::OscShape::Saw, m_increment);
            carrierRight = carrierLeft;
            break;
        case VocoderCarrier::Noise:
            carrierLeft = m_noise.next();
            carrierRight = carrierLeft;
            break;
        case VocoderCarrier::SawNoise:
            carrierLeft = (0.7F * m_saw.nextBandLimited(dsp::OscShape::Saw, m_increment)) +
                          (0.3F * m_noise.next());
            carrierRight = carrierLeft;
            break;
        }

        float left = 0.0F;
        float right = 0.0F;
        for (std::size_t b = 0; b < m_bands; ++b) {
            const dsp::SvfCoefficients& a = m_analysisCoefficients[b];
            const float band = a.k * m_analysis[b].tick(modulator, a).band;
            if (!m_freeze) {
                const float level = std::abs(band);
                const float coefficient = level > m_envelope[b] ? m_attack : m_release;
                m_envelope[b] = level + (coefficient * (m_envelope[b] - level));
            }
            const dsp::SvfCoefficients& s = m_synthesisCoefficients[b];
            left += m_envelope[b] * s.k * m_synthesis[0][b].tick(carrierLeft, s).band;
            right += m_envelope[b] * s.k * m_synthesis[1][b].tick(carrierRight, s).band;
        }
        // The sibilance no carrier has: the modulator's top, straight through.
        const float hiss =
            m_sibilanceFilter.process(modulator, m_sibilanceCoefficients, dsp::SvfMode::HighPass) *
            std::clamp(context.paramAt(idx(VocoderParam::Sibilance), i), 0.0F, 1.0F);
        const float gain = dsp::dbToGainF(context.paramAt(idx(VocoderParam::Gain), i));
        outLeft[i] = ((left * kMakeup) + hiss) * gain;
        outRight[i] = ((right * kMakeup) + hiss) * gain;
    }
}

// --- FormantFilter ---------------------------------------------------------------------

void FormantFilter::resetEffect() noexcept {
    for (dsp::FormantBank& bank : m_bank) {
        bank.reset();
    }
    m_lfo = dsp::Lfo{0xF0E3A7U};
    m_lfo.reset(0.0F);
}

void FormantFilter::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                               std::span<float> outLeft, std::span<float> outRight,
                               const EffectContext& context) noexcept {
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        const float lfo = m_lfo.next(dsp::LfoShape::Sine, m_increment);
        if (context.control(i)) {
            m_increment = context.paramAt(idx(FormantFilterParam::LfoRate), i) /
                          static_cast<float>(context.sampleRate);
            m_depth = std::clamp(context.paramAt(idx(FormantFilterParam::LfoDepth), i), 0.0F, 1.0F);
            const auto vowelA = static_cast<std::size_t>(
                std::clamp(stepped(context, idx(FormantFilterParam::VowelA), i), 0, 4));
            const auto vowelB = static_cast<std::size_t>(
                std::clamp(stepped(context, idx(FormantFilterParam::VowelB), i), 0, 4));
            const auto set = stepped(context, idx(FormantFilterParam::Set), i) >= 1
                                 ? dsp::VowelSet::Tenor5
                                 : dsp::VowelSet::Classic3;
            const float morph = std::clamp(context.paramAt(idx(FormantFilterParam::Morph), i) +
                                               (0.5F * m_depth * lfo),
                                           0.0F, 1.0F);
            const float shift =
                dsp::exp2F(context.paramAt(idx(FormantFilterParam::Shift), i) / 12.0F);
            for (dsp::FormantBank& bank : m_bank) {
                bank.configure(set, vowelA, vowelB, morph, context.sampleRate, shift);
            }
        }
        const float gain = dsp::dbToGainF(context.paramAt(idx(FormantFilterParam::Gain), i));
        outLeft[i] = m_bank[0].process(inLeft[i]) * gain;
        outRight[i] = m_bank[1].process(inRight[i]) * gain;
    }
}

} // namespace adx::effects
