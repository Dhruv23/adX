// White and pink noise from a seeded xorshift32.
//
// Seeded, never rand(): rand() locks, and its sequence is global state, which would
// make two renders of one project differ by whatever else drew from it first
// (phase_3.md §4.10). A voice seeds its generator from its own identity, so the same
// note in the same project always makes the same noise.
#pragma once

#include <array>
#include <cstdint>

namespace adx::dsp {

class WhiteNoise {
public:
    explicit WhiteNoise(std::uint32_t seed = 0x9E3779B9U) noexcept {
        reseed(seed);
    }

    /// Zero is a fixed point of xorshift; it is replaced, not used.
    void reseed(std::uint32_t seed) noexcept {
        m_state = seed == 0 ? 0x9E3779B9U : seed;
    }

    [[nodiscard]] std::uint32_t nextBits() noexcept {
        m_state ^= m_state << 13U;
        m_state ^= m_state >> 17U;
        m_state ^= m_state << 5U;
        return m_state;
    }

    /// Uniform in [-1, 1).
    [[nodiscard]] float next() noexcept {
        // The top 24 bits, so every value is exactly representable in a float.
        const auto bits = static_cast<float>(nextBits() >> 8U);
        return (bits * (2.0F / 16777216.0F)) - 1.0F;
    }

    /// Uniform in [0, 1).
    [[nodiscard]] float nextUnit() noexcept {
        return static_cast<float>(nextBits() >> 8U) * (1.0F / 16777216.0F);
    }

    [[nodiscard]] std::uint32_t state() const noexcept {
        return m_state;
    }

private:
    std::uint32_t m_state{0x9E3779B9U};
};

/// Pink noise by Voss-McCartney: sixteen white rows, row k updated every 2^k samples,
/// summed. -3 dB per octave over the audio band; the test fits the slope.
class PinkNoise {
public:
    explicit PinkNoise(std::uint32_t seed = 0x2545F491U) noexcept : m_white(seed) {
        reset();
    }

    void reseed(std::uint32_t seed) noexcept {
        m_white.reseed(seed);
        reset();
    }

    void reset() noexcept {
        m_sum = 0.0F;
        for (float& row : m_rows) {
            row = m_white.next();
            m_sum += row;
        }
        m_counter = 0;
    }

    [[nodiscard]] float next() noexcept {
        ++m_counter;
        // The row to update is the number of trailing zeros of the counter: row 0
        // every other sample, row 1 every fourth, and so on.
        std::uint32_t row = 0;
        std::uint32_t c = m_counter;
        while ((c & 1U) == 0U && row + 1 < kRows) {
            c >>= 1U;
            ++row;
        }
        m_rows[row] = m_white.next();
        // Summed afresh rather than updated by difference: a running sum would random-
        // walk away from the rows' true sum by its rounding error, sample after sample.
        m_sum = 0.0F;
        for (const float value : m_rows) {
            m_sum += value;
        }
        // Plus one white sample for the top octave; scaled to roughly unit peak.
        return (m_sum + m_white.next()) * (1.0F / static_cast<float>(kRows + 1));
    }

private:
    static constexpr std::uint32_t kRows = 16;
    WhiteNoise m_white;
    std::array<float, kRows> m_rows{};
    float m_sum{0.0F};
    std::uint32_t m_counter{0};
};

} // namespace adx::dsp
