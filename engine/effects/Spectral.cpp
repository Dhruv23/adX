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

/// A phase in turns, wrapped to [0, 1) in double, as the float the unit-range sine
/// takes. The cast can round up to 1.0, which sinUnitF and cosUnitF both accept.
[[nodiscard]] float unitPhase(double turns) noexcept {
    return static_cast<float>(turns - dsp::floorExact(turns));
}

} // namespace

// --- StereoStft ------------------------------------------------------------------------

void StereoStft::allocate(std::size_t size) {
    m_size = size;
    for (std::size_t c = 0; c < 2; ++c) {
        m_input[c].allocate(size);
        m_output[c].allocate(size);
        m_accumulator[c].allocate(size);
        m_spectrum[c].allocate(size);
    }
    m_work.allocate(size);
    m_window.allocate(size);
    const std::span<float> window = m_window.view();
    for (std::size_t n = 0; n < size; ++n) {
        window[n] = static_cast<float>(
            0.5 - (0.5 * dsp::cosTurns(static_cast<double>(n) / static_cast<double>(size))));
    }
    reset();
}

void StereoStft::reset() noexcept {
    for (std::size_t c = 0; c < 2; ++c) {
        std::ranges::fill(m_input[c].view(), 0.0F);
        std::ranges::fill(m_output[c].view(), 0.0F);
        std::ranges::fill(m_accumulator[c].view(), 0.0F);
    }
    m_fill = readOffset();
}

void StereoStft::runFrame(const dsp::Fft& fft) noexcept {
    const std::span<const float> left = m_input[0].view();
    const std::span<const float> right = m_input[1].view();
    const std::span<const float> window = m_window.view();
    const std::span<dsp::Complex> work = m_work.view();
    for (std::size_t n = 0; n < m_size; ++n) {
        work[n] = dsp::Complex{left[n] * window[n], right[n] * window[n]};
    }
    fft.forward(work);
    // z = l + i r transforms to Z = L + i R, and a real signal's spectrum is Hermitian,
    // so L[k] = (Z[k] + conj Z[N-k]) / 2 and R[k] = (Z[k] - conj Z[N-k]) / 2i.
    const std::span<dsp::Complex> outLeft = m_spectrum[0].view();
    const std::span<dsp::Complex> outRight = m_spectrum[1].view();
    for (std::size_t k = 0; k < m_size; ++k) {
        const dsp::Complex z = work[k];
        const dsp::Complex mirror = work[k == 0 ? 0 : m_size - k];
        const float sumRe = z.real() + mirror.real();
        const float sumIm = z.imag() - mirror.imag();
        const float diffRe = z.real() - mirror.real();
        const float diffIm = z.imag() + mirror.imag();
        outLeft[k] = dsp::Complex{0.5F * sumRe, 0.5F * sumIm};
        outRight[k] = dsp::Complex{0.5F * diffIm, -0.5F * diffRe};
    }
}

void StereoStft::finishFrame(const dsp::Fft& fft) noexcept {
    const std::span<dsp::Complex> work = m_work.view();
    const std::span<const dsp::Complex> left = m_spectrum[0].view();
    const std::span<const dsp::Complex> right = m_spectrum[1].view();
    // Packed back as L + i R: both spectra are Hermitian, so the inverse is l + i r.
    for (std::size_t k = 0; k < m_size; ++k) {
        work[k] = dsp::Complex{left[k].real() - right[k].imag(), left[k].imag() + right[k].real()};
    }
    fft.inverse(work);
    const std::span<const float> window = m_window.view();
    const std::span<float> accLeft = m_accumulator[0].view();
    const std::span<float> accRight = m_accumulator[1].view();
    for (std::size_t n = 0; n < m_size; ++n) {
        accLeft[n] += work[n].real() * window[n] / kOverlapGain;
        accRight[n] += work[n].imag() * window[n] / kOverlapGain;
    }
    // The first hop is complete: it plays out next. Then everything moves up a hop.
    const std::size_t step = hop();
    for (std::size_t c = 0; c < 2; ++c) {
        const std::span<float> accumulator = m_accumulator[c].view();
        const std::span<float> output = m_output[c].view();
        std::copy_n(accumulator.begin(), step, output.begin());
        std::copy(accumulator.begin() + static_cast<std::ptrdiff_t>(step), accumulator.end(),
                  accumulator.begin());
        std::fill(accumulator.end() - static_cast<std::ptrdiff_t>(step), accumulator.end(), 0.0F);
        const std::span<float> input = m_input[c].view();
        std::copy(input.begin() + static_cast<std::ptrdiff_t>(step), input.end(), input.begin());
    }
    m_fill = readOffset();
}

