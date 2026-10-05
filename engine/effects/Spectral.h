// Spectral effects: PitchShifter and SpectralFreeze, on one short-time Fourier frame
// engine (StftChannel).
//
// StftChannel is the streaming skeleton: a Hann-windowed frame of `size` samples every
// `size / 4`, handed to the effect as a spectrum to change in place, then windowed again
// and overlap-added. With Hann on both sides at 75% overlap the windows sum to a
// constant 1.5, so an untouched spectrum comes back as the input, delayed by `size`
// samples - the latency both effects declare (measured: effect_declares_latency_fixed).
// A sample enters a frame at its newest end and leaves once the frames that hold it
// have all been added, which is one frame's length later.
//
//   PitchShifter    the phase vocoder (phase_4.md §4.9): each bin's true frequency from
//                   its phase advance, bins moved by the pitch ratio, phases re-summed
//                   at the new frequencies. 1024-point frames, 1024 samples of latency.
//                   RubberBand's realtime mode was the plan's first choice; it is not a
//                   dependency of this tree (phase_4.md §11), and this path is the one
//                   §4.9 names for low-latency use.
//   SpectralFreeze  holds the magnitude spectrum when `freeze` turns on, and plays it
//                   back with a fresh random phase every frame - a sustained smear of
//                   one instant. The change in or out of the freeze blends over four
//                   frames, so it is not a click. 2048-point frames.
#pragma once

#include <array>
#include <cstdint>

#include "engine/dsp/Fft.h"
#include "engine/dsp/Noise.h"
#include "engine/effects/Effect.h"
#include "engine/effects/Params.h"
#include "engine/rt/OwnedArray.h"

namespace adx::effects {

enum class PitchShifterParam : std::uint32_t { Semitones, Cents, Count };
inline constexpr auto kPitchShifterParams = std::to_array<project::ParamDescriptor>({
    param("semitones", -24.0F, 24.0F, 0.0F, project::Unit::Semitones),
    param("cents", -100.0F, 100.0F, 0.0F, project::Unit::Cents),
});

enum class SpectralFreezeParam : std::uint32_t { Freeze, Gain, Count };
inline constexpr auto kSpectralFreezeParams = std::to_array<project::ParamDescriptor>({
    param("freeze", 0.0F, 1.0F, 0.0F, project::Unit::Boolean, project::ScaleKind::Stepped),
    perFrame("gain", -24.0F, 12.0F, 0.0F, project::Unit::Decibels),
});

inline constexpr std::size_t kPitchFrame = 1024;
inline constexpr std::size_t kFreezeFrame = 2048;
inline constexpr std::size_t kStftOverlap = 4;

/// One channel's streaming STFT. The effect owns the Fft and the per-bin state.
class StftChannel {
public:
    /// Main thread.
    void allocate(std::size_t size);
    void reset() noexcept;

    [[nodiscard]] std::size_t size() const noexcept {
        return m_size;
    }
    [[nodiscard]] std::size_t hop() const noexcept {
        return m_size / kStftOverlap;
    }
    /// Input to output, in samples: one frame.
    [[nodiscard]] std::uint32_t latency() const noexcept {
        return static_cast<std::uint32_t>(m_size);
    }

    /// One sample in, one out. When a frame is due, `frame(spectrum)` is called with the
    /// windowed frame's spectrum (size() bins) to change in place.
    template<class Frame>
    [[nodiscard]] float process(float x, const dsp::Fft& fft, Frame&& frame) noexcept {
        // m_fill runs from latency() to size(): the input frame's newest hop is being
        // filled while the oldest finished hop of output plays out.
        const std::span<float> input = m_input.view();
        const std::span<float> output = m_output.view();
        input[m_fill] = x;
        const float y = output[m_fill - readOffset()];
        if (++m_fill == m_size) {
            runFrame(fft);
            frame(m_work.view());
            finishFrame(fft);
        }
        return y;
    }

private:
    /// Where the hop being filled starts in the input frame; output index 0 is read then.
    [[nodiscard]] std::size_t readOffset() const noexcept {
        return m_size - hop();
    }
    void runFrame(const dsp::Fft& fft) noexcept;
    void finishFrame(const dsp::Fft& fft) noexcept;

    std::size_t m_size{0};
    std::size_t m_fill{0};
    /// The last size() input samples; the window of output being played out (hop
    /// samples at the front are ready); the overlap-add accumulator.
    rt::OwnedArray<float> m_input;
    rt::OwnedArray<float> m_output;
    rt::OwnedArray<float> m_accumulator;
    rt::OwnedArray<float> m_window;
    rt::OwnedArray<dsp::Complex> m_work;
};

class PitchShifter final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "PitchShifter";
    }

protected:
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;
    [[nodiscard]] std::uint32_t effectLatency() const noexcept override {
        return static_cast<std::uint32_t>(kPitchFrame);
    }

private:
    static constexpr std::size_t kBins = (kPitchFrame / 2) + 1;
    void shift(std::span<dsp::Complex> spectrum, std::size_t channel) noexcept;

    dsp::Fft m_fft;
    std::array<StftChannel, 2> m_stft{};
    /// Per channel: the last analysis phase and the running synthesis phase, in turns.
    std::array<std::array<double, kBins>, 2> m_lastPhase{};
    std::array<std::array<double, kBins>, 2> m_sumPhase{};
    std::array<float, kBins> m_magnitude{};
    std::array<float, kBins> m_frequency{};
    std::array<float, kBins> m_synthMagnitude{};
    std::array<float, kBins> m_synthFrequency{};
    float m_ratio{1.0F};
};

class SpectralFreeze final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "SpectralFreeze";
    }

protected:
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;
    [[nodiscard]] std::uint32_t effectLatency() const noexcept override {
        return static_cast<std::uint32_t>(kFreezeFrame);
    }

private:
    static constexpr std::size_t kBins = (kFreezeFrame / 2) + 1;
    void frame(std::span<dsp::Complex> spectrum, std::size_t channel) noexcept;

    dsp::Fft m_fft;
    std::array<StftChannel, 2> m_stft{};
    std::array<std::array<float, kBins>, 2> m_held{};
    dsp::WhiteNoise m_random{0xF4EE2EU};
    bool m_frozen{false};
    /// Set when the freeze turns on: the next frame of each channel is the one held.
    std::array<bool, 2> m_capture{};
    /// 0 = the input passes; 1 = the held spectrum plays. Steps once per frame.
    std::array<float, 2> m_blend{};
};

} // namespace adx::effects
