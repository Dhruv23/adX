// A multi-stage ADSR whose attack, decay and release each have a core::Curve.
//
// One evaluator for every curve in the program (FINAL_PLAN §3.2): a stage's shape is
// the same Curve an automation lane uses, so the UI can draw it and the file can store
// it. Stage boundaries are sample-exact - a 10 ms attack at 48 kHz is exactly 480
// samples of attack - and a release starts from wherever the level is, so releasing
// mid-attack never jumps (iteration one's release table started from the sustain
// level whatever the note was doing, which clicked; phase_4.md §4.3).
//
// The curve is evaluated every kCurveStride samples on the stage's own clock and
// interpolated linearly between, so a Bezier stage costs a Newton solve every 16
// samples rather than every sample. The stage clock starts with the stage, so the
// result does not depend on the block size.
#pragma once

#include <cstdint>

#include "engine/core/Curve.h"

namespace adx::dsp {

enum class EnvStage : std::uint8_t { Idle, Attack, Decay, Sustain, Release };

struct EnvelopeShape {
    float attackSeconds{0.01F};
    float decaySeconds{0.2F};
    float sustain{0.7F};
    float releaseSeconds{0.2F};
    core::Curve attackCurve;
    core::Curve decayCurve;
    core::Curve releaseCurve;
};

class Envelope {
public:
    static constexpr std::uint32_t kCurveStride = 16;

    /// Starts the attack from the current level (zero for a fresh voice, the current
    /// level for a retrigger, so retriggering never jumps).
    void trigger(const EnvelopeShape& shape, std::uint32_t sampleRate) noexcept;

    /// Starts the release from the current level. Idempotent.
    void release(const EnvelopeShape& shape, std::uint32_t sampleRate) noexcept;

    /// Silences immediately. For a voice being freed, not for a note-off.
    void reset() noexcept;

    /// The next level, in [0, 1]. `shape` may change between samples (an automated
    /// sustain); a stage's length is fixed when the stage starts.
    [[nodiscard]] float next(const EnvelopeShape& shape) noexcept;

    [[nodiscard]] EnvStage stage() const noexcept {
        return m_stage;
    }
    [[nodiscard]] float level() const noexcept {
        return m_level;
    }
    [[nodiscard]] bool finished() const noexcept {
        return m_stage == EnvStage::Idle;
    }

private:
    void enter(EnvStage stage, float from, float to, float seconds, const core::Curve& curve,
               std::uint32_t sampleRate) noexcept;
    [[nodiscard]] float shaped(std::uint32_t position) const noexcept;

    EnvStage m_stage{EnvStage::Idle};
    float m_level{0.0F};
    float m_from{0.0F};
    float m_to{0.0F};
    std::uint32_t m_length{0};
    std::uint32_t m_stageFrame{0};
    core::Curve m_curve;
    std::uint32_t m_sampleRate{48000};
    /// The curve's value at the last and next stride points, for interpolation.
    float m_strideFrom{0.0F};
    float m_strideTo{0.0F};
};

} // namespace adx::dsp
