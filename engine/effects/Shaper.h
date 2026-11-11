// Shaper and OTT (phase_12.md §4.6, closes part of G5).
//
// Shaper: a drawn transfer curve. Seven knots at x = -1, -2/3, ... 1, each a y in -1..1,
// joined linearly or by a monotone cubic; the default knots lie on y = x, so a fresh
// Shaper changes nothing but its drive. 4x oversampled. The plan's `core::Curve`
// parameter needs a curve-valued parameter path the snapshot does not have; seven
// numbers are automatable and drawable today (phase_12.md §11).
//
// OTT: the "over the top" multiband compressor every hyperpop and hardstyle lead runs
// through. Three Linkwitz-Riley bands (88 Hz and 2.5 kHz by default, as Xfer's), each
// compressed downward above one threshold and upward below another - quiet detail is
// pulled up toward the threshold as loud peaks are pushed down to it. `depth` scales
// every ratio from none to full, `time` the attack and release together. It is its own
// type rather than MultibandComp with more knots, because MultibandComp's golden hashes
// set every parameter off its default and added parameters would move them (§11).
#pragma once

#include <array>

#include "engine/dsp/Oversampler.h"
#include "engine/dsp/RbjFilter.h"
#include "engine/effects/Effect.h"
#include "engine/effects/Params.h"

namespace adx::effects {

inline constexpr std::size_t kShaperKnots = 7;

enum class ShaperParam : std::uint32_t {
    Drive,
    Bias,
    Smooth,
    DcBlock,
    Output,
    Knot1,
    Count = Knot1 + kShaperKnots,
};

inline constexpr auto kShaperParams = std::to_array<project::ParamDescriptor>({
    perFrame("drive", 0.0F, 36.0F, 0.0F, project::Unit::Decibels),
    perFrame("bias", -1.0F, 1.0F, 0.0F, project::Unit::Normalized),
    param("smooth", 0.0F, 1.0F, 1.0F, project::Unit::Boolean, project::ScaleKind::Stepped),
    param("dcBlock", 0.0F, 1.0F, 1.0F, project::Unit::Boolean, project::ScaleKind::Stepped),
    perFrame("output", -24.0F, 12.0F, 0.0F, project::Unit::Decibels),
    param("knot1", -1.0F, 1.0F, -1.0F, project::Unit::Normalized),
    param("knot2", -1.0F, 1.0F, -2.0F / 3.0F, project::Unit::Normalized),
    param("knot3", -1.0F, 1.0F, -1.0F / 3.0F, project::Unit::Normalized),
    param("knot4", -1.0F, 1.0F, 0.0F, project::Unit::Normalized),
    param("knot5", -1.0F, 1.0F, 1.0F / 3.0F, project::Unit::Normalized),
    param("knot6", -1.0F, 1.0F, 2.0F / 3.0F, project::Unit::Normalized),
    param("knot7", -1.0F, 1.0F, 1.0F, project::Unit::Normalized),
});
static_assert(kShaperParams.size() == static_cast<std::size_t>(ShaperParam::Count));

/// The curve through `knots` at `x` (clamped to -1..1): linear, or a monotone cubic
/// (Fritsch-Carlson), which has no overshoot between knots.
[[nodiscard]] float shaperCurve(const std::array<float, kShaperKnots>& knots, bool smooth,
                                float x) noexcept;

class Shaper final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Shaper";
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
    [[nodiscard]] std::uint32_t effectLatency() const noexcept override {
        return dsp::kOversampleLatency;
    }

private:
    std::array<dsp::Oversampler4x, 2> m_over{};
    std::array<float, 2> m_dcIn{};
    std::array<float, 2> m_dcOut{};
    float m_dcPole{0.9995F};
};

// --- OTT ---------------------------------------------------------------------------

inline constexpr std::size_t kOttBands = 3;

enum class OttParam : std::uint32_t {
    Depth,
    Time,
    Upward,
    Downward,
    InGain,
    OutGain,
    CrossoverLow,
    CrossoverHigh,
    /// Per band b (0 low, 1 mid, 2 high), at Band0 + 5 b: down.threshold, down.ratio,
    /// up.threshold, up.ratio, gain.
    Band0,
    Count = Band0 + (5 * kOttBands),
};

[[nodiscard]] constexpr std::uint32_t ottBand(std::size_t band, std::uint32_t field) noexcept {
    return static_cast<std::uint32_t>(OttParam::Band0) + (5 * static_cast<std::uint32_t>(band)) +
           field;
}

