// Low-frequency oscillator: six shapes, a phase offset, and three ways to keep time.
//
//   free     runs from wherever it is, at a rate in Hz.
//   retrig   restarts at its phase offset on every note - the voice calls reset().
//   sync     its phase is a function of the timeline: the cycle count at the current
//            position, from the node's TimeSource. Nothing is carried from block to
//            block, so a seek lands the LFO where it would have been, and two time
//            sources at two tempos each get their own (phase_3.md §2). This is the
//            "node whose output depends on the timeline position" P3-5 asked for,
//            solved by having no state to reset.
//
// Output is in [-1, 1].
#pragma once

#include <cstdint>

#include "engine/dsp/Math.h"
#include "engine/dsp/Noise.h"

namespace adx::dsp {

enum class LfoShape : std::uint8_t { Sine, Triangle, SawUp, SawDown, Square, SampleHold };
inline constexpr std::uint8_t kLfoShapeCount = 6;

enum class LfoMode : std::uint8_t { Free, Retrig, Sync };

class Lfo {
public:
    explicit Lfo(std::uint32_t seed = 0x51ED270BU) noexcept : m_noise(seed) {}

    /// Restarts at `offset` turns: a retriggered LFO's note-on.
    void reset(float offset = 0.0F) noexcept {
        m_phase = wrap(offset);
        m_held = m_noise.next();
    }

    /// Sync mode: the phase for a timeline position of `cycles` cycles of the LFO, plus
    /// the offset. Call once per sample (or per control frame) before value().
    void syncTo(double cycles, float offset) noexcept {
        const double whole = std::floor(cycles);
        const auto next = wrap(static_cast<float>(cycles - whole) + offset);
        if (next < m_phase) {
            m_held = m_noise.next(); // a new cycle: a new held value
        }
        m_phase = next;
    }

    /// The value at the current phase.
    [[nodiscard]] float value(LfoShape shape) const noexcept {
        switch (shape) {
        case LfoShape::Sine:
            return sinTurnsF(m_phase);
        case LfoShape::Triangle:
            return m_phase < 0.25F   ? 4.0F * m_phase
                   : m_phase < 0.75F ? 2.0F - (4.0F * m_phase)
                                     : (4.0F * m_phase) - 4.0F;
        case LfoShape::SawUp:
            return (2.0F * m_phase) - 1.0F;
        case LfoShape::SawDown:
            return 1.0F - (2.0F * m_phase);
        case LfoShape::Square:
            return m_phase < 0.5F ? 1.0F : -1.0F;
        case LfoShape::SampleHold:
            return m_held;
        }
        return 0.0F;
    }

    /// value(), then advances by `increment` turns (rate / sample rate): free and
    /// retrig modes.
    [[nodiscard]] float next(LfoShape shape, float increment) noexcept {
        const float out = value(shape);
        m_phase += increment;
        if (m_phase >= 1.0F) {
            m_phase -= 1.0F;
            m_held = m_noise.next();
        }
        return out;
    }

    [[nodiscard]] float phase() const noexcept {
        return m_phase;
    }

private:
    [[nodiscard]] static float wrap(float x) noexcept {
        return x - std::floor(x);
    }

    float m_phase{0.0F};
    float m_held{0.0F};
    WhiteNoise m_noise;
};

} // namespace adx::dsp
