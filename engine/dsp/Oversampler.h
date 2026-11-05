// 4x oversampling for nonlinear stages: two cascaded half-band FIR stages.
//
// A waveshaper makes harmonics above Nyquist that fold back as inharmonic aliasing.
// Running it at four times the rate moves the fold point to 96 kHz (at 48 kHz) and
// the half-band filters remove what lies above the audio band before decimating
// (phase_4.md §4.9, Overdrive: "a polyphase half-band pair so the clipper does not
// alias").
//
// Each stage is a linear-phase Kaiser-windowed half-band FIR - every second tap is
// exactly zero, so half the multiplies vanish, and the rest are symmetric, so half of
// those again (FoldedTaps). Stage 1 (base <-> 2x) has 65 taps,
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

/// A half-band's taps as the filter loops read them. Linear phase makes the taps
/// symmetric, h[n] = h[Taps - 1 - n], so each pair of equal taps is applied once to the
/// sum of the two samples it weighs: half the multiplies again, on top of the zero taps
/// a half-band never computes.
template<std::size_t Taps> struct FoldedTaps {
    /// Odd taps 1, 3, 5, ... up to the centre: the upsampler's odd phase reads them
    /// against pairs of input history, the downsampler against pairs of its window.
    static constexpr std::size_t kPairs = (Taps - 1) / 4;
    std::array<float, kPairs> pairs{};
    float centre{0.0F};
};

/// One 2x stage, one channel. Taps are shared and built once (foldedTaps1/2).
template<std::size_t Taps> class HalfBand {
    static_assert(Taps % 4 == 1, "a half-band with an even centre delay");

public:
    using Folded = FoldedTaps<Taps>;

    void reset() noexcept {
        m_up.fill(0.0F);
        m_down.fill(0.0F);
        m_upIndex = 0;
        m_downIndex = 0;
    }

    /// One input sample in, two at twice the rate out (out[0] first).
    void upsample(float x, const Folded& taps, float* out) noexcept {
        // The zero-stuffed stream is x, 0, x, 0...: output 2m reads the even taps
        // against the input history, output 2m + 1 the odd taps. Of the even taps only
        // the centre is not zero, so the even phase is one multiply.
        const float* history = push(m_up, m_upIndex, x); // history[-k]: k samples back
        const float even = taps.centre * history[-static_cast<std::ptrdiff_t>(kCentre / 2)];
        // Odd tap 2k + 1 weighs the sample k back, and equals odd tap 2(K - 1 - k) + 1.
        constexpr std::size_t kOdd = (Taps - 1) / 2; // K
        out[0] = 2.0F * even;                        // x 2: zero-stuffing halved the energy
        out[1] = 2.0F * dot<Folded::kPairs, 1>(taps.pairs.data(), history, 0, kOdd - 1);
    }

    /// Two samples at twice the rate in, one out: filter, keep every second.
    [[nodiscard]] float downsample(float first, float second, const Folded& taps) noexcept {
        push(m_down, m_downIndex, first);
        const float* window = push(m_down, m_downIndex, second);
        // Output aligned to the *first* of the pair: tap n against the sample n + 1
        // back. Odd tap n = 2t + 1 pairs with tap Taps - 1 - n, which reads the sample
        // Taps - n = Taps - 1 - 2t back.
        const float pairs = dot<Folded::kPairs, 2>(taps.pairs.data(), window, 2, Taps - 1);
        return pairs + (taps.centre * window[-static_cast<std::ptrdiff_t>(kCentre + 1)]);
    }

private:
    static constexpr std::size_t kCentre = Taps / 2;
    static_assert(kCentre % 2 == 0, "the centre tap is an even one");
    /// Samples of history kept; each is written twice, at i and i + kRing, so the last
    /// kRing samples always sit in one run of memory, newest last, and the loops below
    /// read them with plain offsets rather than a mask per tap.
    static constexpr std::size_t kRing = 128;
    static_assert(Taps + 2 <= kRing); // the downsampler looks Taps - 1 back

    using Ring = std::array<float, 2 * kRing>;

    /// Writes x and returns a pointer to it: p[-k] is the sample k places before.
    static const float* push(Ring& ring, std::size_t& index, float x) noexcept {
        index = (index + 1) & (kRing - 1);
        ring[index] = x;
        ring[index + kRing] = x;
        return ring.data() + index + kRing;
    }

    /// The sum over t of taps[t] * (p[-(near + step t)] + p[-(far - step t)]): pair t's
    /// two samples, `near` and `far` places back for t = 0 and moving towards each
    /// other. Four independent partial sums, combined in a fixed order at the end. The
    /// order is written out, so every build adds the same numbers in the same order,
    /// vectorised or not.
    template<std::size_t N, std::size_t Step>
    [[nodiscard]] static float dot(const float* taps, const float* p, std::size_t near,
                                   std::size_t far) noexcept {
        static_assert(N % 4 == 0, "four partial sums");
        std::array<float, 4> sum{};
        for (std::size_t j = 0; j < N; j += 4) {
            for (std::size_t lane = 0; lane < 4; ++lane) {
                const std::size_t t = j + lane;
                const auto a = static_cast<std::ptrdiff_t>(near + (Step * t));
                const auto b = static_cast<std::ptrdiff_t>(far - (Step * t));
                sum[lane] += taps[t] * (p[-a] + p[-b]);
            }
        }
        return (sum[0] + sum[1]) + (sum[2] + sum[3]);
    }

    Ring m_up{};
    Ring m_down{};
    std::size_t m_upIndex{0};
    std::size_t m_downIndex{0};
};

/// The taps of a `taps`-long half-band at the given Kaiser beta. Main thread; built
/// with deterministic math, so every configuration filters identically.
[[nodiscard]] const float* halfBandTaps1() noexcept;
[[nodiscard]] const float* halfBandTaps2() noexcept;
/// The same taps, folded for the filter loops.
[[nodiscard]] const FoldedTaps<kHalfBandTaps1>& foldedTaps1() noexcept;
[[nodiscard]] const FoldedTaps<kHalfBandTaps2>& foldedTaps2() noexcept;

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
        const FoldedTaps<kHalfBandTaps1>& taps1 = foldedTaps1();
        const FoldedTaps<kHalfBandTaps2>& taps2 = foldedTaps2();
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
