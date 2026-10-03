// Dynamics: Compressor, Ducker, Limiter, Gate.
//
// Each declares its lookahead as latency and lets PDC compensate (phase_4.md §4.9:
// "Getting this wrong is the classic 'my master bus is 8 ms late' bug, and it is
// already solved - declare the latency honestly and it disappears"). Lookahead is
// therefore structural, not automatable: latency has to be known when the graph is
// built, so it is read from the slot when the node is made (effects/Factory.cpp), and
// a different lookahead is a different node, the way a different instrument type is.
//
// Compressor and Gate key from their own input or from the sidechain port; the
// Ducker always keys from the sidechain - it is iteration one's STAKILLAZ pump
// (AudioEngine.cpp, "Track 1's bus ducks the master"), now with an explicit key input
// (P2-3, P3-4).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "engine/dsp/TruePeak.h"
#include "engine/effects/Effect.h"
#include "engine/rt/OwnedArray.h"

namespace adx::effects {

// NOLINTBEGIN(modernize-use-designated-initializers)
using project::CurvePart;
using project::ParamDescriptor;
using project::RateClass;
using project::ScaleKind;
using project::Unit;

enum class CompressorParam : std::uint32_t {
    Threshold,
    Ratio,
    Knee,
    Attack,
    Release,
    Makeup,
    Detector,
    KeyInput,
    Lookahead,
    Smoothing,
    Count,
};
inline constexpr auto kCompressorParams = std::to_array<ParamDescriptor>({
    {"threshold", -60.0F, 0.0F, -18.0F, Unit::Decibels, ScaleKind::Linear, RateClass::Sample,
     CurvePart::None},
    {"ratio", 1.0F, 20.0F, 4.0F, Unit::Ratio, ScaleKind::Logarithmic, RateClass::Block,
     CurvePart::None},
    {"knee", 0.0F, 24.0F, 6.0F, Unit::Decibels, ScaleKind::Linear, RateClass::Block,
     CurvePart::None},
    {"attack", 0.1F, 200.0F, 10.0F, Unit::Milliseconds, ScaleKind::Logarithmic, RateClass::Block,
     CurvePart::None},
    {"release", 5.0F, 2000.0F, 100.0F, Unit::Milliseconds, ScaleKind::Logarithmic, RateClass::Block,
     CurvePart::None},
    {"makeup", 0.0F, 24.0F, 0.0F, Unit::Decibels, ScaleKind::Linear, RateClass::Sample,
     CurvePart::None},
    {"detector", 0.0F, 1.0F, 0.0F, Unit::Count, ScaleKind::Stepped, RateClass::Block,
     CurvePart::None},
    {"keyInput", 0.0F, 1.0F, 0.0F, Unit::Count, ScaleKind::Stepped, RateClass::Block,
     CurvePart::None},
    {"lookahead", 0.0F, 10.0F, 0.0F, Unit::Milliseconds, ScaleKind::Linear, RateClass::Block,
     CurvePart::None},
    // 0 smooths the gain reduction in dB - attack and release are even in loudness.
    // 1 smooths the linear gain, as iteration one's master compressor did; under heavy
    // reduction the two recover differently, and v1's mixes were balanced against its.
    {"smoothing", 0.0F, 1.0F, 0.0F, Unit::Count, ScaleKind::Stepped, RateClass::Block,
     CurvePart::None},
});

enum class DuckerParam : std::uint32_t { Enabled, Amount, ReleaseMs, Count };
inline constexpr auto kDuckerParams = std::to_array<ParamDescriptor>({
    {"enabled", 0.0F, 1.0F, 1.0F, Unit::Boolean, ScaleKind::Stepped, RateClass::Block,
     CurvePart::None},
    {"amount", 0.0F, 1.0F, 0.6F, Unit::Normalized, ScaleKind::Linear, RateClass::Sample,
     CurvePart::None},
    {"releaseMs", 10.0F, 1000.0F, 120.0F, Unit::Milliseconds, ScaleKind::Logarithmic,
     RateClass::Block, CurvePart::None},
});

enum class LimiterParam : std::uint32_t { Ceiling, Release, TruePeak, Lookahead, Count };
inline constexpr auto kLimiterParams = std::to_array<ParamDescriptor>({
    {"ceiling", -24.0F, 0.0F, -0.3F, Unit::Decibels, ScaleKind::Linear, RateClass::Block,
     CurvePart::None},
    {"release", 1.0F, 1000.0F, 60.0F, Unit::Milliseconds, ScaleKind::Logarithmic, RateClass::Block,
     CurvePart::None},
    {"truePeak", 0.0F, 1.0F, 1.0F, Unit::Boolean, ScaleKind::Stepped, RateClass::Block,
     CurvePart::None},
    {"lookahead", 0.1F, 10.0F, 1.5F, Unit::Milliseconds, ScaleKind::Linear, RateClass::Block,
     CurvePart::None},
});

enum class GateParam : std::uint32_t {
    Threshold,
    Hysteresis,
    Attack,
    Hold,
    Release,
    Range,
    Ratio,
    KeyInput,
    Lookahead,
    Count,
};
inline constexpr auto kGateParams = std::to_array<ParamDescriptor>({
    {"threshold", -80.0F, 0.0F, -40.0F, Unit::Decibels, ScaleKind::Linear, RateClass::Sample,
     CurvePart::None},
    {"hysteresis", 0.0F, 12.0F, 3.0F, Unit::Decibels, ScaleKind::Linear, RateClass::Block,
     CurvePart::None},
    {"attack", 0.01F, 50.0F, 0.5F, Unit::Milliseconds, ScaleKind::Logarithmic, RateClass::Block,
     CurvePart::None},
    {"hold", 0.0F, 500.0F, 10.0F, Unit::Milliseconds, ScaleKind::Linear, RateClass::Block,
     CurvePart::None},
    {"release", 1.0F, 2000.0F, 100.0F, Unit::Milliseconds, ScaleKind::Logarithmic, RateClass::Block,
     CurvePart::None},
    {"range", -80.0F, 0.0F, -80.0F, Unit::Decibels, ScaleKind::Linear, RateClass::Block,
     CurvePart::None},
    {"ratio", 1.0F, 100.0F, 100.0F, Unit::Ratio, ScaleKind::Logarithmic, RateClass::Block,
     CurvePart::None},
    {"keyInput", 0.0F, 1.0F, 0.0F, Unit::Count, ScaleKind::Stepped, RateClass::Block,
     CurvePart::None},
    {"lookahead", 0.0F, 10.0F, 0.0F, Unit::Milliseconds, ScaleKind::Linear, RateClass::Block,
     CurvePart::None},
});
// NOLINTEND(modernize-use-designated-initializers)

/// A fixed-length delay line for a lookahead: what comes out is what went in
/// `length` samples ago.
class LookaheadLine {
public:
    void allocate(std::size_t length) {
        m_length = length;
        m_left.allocate(length + 1);
        m_right.allocate(length + 1);
        reset();
    }
    void reset() noexcept {
        std::ranges::fill(m_left.view(), 0.0F);
        std::ranges::fill(m_right.view(), 0.0F);
        m_writeIndex = 0;
    }
    void push(float left, float right, float& outLeft, float& outRight) noexcept {
        if (m_length == 0) {
            outLeft = left;
            outRight = right;
            return;
        }
        const std::span<float> l = m_left.view();
        const std::span<float> r = m_right.view();
        l[m_writeIndex] = left;
        r[m_writeIndex] = right;
        m_writeIndex = (m_writeIndex + 1) % l.size();
        outLeft = l[m_writeIndex];
        outRight = r[m_writeIndex];
    }
    [[nodiscard]] std::size_t length() const noexcept {
        return m_length;
    }

private:
    rt::OwnedArray<float> m_left;
    rt::OwnedArray<float> m_right;
    std::size_t m_writeIndex{0};
    std::size_t m_length{0};
};

/// Shared by the effects whose lookahead is structural: set before prepare().
class LookaheadEffect : public Effect {
public:
    void setLookaheadMs(float ms) noexcept {
        m_lookaheadMs = ms < 0.0F ? 0.0F : ms;
    }
    [[nodiscard]] float lookaheadMs() const noexcept {
        return m_lookaheadMs;
    }

protected:
    [[nodiscard]] std::uint32_t effectLatency() const noexcept override {
        return m_latency;
    }
    void prepareLookahead(std::uint32_t sampleRate) {
        m_latency = static_cast<std::uint32_t>(
            static_cast<double>(m_lookaheadMs) * 0.001 * static_cast<double>(sampleRate) + 0.5);
        m_line.allocate(m_latency);
    }

