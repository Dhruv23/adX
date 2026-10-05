// Convolution: an impulse response through uniform-partition FFT convolution
// (phase_4.md §4.9).
//
// The partitions are kPartition frames, so the declared latency is one partition, not
// the length of the IR (dsp/Convolve.h). The IR is the slot's `ir=` file, decoded on
// the main thread when the node is built - mono convolves both sides with it, stereo
// each side with its own channel - or, with no file, a synthetic room: decorrelated
// noise per side under an exponential decay of `size` seconds (RT60), darkening with
// time by `damping`, after a 10 ms gap. Both are structure, like a lookahead: they are
// read from the slot when the node is made (effects::makeEffect), and a different file
// or room is a different node (effects::configMatches). What turns while it plays is
// the output `gain`.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "engine/dsp/Convolve.h"
#include "engine/effects/Effect.h"
#include "engine/effects/Params.h"

namespace adx::effects {

enum class ConvolutionParam : std::uint32_t { Size, Damping, Gain, Count };
inline constexpr auto kConvolutionParams = std::to_array<project::ParamDescriptor>({
    // The synthetic room's RT60. Structure: read when the node is built.
    param("size", 0.1F, 4.0F, 1.0F, project::Unit::Seconds, project::ScaleKind::Logarithmic),
    // How fast the synthetic room's highs die relative to its lows. Structure.
    param("damping", 0.0F, 1.0F, 0.5F, project::Unit::Normalized),
    perFrame("gain", -48.0F, 12.0F, 0.0F, project::Unit::Decibels),
});

/// Frames per partition: the effect's latency.
inline constexpr std::uint32_t kConvolutionPartition = 256;

/// What a Convolution was built from - its IR and where it came from - kept alive by
/// the node and compared by configMatches. Convolution.cpp, main thread.
struct ConvolutionPins;
void destroyConvolutionPins(ConvolutionPins* pins) noexcept;

class Convolution final : public Effect {
public:
    Convolution() = default;
    ~Convolution() override;
    Convolution(const Convolution&) = delete;
    Convolution& operator=(const Convolution&) = delete;
    Convolution(Convolution&&) = delete;
    Convolution& operator=(Convolution&&) = delete;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Convolution";
    }

    /// Main thread, before prepare(). Takes ownership of `pins`, which holds the IR.
    void setImpulse(std::span<const float> left, std::span<const float> right,
                    ConvolutionPins* pins) noexcept;
    [[nodiscard]] const ConvolutionPins* pins() const noexcept {
        return m_pins;
    }

    void copyConfigTo(Effect& fresh) const override;
    [[nodiscard]] bool isEquivalent(const Effect& other) const noexcept override;

protected:
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;
    [[nodiscard]] std::uint32_t effectLatency() const noexcept override {
        return kConvolutionPartition;
    }

private:
    std::array<dsp::Convolver, 2> m_convolver{};
    std::span<const float> m_irLeft;
    std::span<const float> m_irRight;
    ConvolutionPins* m_pins{nullptr};
};

} // namespace adx::effects
