#include "engine/effects/Filter.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Interpolate.h"
#include "engine/dsp/Math.h"
#include "engine/effects/Delay.h"
#include "engine/transport/TimeSource.h"

namespace adx::effects {

namespace {

constexpr double kMaxCombSeconds = 0.06; // a comb down to about 17 Hz

[[nodiscard]] float combRead(const rt::OwnedArray<float>& line, std::size_t write,
                             float delay) noexcept {
    const std::span<const float> data = line.view();
    const std::size_t size = data.size();
    const float whole = dsp::floorExact(delay);
    const auto d = static_cast<std::size_t>(whole);
    const float frac = delay - whole;
    const std::size_t a = (write + size - d) % size;
    const std::size_t b = (write + size - d - 1) % size;
    return data[a] + ((data[b] - data[a]) * frac);
}

[[nodiscard]] double bpmOf(const EffectContext& c) noexcept {
    if (c.time == nullptr || c.time->tempo().events.empty()) {
        return 120.0;
    }
    return std::max(1.0, c.time->tempo().bpmAt(c.time->positionTicks()));
}

} // namespace

// --- Filter -------------------------------------------------------------------------

void Filter::prepareEffect(const graph::PrepareInfo& info) {
    m_rate = static_cast<double>(info.sampleRate);
    for (rt::OwnedArray<float>& line : m_comb) {
        line.allocate(static_cast<std::size_t>(kMaxCombSeconds * m_rate) + 4);
    }
    resetEffect();
}

void Filter::resetEffect() noexcept {
    for (std::size_t c = 0; c < 2; ++c) {
        m_ladder.at(c).reset();
        m_svf.at(c).reset();
        m_formant.at(c).reset();
        std::ranges::fill(m_comb.at(c).view(), 0.0F);
    }
    m_combIndex = 0;
    m_lfoPhase = 0.0;
    m_follow = 0.0F;
    m_tuned = false;
}

float Filter::lfo(float phase, int shape) noexcept {
    switch (shape) {
    case 1:
        return phase < 0.5F ? (4.0F * phase) - 1.0F : 3.0F - (4.0F * phase);
    case 2:
        return 1.0F - (2.0F * phase);
    case 3:
        return phase < 0.5F ? 1.0F : -1.0F;
    default:
        return dsp::sinTurnsF(phase);
    }
}

void Filter::retune(const EffectContext& c, float octaves) noexcept {
    const double cutoff = std::clamp(static_cast<double>(c.param(idx(FilterParam::Cutoff))) *
                                         dsp::exp2(static_cast<double>(octaves)),
                                     20.0, m_rate * 0.45);
    const double reso =
        std::clamp(static_cast<double>(c.param(idx(FilterParam::Resonance))), 0.0, 1.0);
    m_ladderCoefficients = dsp::ladderCoefficients(cutoff, reso, m_rate);
    m_svfCoefficients = dsp::svfCoefficients(cutoff, 0.5 + (reso * 19.5), m_rate);
    m_formantCoefficients =
        dsp::svfCoefficients(std::min(cutoff * 2.3, m_rate * 0.45), 4.0 + (reso * 12.0), m_rate);
    m_combDelay =
        static_cast<float>(std::clamp(m_rate / cutoff, 2.0, (kMaxCombSeconds * m_rate) - 2.0));
}

void Filter::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                        std::span<float> outLeft, std::span<float> outRight,
                        const EffectContext& c) noexcept {
    const auto type = static_cast<FilterType>(std::clamp(
        static_cast<int>(dsp::floorExact(c.param(idx(FilterParam::Type)) + 0.5F)), 0, 7));
    const int shape = std::clamp(
        static_cast<int>(dsp::floorExact(c.param(idx(FilterParam::LfoShape)) + 0.5F)), 0, 3);
    const int sync = std::clamp(
        static_cast<int>(dsp::floorExact(c.param(idx(FilterParam::LfoSync)) + 0.5F)), 0, 9);
    const double hz =
        sync > 0
            ? 1.0 / (kDelaySyncWholes.at(static_cast<std::size_t>(sync)) * 4.0 * 60.0 / bpmOf(c))
            : static_cast<double>(c.param(idx(FilterParam::LfoRate)));
    const float depth = c.param(idx(FilterParam::LfoDepth));
    const float followAmount = c.param(idx(FilterParam::FollowAmount));
    const auto coefficient = [this](float ms) {
        return static_cast<float>(dsp::exp(-1000.0 / (std::max(0.01F, ms) * m_rate)));
    };
    const float attack = coefficient(c.param(idx(FilterParam::FollowAttack)));
    const float release = coefficient(c.param(idx(FilterParam::FollowRelease)));
    const bool keyed = c.param(idx(FilterParam::KeyInput)) >= 0.5F && !c.sideLeft.empty();
    const float reso = std::clamp(c.param(idx(FilterParam::Resonance)), 0.0F, 1.0F);
    const float combFeedback = (type == FilterType::CombMinus ? -0.98F : 0.98F) * reso;
    dsp::DbGain drive;
    const std::array<std::span<const float>, 2> in = {inLeft, inRight};
    const std::array<std::span<float>, 2> out = {outLeft, outRight};
    for (std::uint32_t i = 0; i < c.frames; ++i) {
        const float level = keyed ? std::max(std::abs(c.sideLeft[i]), std::abs(c.sideRight[i]))
                                  : std::max(std::abs(inLeft[i]), std::abs(inRight[i]));
        m_follow = level + ((level > m_follow ? attack : release) * (m_follow - level));
        if (c.control(i) || !m_tuned) {
            const float octaves =
                (depth * lfo(static_cast<float>(m_lfoPhase), shape)) + (followAmount * m_follow);
            retune(c, octaves);
            m_tuned = true;
        }
        m_lfoPhase += hz / m_rate;
        m_lfoPhase -= dsp::floorExact(m_lfoPhase);
        const float gain = drive(c.paramAt(idx(FilterParam::Drive), i));
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const float x = in.at(ch)[i] * gain;
            float y = 0.0F;
            switch (type) {
            case FilterType::Ladder:
                y = m_ladder.at(ch).process(x, m_ladderCoefficients, gain - 1.0F);
                break;
            case FilterType::LowPass:
                y = m_svf.at(ch).process(x, m_svfCoefficients, dsp::SvfMode::LowPass);
                break;
            case FilterType::BandPass:
                y = m_svf.at(ch).process(x, m_svfCoefficients, dsp::SvfMode::BandPass);
                break;
            case FilterType::HighPass:
                y = m_svf.at(ch).process(x, m_svfCoefficients, dsp::SvfMode::HighPass);
                break;
            case FilterType::Notch:
                y = m_svf.at(ch).process(x, m_svfCoefficients, dsp::SvfMode::Notch);
                break;
            case FilterType::CombPlus:
            case FilterType::CombMinus: {
                rt::OwnedArray<float>& line = m_comb.at(ch);
                const float delayed = combRead(line, m_combIndex, m_combDelay);
                y = x + (combFeedback * delayed);
                line.view()[m_combIndex] = y;
                y *= 1.0F - std::abs(combFeedback) * 0.5F;
                break;
            }
            case FilterType::Formant:
                y = 0.6F *
                    (m_svf.at(ch).process(x, m_svfCoefficients, dsp::SvfMode::BandPass) +
                     m_formant.at(ch).process(x, m_formantCoefficients, dsp::SvfMode::BandPass));
                break;
            }
            out.at(ch)[i] = y;
        }
        m_combIndex = (m_combIndex + 1) % m_comb[0].size();
    }
}

