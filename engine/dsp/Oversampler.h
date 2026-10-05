// 4x oversampling for nonlinear stages: two cascaded half-band FIR stages.
//
// A waveshaper makes harmonics above Nyquist that fold back as inharmonic aliasing.
// Running it at four times the rate moves the fold point to 96 kHz (at 48 kHz) and
// the half-band filters remove what lies above the audio band before decimating
// (phase_4.md §4.9, Overdrive: "a polyphase half-band pair so the clipper does not
// alias").
//
// Each stage is a linear-phase Kaiser-windowed half-band FIR - every second tap is
// exactly zero, so half the multiplies vanish. Stage 1 (base <-> 2x) has 65 taps,
// stage 2 (2x <-> 4x) 33, its transition band being far wider. Linear phase means a
// fixed delay: stage 1 delays by 32 samples at 2x on the way up and again on the way
// down, 32 base samples; stage 2 by 16 + 16 at 4x, 8 base samples. kOversampleLatency
// is their sum, an integer by choice of tap counts, so an effect can declare it to
// PDC exactly (effect_declares_latency).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace adx::dsp {

inline constexpr std::size_t kHalfBandTaps1 = 65;
inline constexpr std::size_t kHalfBandTaps2 = 33;
inline constexpr std::uint32_t kOversampleLatency = 40;

/// One 2x stage, one channel. Taps are shared and built once (halfBandTaps).
template<std::size_t Taps> class HalfBand {
    static_assert(Taps % 4 == 1, "a half-band with an even centre delay");

public:
    void reset() noexcept {
        m_up.fill(0.0F);
        m_down.fill(0.0F);
        m_upIndex = 0;
        m_downIndex = 0;
    }

    /// One input sample in, two at twice the rate out (out[0] first).
    void upsample(float x, const float* taps, float* out) noexcept {
        // The zero-stuffed stream is x, 0, x, 0...: output 2m reads the even taps
        // against the input history, output 2m + 1 the odd taps.
        push(m_up, m_upIndex, x);
        float even = 0.0F;
        float odd = 0.0F;
        for (std::size_t k = 0; k < kHalf; ++k) {
            const float h = read(m_up, m_upIndex, k);
            even += taps[2 * k] * h;
            if ((2 * k) + 1 < Taps) {
                odd += taps[(2 * k) + 1] * h;
            }
        }
        // x 2: zero-stuffing halved the energy.
        out[0] = 2.0F * even;
        out[1] = 2.0F * odd;
    }

    /// Two samples at twice the rate in, one out: filter, keep every second.
    [[nodiscard]] float downsample(float first, float second, const float* taps) noexcept {
        push(m_down, m_downIndex, first);
        push(m_down, m_downIndex, second);
        float sum = 0.0F;
        // Output aligned to the *first* of the pair: tap n against the sample n back.
        for (std::size_t n = 0; n < Taps; ++n) {
            sum += taps[n] * read(m_down, m_downIndex, n + 1);
        }
        return sum;
    }

private:
    static constexpr std::size_t kHalf = (Taps + 1) / 2;
    static constexpr std::size_t kRing = 128;
    static_assert(Taps + 2 <= kRing);

    static void push(std::array<float, kRing>& ring, std::size_t& index, float x) noexcept {
        index = (index + 1) & (kRing - 1);
        ring[index] = x;
    }
    /// The sample `back` places before the newest.
    [[nodiscard]] static float read(const std::array<float, kRing>& ring, std::size_t index,
                                    std::size_t back) noexcept {
        return ring[(index - back) & (kRing - 1)];
    }

    std::array<float, kRing> m_up{};
    std::array<float, kRing> m_down{};
    std::size_t m_upIndex{0};
    std::size_t m_downIndex{0};
};

/// The taps of a `taps`-long half-band at the given Kaiser beta. Main thread; built
/// with deterministic math, so every configuration filters identically.
[[nodiscard]] const float* halfBandTaps1() noexcept;
[[nodiscard]] const float* halfBandTaps2() noexcept;
/// Main thread: builds the shared taps before any audio-thread use.
void prepareOversampler();

/// 4x up, a per-sample function at 4x, 4x down; one channel.
class Oversampler4x {
public:
    void reset() noexcept {
        m_up1.reset();
        m_up2.reset();
        m_down2.reset();
        m_down1.reset();
    }

    template<class Shape> [[nodiscard]] float process(float x, Shape&& shape) noexcept {
        const float* taps1 = halfBandTaps1();
        const float* taps2 = halfBandTaps2();
        std::array<float, 2> twice{};
        m_up1.upsample(x, taps1, twice.data());
        // One stage-2 filter over the 2x stream, in time order.
        std::array<float, 4> four{};
        m_up2.upsample(twice[0], taps2, four.data());
        m_up2.upsample(twice[1], taps2, four.data() + 2);
        for (float& s : four) {
            s = shape(s);
        }
        const float a = m_down2.downsample(four[0], four[1], taps2);
        const float b = m_down2.downsample(four[2], four[3], taps2);
        return m_down1.downsample(a, b, taps1);
    }

private:
    HalfBand<kHalfBandTaps1> m_up1;
    HalfBand<kHalfBandTaps2> m_up2;
    HalfBand<kHalfBandTaps2> m_down2;
    HalfBand<kHalfBandTaps1> m_down1;
};

} // namespace adx::dsp
