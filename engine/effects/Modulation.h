// Modulation and stereo effects (FINAL_PLAN §5.4, phase_4.md Tranche C).
//
//   Flanger           a 0.1-10 ms delay swept by a triangle LFO, with feedback; the wet
//                     signal is input plus delayed, halved, so mix 1 is the full comb
//   Phaser            2-12 first-order allpasses swept around a centre by a sine LFO,
//                     with feedback; wet is input plus allpassed, halved
//   Tremolo           amplitude by an LFO of six shapes; the right side's LFO offset
//   RingMod           the input times a sine - or times the sidechain - with a bias
//                     from ring modulation (0) to amplitude modulation (1)
//   FrequencyShifter  a true shift: an analytic signal from Niemitalo's 90-degree
//                     allpass pair, single-sideband modulated, so every partial moves
//                     by the same number of hertz
//   StereoImager      M/S width, with a separate width below a crossover - the usual
//                     "mono the bass, widen the top"
//
// None has latency. Each LFO is a phase the effect accumulates per sample from its
// own reset, so the output is the same at every block size.
#pragma once

#include <array>
#include <cstdint>

#include "engine/dsp/Lfo.h"
#include "engine/dsp/SvFilter.h"
#include "engine/effects/Effect.h"
#include "engine/effects/Params.h"
#include "engine/rt/OwnedArray.h"

namespace adx::effects {

enum class FlangerParam : std::uint32_t { Rate, Depth, Delay, Feedback, Stereo, Count };
inline constexpr auto kFlangerParams = std::to_array<project::ParamDescriptor>({
    param("rate", 0.01F, 10.0F, 0.25F, project::Unit::Hertz, project::ScaleKind::Logarithmic),
    perFrame("depth", 0.0F, 10.0F, 2.0F, project::Unit::Milliseconds),
    perFrame("delay", 0.1F, 10.0F, 1.0F, project::Unit::Milliseconds),
    perFrame("feedback", -0.95F, 0.95F, 0.5F, project::Unit::Normalized),
    // The right LFO's lead, in cycles: 0.25 is quadrature.
    param("stereo", 0.0F, 0.5F, 0.25F, project::Unit::Normalized),
});

enum class PhaserParam : std::uint32_t { Rate, Depth, Stages, Centre, Feedback, Stereo, Count };
inline constexpr auto kPhaserParams = std::to_array<project::ParamDescriptor>({
    param("rate", 0.01F, 10.0F, 0.4F, project::Unit::Hertz, project::ScaleKind::Logarithmic),
    // The sweep, +- this many times three octaves.
    perFrame("depth", 0.0F, 1.0F, 0.6F, project::Unit::Normalized),
    // Pairs of allpasses: 1..6, so 2..12 stages.
    param("stages", 1.0F, 6.0F, 2.0F, project::Unit::Count, project::ScaleKind::Stepped),
    perFrame("centre", 100.0F, 5000.0F, 800.0F, project::Unit::Hertz,
             project::ScaleKind::Logarithmic),
    perFrame("feedback", -0.95F, 0.95F, 0.3F, project::Unit::Normalized),
    param("stereo", 0.0F, 0.5F, 0.25F, project::Unit::Normalized),
});

enum class TremoloParam : std::uint32_t { Rate, Depth, Shape, Stereo, Count };
inline constexpr auto kTremoloParams = std::to_array<project::ParamDescriptor>({
    param("rate", 0.01F, 40.0F, 5.0F, project::Unit::Hertz, project::ScaleKind::Logarithmic),
    perFrame("depth", 0.0F, 1.0F, 0.5F, project::Unit::Normalized),
    // dsp::LfoShape: sine, triangle, saw up, saw down, square, sample and hold.
    choice("shape", 5.0F, 0.0F),
    param("stereo", 0.0F, 0.5F, 0.0F, project::Unit::Normalized),
});

enum class RingModParam : std::uint32_t { Frequency, Bias, Carrier, Count };
inline constexpr auto kRingModParams = std::to_array<project::ParamDescriptor>({
    perFrame("frequency", 1.0F, 5000.0F, 440.0F, project::Unit::Hertz,
             project::ScaleKind::Logarithmic),
    // 0 ring (the input vanishes, sum and difference remain); 1 amplitude modulation.
    perFrame("bias", 0.0F, 1.0F, 0.0F, project::Unit::Normalized),
    // 0 the internal sine; 1 the sidechain input.
    choice("carrier", 1.0F, 0.0F),
});

enum class FrequencyShifterParam : std::uint32_t { Shift, Spread, Count };
inline constexpr auto kFrequencyShifterParams = std::to_array<project::ParamDescriptor>({
    perFrame("shift", -5000.0F, 5000.0F, 100.0F, project::Unit::Hertz),
    // Added to the right channel's shift, subtracted from the left's.
    perFrame("spread", -500.0F, 500.0F, 0.0F, project::Unit::Hertz),
});

enum class StereoImagerParam : std::uint32_t { Width, LowWidth, Crossover, Count };
inline constexpr auto kStereoImagerParams = std::to_array<project::ParamDescriptor>({
    // Side gain: 0 mono, 1 unchanged, 2 twice as wide.
    perFrame("width", 0.0F, 2.0F, 1.0F, project::Unit::Ratio),
    perFrame("lowWidth", 0.0F, 2.0F, 1.0F, project::Unit::Ratio),
    param("crossover", 20.0F, 1000.0F, 150.0F, project::Unit::Hertz,
          project::ScaleKind::Logarithmic),
});

/// A power-of-two ring of floats with a fractional, linearly interpolated read.
class ModDelay {
public:
    void allocate(std::size_t frames);
    void reset() noexcept;
    void write(float x) noexcept;
    /// The sample `delay` frames before the last write: 0 is the last write itself.
    [[nodiscard]] float read(float delay) const noexcept;

private:
    rt::OwnedArray<float> m_buffer;
    std::size_t m_mask{0};
    std::size_t m_write{0};
};

class Flanger final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Flanger";
    }
    [[nodiscard]] bool runsWhileBypassed() const noexcept override {
        return true;
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
    std::array<ModDelay, 2> m_delay;
    std::array<float, 2> m_feedback{};
    double m_phase{0.0};
    // Control-grid values: they hold between grid frames whatever the block size.
    double m_increment{0.0};
    float m_stereo{0.0F};
};

