#include "engine/dsp/Oscillator.h"

#include <algorithm>

#include "engine/dsp/Math.h"

namespace adx::dsp {

void Oscillator::prepareTables() {
    static_cast<void>(builtinWaveTable(Waveform::Sine));
}

float Oscillator::nextBandLimited(OscShape shape, float increment, float pulseWidth) noexcept {
    const float magnitude = increment < 0.0F ? -increment : increment;
    const std::size_t level = waveTableLevel(magnitude);
    float out = 0.0F;
    switch (shape) {
    case OscShape::Sine:
        out = sinTurnsF(m_phase);
        break;
    case OscShape::Saw:
        out = builtinWaveTable(Waveform::Saw).read(0, level, m_phase);
        break;
    case OscShape::Square:
        out = builtinWaveTable(Waveform::Square).read(0, level, m_phase);
        break;
    case OscShape::Triangle:
        out = builtinWaveTable(Waveform::Triangle).read(0, level, m_phase);
        break;
    case OscShape::Pulse: {
        // Two band-limited saws, one shifted by the width: their difference is a pulse
        // with no aliasing of its own, at any width.
        const float width = std::clamp(pulseWidth, 0.01F, 0.99F);
        float shifted = m_phase + width;
        if (shifted >= 1.0F) {
            shifted -= 1.0F;
        }
        const WaveTable& saw = builtinWaveTable(Waveform::Saw);
        out = saw.read(0, level, m_phase) - saw.read(0, level, shifted) + (2.0F * width) - 1.0F;
        break;
    }
    }
    advance(increment);
    return out;
}

float Oscillator::nextPolyBlep(OscShape shape, float increment, float pulseWidth) noexcept {
    const float phase = m_phase;
    float out = 0.0F;
    switch (shape) {
    case OscShape::Sine:
        out = sinTurnsF(phase);
        break;
    case OscShape::Saw:
        out = (2.0F * phase) - 1.0F - polyBlep(phase, increment);
        break;
    case OscShape::Square:
    case OscShape::Pulse: {
        const float width = shape == OscShape::Square ? 0.5F : std::clamp(pulseWidth, 0.05F, 0.95F);
        out = phase < width ? 1.0F : -1.0F;
        out += polyBlep(phase, increment);
        float shifted = phase + (1.0F - width);
        if (shifted >= 1.0F) {
            shifted -= 1.0F;
        }
        out -= polyBlep(shifted, increment);
        break;
    }
    case OscShape::Triangle: {
        float square = phase < 0.5F ? 1.0F : -1.0F;
        square += polyBlep(phase, increment);
        float shifted = phase + 0.5F;
        if (shifted >= 1.0F) {
            shifted -= 1.0F;
        }
        square -= polyBlep(shifted, increment);
        m_integrator += 4.0F * increment * square;
        m_integrator -= m_integrator * 0.002F; // gentle leak against DC drift
        out = m_integrator;
        break;
    }
    }
    advance(increment);
    return out;
}

} // namespace adx::dsp
