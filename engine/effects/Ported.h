// The five effects iteration one shipped, ported from _archive/src-cpp/src/AudioEffects.cpp
// with their algorithms unchanged (phase_4.md §5: "Algorithms verbatim").
//
//   Reverb      Freeverb: 8 combs and 4 allpasses per side, the right side's delays 23
//               samples longer. The tunings are Freeverb's 44.1 kHz sample counts,
//               scaled to the prepared rate so the room is the same size at 48 kHz.
//   Distortion  tanh(x * drive) / tanh(drive): drive changes the shape, not the level.
//   Bitcrush    sample-and-hold decimation, then quantisation to `bits`.
//   Chorus      a 7 ms delay swept +-6 ms by a sine, the right channel a quarter cycle
//               ahead of the left.
//   EQ          three RBJ bands at v1's fixed corners: a 250 Hz low shelf, a 1.2 kHz
//               peak at Q 0.7, a 4 kHz high shelf. (phase_4.md names it Eq3; the type
//               name stays `EQ`, which is what every v1 file and the shim say.)
//
// Wet/dry is the slot's now, not each effect's: v1's per-effect `mix` became the
// slot's `mix`, and processWet produces the fully wet signal.
#pragma once

#include <array>
#include <cstdint>

#include "engine/dsp/RbjFilter.h"
#include "engine/effects/Effect.h"
#include "engine/rt/OwnedArray.h"

namespace adx::effects {

// NOLINTBEGIN(modernize-use-designated-initializers) - tables read as tables.
using project::CurvePart;
using project::ParamDescriptor;
using project::RateClass;
using project::ScaleKind;
using project::Unit;

enum class ReverbParam : std::uint32_t { Room, Damp, Width, Count };
inline constexpr auto kReverbParams = std::to_array<ParamDescriptor>({
    {"room", 0.0F, 1.0F, 0.5F, Unit::Normalized, ScaleKind::Linear, RateClass::Block,
     CurvePart::None},
    {"damp", 0.0F, 1.0F, 0.5F, Unit::Normalized, ScaleKind::Linear, RateClass::Block,
     CurvePart::None},
    {"width", 0.0F, 1.0F, 1.0F, Unit::Normalized, ScaleKind::Linear, RateClass::Block,
     CurvePart::None},
});

enum class DistortionParam : std::uint32_t { Drive, Count };
inline constexpr auto kDistortionParams = std::to_array<ParamDescriptor>({
    {"drive", 1.0F, 30.0F, 8.0F, Unit::Ratio, ScaleKind::Logarithmic, RateClass::Sample,
     CurvePart::None},
});

enum class BitcrushParam : std::uint32_t { Bits, Rate, Count };
inline constexpr auto kBitcrushParams = std::to_array<ParamDescriptor>({
    {"bits", 1.0F, 24.0F, 8.0F, Unit::Count, ScaleKind::Linear, RateClass::Block, CurvePart::None},
    {"rate", 100.0F, 192000.0F, 22050.0F, Unit::Hertz, ScaleKind::Logarithmic, RateClass::Block,
     CurvePart::None},
});

enum class ChorusParam : std::uint32_t { Rate, Depth, Count };
inline constexpr auto kChorusParams = std::to_array<ParamDescriptor>({
    {"rate", 0.01F, 20.0F, 0.5F, Unit::Hertz, ScaleKind::Logarithmic, RateClass::Block,
     CurvePart::None},
    {"depth", 0.0F, 1.0F, 0.5F, Unit::Normalized, ScaleKind::Linear, RateClass::Sample,
     CurvePart::None},
});

enum class EqParam : std::uint32_t { Low, Mid, High, Count };
inline constexpr auto kEqParams = std::to_array<ParamDescriptor>({
    {"low", -24.0F, 24.0F, 0.0F, Unit::Decibels, ScaleKind::Linear, RateClass::Block,
     CurvePart::None},
    {"mid", -24.0F, 24.0F, 0.0F, Unit::Decibels, ScaleKind::Linear, RateClass::Block,
     CurvePart::None},
    {"high", -24.0F, 24.0F, 0.0F, Unit::Decibels, ScaleKind::Linear, RateClass::Block,
     CurvePart::None},
});
// NOLINTEND(modernize-use-designated-initializers)

class Reverb final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Reverb";
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
    static constexpr std::size_t kCombs = 8;
    static constexpr std::size_t kAllpasses = 4;
    struct Line {
        rt::OwnedArray<float> buffer;
        std::size_t index{0};
        float store{0.0F};
    };
    std::array<Line, kCombs> m_combLeft;
    std::array<Line, kCombs> m_combRight;
    std::array<Line, kAllpasses> m_allpassLeft;
    std::array<Line, kAllpasses> m_allpassRight;
    // Control-grid values: they persist between calls, so a value read at a grid
    // frame holds until the next grid frame whatever the block size.
    float m_feedback{0.84F};
    float m_damp{0.2F};
    float m_wet1{1.0F};
    float m_wet2{0.0F};
};

class Distortion final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Distortion";
    }
    [[nodiscard]] MixLaw mixLaw() const noexcept override {
        return MixLaw::Linear;
    }

protected:
    void prepareEffect(const graph::PrepareInfo& /*info*/) override {}
    void resetEffect() noexcept override {}
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;
};

class Bitcrush final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Bitcrush";
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
    float m_heldLeft{0.0F};
    float m_heldRight{0.0F};
    /// Starts at 1 so the first sample is captured at once, as v1's did.
    float m_phase{1.0F};
    float m_step{0.5F};
    float m_levels{256.0F};
};

class Chorus final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Chorus";
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
    rt::OwnedArray<float> m_left;
    rt::OwnedArray<float> m_right;
    std::size_t m_write{0};
    float m_lfoPhase{0.0F};
};

class Eq3 final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "EQ";
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
    std::array<dsp::Biquad, 3> m_left{};
    std::array<dsp::Biquad, 3> m_right{};
    std::array<dsp::BiquadCoefficients, 3> m_coefficients{};
};

} // namespace adx::effects
