#include "engine/effects/fm1/Fm1Effects.h"

#include "engine/dsp/Math.h"

#include <algorithm>
#include <cmath>

namespace adx::effects {

namespace {

[[nodiscard]] float norm(const EffectContext& c, Fm1Param p) noexcept {
    return std::clamp(c.param(static_cast<std::uint32_t>(p)), 0.0F, 127.0F) / 127.0F;
}

/// 0..1 to lo..hi on a log scale.
[[nodiscard]] double logScale(double x, double lo, double hi) noexcept {
    return lo * dsp::pow(hi / lo, x);
}

} // namespace

void Fm1Effects::prepareEffect(const graph::PrepareInfo& info) {
    m_rate = static_cast<double>(info.sampleRate);
    m_dist.prepare(info);
    m_chorus.prepare(info);
    m_phaser.prepare(info);
    m_delay.prepare(info);
    m_reverb.prepare(info);
    m_stageLeft.allocate(info.maxBlockFrames);
    m_stageRight.allocate(info.maxBlockFrames);
    // 100 ms of reverb pre-delay, stereo interleaved.
    m_predelay.allocate((static_cast<std::size_t>(0.1 * m_rate) + 1) * 2);
    resetEffect();
}

void Fm1Effects::resetEffect() noexcept {
    m_dist.reset();
    m_chorus.reset();
    m_phaser.reset();
    m_delay.reset();
    m_reverb.reset();
    for (auto* filters : {&m_filter, &m_distTone}) {
        for (dsp::SvFilter& f : *filters) {
            f.reset();
        }
    }
    std::ranges::fill(m_predelay.view(), 0.0F);
    m_predelayIndex = 0;
}

template<class Stage, std::size_t N>
void Fm1Effects::stage(Stage& effect, const std::array<float, N>& params, float mix,
                       std::span<float> left, std::span<float> right,
                       const EffectContext& context) noexcept {
    if (mix <= 0.0F) {
        return;
    }
    const std::uint32_t frames = context.frames;
    const std::span<float> wetLeft = m_stageLeft.view().first(frames);
    const std::span<float> wetRight = m_stageRight.view().first(frames);
    EffectContext inner = context;
    inner.params = params;
    inner.automation = {};
    effect.processWet(left, right, wetLeft, wetRight, inner);
    for (std::uint32_t i = 0; i < frames; ++i) {
        left[i] = (left[i] * (1.0F - mix)) + (wetLeft[i] * mix);
        right[i] = (right[i] * (1.0F - mix)) + (wetRight[i] * mix);
    }
}

void Fm1Effects::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                            std::span<float> outLeft, std::span<float> outRight,
                            const EffectContext& c) noexcept {
    const std::uint32_t frames = c.frames;
    std::copy_n(inLeft.begin(), frames, outLeft.begin());
    std::copy_n(inRight.begin(), frames, outRight.begin());
    const std::span<float> left = outLeft.first(frames);
    const std::span<float> right = outRight.first(frames);
    const double nyquistSafe = 0.45 * m_rate;

    // Filter: LP / HP / BP. Open (LP, full cutoff, no resonance) is the identity.
    const float type = norm(c, Fm1Param::FilterType);
    const float cutoff = norm(c, Fm1Param::FilterCutoff);
    const float reso = norm(c, Fm1Param::FilterReso);
    dsp::SvfMode mode = dsp::SvfMode::BandPass;
    if (type < 1.0F / 3.0F) {
        mode = dsp::SvfMode::LowPass;
    } else if (type < 2.0F / 3.0F) {
        mode = dsp::SvfMode::HighPass;
    }
    if (mode != dsp::SvfMode::LowPass || cutoff < 1.0F || reso > 0.0F) {
        const dsp::SvfCoefficients k = dsp::svfCoefficients(
            std::min(logScale(cutoff, 20.0, 20000.0), nyquistSafe), 0.6 + (reso * 11.4), m_rate);
        for (std::uint32_t i = 0; i < frames; ++i) {
            left[i] = m_filter[0].process(left[i], k, mode);
            right[i] = m_filter[1].process(right[i], k, mode);
        }
    }

    // Distortion, its wet through a tone low-pass.
    const float distMix = norm(c, Fm1Param::DistMix);
    if (distMix > 0.0F) {
        const std::array<float, 1> drive = {1.0F + (norm(c, Fm1Param::DistDrive) * 29.0F)};
        const std::span<float> wetLeft = m_stageLeft.view().first(frames);
        const std::span<float> wetRight = m_stageRight.view().first(frames);
        EffectContext inner = c;
        inner.params = drive;
        inner.automation = {};
        m_dist.processWet(left, right, wetLeft, wetRight, inner);
        const dsp::SvfCoefficients tone = dsp::svfCoefficients(
            std::min(logScale(norm(c, Fm1Param::DistTone), 1000.0, 20000.0), nyquistSafe), 0.707,
            m_rate);
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float l = m_distTone[0].process(wetLeft[i], tone, dsp::SvfMode::LowPass);
            const float r = m_distTone[1].process(wetRight[i], tone, dsp::SvfMode::LowPass);
            left[i] = (left[i] * (1.0F - distMix)) + (l * distMix);
            right[i] = (right[i] * (1.0F - distMix)) + (r * distMix);
        }
    }

    stage(m_chorus,
          std::array<float, 2>{
              static_cast<float>(logScale(norm(c, Fm1Param::ChorusRate), 0.05, 10.0)),
              norm(c, Fm1Param::ChorusDepth)},
          norm(c, Fm1Param::ChorusMix), left, right, c);

    stage(m_phaser,
          std::array<float, 6>{
              static_cast<float>(logScale(norm(c, Fm1Param::PhaserRate), 0.05, 10.0)),
              norm(c, Fm1Param::PhaserDepth), 2.0F, 800.0F,
              (norm(c, Fm1Param::PhaserFeedback) * 1.9F) - 0.95F, 0.25F},
          norm(c, Fm1Param::PhaserMix), left, right, c);

    const float sync = std::round(norm(c, Fm1Param::DelaySync) * 9.0F);
    stage(m_delay,
          std::array<float, 8>{
              1.0F + (norm(c, Fm1Param::DelayTime) * 1999.0F),
              norm(c, Fm1Param::DelayFeedback) * 0.9F, 0.0F, sync, 20.0F,
              static_cast<float>(logScale(norm(c, Fm1Param::DelayTone), 1000.0, 20000.0)), 0.0F,
              1.0F},
          norm(c, Fm1Param::DelayMix), left, right, c);

    // Reverb: room, hall or plate set where the size and damping sit; pre-delay first.
    const float reverbMix = norm(c, Fm1Param::ReverbMix);
    if (reverbMix > 0.0F) {
        const float kind = norm(c, Fm1Param::ReverbType);
        const float size = norm(c, Fm1Param::ReverbSize);
        const float damping = norm(c, Fm1Param::ReverbDamping);
        float room = 0.15F + (size * 0.45F); // room
        float damp = 0.3F + (damping * 0.6F);
        if (kind >= 2.0F / 3.0F) { // plate: bright, dense
            room = 0.4F + (size * 0.5F);
            damp = damping * 0.3F;
        } else if (kind >= 1.0F / 3.0F) { // hall
            room = 0.55F + (size * 0.43F);
        }
        const std::array<float, 3> params = {std::min(room, 0.98F), damp,
                                             norm(c, Fm1Param::ReverbWidth)};
        const auto delayFrames =
            static_cast<std::size_t>(std::lround(norm(c, Fm1Param::ReverbPredelay) * 0.1 * m_rate));
        const std::span<float> ring = m_predelay.view();
        const std::size_t ringFrames = ring.size() / 2;
        // The reverb's input: the signal so far, `delayFrames` late.
        const std::span<float> inL = m_stageLeft.view().first(frames);
        const std::span<float> inR = m_stageRight.view().first(frames);
        for (std::uint32_t i = 0; i < frames; ++i) {
            ring[(m_predelayIndex * 2)] = left[i];
            ring[(m_predelayIndex * 2) + 1] = right[i];
            const std::size_t read = (m_predelayIndex + ringFrames - delayFrames) % ringFrames;
            inL[i] = ring[read * 2];
            inR[i] = ring[(read * 2) + 1];
            m_predelayIndex = (m_predelayIndex + 1) % ringFrames;
        }
        // The stage buffers hold the reverb's input, so its wet goes through a small
        // buffer of its own, a chunk at a time.
        EffectContext inner = c;
        inner.params = params;
        inner.automation = {};
        std::array<float, 256> wetL{};
        std::array<float, 256> wetR{};
        for (std::uint32_t at = 0; at < frames; at += 256) {
            const std::uint32_t n = std::min<std::uint32_t>(256, frames - at);
            inner.frames = n;
            inner.clock = c.clock + at;
            m_reverb.processWet(inL.subspan(at, n), inR.subspan(at, n),
                                std::span<float>(wetL).first(n), std::span<float>(wetR).first(n),
                                inner);
            for (std::uint32_t i = 0; i < n; ++i) {
                left[at + i] = (left[at + i] * (1.0F - reverbMix)) + (wetL[i] * reverbMix);
                right[at + i] = (right[at + i] * (1.0F - reverbMix)) + (wetR[i] * reverbMix);
            }
        }
    }
}

} // namespace adx::effects
