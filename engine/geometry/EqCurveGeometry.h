// adx-thread: main
//
// An EQ's response curve, its band handles, and the live spectrum under it
// (phase_6.md §4.4: the hand-built EQ editor "needs a draggable response curve with a
// live spectrum overlay").
//
// The response is computed from the same biquad designs the effect runs
// (dsp::biquadCoefficients), so the curve is the filter, not a drawing of one. The
// spectrum comes from ParametricEq's tap - the post-EQ signal the audio thread already
// writes into an overwrite ring - through the engine's own FFT.
//
// World space: x is position on a log frequency axis, 0 at 20 Hz and 1 at 20 kHz; y is
// 0 at +range dB and 1 at -range dB. Buffers (three floats a vertex):
//
//   curve     lines      the summed response
//   handles   triangles  one square per active band, at (frequency, gain)
//   spectrum  lines      the tap's log-magnitude spectrum
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "engine/dsp/Fft.h"
#include "engine/dsp/RbjFilter.h"
#include "engine/geometry/GeometryBuffer.h"

namespace adx::geometry {

inline constexpr double kEqLowHz = 20.0;
inline constexpr double kEqHighHz = 20000.0;

/// One band of an EQ as the effect designs it.
struct EqBand {
    dsp::BiquadKind kind{dsp::BiquadKind::Peaking};
    double frequency{1000.0};
    double q{0.707};
    double gainDb{0.0};
};

/// The bands `type` ("ParametricEq" or "EQ") runs with `params`: the effect's own
/// parameters, in descriptor order, as the graph lays them out. Empty for anything else.
[[nodiscard]] std::vector<EqBand> eqBandsOf(std::string_view type, std::span<const float> params);

/// The x position of `frequency` on the curve's axis.
[[nodiscard]] float eqAxisPosition(double frequency) noexcept;
/// The frequency at axis position `x`.
[[nodiscard]] double eqAxisFrequency(double x) noexcept;

class EqCurveGeometry {
public:
    /// The response of `bands` at `points` log-spaced frequencies, into `curve` and
    /// `handles`. `rangeDb` is the axis's half-height.
    void build(std::span<const EqBand> bands, double sampleRate, float rangeDb,
               std::uint32_t points);

    /// The spectrum of the latest `samples` (a power of two, 4096 at most) into
    /// `spectrum`: the engine's Hann-windowed log magnitude, on the same axis.
    void buildSpectrum(std::span<const float> samples, double sampleRate);

    [[nodiscard]] const GeometryBuffer& curve() const noexcept {
        return m_curve;
    }
    [[nodiscard]] const GeometryBuffer& handles() const noexcept {
        return m_handles;
    }
    [[nodiscard]] const GeometryBuffer& spectrum() const noexcept {
        return m_spectrum;
    }
    [[nodiscard]] GeometryBuffer& buffer(std::size_t index) noexcept;

    /// The summed response in dB at each point of the last build: what tests compare.
    [[nodiscard]] std::span<const double> responseDb() const noexcept {
        return m_response;
    }

private:
    GeometryBuffer m_curve;
    GeometryBuffer m_handles;
    GeometryBuffer m_spectrum;
    std::vector<double> m_response;
    std::vector<float> m_scratch;
    dsp::Fft m_fft;
    std::vector<dsp::Complex> m_bins;
    std::vector<float> m_magnitudes;
};

} // namespace adx::geometry
