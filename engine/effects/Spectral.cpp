#include "engine/effects/Spectral.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"

namespace adx::effects {
namespace {

/// Periodic Hann at 75% overlap: the squared windows of overlapping frames sum to 1.5.
constexpr float kOverlapGain = 1.5F;

[[nodiscard]] float magnitudeOf(dsp::Complex c) noexcept {
    return std::sqrt((c.real() * c.real()) + (c.imag() * c.imag()));
}

} // namespace

// --- StftChannel -----------------------------------------------------------------------

void StftChannel::allocate(std::size_t size) {
    m_size = size;
    m_input.allocate(size);
    m_output.allocate(size);
    m_accumulator.allocate(size);
    m_work.allocate(size);
    m_window.allocate(size);
    const std::span<float> window = m_window.view();
    for (std::size_t n = 0; n < size; ++n) {
        window[n] = static_cast<float>(
            0.5 - (0.5 * dsp::cosTurns(static_cast<double>(n) / static_cast<double>(size))));
    }
    reset();
}

void StftChannel::reset() noexcept {
    std::ranges::fill(m_input.view(), 0.0F);
    std::ranges::fill(m_output.view(), 0.0F);
    std::ranges::fill(m_accumulator.view(), 0.0F);
    m_fill = readOffset();
}

void StftChannel::runFrame(const dsp::Fft& fft) noexcept {
    const std::span<const float> input = m_input.view();
    const std::span<const float> window = m_window.view();
    const std::span<dsp::Complex> work = m_work.view();
    for (std::size_t n = 0; n < m_size; ++n) {
        work[n] = dsp::Complex{input[n] * window[n], 0.0F};
    }
    fft.forward(work);
}

void StftChannel::finishFrame(const dsp::Fft& fft) noexcept {
    const std::span<dsp::Complex> work = m_work.view();
    fft.inverse(work);
    const std::span<const float> window = m_window.view();
    const std::span<float> accumulator = m_accumulator.view();
    for (std::size_t n = 0; n < m_size; ++n) {
        accumulator[n] += work[n].real() * window[n] / kOverlapGain;
    }
    // The first hop is complete: it plays out next. Then everything moves up a hop.
    const std::size_t step = hop();
    const std::span<float> output = m_output.view();
    std::copy_n(accumulator.begin(), step, output.begin());
    std::copy(accumulator.begin() + static_cast<std::ptrdiff_t>(step), accumulator.end(),
              accumulator.begin());
    std::fill(accumulator.end() - static_cast<std::ptrdiff_t>(step), accumulator.end(), 0.0F);
    const std::span<float> input = m_input.view();
    std::copy(input.begin() + static_cast<std::ptrdiff_t>(step), input.end(), input.begin());
    m_fill = readOffset();
}

// --- PitchShifter ------------------------------------------------------------------------

void PitchShifter::prepareEffect(const graph::PrepareInfo& /*info*/) {
    m_fft.prepare(kPitchFrame);
    for (StftChannel& stft : m_stft) {
        stft.allocate(kPitchFrame);
    }
}

void PitchShifter::resetEffect() noexcept {
    for (StftChannel& stft : m_stft) {
        stft.reset();
    }
    for (auto& channel : m_lastPhase) {
        channel.fill(0.0);
    }
    for (auto& channel : m_sumPhase) {
        channel.fill(0.0);
    }
}

void PitchShifter::shift(std::span<dsp::Complex> spectrum, std::size_t channel) noexcept {
    constexpr auto kOversample = static_cast<double>(kStftOverlap);
    const float ratio = m_ratio;
    // Analysis: each bin's magnitude and true frequency (in bins) from its phase advance.
    for (std::size_t k = 0; k < kBins; ++k) {
        const dsp::Complex x = spectrum[k];
        const double phase = dsp::atan2Turns(x.imag(), x.real());
        double delta = phase - m_lastPhase[channel][k];
        m_lastPhase[channel][k] = phase;
        // Less the advance a bin-centred sinusoid would make in a hop, folded to +-1/2.
        delta -= static_cast<double>(k) / kOversample;
        delta -= std::floor(delta + 0.5);
        m_magnitude[k] = magnitudeOf(x);
        m_frequency[k] = static_cast<float>(static_cast<double>(k) + (delta * kOversample));
    }
    // Move each bin to its new place.
    m_synthMagnitude.fill(0.0F);
    m_synthFrequency.fill(0.0F);
    for (std::size_t k = 0; k < kBins; ++k) {
        const auto target = static_cast<std::size_t>(std::lround(static_cast<float>(k) * ratio));
        if (target < kBins) {
            m_synthMagnitude[target] += m_magnitude[k];
            m_synthFrequency[target] = m_frequency[k] * ratio;
        }
    }
    // Synthesis: advance each bin's phase at its new frequency.
    for (std::size_t k = 0; k < kBins; ++k) {
        double& sum = m_sumPhase[channel][k];
        sum += static_cast<double>(m_synthFrequency[k]) / kOversample;
        sum -= std::floor(sum);
        const float magnitude = m_synthMagnitude[k];
        spectrum[k] = dsp::Complex{magnitude * static_cast<float>(dsp::cosTurns(sum)),
                                   magnitude * static_cast<float>(dsp::sinTurns(sum))};
    }
    for (std::size_t k = 1; k + 1 < kBins; ++k) {
        spectrum[kPitchFrame - k] = std::conj(spectrum[k]);
    }
}

void PitchShifter::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                              std::span<float> outLeft, std::span<float> outRight,
                              const EffectContext& context) noexcept {
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            const float semitones = context.paramAt(idx(PitchShifterParam::Semitones), i) +
                                    (context.paramAt(idx(PitchShifterParam::Cents), i) * 0.01F);
            m_ratio = dsp::exp2F(std::clamp(semitones, -36.0F, 36.0F) / 12.0F);
        }
        outLeft[i] =
            m_stft[0].process(inLeft[i], m_fft, [this](std::span<dsp::Complex> s) { shift(s, 0); });
        outRight[i] = m_stft[1].process(inRight[i], m_fft,
                                        [this](std::span<dsp::Complex> s) { shift(s, 1); });
    }
}