// --- PitchShifter ------------------------------------------------------------------------

void PitchShifter::prepareEffect(const graph::PrepareInfo& /*info*/) {
    m_fft.prepare(kPitchFrame);
    m_stft.allocate(kPitchFrame);
}

void PitchShifter::resetEffect() noexcept {
    m_stft.reset();
    for (auto& channel : m_lastPhase) {
        channel.fill(0.0F);
    }
    for (auto& channel : m_sumPhase) {
        channel.fill(0.0);
    }
}

void PitchShifter::shift(std::span<dsp::Complex> spectrum, std::size_t channel) noexcept {
    constexpr auto kOversample = static_cast<float>(kStftOverlap);
    const float ratio = m_ratio;
    // Analysis: each bin's magnitude and true frequency (in bins) from its phase advance.
    for (std::size_t k = 0; k < kBins; ++k) {
        const dsp::Complex x = spectrum[k];
        const float phase = dsp::atan2TurnsF(x.imag(), x.real());
        float delta = phase - m_lastPhase[channel][k];
        m_lastPhase[channel][k] = phase;
        // Less the advance a bin-centred sinusoid would make in a hop, k / 4 turns,
        // folded to +-1/2. Only k / 4's fraction matters to the fold, so that is all
        // that is subtracted: delta stays near 1 in size, where float keeps its bits.
        delta -= static_cast<float>(k % kStftOverlap) / kOversample;
        delta -= dsp::floorExact(delta + 0.5F);
        m_magnitude[k] = magnitudeOf(x);
        m_frequency[k] = static_cast<float>(k) + (delta * kOversample);
    }
    // Move each bin to its new place.
    m_synthMagnitude.fill(0.0F);
    m_synthFrequency.fill(0.0F);
    if (ratio != m_targetsFor) {
        for (std::size_t k = 0; k < kBins; ++k) {
            m_targets[k] = static_cast<std::size_t>(std::lround(static_cast<float>(k) * ratio));
        }
        m_targetsFor = ratio;
    }
    for (std::size_t k = 0; k < kBins; ++k) {
        const std::size_t target = m_targets[k];
        if (target < kBins) {
            m_synthMagnitude[target] += m_magnitude[k];
            m_synthFrequency[target] = m_frequency[k] * ratio;
        }
    }
    // Synthesis: advance each bin's phase at its new frequency. The running phase stays
    // in double, where adding a large step every frame does not wear its bits away.
    for (std::size_t k = 0; k < kBins; ++k) {
        double& sum = m_sumPhase[channel][k];
        sum += static_cast<double>(m_synthFrequency[k]) / static_cast<double>(kOversample);
        sum -= dsp::floorExact(sum);
        const float turns = unitPhase(sum);
        const float magnitude = m_synthMagnitude[k];
        spectrum[k] =
            dsp::Complex{magnitude * dsp::cosUnitF(turns), magnitude * dsp::sinUnitF(turns)};
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
        m_stft.process(inLeft[i], inRight[i], outLeft[i], outRight[i], m_fft,
                       [this](std::span<dsp::Complex> left, std::span<dsp::Complex> right) {
                           shift(left, 0);
                           shift(right, 1);
                       });
    }
}

// --- SpectralFreeze -----------------------------------------------------------------------

void SpectralFreeze::prepareEffect(const graph::PrepareInfo& /*info*/) {
    m_fft.prepare(kFreezeFrame);
    m_stft.allocate(kFreezeFrame);
}

void SpectralFreeze::resetEffect() noexcept {
    m_stft.reset();
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
        return; // the input, untouched: the STFT gives it back
    }
    for (std::size_t k = 0; k < kBins; ++k) {
        // A fresh random phase each frame: the held instant, smeared into a texture.
        const float turns = unitPhase(m_random.nextUnit());
        const bool edge = k == 0 || k + 1 == kBins;
        const dsp::Complex frozen =
            edge ? dsp::Complex{held[k], 0.0F}
                 : dsp::Complex{held[k] * dsp::cosUnitF(turns), held[k] * dsp::sinUnitF(turns)};
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
        const float gain = m_gain(context.paramAt(idx(SpectralFreezeParam::Gain), i));
        float left = 0.0F;
        float right = 0.0F;
        m_stft.process(inLeft[i], inRight[i], left, right, m_fft,
                       [this](std::span<dsp::Complex> l, std::span<dsp::Complex> r) {
                           frame(l, 0);
                           frame(r, 1);
                       });
        outLeft[i] = gain * left;
        outRight[i] = gain * right;
    }
}

} // namespace adx::effects
