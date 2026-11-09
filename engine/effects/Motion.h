// Movement effects (phase_12.md §4.6, closes part of G5): VolumeShaper, Tape, Ensemble and
// Rotary.
//
//   VolumeShaper  a tempo-synced gain curve, one cycle per `length` (LFOTool, Kickstart):
//                 a pump, a ramp or a sine, or a 16-step gate. Locked to the transport's
//                 position while it rolls; on its own clock at the tempo otherwise.
//   Tape          wow and flutter (a modulated delay), saturation, high-frequency age,
//                 hiss. Its delay is declared as latency, so the dry path stays aligned.
//   Ensemble      2-8 modulated delay voices spread across the stereo field, in Juno I,
//                 II, I+II, BBD and dimension voicings.
//   Rotary        a Leslie: a crossover at 800 Hz, the horn and the drum each turning at
//                 their own speed (Doppler and amplitude), ramping between slow and fast.
#pragma once

#include <array>

#include "engine/dsp/Noise.h"
#include "engine/dsp/SvFilter.h"
#include "engine/effects/Effect.h"
#include "engine/effects/Params.h"
#include "engine/rt/OwnedArray.h"

namespace adx::effects {

// --- VolumeShaper ----------------------------------------------------------------------

enum class VolumeShaperParam : std::uint32_t { Length, Shape, Depth, Steps, Smooth, Count };

/// The lengths, as a fraction of a whole note.
inline constexpr std::array<double, 5> kShaperWholes{1.0 / 16.0, 1.0 / 8.0, 1.0 / 4.0, 1.0 / 2.0,
                                                     1.0};

inline constexpr auto kVolumeShaperParams = std::to_array<project::ParamDescriptor>({
    choice("length", 4.0F, 2.0F),
    // pump, ramp up, ramp down, sine
    choice("shape", 3.0F, 0.0F),
    perFrame("depth", 0.0F, 1.0F, 1.0F, project::Unit::Normalized),
    // A 16-step gate, one bit a step (bit 0 the first); 0 is off and the shape plays.
    param("steps", 0.0F, 65535.0F, 0.0F, project::Unit::Count, project::ScaleKind::Stepped),
    param("smooth", 0.1F, 20.0F, 2.0F, project::Unit::Milliseconds,
          project::ScaleKind::Logarithmic),
});

/// The gain of shape `shape` at `phase` (0..1 of a cycle), before depth: 1 is unity.
[[nodiscard]] float shaperCurve(int shape, float phase) noexcept;

class VolumeShaper final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "VolumeShaper";
    }
    [[nodiscard]] MixLaw mixLaw() const noexcept override {
        return MixLaw::Linear;
    }

protected:
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;

private:
    double m_rate{48000.0};
    float m_gain{1.0F};
};

// --- Tape ---------------------------------------------------------------------------------

enum class TapeParam : std::uint32_t {
    WowRate,
    WowDepth,
    FlutterRate,
    FlutterDepth,
    Saturation,
    Hiss,
    Age,
    Speed,
    Count
};

inline constexpr auto kTapeParams = std::to_array<project::ParamDescriptor>({
    param("wow.rate", 0.1F, 3.0F, 0.6F, project::Unit::Hertz, project::ScaleKind::Logarithmic),
    param("wow.depth", 0.0F, 1.0F, 0.3F, project::Unit::Normalized),
    param("flutter.rate", 3.0F, 20.0F, 8.0F, project::Unit::Hertz, project::ScaleKind::Logarithmic),
    param("flutter.depth", 0.0F, 1.0F, 0.2F, project::Unit::Normalized),
    perFrame("saturation", 0.0F, 1.0F, 0.3F, project::Unit::Normalized),
    perFrame("hiss", 0.0F, 1.0F, 0.0F, project::Unit::Normalized),
    param("age", 0.0F, 1.0F, 0.2F, project::Unit::Normalized),
    // 0: 15 ips, 1: 7.5 ips (more wow, less top)
    choice("speed", 1.0F, 0.0F),
});

class Tape final : public Effect {
public:
    /// The delay line's centre: wow and flutter swing around it.
    static constexpr double kCentreSeconds = 0.008;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Tape";
    }

protected:
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;
    [[nodiscard]] std::uint32_t effectLatency() const noexcept override {
        return m_centre;
    }

private:
    std::array<rt::OwnedArray<float>, 2> m_line;
    std::array<dsp::SvFilter, 2> m_age{};
    dsp::WhiteNoise m_noise;
    std::size_t m_write{0};
    double m_wow{0.0};
    double m_flutter{0.0};
    double m_rate{48000.0};
    std::uint32_t m_centre{384};
};

// --- Ensemble ------------------------------------------------------------------------------

enum class EnsembleParam : std::uint32_t { Voices, Mode, Rate, Depth, Width, Count };

inline constexpr auto kEnsembleParams = std::to_array<project::ParamDescriptor>({
    param("voices", 2.0F, 8.0F, 3.0F, project::Unit::Count, project::ScaleKind::Stepped),
    // Juno I, Juno II, Juno I+II, BBD, dimension
    choice("mode", 4.0F, 0.0F),
    param("rate", 0.05F, 10.0F, 0.5F, project::Unit::Hertz, project::ScaleKind::Logarithmic),
    param("depth", 0.0F, 1.0F, 0.5F, project::Unit::Normalized),
    param("width", 0.0F, 1.0F, 1.0F, project::Unit::Normalized),
});

class Ensemble final : public Effect {
public:
    static constexpr std::size_t kMaxVoices = 8;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Ensemble";
    }
    [[nodiscard]] MixLaw mixLaw() const noexcept override {
        return MixLaw::Linear;
    }

protected:
    [[nodiscard]] bool runsWhileBypassed() const noexcept override {
        return true;
    }
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;

private:
    rt::OwnedArray<float> m_line;
    std::size_t m_write{0};
    double m_phase{0.0};
    double m_rate{48000.0};
};

// --- Rotary --------------------------------------------------------------------------------

enum class RotaryParam : std::uint32_t {
    Speed,
    Ramp,
    HornLevel,
    DrumLevel,
    Distance,
    Drive,
    Count
};

inline constexpr auto kRotaryParams = std::to_array<project::ParamDescriptor>({
    // 0 slow (chorale), 1 fast (tremolo)
    choice("speed", 1.0F, 0.0F),
    param("ramp", 0.1F, 5.0F, 1.0F, project::Unit::Seconds, project::ScaleKind::Logarithmic),
    perFrame("hornLevel", 0.0F, 1.0F, 0.8F, project::Unit::Normalized),
    perFrame("drumLevel", 0.0F, 1.0F, 0.8F, project::Unit::Normalized),
    // How far the microphones are: nearer is deeper modulation.
    param("distance", 0.0F, 1.0F, 0.5F, project::Unit::Normalized),
    perFrame("drive", 0.0F, 24.0F, 0.0F, project::Unit::Decibels),
});

class Rotary final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Rotary";
    }

protected:
    [[nodiscard]] bool runsWhileBypassed() const noexcept override {
        return true;
    }
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;

private:
    rt::OwnedArray<float> m_line;
    dsp::SvFilter m_crossLow;
    dsp::SvFilter m_crossHigh;
    std::size_t m_write{0};
    double m_hornPhase{0.0};
    double m_drumPhase{0.0};
    /// Rotation speeds now, Hz: they ramp to the target speed's.
    double m_hornHz{0.8};
    double m_drumHz{0.7};
    double m_rate{48000.0};
};

} // namespace adx::effects
