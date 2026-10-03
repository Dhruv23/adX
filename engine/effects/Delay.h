// Delay: ping-pong or straight, tempo-synced or free, with filtered feedback.
//
// Iteration one's master delay (AudioEngine.cpp, "C418 stereo ping-pong delay") was a
// fixed part of the master bus: each channel's feedback crossed to the other side.
// Here it is an ordinary effect with that as its default, plus what v1 lacked: a
// time that follows the project tempo, high- and low-cut filters inside the feedback
// loop so repeats darken and thin the way a tape echo's do, and a delay time that
// glides when it changes rather than jumping (a jump is a click).
#pragma once

#include <array>
#include <cstdint>

#include "engine/effects/Effect.h"
#include "engine/rt/OwnedArray.h"

namespace adx::effects {

enum class DelayParam : std::uint32_t {
    TimeMs,
    Feedback,
    PingPong,
    Sync,
    LowCut,
    HighCut,
    Dry,
    Level,
    Count
};

/// Sync divisions, as a fraction of a whole note: index 1.. of the `sync` parameter.
inline constexpr std::array<double, 10> kDelaySyncWholes{
    0.0,        1.0 / 32.0, 1.0 / 16.0, 3.0 / 32.0, 1.0 / 8.0,
    3.0 / 16.0, 1.0 / 4.0,  3.0 / 8.0,  1.0 / 2.0,  1.0};

// NOLINTBEGIN(modernize-use-designated-initializers)
inline constexpr auto kDelayParams = std::to_array<project::ParamDescriptor>({
    {"timeMs", 1.0F, 5000.0F, 375.0F, project::Unit::Milliseconds, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"feedback", 0.0F, 0.95F, 0.3F, project::Unit::Normalized, project::ScaleKind::Linear,
     project::RateClass::Sample, project::CurvePart::None},
    {"pingPong", 0.0F, 1.0F, 1.0F, project::Unit::Boolean, project::ScaleKind::Stepped,
     project::RateClass::Block, project::CurvePart::None},
    {"sync", 0.0F, 9.0F, 0.0F, project::Unit::Count, project::ScaleKind::Stepped,
     project::RateClass::Block, project::CurvePart::None},
    {"lowCut", 20.0F, 2000.0F, 20.0F, project::Unit::Hertz, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    {"highCut", 500.0F, 20000.0F, 20000.0F, project::Unit::Hertz, project::ScaleKind::Logarithmic,
     project::RateClass::Block, project::CurvePart::None},
    // The effect's own output is `dry * input + level * echoes`. At the defaults it is
    // the echoes alone and the slot's mix does the mixing; dry=1 with the slot fully
    // wet is v1's master delay exactly - echoes added on top of an untouched signal,
    // `level` loud - which no crossfade can reproduce.
    {"dry", 0.0F, 1.0F, 0.0F, project::Unit::Normalized, project::ScaleKind::Linear,
     project::RateClass::Sample, project::CurvePart::None},
    {"level", 0.0F, 1.0F, 1.0F, project::Unit::Normalized, project::ScaleKind::Linear,
     project::RateClass::Sample, project::CurvePart::None},
});
// NOLINTEND(modernize-use-designated-initializers)

class Delay final : public Effect {
public:
    static constexpr double kMaxSeconds = 5.0;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Delay";
    }
    /// v1's master delay added its echoes to the dry signal; linear keeps the ported
    /// `mix` values meaning what they meant.
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
    rt::OwnedArray<float> m_left;
    rt::OwnedArray<float> m_right;
    std::size_t m_write{0};
    /// The delay in samples, gliding toward m_target.
    float m_delay{0.0F};
    float m_target{0.0F};
    bool m_primed{false};
    float m_glide{1.0F};
    // Feedback-path filters: a one-pole highpass and lowpass per side.
    std::array<float, 2> m_lowState{};
    std::array<float, 2> m_highState{};
    float m_lowCoefficient{0.0F};
    float m_highCoefficient{1.0F};
    bool m_filtered{false};
    bool m_pingPong{true};
};

} // namespace adx::effects
