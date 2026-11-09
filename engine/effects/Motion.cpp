#include "engine/effects/Motion.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"
#include "engine/effects/GrossBeat.h"
#include "engine/transport/TimeSource.h"

namespace adx::effects {

namespace {

[[nodiscard]] int choiceOf(const EffectContext& c, std::uint32_t index, int top) noexcept {
    return std::clamp(static_cast<int>(dsp::floorExact(c.param(index) + 0.5F)), 0, top);
}

/// A sample `delay` (fractional) behind `write` in a ring, read linearly.
[[nodiscard]] float ringRead(const float* data, std::size_t size, std::size_t write,
                             double delay) noexcept {
    const double whole = dsp::floorExact(delay);
    const auto d = static_cast<std::size_t>(whole);
    const auto frac = static_cast<float>(delay - whole);
    const std::size_t a = (write + size - (d % size)) % size;
    const std::size_t b = (a + size - 1) % size;
    return data[a] + ((data[b] - data[a]) * frac);
}

[[nodiscard]] double bpmOf(const EffectContext& c) noexcept {
    if (c.time == nullptr || c.time->tempo().events.empty()) {
        return 120.0;
    }
    return std::max(1.0, c.time->tempo().bpmAt(c.time->positionTicks()));
}

} // namespace

// --- VolumeShaper -----------------------------------------------------------------------

float shaperCurve(int shape, float phase) noexcept {
    switch (shape) {
    case 1:
        return phase;
    case 2:
        return 1.0F - phase;
    case 3:
        return 0.5F - (0.5F * dsp::cosTurnsF(phase));
    default: {
        // The sidechain pump: silent on the beat, back to unity by 60 % of the cycle.
        const float t = std::min(1.0F, phase / 0.6F);
        const float rest = 1.0F - t;
        return 1.0F - (rest * rest * rest);
    }
    }
}

void VolumeShaper::prepareEffect(const graph::PrepareInfo& info) {
    m_rate = static_cast<double>(info.sampleRate);
    resetEffect();
}

void VolumeShaper::resetEffect() noexcept {
    m_gain = 1.0F;
}

void VolumeShaper::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                              std::span<float> outLeft, std::span<float> outRight,
                              const EffectContext& c) noexcept {
    const int length = choiceOf(c, idx(VolumeShaperParam::Length), 4);
    const int shape = choiceOf(c, idx(VolumeShaperParam::Shape), 3);
    const auto steps = static_cast<std::uint32_t>(
        std::clamp(dsp::floorExact(c.param(idx(VolumeShaperParam::Steps)) + 0.5F), 0.0F, 65535.0F));
    const double cycleBeats = kShaperWholes.at(static_cast<std::size_t>(length)) * 4.0;
    // Rolling: the transport's position, in exact fractional beats (GrossBeat's
    // beatsAt). Otherwise the effect's own clock at the current tempo.
    const bool rolling = c.time != nullptr && c.time->rolling();
    const double beatsPerSample = bpmOf(c) / (60.0 * m_rate);
    const float smooth = static_cast<float>(
        dsp::exp(-1000.0 / (std::max(0.1F, c.param(idx(VolumeShaperParam::Smooth))) * m_rate)));
    for (std::uint32_t i = 0; i < c.frames; ++i) {
        const double beats =
            rolling ? beatsAt(c.time->tempo(), static_cast<double>(c.time->positionSamples() +
                                                                   static_cast<std::int64_t>(i)) /
                                                   m_rate)
                    : static_cast<double>(c.clock + i) * beatsPerSample;
        const double cycles = beats / cycleBeats;
        const auto phase = static_cast<float>(cycles - dsp::floorExact(cycles));
        const float depth = std::clamp(c.paramAt(idx(VolumeShaperParam::Depth), i), 0.0F, 1.0F);
        float target = 0.0F;
        if (steps != 0) {
            const auto gate = static_cast<std::uint32_t>(phase * 16.0F) & 15U;
            target = ((steps >> gate) & 1U) != 0 ? 1.0F : 1.0F - depth;
            m_gain = target + (smooth * (m_gain - target));
        } else {
            m_gain = 1.0F - (depth * (1.0F - shaperCurve(shape, phase)));
        }
        outLeft[i] = inLeft[i] * m_gain;
        outRight[i] = inRight[i] * m_gain;
    }
}

// --- Tape ---------------------------------------------------------------------------------

void Tape::prepareEffect(const graph::PrepareInfo& info) {
    m_rate = static_cast<double>(info.sampleRate);
    m_centre = static_cast<std::uint32_t>(kCentreSeconds * m_rate);
    for (rt::OwnedArray<float>& line : m_line) {
        line.allocate((static_cast<std::size_t>(m_centre) * 2) + 8);
    }
    resetEffect();
}

void Tape::resetEffect() noexcept {
    for (std::size_t c = 0; c < 2; ++c) {
        std::ranges::fill(m_line.at(c).view(), 0.0F);
        m_age.at(c).reset();
    }
    m_noise.reseed(0x7A9E5EEDU);
    m_write = 0;
    m_wow = 0.0;
    m_flutter = 0.0;
}