// --- SpectralFreeze -----------------------------------------------------------------------

void SpectralFreeze::prepareEffect(const graph::PrepareInfo& /*info*/) {
    m_fft.prepare(kFreezeFrame);
    for (StftChannel& stft : m_stft) {
        stft.allocate(kFreezeFrame);
    }
}

void SpectralFreeze::resetEffect() noexcept {
    for (StftChannel& stft : m_stft) {
        stft.reset();
    }
    for (auto& held : m_held) {
        held.fill(0.0F);
    }
    m_random.reseed(0xF4EE2EU);
    m_frozen = false;
    m_capture = {};
    m_blend = {};
}

void SpectralFreeze::frame(std::span<dsp::Complex> spectrum, std::size_t channel) noexcept {
    std::array<float, kBins>& held = m_held[channel];
    if (m_capture[channel]) {
        for (std::size_t k = 0; k < kBins; ++k) {
            held[k] = magnitudeOf(spectrum[k]);
        }
        m_capture[channel] = false;
    }
    float& blend = m_blend[channel];
    blend = m_frozen ? std::min(1.0F, blend + 0.25F) : std::max(0.0F, blend - 0.25F);
    if (blend <= 0.0F) {
        return; // the input, untouched: the STFT gives it back exactly
    }
    for (std::size_t k = 0; k < kBins; ++k) {
        // A fresh random phase each frame: the held instant, smeared into a texture.
        const double turns = m_random.nextUnit();
        const bool edge = k == 0 || k + 1 == kBins;
        const dsp::Complex frozen =
            edge ? dsp::Complex{held[k], 0.0F}
                 : dsp::Complex{held[k] * static_cast<float>(dsp::cosTurns(turns)),
                                held[k] * static_cast<float>(dsp::sinTurns(turns))};
        spectrum[k] = (spectrum[k] * (1.0F - blend)) + (frozen * blend);
    }
    for (std::size_t k = 1; k + 1 < kBins; ++k) {
        spectrum[kFreezeFrame - k] = std::conj(spectrum[k]);
    }
}

void SpectralFreeze::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                                std::span<float> outLeft, std::span<float> outRight,
                                const EffectContext& context) noexcept {
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            const bool frozen = context.paramAt(idx(SpectralFreezeParam::Freeze), i) >= 0.5F;
            if (frozen && !m_frozen) {
                m_capture = {true, true};
            }
            m_frozen = frozen;
        }
        const float gain = dsp::dbToGainF(context.paramAt(idx(SpectralFreezeParam::Gain), i));
        outLeft[i] = gain * m_stft[0].process(inLeft[i], m_fft,
                                              [this](std::span<dsp::Complex> s) { frame(s, 0); });
        outRight[i] = gain * m_stft[1].process(inRight[i], m_fft,
                                               [this](std::span<dsp::Complex> s) { frame(s, 1); });
    }
}

} // namespace adx::effects
