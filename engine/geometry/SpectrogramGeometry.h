// adx-thread: main
//
// The spectrogram waterfall (phase_10.md §4.4), ported from
// _archive/src-cpp/include/SpectrogramHistory.h: a fixed-capacity ring of rows, the
// newest overwriting the oldest. v1 kept a vector of float vectors and drew each as a
// line; this keeps one RGBA8 image, `rows` x `columns`, written a row at a time and
// uploaded as a texture - so scrolling costs one row's colour lookup, never a vertex
// per pixel, and nothing allocates after configure() (spectrogram_ring_fixed).
//
//   columns   log frequency, 20 Hz to 20 kHz, as SpectrumGeometry's
//   colour    a fixed dark-to-bright ramp over floorDb..0 dB
//   order     row head() is the newest; the image is drawn from head() + 1 (the oldest)
//             round to head(), which is two blits, not a copy
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace adx::geometry {

class SpectrogramGeometry {
public:
    /// Allocates the image. The only call that allocates; anything else that changes
    /// shape is this again.
    void configure(std::uint32_t rows, std::uint32_t columns, float floorDb = -90.0F);

    /// One new row from FFT magnitudes in dB (bin k at k * sampleRate / (2 * (size-1))).
    void push(std::span<const float> db, std::uint32_t sampleRate) noexcept;

    [[nodiscard]] std::uint32_t rows() const noexcept {
        return m_rows;
    }
    [[nodiscard]] std::uint32_t columns() const noexcept {
        return m_columns;
    }
    /// The newest row's index.
    [[nodiscard]] std::uint32_t head() const noexcept {
        return m_head;
    }
    /// Rows written since configure().
    [[nodiscard]] std::uint64_t pushed() const noexcept {
        return m_pushed;
    }
    /// rows x columns x 4 bytes, RGBA, row-major.
    [[nodiscard]] std::span<const std::uint8_t> pixels() const noexcept {
        return m_pixels;
    }
    [[nodiscard]] const std::uint8_t* data() const noexcept {
        return m_pixels.data();
    }

private:
    std::uint32_t m_rows{0};
    std::uint32_t m_columns{0};
    std::uint32_t m_head{0};
    std::uint64_t m_pushed{0};
    float m_floorDb{-90.0F};
    std::vector<std::uint8_t> m_pixels;
    /// Per column, the first and last bin it covers, for a given bin count (cached).
    std::vector<std::uint32_t> m_firstBin;
    std::vector<std::uint32_t> m_lastBin;
    std::size_t m_mappedBins{0};
    std::uint32_t m_mappedRate{0};
};

} // namespace adx::geometry
