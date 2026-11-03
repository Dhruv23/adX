// adx-thread: main
//
// Dither for bit-depth reduction: TPDF, or TPDF with noise shaping (phase_8.md §3.5).
//
// Applied last - after normalization, after resampling - and only when the target
// stores integers narrower than the float it came from. Dithering a 32-bit float
// export is a bug (export_dither_only_on_reduction), so the export job asks
// integerBits() before calling this at all.
//
// Deterministic: the noise comes from a fixed-seed generator of adX's own, so an export
// of an unchanged project is the same bytes every time and the golden corpus can hash
// a dithered 16-bit file like any other.
#pragma once

#include <cstdint>
#include <span>

namespace adx::render {

enum class DitherKind : std::uint8_t {
    Off,
    /// Triangular PDF, two LSB peak to peak: removes the quantization error's
    /// correlation with the signal, at a flat noise floor.
    Tpdf,
    /// TPDF through a three-tap error-feedback filter (Wannamaker's E-weighted
    /// coefficients) that moves the noise towards the top octave, where it is hardest
    /// to hear.
    Shaped,
};

[[nodiscard]] const char* toString(DitherKind kind) noexcept;

/// Quantizes interleaved `audio` in place to `bits`, dithered: every sample afterwards
/// is exactly k / 2^(bits-1) for an integer k in range, so the encoder's rounding is
/// exact. `Off` quantizes without dither.
void quantize(std::span<float> audio, std::uint32_t channels, std::uint32_t bits, DitherKind kind,
              std::uint32_t seed = 0x5EEDU);

} // namespace adx::render