// --- Resonator ----------------------------------------------------------------------

void Resonator::prepareEffect(const graph::PrepareInfo& info) {
    m_rate = static_cast<double>(info.sampleRate);
    // Down to two octaves below C4 (65 Hz): a period of rate / 65 samples.
    const auto size = static_cast<std::size_t>(m_rate / 60.0) + 4;
    for (auto& lines : m_lines) {
        for (rt::OwnedArray<float>& line : lines) {
            line.allocate(size);
        }
    }
    resetEffect();
}

void Resonator::resetEffect() noexcept {
    for (auto& lines : m_lines) {
        for (rt::OwnedArray<float>& line : lines) {
            std::ranges::fill(line.view(), 0.0F);
        }
    }
    for (auto& damp : m_damp) {
        damp.fill(0.0F);
    }
    m_index = 0;
}

void Resonator::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                           std::span<float> outLeft, std::span<float> outRight,
                           const EffectContext& c) noexcept {
    const auto voices = static_cast<std::size_t>(std::clamp(
        static_cast<int>(dsp::floorExact(c.param(idx(ResonatorParam::Voices)) + 0.5F)), 1, 4));
    std::array<float, kVoices> delay{};
    const std::array<ResonatorParam, kVoices> notes = {
        ResonatorParam::Note1, ResonatorParam::Note2, ResonatorParam::Note3, ResonatorParam::Note4};
    const double maxDelay = static_cast<double>(m_lines[0][0].size()) - 3.0;
    for (std::size_t v = 0; v < kVoices; ++v) {
        const double hz =
            261.6255653 * dsp::exp2(static_cast<double>(c.param(idx(notes.at(v)))) / 12.0);
        delay.at(v) = static_cast<float>(std::clamp(m_rate / hz, 2.0, maxDelay));
    }
    const float feedback = std::clamp(c.param(idx(ResonatorParam::Feedback)), 0.0F, 0.995F);
    const float damping = std::clamp(c.param(idx(ResonatorParam::Damping)), 0.0F, 1.0F) * 0.95F;
    const float norm = (1.0F - feedback) / static_cast<float>(voices) * 2.0F;
    const std::array<std::span<const float>, 2> in = {inLeft, inRight};
    const std::array<std::span<float>, 2> out = {outLeft, outRight};
    const std::size_t size = m_lines[0][0].size();
    for (std::uint32_t i = 0; i < c.frames; ++i) {
        for (std::size_t ch = 0; ch < 2; ++ch) {
            float sum = 0.0F;
            for (std::size_t v = 0; v < voices; ++v) {
                rt::OwnedArray<float>& line = m_lines.at(ch).at(v);
                const float delayed = combRead(line, m_index, delay.at(v));
                // A one-pole low-pass in the loop: damping darkens each pass.
                float& state = m_damp.at(ch).at(v);
                state = delayed + (damping * (state - delayed));
                const float y = in.at(ch)[i] + (feedback * state);
                line.view()[m_index] = y;
                sum += y;
            }
            out.at(ch)[i] = sum * norm;
        }
        m_index = (m_index + 1) % size;
    }
}

} // namespace adx::effects
