// Hall (phase_12.md §4.6, closes part of G5): a long reverb beyond Freeverb.
//
// An eight-line feedback delay network. The input is predelayed, diffused by four
// allpasses, then fed to eight delay lines whose outputs are mixed by an 8x8 Hadamard
// matrix (orthogonal, so the loop by itself neither gains nor loses energy) and fed
// back. Each line's loop gain is 10^(-3 d / (T fs)) for its length d and the decay T, so
// every line falls 60 dB in T seconds whatever its length: the decay is exact by
// construction, 0.1 to 60 s (hall_decay). `damping` puts a one-pole lowpass in each loop
// so the highs die first; at 0 it is a wire.
//
//   size        scales the line lengths (and so the echo density and the room's size)
//   modulation  moves each line's length by up to 1 ms, each at its own slow rate, which
//               breaks up the metallic ring a static FDN has on long tails
//   shimmer     a pitch shifter in the loop: each pass round the network comes back
//               `shimmerInterval` higher (+12, +7 or +19), so a held chord blooms upward
//   freeze      the loop gain goes to 1 and the input to 0: the tail holds forever
//   width       0 mono, 1 the two decorrelated outputs as they are
#pragma once

#include <array>

#include "engine/effects/Effect.h"
#include "engine/effects/Params.h"
#include "engine/rt/OwnedArray.h"

namespace adx::effects {

inline constexpr std::size_t kHallLines = 8;

enum class HallParam : std::uint32_t {
    Size,
    Decay,
    Predelay,
    Damping,
    Modulation,
    Shimmer,
    ShimmerInterval,
    Width,
    Freeze,
    Count,
};

inline constexpr auto kHallParams = std::to_array<project::ParamDescriptor>({
    param("size", 0.0F, 1.0F, 0.6F, project::Unit::Normalized),
    param("decay", 0.1F, 60.0F, 3.0F, project::Unit::Seconds, project::ScaleKind::Logarithmic),
    param("predelay", 0.0F, 250.0F, 20.0F, project::Unit::Milliseconds),
    param("damping", 0.0F, 1.0F, 0.3F, project::Unit::Normalized),
    param("modulation", 0.0F, 1.0F, 0.3F, project::Unit::Normalized),
    perFrame("shimmer", 0.0F, 1.0F, 0.0F, project::Unit::Normalized),
    choice("shimmerInterval", 2.0F, 0.0F),
    perFrame("width", 0.0F, 1.0F, 1.0F, project::Unit::Normalized),
    param("freeze", 0.0F, 1.0F, 0.0F, project::Unit::Boolean, project::ScaleKind::Stepped),
});

/// The line lengths at size 1, in seconds: mutually prime at 48 kHz, 30-80 ms.
inline constexpr std::array<double, kHallLines> kHallLineSeconds{0.0297, 0.0371, 0.0411, 0.0437,
                                                                 0.0533, 0.0599, 0.0671, 0.0793};

/// A loop gain for a line `seconds` long to fall 60 dB in `decay` seconds.
[[nodiscard]] double hallLineGain(double seconds, double decay) noexcept;

class Hall final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Hall";
    }

protected:
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;
    [[nodiscard]] bool runsWhileBypassed() const noexcept override {
        return true;
    }

private:
    void refresh(const EffectContext& context, std::uint32_t frame) noexcept;
    [[nodiscard]] float readLine(std::size_t line, double delay) const noexcept;
    [[nodiscard]] float shimmerOut(float in, float ratio) noexcept;

    double m_rate{48000.0};
    /// One ring per line, and the predelay's, each a power of two.
    std::array<rt::OwnedArray<float>, kHallLines> m_lines;
    std::size_t m_lineMask{0};
    std::size_t m_write{0};
    rt::OwnedArray<float> m_predelay;
    std::size_t m_predelayMask{0};
    /// Four input diffusers: allpass delay rings.
    std::array<rt::OwnedArray<float>, 4> m_diffuser;
    std::array<std::size_t, 4> m_diffuserLength{};
    std::array<std::size_t, 4> m_diffuserIndex{};
    /// The shimmer's grain buffer.
    rt::OwnedArray<float> m_shimmer;
    std::size_t m_shimmerMask{0};
    std::size_t m_shimmerWrite{0};
    double m_shimmerPhase{0.0};

    std::array<double, kHallLines> m_delay{};
    std::array<float, kHallLines> m_gain{};
    std::array<float, kHallLines> m_damp{};
    std::array<double, kHallLines> m_modPhase{};
    double m_modDepth{0.0};
    std::size_t m_predelayFrames{0};
    float m_dampCoefficient{0.0F};
    bool m_freeze{false};
    float m_input{1.0F};
};

} // namespace adx::effects
