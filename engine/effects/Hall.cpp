#include "engine/effects/Hall.h"

#include <algorithm>
#include <bit>
#include <cmath>

#include "engine/dsp/Math.h"

namespace adx::effects {
namespace {

/// Line lengths at the largest size, plus the modulation's swing and the interpolator.
constexpr double kMaxSizeScale = 2.0;
constexpr double kModulationSeconds = 0.001;
/// The diffusers' lengths, seconds, and their coefficient.
constexpr std::array<double, 4> kDiffuserSeconds{0.0047, 0.0036, 0.0127, 0.0093};
constexpr float kDiffusion = 0.6F;
/// The shimmer's grain: two read heads half a grain apart, crossfaded.
constexpr double kShimmerGrainSeconds = 0.08;
/// The modulation rates, Hz: slow, and no two alike.
constexpr std::array<double, kHallLines> kModRates{0.31, 0.47, 0.53, 0.71, 0.83, 0.97, 1.13, 1.29};
constexpr std::array<float, 3> kShimmerSemitones{12.0F, 7.0F, 19.0F};

std::size_t powerOfTwoAtLeast(double frames) {
    return std::bit_ceil(static_cast<std::size_t>(frames) + 4);
}

/// The 8x8 Hadamard transform, scaled to be orthonormal: x <- H x / sqrt(8).
void hadamard(std::array<float, kHallLines>& x) noexcept {
    for (std::size_t span = 1; span < kHallLines; span *= 2) {
        for (std::size_t i = 0; i < kHallLines; i += 2 * span) {
            for (std::size_t j = i; j < i + span; ++j) {
                const float a = x[j];
                const float b = x[j + span];
                x[j] = a + b;
                x[j + span] = a - b;
            }
        }
    }
    constexpr float kScale = 0.35355339059327376F; // 1 / sqrt(8)
    for (float& v : x) {
        v *= kScale;
    }
}

} // namespace

double hallLineGain(double seconds, double decay) noexcept {
    return dsp::exp2(-3.0 * seconds / std::max(decay, 1e-3) * dsp::kLog2Of10);
}

void Hall::prepareEffect(const graph::PrepareInfo& info) {
    m_rate = static_cast<double>(info.sampleRate);
    const std::size_t lineSize = powerOfTwoAtLeast(
        ((kHallLineSeconds.back() * kMaxSizeScale) + (2.0 * kModulationSeconds)) * m_rate);
    m_lineMask = lineSize - 1;
    for (rt::OwnedArray<float>& line : m_lines) {
        line.allocate(lineSize);
    }
    const std::size_t predelaySize = powerOfTwoAtLeast(0.25 * m_rate);
    m_predelay.allocate(predelaySize);
    m_predelayMask = predelaySize - 1;
    for (std::size_t d = 0; d < kDiffuserSeconds.size(); ++d) {
        m_diffuserLength.at(d) =
            std::max<std::size_t>(1, static_cast<std::size_t>(kDiffuserSeconds.at(d) * m_rate));
        m_diffuser.at(d).allocate(m_diffuserLength.at(d));
    }
    const std::size_t shimmerSize = powerOfTwoAtLeast(2.0 * kShimmerGrainSeconds * m_rate);
    m_shimmer.allocate(shimmerSize);
    m_shimmerMask = shimmerSize - 1;
    resetEffect();
}

void Hall::resetEffect() noexcept {
    for (rt::OwnedArray<float>& line : m_lines) {
        std::ranges::fill(line.view(), 0.0F);
    }
    std::ranges::fill(m_predelay.view(), 0.0F);
    for (rt::OwnedArray<float>& diffuser : m_diffuser) {
        std::ranges::fill(diffuser.view(), 0.0F);
    }
    std::ranges::fill(m_shimmer.view(), 0.0F);
    m_diffuserIndex.fill(0);
    m_damp.fill(0.0F);
    m_modPhase.fill(0.0);
    m_write = 0;
    m_shimmerWrite = 0;
    m_shimmerPhase = 0.0;
}

void Hall::refresh(const EffectContext& context, std::uint32_t frame) noexcept {
    const double size = std::clamp(context.paramAt(idx(HallParam::Size), frame), 0.0F, 1.0F);
    const double scale = 0.25 + ((kMaxSizeScale - 0.25) * size);
    const double decay = std::clamp(context.paramAt(idx(HallParam::Decay), frame), 0.1F, 60.0F);
    m_freeze = context.paramAt(idx(HallParam::Freeze), frame) >= 0.5F;
    m_input = m_freeze ? 0.0F : 1.0F;
    for (std::size_t l = 0; l < kHallLines; ++l) {
        // Whole samples: an unmodulated line then needs no interpolation, which would
        // lose highs on every pass and shorten (or, frozen, slowly kill) the tail.
        m_delay.at(l) = dsp::floorExact((kHallLineSeconds.at(l) * scale * m_rate) + 0.5);
        m_gain.at(l) =
            m_freeze ? 1.0F : static_cast<float>(hallLineGain(m_delay.at(l) / m_rate, decay));
    }
    const double damping = std::clamp(context.paramAt(idx(HallParam::Damping), frame), 0.0F, 1.0F);
    // 0: no filter. 1: a pole at 1 kHz. Frozen: no filter, or the tail would still die.
    const double cutoff = 20000.0 * dsp::exp2(-damping * dsp::log2(20.0));
    m_dampCoefficient = m_freeze || damping <= 0.0
                            ? 0.0F
                            : static_cast<float>(dsp::exp(-dsp::kTwoPi * cutoff / m_rate));
    m_modDepth = m_freeze
                     ? 0.0
                     : std::clamp(context.paramAt(idx(HallParam::Modulation), frame), 0.0F, 1.0F) *
                           kModulationSeconds * m_rate;
    m_predelayFrames = std::min(
        m_predelayMask,
        static_cast<std::size_t>(std::max(0.0F, context.paramAt(idx(HallParam::Predelay), frame)) *
                                 0.001 * m_rate));
}

float Hall::readLine(std::size_t line, double delay) const noexcept {
    const double position = static_cast<double>(m_write) - delay;
    const double base = dsp::floorExact(position);
    const auto fraction = static_cast<float>(position - base);
    const auto index = static_cast<std::size_t>(static_cast<std::int64_t>(base)) & m_lineMask;
    const std::span<const float> ring = m_lines.at(line).view();
    const float a = ring[index];
    if (fraction == 0.0F) {
        return a;
    }
    const float b = ring[(index + 1) & m_lineMask];
    return a + ((b - a) * fraction);
}

float Hall::shimmerOut(float in, float ratio) noexcept {
    // Two read heads sweep backwards through a grain as the write head moves forwards,
    // each faded by a raised cosine, half a grain apart: a delay-line pitch shifter.
    const std::span<float> ring = m_shimmer.view();
    ring[m_shimmerWrite & m_shimmerMask] = in;
    const double grain = kShimmerGrainSeconds * m_rate;
    m_shimmerPhase += (1.0 - static_cast<double>(ratio)) / grain;
    m_shimmerPhase -= dsp::floorExact(m_shimmerPhase);
    float out = 0.0F;
    for (const double offset : {0.0, 0.5}) {
        double phase = m_shimmerPhase + offset;
        phase -= dsp::floorExact(phase);
        const double delay = (phase * grain) + 2.0;
        const double position = static_cast<double>(m_shimmerWrite) - delay;
        const double base = dsp::floorExact(position);
        const auto fraction = static_cast<float>(position - base);
        const auto index = static_cast<std::size_t>(static_cast<std::int64_t>(base));
        const float a = ring[index & m_shimmerMask];
        const float b = ring[(index + 1) & m_shimmerMask];
        const float window = 0.5F - (0.5F * dsp::cosTurnsF(static_cast<float>(phase)));
        out += (a + ((b - a) * fraction)) * window;
    }
    ++m_shimmerWrite;
    return out;
}

// NOLINTNEXTLINE(readability-function-size) - predelay, diffuse, the network, out.
void Hall::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                      std::span<float> outLeft, std::span<float> outRight,
                      const EffectContext& context) noexcept {
    const auto interval = static_cast<std::size_t>(std::clamp(
        static_cast<int>(dsp::floorExact(context.param(idx(HallParam::ShimmerInterval)) + 0.5F)), 0,
        2));
    const float shimmerRatio = dsp::exp2F(kShimmerSemitones.at(interval) / 12.0F);
    const std::span<float> predelay = m_predelay.view();
    std::size_t predelayWrite = m_write & m_predelayMask;
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            refresh(context, i);
        }
        // --- predelay and diffusion, mono in -----------------------------------
        const float mono = 0.5F * (inLeft[i] + inRight[i]) * m_input;
        predelay[predelayWrite] = mono;
        float x = predelay[(predelayWrite - m_predelayFrames) & m_predelayMask];
        predelayWrite = (predelayWrite + 1) & m_predelayMask;
        for (std::size_t d = 0; d < m_diffuser.size(); ++d) {
            const std::span<float> ring = m_diffuser.at(d).view();
            std::size_t& at = m_diffuserIndex.at(d);
            const float delayed = ring[at];
            const float v = x + (kDiffusion * delayed);
            ring[at] = v;
            x = delayed - (kDiffusion * v);
            at = (at + 1) % m_diffuserLength.at(d);
        }

        // --- the network --------------------------------------------------------
        std::array<float, kHallLines> taps{};
        for (std::size_t l = 0; l < kHallLines; ++l) {
            double delay = m_delay.at(l);
            if (m_modDepth > 0.0) {
                m_modPhase.at(l) += kModRates.at(l) / m_rate;
                m_modPhase.at(l) -= dsp::floorExact(m_modPhase.at(l));
                delay += m_modDepth * (1.0 + static_cast<double>(dsp::sinTurnsF(
                                                 static_cast<float>(m_modPhase.at(l)))));
            }
            float tap = readLine(l, delay) * m_gain.at(l);
            if (m_dampCoefficient > 0.0F) {
                m_damp.at(l) = tap + (m_dampCoefficient * (m_damp.at(l) - tap));
                tap = m_damp.at(l);
            }
            taps.at(l) = tap;
        }
        // Two decorrelated outputs, before the mixing.
        const float wetLeft = taps[0] - taps[2] + taps[4] - taps[6];
        const float wetRight = taps[1] - taps[3] + taps[5] - taps[7];

        std::array<float, kHallLines> feedback = taps;
        hadamard(feedback);
        const float shimmer = std::clamp(context.paramAt(idx(HallParam::Shimmer), i), 0.0F, 1.0F);
        float lifted = 0.0F;
        if (shimmer > 0.0F) {
            // The network's sum, an interval up, back in: a fraction short of unity so a
            // shimmer never grows on its own.
            lifted = shimmerOut(0.25F * (wetLeft + wetRight), shimmerRatio) * shimmer * 0.5F;
        }
        const std::size_t at = m_write & m_lineMask;
        for (std::size_t l = 0; l < kHallLines; ++l) {
            m_lines.at(l).view()[at] = feedback.at(l) + x + lifted;
        }
        ++m_write;

        // --- out ------------------------------------------------------------------
        const float width = std::clamp(context.paramAt(idx(HallParam::Width), i), 0.0F, 1.0F);
        const float mid = 0.5F * (wetLeft + wetRight);
        const float side = 0.5F * (wetLeft - wetRight) * width;
        constexpr float kOut = 0.35F;
        outLeft[i] = (mid + side) * kOut;
        outRight[i] = (mid - side) * kOut;
    }
}

} // namespace adx::effects
