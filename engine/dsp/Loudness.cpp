#include "engine/dsp/Loudness.h"

#include <algorithm>

#include "engine/dsp/Math.h"

namespace adx::dsp {
namespace {

/// -0.691 + 10 log10(mean square), the standard's loudness of one block.
[[nodiscard]] float lufsOf(double meanSquare) noexcept {
    if (meanSquare <= 1e-20) {
        return kSilentLufs;
    }
    return static_cast<float>(-0.691 + (10.0 * dsp::log10(meanSquare)));
}

} // namespace

void LoudnessMeter::kWeighting(std::uint32_t sampleRate, KBiquad& shelf,
                               KBiquad& highPass) noexcept {
    // BS.1770-4 Annex 1's two stages, re-derived for `sampleRate` from their analogue
    // prototypes (the derivation libebur128 uses): at 48 kHz they reproduce the
    // standard's published coefficients to the printed precision.
    const auto rate = static_cast<double>(sampleRate);
    {
        constexpr double kF0 = 1681.974450955533;
        constexpr double kGainDb = 3.999843853973347;
        constexpr double kQ = 0.7071752369554196;
        const double k = dsp::tanTurns(0.5 * kF0 / rate);
        const double vh = dsp::pow(10.0, kGainDb / 20.0);
        const double vb = dsp::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + (k / kQ) + (k * k);
        shelf.b0 = (vh + (vb * k / kQ) + (k * k)) / a0;
        shelf.b1 = 2.0 * ((k * k) - vh) / a0;
        shelf.b2 = (vh - (vb * k / kQ) + (k * k)) / a0;
        shelf.a1 = 2.0 * ((k * k) - 1.0) / a0;
        shelf.a2 = (1.0 - (k / kQ) + (k * k)) / a0;
    }
    {
        constexpr double kF0 = 38.13547087602444;
        constexpr double kQ = 0.5003270373238773;
        const double k = dsp::tanTurns(0.5 * kF0 / rate);
        const double a0 = 1.0 + (k / kQ) + (k * k);
        highPass.b0 = 1.0;
        highPass.b1 = -2.0;
        highPass.b2 = 1.0;
        highPass.a1 = 2.0 * ((k * k) - 1.0) / a0;
        highPass.a2 = (1.0 - (k / kQ) + (k * k)) / a0;
    }
}

void LoudnessMeter::prepare(std::uint32_t sampleRate) {
    kWeighting(sampleRate, m_shelf[0], m_highPass[0]);
    m_shelf[1] = m_shelf[0];
    m_highPass[1] = m_highPass[0];
    m_stepLength = std::max<std::uint32_t>(1, sampleRate / 10);
    const auto bins =
        static_cast<std::size_t>((kHistogramCeiling - kHistogramFloor) / kBinWidth) + 1;
    m_binCount.allocate(bins);
    m_binEnergy.allocate(bins);
    reset();
}

void LoudnessMeter::reset() noexcept {
    for (std::size_t c = 0; c < 2; ++c) {
        m_shelf[c].z1 = m_shelf[c].z2 = 0.0;
        m_highPass[c].z1 = m_highPass[c].z2 = 0.0;
    }
    m_inStep = 0;
    m_stepEnergy = 0.0;
    m_steps.fill(0.0);
    m_stepCursor = 0;
    m_stepsSeen = 0;
    m_momentary = kSilentLufs;
    m_shortTerm = kSilentLufs;
    std::ranges::fill(m_binCount.view(), 0U);
    std::ranges::fill(m_binEnergy.view(), 0.0);
}

void LoudnessMeter::process(std::span<const float> left, std::span<const float> right) noexcept {
    for (std::size_t i = 0; i < left.size(); ++i) {
        const double l = m_highPass[0].process(m_shelf[0].process(left[i]));
        const double r = m_highPass[1].process(m_shelf[1].process(right[i]));
        m_stepEnergy += (l * l) + (r * r);
        if (++m_inStep == m_stepLength) {
            finishStep();
        }
    }
    // Flush filter states that have decayed into denormals: a meter left running on
    // silence must not slow the callback.
    for (std::size_t c = 0; c < 2; ++c) {
        for (KBiquad* f : {&m_shelf[c], &m_highPass[c]}) {
            if (f->z1 < 1e-30 && f->z1 > -1e-30) {
                f->z1 = 0.0;
            }
            if (f->z2 < 1e-30 && f->z2 > -1e-30) {
                f->z2 = 0.0;
            }
        }
    }
}

void LoudnessMeter::finishStep() noexcept {
    m_steps[m_stepCursor] = m_stepEnergy / static_cast<double>(m_stepLength);
    m_stepCursor = (m_stepCursor + 1) % kSteps;
    ++m_stepsSeen;
    m_inStep = 0;
    m_stepEnergy = 0.0;

    const auto meanOfLast = [this](std::size_t count) {
        double sum = 0.0;
        for (std::size_t s = 1; s <= count; ++s) {
            sum += m_steps[(m_stepCursor + kSteps - s) % kSteps];
        }
        return sum / static_cast<double>(count);
    };
    if (m_stepsSeen < 4) {
        return; // no full 400 ms block yet
    }
    const double block = meanOfLast(4);
    m_momentary = lufsOf(block);
    m_shortTerm = m_stepsSeen >= kSteps ? lufsOf(meanOfLast(kSteps)) : kSilentLufs;

    // One 400 ms block per 100 ms step: the 75 % overlap the standard specifies.
    if (m_momentary > kHistogramFloor) {
        const auto bin =
            static_cast<std::size_t>(std::clamp((m_momentary - kHistogramFloor) / kBinWidth, 0.0F,
                                                static_cast<float>(m_binCount.size() - 1)));
        ++m_binCount.view()[bin];
        m_binEnergy.view()[bin] += block;
    }
}

float LoudnessMeter::integrated() const noexcept {
    const std::span<const std::uint32_t> counts = m_binCount.view();
    const std::span<const double> energies = m_binEnergy.view();
    std::uint64_t blocks = 0;
    double energy = 0.0;
    for (std::size_t b = 0; b < counts.size(); ++b) {
        blocks += counts[b];
        energy += energies[b];
    }
    if (blocks == 0) {
        return kSilentLufs;
    }
    // The relative gate: 10 LU below the loudness of everything above the absolute gate.
    const float threshold = lufsOf(energy / static_cast<double>(blocks)) - 10.0F;
    const auto first = static_cast<std::size_t>(std::clamp(
        (threshold - kHistogramFloor) / kBinWidth, 0.0F, static_cast<float>(counts.size())));
    blocks = 0;
    energy = 0.0;
    for (std::size_t b = first; b < counts.size(); ++b) {
        blocks += counts[b];
        energy += energies[b];
    }
    return blocks == 0 ? kSilentLufs : lufsOf(energy / static_cast<double>(blocks));
}

} // namespace adx::dsp