namespace detail {
constexpr std::array<project::ParamDescriptor, 5>
ottBandRows(std::string_view threshold, std::string_view ratio, std::string_view up,
            std::string_view upRatio, std::string_view gain) {
    return {
        perFrame(threshold, -60.0F, 0.0F, -24.0F, project::Unit::Decibels),
        param(ratio, 1.0F, 66.0F, 66.0F, project::Unit::Ratio, project::ScaleKind::Logarithmic),
        perFrame(up, -90.0F, 0.0F, -40.0F, project::Unit::Decibels),
        param(upRatio, 1.0F, 20.0F, 4.0F, project::Unit::Ratio, project::ScaleKind::Logarithmic),
        perFrame(gain, -24.0F, 24.0F, 0.0F, project::Unit::Decibels)};
}
constexpr auto ottParams() {
    constexpr std::array<project::ParamDescriptor, 8> head{
        perFrame("depth", 0.0F, 1.0F, 1.0F, project::Unit::Normalized),
        param("time", 0.0F, 1.0F, 0.5F, project::Unit::Normalized),
        param("upward", 0.0F, 1.0F, 1.0F, project::Unit::Normalized),
        param("downward", 0.0F, 1.0F, 1.0F, project::Unit::Normalized),
        perFrame("inGain", -24.0F, 24.0F, 0.0F, project::Unit::Decibels),
        perFrame("outGain", -24.0F, 24.0F, 0.0F, project::Unit::Decibels),
        param("crossoverLow", 20.0F, 1000.0F, 88.0F, project::Unit::Hertz,
              project::ScaleKind::Logarithmic),
        param("crossoverHigh", 500.0F, 16000.0F, 2500.0F, project::Unit::Hertz,
              project::ScaleKind::Logarithmic),
    };
    constexpr std::array<std::array<project::ParamDescriptor, 5>, kOttBands> bands{
        ottBandRows("low.down.threshold", "low.down.ratio", "low.up.threshold", "low.up.ratio",
                    "low.gain"),
        ottBandRows("mid.down.threshold", "mid.down.ratio", "mid.up.threshold", "mid.up.ratio",
                    "mid.gain"),
        ottBandRows("high.down.threshold", "high.down.ratio", "high.up.threshold", "high.up.ratio",
                    "high.gain"),
    };
    std::array<project::ParamDescriptor, static_cast<std::size_t>(OttParam::Count)> table{};
    for (std::size_t i = 0; i < head.size(); ++i) {
        table[i] = head[i];
    }
    for (std::size_t b = 0; b < kOttBands; ++b) {
        for (std::size_t f = 0; f < 5; ++f) {
            table[head.size() + (5 * b) + f] = bands[b][f];
        }
    }
    return table;
}
} // namespace detail

inline constexpr auto kOttParams = detail::ottParams();

/// Upward gain is capped here: below it a band is noise, and pulling noise up 60 dB
/// is not detail.
inline constexpr float kOttMaxUpwardDb = 30.0F;

/// The gain in dB OTT applies to a band whose level is `level` dB: negative above the
/// downward threshold, positive below the upward one, 0 between. `depth`, `upward` and
/// `downward` scale the slopes. What ott_upward holds the effect to.
[[nodiscard]] float ottGainDb(float level, float downThreshold, float downRatio, float upThreshold,
                              float upRatio, float depth, float upward, float downward) noexcept;

class Ott final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "OTT";
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
    [[nodiscard]] bool runsWhileBypassed() const noexcept override {
        return true;
    }

private:
    void refresh(const EffectContext& context, std::uint32_t frame) noexcept;

    struct Split {
        std::array<dsp::Biquad, 2> low{};
        std::array<dsp::Biquad, 2> high{};
    };
    /// Per channel: the two crossovers, and the low band's allpass for the second.
    std::array<std::array<Split, 2>, 2> m_split{};
    std::array<dsp::Biquad, 2> m_lowAllpass{};
    std::array<dsp::BiquadCoefficients, 2> m_lowCoefficients{};
    std::array<dsp::BiquadCoefficients, 2> m_highCoefficients{};
    dsp::BiquadCoefficients m_allpassCoefficients{};
    /// Each band's level follower, linear peak.
    std::array<float, kOttBands> m_level{};
    float m_attack{0.0F};
    float m_release{0.0F};
};

} // namespace adx::effects