void Tape::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                      std::span<float> outLeft, std::span<float> outRight,
                      const EffectContext& c) noexcept {
    const bool slow = choiceOf(c, idx(TapeParam::Speed), 1) == 1;
    const double wowRate = c.param(idx(TapeParam::WowRate)) * (slow ? 0.5 : 1.0);
    const double flutterRate = c.param(idx(TapeParam::FlutterRate));
    // Wow swings up to 3 ms (6 at 7.5 ips), flutter up to 0.3 ms.
    const double wowDepth =
        std::clamp(static_cast<double>(c.param(idx(TapeParam::WowDepth))), 0.0, 1.0) * 0.003 *
        (slow ? 2.0 : 1.0) * m_rate;
    const double flutterDepth =
        std::clamp(static_cast<double>(c.param(idx(TapeParam::FlutterDepth))), 0.0, 1.0) * 0.0003 *
        m_rate;
    const double age = std::clamp(static_cast<double>(c.param(idx(TapeParam::Age))), 0.0, 1.0);
    const double cutoff =
        std::min(20000.0 * dsp::pow(0.15, age) * (slow ? 0.6 : 1.0), m_rate * 0.45);
    const dsp::SvfCoefficients ageCoefficients = dsp::svfCoefficients(cutoff, 0.6, m_rate);
    const std::array<std::span<const float>, 2> in = {inLeft, inRight};
    const std::array<std::span<float>, 2> out = {outLeft, outRight};
    const std::size_t size = m_line[0].size();
    for (std::uint32_t i = 0; i < c.frames; ++i) {
        m_wow += wowRate / m_rate;
        m_wow -= dsp::floorExact(m_wow);
        m_flutter += flutterRate / m_rate;
        m_flutter -= dsp::floorExact(m_flutter);
        const double delay = static_cast<double>(m_centre) + (wowDepth * dsp::sinTurns(m_wow)) +
                             (flutterDepth * dsp::sinTurns(m_flutter));
        const float drive =
            1.0F + (4.0F * std::clamp(c.paramAt(idx(TapeParam::Saturation), i), 0.0F, 1.0F));
        const float hiss = std::clamp(c.paramAt(idx(TapeParam::Hiss), i), 0.0F, 1.0F) * 0.01F;
        const float noise = m_noise.next() * hiss;
        for (std::size_t ch = 0; ch < 2; ++ch) {
            m_line.at(ch).view()[m_write] = in.at(ch)[i];
            const float played = ringRead(m_line.at(ch).view().data(), size, m_write, delay);
            const float saturated =
                drive > 1.0F ? dsp::tanhF(played * drive) / dsp::tanhF(drive) : played;
            out.at(ch)[i] =
                m_age.at(ch).process(saturated, ageCoefficients, dsp::SvfMode::LowPass) + noise;
        }
        m_write = (m_write + 1) % size;
    }
}

// --- Ensemble ------------------------------------------------------------------------------

namespace {

struct EnsembleVoicing {
    double rateScale;
    double centreMs;
    double depthMs;
};

/// Juno I, Juno II, I+II (the fast vibrato-ish setting), a BBD and a dimension: rate
/// multipliers on `rate`, the delay centre and swing.
constexpr std::array<EnsembleVoicing, 5> kVoicings = {{
    {.rateScale = 1.0, .centreMs = 7.0, .depthMs = 2.5},
    {.rateScale = 1.6, .centreMs = 7.0, .depthMs = 2.5},
    {.rateScale = 16.0, .centreMs = 6.0, .depthMs = 0.6},
    {.rateScale = 1.0, .centreMs = 12.0, .depthMs = 4.0},
    {.rateScale = 0.7, .centreMs = 5.0, .depthMs = 0.8},
}};

} // namespace

void Ensemble::prepareEffect(const graph::PrepareInfo& info) {
    m_rate = static_cast<double>(info.sampleRate);
    m_line.allocate((static_cast<std::size_t>(0.03 * m_rate) * 2) + 16);
    resetEffect();
}

void Ensemble::resetEffect() noexcept {
    std::ranges::fill(m_line.view(), 0.0F);
    m_write = 0;
    m_phase = 0.0;
}

