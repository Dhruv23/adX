// GrossBeat: time and volume manipulation over a bar grid (phase_4.md §4.9).
//
// The one effect that reads ctx.time. Over a cycle of `length` beats, a pattern maps
// each point p of the cycle (0..1) to the point f(p) <= p whose audio plays there: half
// speed is f = p/2, a stutter f = p mod 1/8, a tape stop slows f to a halt. Playing
// "the audio from (p - f(p)) cycles ago" is a variable delay line, so the effect is a
// rolling buffer of the last ten seconds read at that delay, interpolated, with a 2 ms
// crossfade wherever the pattern jumps. A gate pattern shapes the volume over the same
// cycle. Because the position comes from the time source it is handed - not a global -
// a GrossBeat on a launched clip follows that clip's time (phase_3.md §2, Phase 11).
// Stopped, it passes the input through.
//
// Position is read exactly, in fractional beats, from the tempo map (beatsAt), not from
// the integer tick: a tick is several samples long, and a delay that stepped by ticks
// would zipper.
#pragma once

#include <array>
#include <cstdint>

#include "engine/core/TempoMath.h"
#include "engine/effects/Effect.h"
#include "engine/effects/Modulation.h"
#include "engine/effects/Params.h"

namespace adx::effects {

enum class GrossBeatPattern : std::uint8_t {
    Off,
    HalfSpeed,
    ReverseHalf,
    StutterEighth,
    TapeStop,
    RepeatBeat,
    TwoThirds,
};
inline constexpr std::uint8_t kGrossBeatPatterns = 7;

enum class GrossBeatGate : std::uint8_t { None, Eighths, Sixteenths, Pump };

enum class GrossBeatParam : std::uint32_t { Pattern, Length, Gate, Depth, Count };
inline constexpr auto kGrossBeatParams = std::to_array<project::ParamDescriptor>({
    choice("pattern", kGrossBeatPatterns - 1, 0.0F),
    // The cycle: 0..3 for 1, 2, 4, 8 beats.
    choice("length", 3.0F, 2.0F),
    choice("gate", 3.0F, 0.0F),
    perFrame("depth", 0.0F, 1.0F, 1.0F, project::Unit::Normalized),
});

/// f(p): the point of the cycle that plays at p. Always <= p, so it is causal.
[[nodiscard]] double grossBeatMap(GrossBeatPattern pattern, double p) noexcept;

/// Beats from tick 0 to `seconds`, fractional, through the tempo map's segments -
/// ramps included.
[[nodiscard]] double beatsAt(const core::TempoView& tempo, double seconds) noexcept;

class GrossBeat final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "GrossBeat";
    }

protected:
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;

private:
    std::array<ModDelay, 2> m_buffer;
    std::size_t m_capacity{0};
    /// The delay the last frame read at, and the one a crossfade is leaving.
    double m_delay{0.0};
    double m_fadeFrom{0.0};
    std::uint32_t m_fadeLeft{0};
    std::uint32_t m_fadeLength{96};
    float m_gain{1.0F};
    float m_gainPole{0.99F};
    GrossBeatPattern m_pattern{GrossBeatPattern::Off};
    GrossBeatGate m_gate{GrossBeatGate::None};
    double m_beats{4.0};
};

} // namespace adx::effects
