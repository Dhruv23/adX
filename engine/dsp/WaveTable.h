// Mipmapped, band-limited single-cycle tables.
//
// One table per octave of fundamental: level L holds only the harmonics that stay
// below Nyquist for every fundamental in its octave, so a note reads a table that has
// nothing to alias. This is what meets phase_4.md §4.1's -60 dBc aliasing bar at a
// 10 kHz fundamental; a 2-point polyBLEP cannot (phase_4.md §11).
//
// Tables are kWaveTableSize samples with one guard sample, so a linear read never
// wraps. Level L carries at most kWaveTableSize / 4 >> L harmonics - four table samples
// per cycle of its highest harmonic - which keeps linear interpolation's own error
// below the aliasing it exists to prevent.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "engine/dsp/Fft.h"
#include "engine/rt/OwnedArray.h"

namespace adx::dsp {

inline constexpr std::size_t kWaveTableSize = 4096;
inline constexpr std::size_t kWaveTableLevels = 11;
/// Harmonics in level 0.
inline constexpr std::size_t kWaveTableTopHarmonic = kWaveTableSize / 4;

/// The level whose harmonics all fit below Nyquist at a phase increment of
/// `increment` (fundamental / sample rate).
[[nodiscard]] std::size_t waveTableLevel(float increment) noexcept;

/// Highest harmonic stored at `level`.
[[nodiscard]] constexpr std::size_t harmonicsAtLevel(std::size_t level) noexcept {
    return level >= kWaveTableLevels ? 1 : (kWaveTableTopHarmonic >> level);
}

/// A set of frames (a 2D wavetable; one frame for a plain shape), each mipmapped.
class WaveTable {
public:
    WaveTable() = default;

    /// Main thread. Builds `frames` frames from sine-series amplitudes:
    /// `amplitudes[f * harmonics + (k - 1)]` is harmonic k's sine amplitude in frame
    /// f. Harmonics past kWaveTableTopHarmonic are ignored.
    void buildFromHarmonics(std::span<const float> amplitudes, std::size_t frames,
                            std::size_t harmonics);

    /// Main thread. Builds from single cycles of `frameLength` samples each - the
    /// de-facto `.wav` wavetable layout (2048-sample frames) - by band-limiting each
    /// level through the FFT.
    void buildFromCycles(std::span<const float> samples, std::size_t frameLength);

    /// Main thread. The same cycles with `steps - 1` frames inserted between each
    /// adjacent pair, interpolated spectrally: each harmonic's magnitude moves
    /// geometrically (linearly in dB) and its phase along the shorter arc. A linear
    /// crossfade between two frames dips the harmonics they do not share; this moves a
    /// formant from one place to another instead. Reading frame f * steps of the
    /// result is source frame f.
    void buildSpectralMorph(std::span<const float> samples, std::size_t frameLength,
                            std::size_t steps);

    [[nodiscard]] std::size_t frames() const noexcept {
        return m_frames;
    }

    /// A linear read of frame `frame`, level `level`, at `phase` in [0, 1).
    [[nodiscard]] float read(std::size_t frame, std::size_t level, float phase) const noexcept {
        const float* table = levelData(frame, level);
        const float position = phase * static_cast<float>(kWaveTableSize);
        const auto index = static_cast<std::size_t>(position);
        const float fraction = position - static_cast<float>(index);
        return table[index] + ((table[index + 1] - table[index]) * fraction);
    }

    [[nodiscard]] const float* levelData(std::size_t frame, std::size_t level) const noexcept {
        const std::size_t f = frame < m_frames ? frame : m_frames - 1;
        const std::size_t l = level < kWaveTableLevels ? level : kWaveTableLevels - 1;
        return m_data.view().data() + (((f * kWaveTableLevels) + l) * kStride);
    }

private:
    static constexpr std::size_t kStride = kWaveTableSize + 1;
    void allocateFrames(std::size_t frames);
    void synthesise(std::span<Complex> spectrum, std::size_t frame, const Fft& fft);

    rt::OwnedArray<float> m_data;
    std::size_t m_frames{0};
};

enum class Waveform : std::uint8_t { Sine, Saw, Square, Triangle };

/// The shared table for a classic shape, built on first use. Main thread: call it from
/// prepare() before any audio-thread read.
[[nodiscard]] const WaveTable& builtinWaveTable(Waveform shape);

} // namespace adx::dsp
