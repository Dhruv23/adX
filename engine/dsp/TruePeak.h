// True-peak estimation by 4x oversampling (ITU-R BS.1770-4, Annex 2).
//
// A sample peak can miss the peak of the waveform between samples by up to 3 dB; a
// limiter that only watches samples lets those through, and a converter downstream
// clips them. This interpolates three points between every pair of samples with the
// 48-tap polyphase filter the standard specifies and reports the largest magnitude.
// The limiter and the master meter use it; per-insert meters do not, because it is
// not free (phase_4.md §4.10).
#pragma once

#include <array>
#include <cstddef>

namespace adx::dsp {

class TruePeak {
public:
    void reset() noexcept {
        m_history.fill(0.0F);
        m_ringIndex = 0;
    }

    /// Pushes one sample; returns the largest magnitude among it and the three
    /// interpolated points before it. Delayed by the filter's half-length (6 samples):
    /// the peak returned belongs to the sample pushed 6 calls ago.
    [[nodiscard]] float push(float sample) noexcept;

    /// The delay, in samples, between a sample going in and its peak coming out.
    static constexpr std::size_t kDelay = 6;

private:
    static constexpr std::size_t kTaps = 12;
    std::array<float, kTaps> m_history{};
    std::size_t m_ringIndex{0};
};

} // namespace adx::dsp