class Phaser final : public Effect {
public:
    static constexpr std::size_t kMaxStages = 12;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Phaser";
    }
    [[nodiscard]] MixLaw mixLaw() const noexcept override {
        return MixLaw::Linear;
    }

protected:
    void prepareEffect(const graph::PrepareInfo& /*info*/) override {}
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;

private:
    /// Each allpass's state, per channel.
    std::array<std::array<float, kMaxStages>, 2> m_state{};
    std::array<float, 2> m_feedback{};
    double m_phase{0.0};
    double m_increment{0.0};
    std::size_t m_stages{4};
    float m_stereo{0.0F};
};

class Tremolo final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Tremolo";
    }

protected:
    void prepareEffect(const graph::PrepareInfo& /*info*/) override {}
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;

private:
    std::array<dsp::Lfo, 2> m_lfo{dsp::Lfo{0x7E401U}, dsp::Lfo{0x7E402U}};
    bool m_started{false};
    float m_increment{0.0F};
    dsp::LfoShape m_shape{dsp::LfoShape::Sine};
};

class RingMod final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "RingMod";
    }

protected:
    void prepareEffect(const graph::PrepareInfo& /*info*/) override {}
    void resetEffect() noexcept override {
        m_phase = 0.0;
    }
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;
    [[nodiscard]] bool usesSidechain() const noexcept override {
        return true;
    }

private:
    double m_phase{0.0};
    bool m_external{false};
};

/// Niemitalo's two allpass chains whose outputs differ by 90 degrees across the audio
/// band: the real and imaginary parts of the analytic signal.
class HilbertPair {
public:
    void reset() noexcept;
    /// One sample in; the in-phase and quadrature parts out.
    void process(float x, float& real, float& imaginary) noexcept;

private:
    struct Stage {
        float in1, in2, out1, out2;
    };
    std::array<Stage, 4> m_a{};
    std::array<Stage, 4> m_b{};
    float m_delayed{0.0F};
};

class FrequencyShifter final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "FrequencyShifter";
    }

protected:
    void prepareEffect(const graph::PrepareInfo& /*info*/) override {}
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;

private:
    std::array<HilbertPair, 2> m_hilbert{};
    std::array<double, 2> m_phase{};
};

class StereoImager final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "StereoImager";
    }
    [[nodiscard]] MixLaw mixLaw() const noexcept override {
        return MixLaw::Linear;
    }

protected:
    void prepareEffect(const graph::PrepareInfo& /*info*/) override {}
    void resetEffect() noexcept override {
        m_split.reset();
    }
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;

private:
    dsp::SvFilter m_split;
    dsp::SvfCoefficients m_coefficients{};
};

} // namespace adx::effects
