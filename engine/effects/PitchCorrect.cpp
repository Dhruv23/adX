#include "engine/effects/PitchCorrect.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "engine/dsp/Math.h"

namespace adx::effects {
namespace {

constexpr float kYinThreshold = 0.15F;
constexpr double kLowestHz = 70.0;
constexpr double kHighestHz = 1000.0;
/// The shifter's ring: room for the longest grain (two periods at 70 Hz, or the fixed
/// 40 ms one) and the heads' offset.
constexpr double kRingSeconds = 0.08;
constexpr double kFixedGrainSeconds = 0.04;

/// Pitch-class masks, bit n set when n semitones above the key is in the scale.
constexpr std::array<std::uint16_t, kPitchCorrectScales> kScaleMasks{
    0xFFF, // chromatic
    (1U << 0U) | (1U << 2U) | (1U << 4U) | (1U << 5U) | (1U << 7U) | (1U << 9U) | (1U << 11U),
    (1U << 0U) | (1U << 2U) | (1U << 3U) | (1U << 5U) | (1U << 7U) | (1U << 8U) | (1U << 10U),
    (1U << 0U) | (1U << 2U) | (1U << 3U) | (1U << 5U) | (1U << 7U) | (1U << 8U) | (1U << 11U),
    (1U << 0U) | (1U << 2U) | (1U << 4U) | (1U << 7U) | (1U << 9U),               // maj. pent.
    (1U << 0U) | (1U << 3U) | (1U << 5U) | (1U << 7U) | (1U << 10U),              // min. pent.
    (1U << 0U) | (1U << 3U) | (1U << 5U) | (1U << 6U) | (1U << 7U) | (1U << 10U), // blues
    (1U << 0U) | (1U << 2U) | (1U << 3U) | (1U << 5U) | (1U << 7U) | (1U << 9U) | (1U << 10U),
};

int choiceOf(const EffectContext& c, PitchCorrectParam p, int highest) noexcept {
    return std::clamp(static_cast<int>(dsp::floorExact(c.param(idx(p)) + 0.5F)), 0, highest);
}

} // namespace

float snapToScale(float midi, int key, int scale) noexcept {
    const std::uint16_t mask =
        kScaleMasks.at(static_cast<std::size_t>(std::clamp(scale, 0, kPitchCorrectScales - 1)));
    const float nearest = dsp::floorExact(midi + 0.5F);
    float best = nearest;
    float bestDistance = 1e9F;
    // Every scale has a degree within six semitones; ties go down.
    for (int step = -6; step <= 6; ++step) {
        const float candidate = nearest + static_cast<float>(step);
        const int pitchClass = (((static_cast<int>(candidate) - key) % 12) + 12) % 12;
        const float distance = std::abs(candidate - midi);
        if (((mask >> static_cast<unsigned>(pitchClass)) & 1U) != 0 && distance < bestDistance) {
            best = candidate;
            bestDistance = distance;
        }
    }
    return best;
}

float yinPitch(std::span<const float> window, double rate, std::span<float> scratch) noexcept {
    const std::size_t tauMin = static_cast<std::size_t>(rate / kHighestHz);
    const std::size_t tauMax =
        std::min(static_cast<std::size_t>(rate / kLowestHz), window.size() / 2);
    if (tauMax <= tauMin + 2 || scratch.size() < tauMax + 1) {
        return 0.0F;
    }
    const std::size_t length = window.size() - tauMax;
    // The difference function and its cumulative-mean normalisation, at once.
    scratch[0] = 1.0F;
    double running = 0.0;
    for (std::size_t tau = 1; tau <= tauMax; ++tau) {
        double sum = 0.0;
        for (std::size_t j = 0; j < length; ++j) {
            const double d = static_cast<double>(window[j]) - static_cast<double>(window[j + tau]);
            sum += d * d;
        }
        running += sum;
        scratch[tau] =
            running > 0.0 ? static_cast<float>(sum * static_cast<double>(tau) / running) : 1.0F;
    }
    for (std::size_t tau = std::max<std::size_t>(tauMin, 2); tau < tauMax; ++tau) {
        if (scratch[tau] >= kYinThreshold) {
            continue;
        }
        while (tau + 1 < tauMax && scratch[tau + 1] < scratch[tau]) {
            ++tau;
        }
        // Parabolic refinement of the dip.
        const double a = scratch[tau - 1];
        const double b = scratch[tau];
        const double c = scratch[tau + 1];
        const double denominator = a - (2.0 * b) + c;
        const double shift = denominator != 0.0 ? 0.5 * (a - c) / denominator : 0.0;
        return static_cast<float>(rate / (static_cast<double>(tau) + std::clamp(shift, -1.0, 1.0)));
    }
    return 0.0F;
}

void PitchCorrect::prepareEffect(const graph::PrepareInfo& info) {
    m_rate = static_cast<double>(info.sampleRate);
    m_analysis.allocate(kYinWindow);
    m_window.allocate(kYinWindow);
    m_scratch.allocate((kYinWindow / 2) + 2);
    const std::size_t ring = std::bit_ceil(static_cast<std::size_t>(kRingSeconds * m_rate) + 8);
    m_ringMask = ring - 1;
    for (rt::OwnedArray<float>& r : m_ring) {
        r.allocate(ring);
    }
    resetEffect();
}

void PitchCorrect::resetEffect() noexcept {
    std::ranges::fill(m_analysis.view(), 0.0F);
    for (rt::OwnedArray<float>& r : m_ring) {
        std::ranges::fill(r.view(), 0.0F);
    }
    m_analysisWrite = 0;
    m_sinceHop = 0;
    m_write = 0;
    m_phase = 0.0;
    m_grain = kFixedGrainSeconds * m_rate;
    m_haveNote = false;
    m_logRatio = 0.0F;
    m_targetLogRatio = 0.0F;
}

void PitchCorrect::analyse(const EffectContext& context, std::uint32_t /*frame*/) noexcept {
    const std::span<const float> ring = m_analysis.view();
    const std::span<float> window = m_window.view();
    for (std::size_t j = 0; j < kYinWindow; ++j) {
        window[j] = ring[(m_analysisWrite + j) % kYinWindow];
    }
    const float hz = yinPitch(window, m_rate, m_scratch.view());
    if (hz <= 0.0F) {
        return; // unvoiced: hold the correction
    }
    const float note = 69.0F + (12.0F * dsp::log2F(hz / 440.0F));
    const float pass = std::clamp(context.param(idx(PitchCorrectParam::VibratoPass)), 0.0F, 1.0F);
    // Smoothing over ~0.25 s of hops at full vibratoPass.
    const float keep = pass * 0.95F;
    m_smoothedNote = m_haveNote ? (keep * m_smoothedNote) + ((1.0F - keep) * note) : note;
    m_haveNote = true;
    const float reference = pass > 0.0F ? m_smoothedNote : note;
    const float target =
        snapToScale(reference, choiceOf(context, PitchCorrectParam::Key, 11),
                    choiceOf(context, PitchCorrectParam::Scale, kPitchCorrectScales - 1));
    const float humanize = std::max(0.0F, context.param(idx(PitchCorrectParam::Humanize)));
    const float error = target - reference;
    m_targetLogRatio = std::abs(error) * 100.0F <= humanize ? 0.0F : error / 12.0F;
    const bool formant = context.param(idx(PitchCorrectParam::Formant)) >= 0.5F;
    m_grain = formant ? std::clamp(2.0 * m_rate / static_cast<double>(hz), 0.004 * m_rate,
                                   (kRingSeconds * 0.45) * m_rate)
                      : kFixedGrainSeconds * m_rate;
}

// NOLINTNEXTLINE(readability-function-size) - analyse, glide, shift.
void PitchCorrect::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                              std::span<float> outLeft, std::span<float> outRight,
                              const EffectContext& context) noexcept {
    const float retune = std::max(0.0F, context.param(idx(PitchCorrectParam::Retune)));
    // One pole reaching 1 - 1/e in `retune`; 0 jumps at once.
    const float glide =
        retune <= 0.0F
            ? 0.0F
            : static_cast<float>(dsp::exp(-1.0 / (static_cast<double>(retune) * 0.001 * m_rate)));
    const std::span<float> analysis = m_analysis.view();
    const std::array<std::span<const float>, 2> in{inLeft, inRight};
    const std::array<std::span<float>, 2> out{outLeft, outRight};
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        analysis[m_analysisWrite] = 0.5F * (inLeft[i] + inRight[i]);
        m_analysisWrite = (m_analysisWrite + 1) % kYinWindow;
        if (++m_sinceHop >= kYinHop) {
            m_sinceHop = 0;
            analyse(context, i);
        }
        m_logRatio = m_targetLogRatio + (glide * (m_logRatio - m_targetLogRatio));
        const double ratio = static_cast<double>(dsp::exp2F(m_logRatio));
        m_phase += (1.0 - ratio) / m_grain;
        m_phase -= dsp::floorExact(m_phase);
        for (std::size_t c = 0; c < 2; ++c) {
            const std::span<float> ring = m_ring.at(c).view();
            ring[m_write & m_ringMask] = in.at(c)[i];
            float y = 0.0F;
            for (const double offset : {0.0, 0.5}) {
                double phase = m_phase + offset;
                phase -= dsp::floorExact(phase);
                const double position = static_cast<double>(m_write) - ((phase * m_grain) + 2.0);
                const double base = dsp::floorExact(position);
                const auto fraction = static_cast<float>(position - base);
                const auto index = static_cast<std::size_t>(static_cast<std::int64_t>(base));
                const float a = ring[index & m_ringMask];
                const float b = ring[(index + 1) & m_ringMask];
                const float window = 0.5F - (0.5F * dsp::cosTurnsF(static_cast<float>(phase)));
                y += (a + ((b - a) * fraction)) * window;
            }
            out.at(c)[i] = y;
        }
        ++m_write;
    }
}

} // namespace adx::effects