    LookaheadLine m_line;
    std::uint32_t m_latency{0};

private:
    float m_lookaheadMs{0.0F};
};

class Compressor final : public LookaheadEffect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Compressor";
    }

protected:
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;
    [[nodiscard]] bool usesSidechain() const noexcept override {
        return true;
    }

private:
    float m_envelopeDb{0.0F};
    float m_meanSquare{0.0F};
    float m_attack{0.0F};
    float m_release{0.0F};
    float m_ratio{4.0F};
    float m_knee{6.0F};
    bool m_rms{false};
    bool m_external{false};
    bool m_linear{false};
    float m_gain{1.0F};
    float m_rmsCoefficient{0.0F};
};

class Ducker final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Ducker";
    }

protected:
    void prepareEffect(const graph::PrepareInfo& /*info*/) override {}
    void resetEffect() noexcept override {
        m_envelope = 0.0F;
    }
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;
    [[nodiscard]] bool usesSidechain() const noexcept override {
        return true;
    }

private:
    float m_envelope{0.0F};
    float m_releaseCoefficient{0.0F};
    bool m_enabled{true};
};

class Limiter final : public LookaheadEffect {
public:
    Limiter() noexcept {
        setLookaheadMs(1.5F);
    }
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Limiter";
    }

protected:
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;

