#include "engine/effects/GrossBeat.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"

namespace adx::effects {
namespace {

constexpr double kBufferSeconds = 10.0;

[[nodiscard]] int stepped(const EffectContext& context, std::uint32_t index,
                          std::uint32_t frame) noexcept {
    return static_cast<int>(std::floor(context.paramAt(index, frame) + 0.5F));
}

[[nodiscard]] double fraction(double x) noexcept {
    return x - std::floor(x);
}

/// The gate's level at p, before smoothing: 1 open, 1 - depth closed.
[[nodiscard]] float gateAt(GrossBeatGate gate, double p, double beats, float depth) noexcept {
    switch (gate) {
    case GrossBeatGate::Eighths:
        return fraction(p * beats * 2.0) < 0.5 ? 1.0F : 1.0F - depth;
    case GrossBeatGate::Sixteenths:
        return fraction(p * beats * 4.0) < 0.5 ? 1.0F : 1.0F - depth;
    case GrossBeatGate::Pump:
        // Ducked at each beat, recovering across it: the sidechain-pump shape.
        return 1.0F - (depth * static_cast<float>(1.0 - fraction(p * beats)));
    case GrossBeatGate::None:
        break;
    }
    return 1.0F;
}

} // namespace

double grossBeatMap(GrossBeatPattern pattern, double p) noexcept {
    switch (pattern) {
    case GrossBeatPattern::HalfSpeed:
        return p * 0.5;
    case GrossBeatPattern::ReverseHalf:
        // The second half plays the first half backwards.
        return p < 0.5 ? p : 1.0 - p;
    case GrossBeatPattern::StutterEighth:
        return p - (std::floor(p * 8.0) / 8.0);
    case GrossBeatPattern::TapeStop: {
        // Normal for the first half; then the speed falls linearly to zero at the end.
        if (p < 0.5) {
            return p;
        }
        const double t = p - 0.5;
        return 0.5 + t - (t * t);
    }
    case GrossBeatPattern::RepeatBeat:
        return p - (std::floor(p * 4.0) / 4.0);
    case GrossBeatPattern::TwoThirds:
        return p * (2.0 / 3.0);
    case GrossBeatPattern::Off:
        break;
    }
    return p;
}

double beatsAt(const core::TempoView& tempo, double seconds) noexcept {
    if (!tempo.valid()) {
        return seconds * 2.0; // 120 bpm
    }
    // The last segment starting at or before `seconds`.
    std::size_t i = 0;
    while (i + 1 < tempo.events.size() && tempo.cumSeconds[i + 1] <= seconds) {
        ++i;
    }
    const core::TempoEvent& event = tempo.events[i];
    const double startBeats = static_cast<double>(event.at.value) / static_cast<double>(core::kPpq);
    const double elapsed = seconds - tempo.cumSeconds[i];
    const bool ramps =
        event.ramp && i + 1 < tempo.events.size() && tempo.events[i + 1].bpm != event.bpm;
    if (!ramps) {
        return startBeats + (elapsed * event.bpm / 60.0);
    }
    // Tempo linear in beats from b0 to b1 over span beats: seconds = 60 span / (b1 - b0)
    // ln(bpm / b0), so bpm = b0 exp(seconds (b1 - b0) / (60 span)).
    const core::TempoEvent& next = tempo.events[i + 1];
    const double span =
        static_cast<double>(next.at.value - event.at.value) / static_cast<double>(core::kPpq);
    const double slope = (next.bpm - event.bpm) / span;
    const double bpm = event.bpm * dsp::exp(elapsed * slope / 60.0);
    return startBeats + ((bpm - event.bpm) / slope);
}

void GrossBeat::prepareEffect(const graph::PrepareInfo& info) {
    m_capacity = static_cast<std::size_t>(kBufferSeconds * info.sampleRate);
    for (ModDelay& line : m_buffer) {
        line.allocate(m_capacity + 4);
    }
    m_fadeLength = std::max<std::uint32_t>(1, info.sampleRate / 500); // 2 ms
    m_gainPole = static_cast<float>(dsp::exp(-1.0 / (0.002 * info.sampleRate)));
}

void GrossBeat::resetEffect() noexcept {
    for (ModDelay& line : m_buffer) {
        line.reset();
    }
    m_delay = 0.0;
    m_fadeFrom = 0.0;
    m_fadeLeft = 0;
    m_gain = 1.0F;
}

// NOLINTNEXTLINE(readability-function-size) - position, map, read, crossfade, gate.
void GrossBeat::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                           std::span<float> outLeft, std::span<float> outRight,
                           const EffectContext& context) noexcept {
    const transport::TimeSource* time = context.time;
    const bool rolling = time != nullptr && time->rolling();
    const auto rate = static_cast<double>(context.sampleRate);
    const double maxDelay = static_cast<double>(m_capacity) - 2.0;
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            m_pattern = static_cast<GrossBeatPattern>(std::clamp(
                stepped(context, idx(GrossBeatParam::Pattern), i), 0, kGrossBeatPatterns - 1));
            m_gate = static_cast<GrossBeatGate>(
                std::clamp(stepped(context, idx(GrossBeatParam::Gate), i), 0, 3));
            m_beats = static_cast<double>(
                1 << std::clamp(stepped(context, idx(GrossBeatParam::Length), i), 0, 3));
        }
        m_buffer[0].write(inLeft[i]);
        m_buffer[1].write(inRight[i]);

        double delay = 0.0;
        float gate = 1.0F;
        if (rolling) {
            const double seconds =
                static_cast<double>(time->positionSamples() + static_cast<std::int64_t>(i)) / rate;
            const double beats = beatsAt(time->tempo(), seconds);
            const double p = fraction(beats / m_beats);
            // The cycle's length in samples at the tempo now.
            const double secondsPerBeat =
                60.0 / time->tempo().bpmAt(core::Ticks{
                           static_cast<std::int64_t>(beats * static_cast<double>(core::kPpq))});
            const double cycle = m_beats * secondsPerBeat * rate;
            delay = std::clamp((p - grossBeatMap(m_pattern, p)) * cycle, 0.0, maxDelay);
            gate = gateAt(m_gate, p, m_beats,
                          std::clamp(context.paramAt(idx(GrossBeatParam::Depth), i), 0.0F, 1.0F));
        }
        // A jump in the delay starts a crossfade from where the read was.
        if (std::abs(delay - m_delay) > 2.0) {
            m_fadeFrom = m_delay;
            m_fadeLeft = m_fadeLength;
        }
        m_delay = delay;
        // Delay 0 reads the sample just written: the effect is the identity when off.
        const auto now = static_cast<float>(delay);
        float left = m_buffer[0].read(now);
        float right = m_buffer[1].read(now);
        if (m_fadeLeft > 0) {
            const float t = static_cast<float>(m_fadeLeft) / static_cast<float>(m_fadeLength);
            const auto before = static_cast<float>(m_fadeFrom);
            left = (left * (1.0F - t)) + (m_buffer[0].read(before) * t);
            right = (right * (1.0F - t)) + (m_buffer[1].read(before) * t);
            --m_fadeLeft;
        }
        m_gain = gate + (m_gainPole * (m_gain - gate));
        outLeft[i] = left * m_gain;
        outRight[i] = right * m_gain;
    }
}

} // namespace adx::effects