void Ensemble::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                          std::span<float> outLeft, std::span<float> outRight,
                          const EffectContext& c) noexcept {
    const int voices = std::clamp(
        static_cast<int>(dsp::floorExact(c.param(idx(EnsembleParam::Voices)) + 0.5F)), 2, 8);
    const EnsembleVoicing& voicing =
        kVoicings.at(static_cast<std::size_t>(choiceOf(c, idx(EnsembleParam::Mode), 4)));
    const double rate = c.param(idx(EnsembleParam::Rate)) * voicing.rateScale;
    const double depth =
        std::clamp(static_cast<double>(c.param(idx(EnsembleParam::Depth))), 0.0, 1.0) *
        voicing.depthMs * 0.001 * m_rate;
    const double centre = voicing.centreMs * 0.001 * m_rate;
    const float width = std::clamp(c.param(idx(EnsembleParam::Width)), 0.0F, 1.0F);
    const std::size_t size = m_line.size();
    const float norm = 1.0F / std::sqrt(static_cast<float>(voices) * 0.5F);
    for (std::uint32_t i = 0; i < c.frames; ++i) {
        m_line.view()[m_write] = 0.5F * (inLeft[i] + inRight[i]);
        m_phase += rate / m_rate;
        m_phase -= dsp::floorExact(m_phase);
        float left = 0.0F;
        float right = 0.0F;
        for (int v = 0; v < voices; ++v) {
            const double offset = static_cast<double>(v) / static_cast<double>(voices);
            const double delay = centre + (depth * dsp::sinTurns(m_phase + offset));
            const float tap = ringRead(m_line.view().data(), size, m_write, delay);
            // Voices alternate sides; `width` 0 puts them all in the middle.
            const float side = ((v & 1) == 0 ? 1.0F : -1.0F) * width;
            left += tap * (1.0F + side) * 0.5F;
            right += tap * (1.0F - side) * 0.5F;
        }
        outLeft[i] = left * norm;
        outRight[i] = right * norm;
        m_write = (m_write + 1) % size;
    }
}

// --- Rotary --------------------------------------------------------------------------------

void Rotary::prepareEffect(const graph::PrepareInfo& info) {
    m_rate = static_cast<double>(info.sampleRate);
    m_line.allocate(static_cast<std::size_t>(0.01 * m_rate) + 8);
    resetEffect();
}

void Rotary::resetEffect() noexcept {
    std::ranges::fill(m_line.view(), 0.0F);
    m_crossLow.reset();
    m_crossHigh.reset();
    m_write = 0;
    m_hornPhase = 0.0;
    m_drumPhase = 0.25;
    m_hornHz = 0.8;
    m_drumHz = 0.7;
}

void Rotary::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                        std::span<float> outLeft, std::span<float> outRight,
                        const EffectContext& c) noexcept {
    const bool fast = choiceOf(c, idx(RotaryParam::Speed), 1) == 1;
    const double hornTarget = fast ? 6.7 : 0.8;
    const double drumTarget = fast ? 5.9 : 0.7;
    // The horn is light and gets there in `ramp`; the drum is heavy and takes three times.
    const double ramp = std::max(0.1, static_cast<double>(c.param(idx(RotaryParam::Ramp))));
    const double hornCoefficient = dsp::exp(-1.0 / (ramp * m_rate / 4.0));
    const double drumCoefficient = dsp::exp(-1.0 / (ramp * 3.0 * m_rate / 4.0));
    const double nearness =
        1.0 -
        (0.7 * std::clamp(static_cast<double>(c.param(idx(RotaryParam::Distance))), 0.0, 1.0));
    const double hornSwing = 0.0006 * m_rate * nearness; // the horn's Doppler, samples
    const dsp::SvfCoefficients cross = dsp::svfCoefficients(800.0, 0.707, m_rate);
    const std::size_t size = m_line.size();
    const double centre = 0.002 * m_rate;
    for (std::uint32_t i = 0; i < c.frames; ++i) {
        m_hornHz = hornTarget + (hornCoefficient * (m_hornHz - hornTarget));
        m_drumHz = drumTarget + (drumCoefficient * (m_drumHz - drumTarget));
        m_hornPhase += m_hornHz / m_rate;
        m_hornPhase -= dsp::floorExact(m_hornPhase);
        m_drumPhase += m_drumHz / m_rate;
        m_drumPhase -= dsp::floorExact(m_drumPhase);
        const float drive = dsp::dbToGainF(c.paramAt(idx(RotaryParam::Drive), i));
        float x = 0.5F * (inLeft[i] + inRight[i]);
        if (drive > 1.0F) {
            x = dsp::tanhF(x * drive) / dsp::tanhF(drive);
        }
        const float low = m_crossLow.process(x, cross, dsp::SvfMode::LowPass);
        const float high = m_crossHigh.process(x, cross, dsp::SvfMode::HighPass);
        m_line.view()[m_write] = high;
        const double hornSin = dsp::sinTurns(m_hornPhase);
        const double hornCos = dsp::cosTurns(m_hornPhase);
        const float horn =
            ringRead(m_line.view().data(), size, m_write, centre + (hornSwing * hornSin)) *
            static_cast<float>(1.0 - (0.35 * nearness * (1.0 - hornCos) * 0.5));
        const double drumSin = dsp::sinTurns(m_drumPhase);
        const float drum =
            low * static_cast<float>(1.0 - (0.25 * nearness * (1.0 + drumSin) * 0.5));
        const float hornLevel = c.paramAt(idx(RotaryParam::HornLevel), i);
        const float drumLevel = c.paramAt(idx(RotaryParam::DrumLevel), i);
        // Two microphones either side: the horn pans with its angle.
        const auto pan = static_cast<float>(hornSin * 0.6 * nearness);
        outLeft[i] = (horn * hornLevel * (1.0F + pan)) + (drum * drumLevel);
        outRight[i] = (horn * hornLevel * (1.0F - pan)) + (drum * drumLevel);
        m_write = (m_write + 1) % size;
    }
}

} // namespace adx::effects