private:
    /// The gain each sample needs, for the last lookahead + 1 samples, and the running
    /// minimum over them kept as a monotonic queue of indices.
    rt::OwnedArray<float> m_required;
    rt::OwnedArray<std::uint64_t> m_queue;
    std::size_t m_queueHead{0};
    std::size_t m_queueSize{0};
    /// The released envelope, and the box filter over it that makes the attack.
    rt::OwnedArray<float> m_box;
    double m_boxSum{0.0};
    std::size_t m_boxIndex{0};
    float m_envelope{1.0F};
    std::uint64_t m_index{0};
    float m_releaseCoefficient{0.0F};
    float m_ceiling{1.0F};
    bool m_truePeak{true};
    dsp::TruePeak m_peakLeft;
    dsp::TruePeak m_peakRight;
    /// |x| for the last kDelay + 1 samples: the oldest is the sample the true-peak
    /// estimate currently describes.
    std::array<float, dsp::TruePeak::kDelay + 1> m_samplePeaks{};
    std::size_t m_samplePeakIndex{0};
};

class Gate final : public LookaheadEffect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Gate";
    }

protected:
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;
    [[nodiscard]] bool usesSidechain() const noexcept override {
        return true;
    }

private:
    bool m_open{false};
    std::uint32_t m_holdLeft{0};
    float m_gain{0.0F};
    float m_attack{0.0F};
    float m_release{0.0F};
    float m_hysteresis{3.0F};
    float m_rangeGain{0.0F};
    float m_range{-80.0F};
    float m_ratio{100.0F};
    std::uint32_t m_hold{0};
    bool m_external{false};
};

} // namespace adx::effects
