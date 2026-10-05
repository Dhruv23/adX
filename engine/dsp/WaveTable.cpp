// adx-thread: main
#include "engine/dsp/WaveTable.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "engine/dsp/Math.h"

namespace adx::dsp {

std::size_t waveTableLevel(float increment) noexcept {
    if (!(increment > 0.0F)) {
        return 0;
    }
    // The most harmonics a fundamental at this increment can carry below Nyquist.
    const float allowed = 0.5F / increment;
    std::size_t level = 0;
    while (level + 1 < kWaveTableLevels && static_cast<float>(harmonicsAtLevel(level)) > allowed) {
        ++level;
    }
    return level;
}

void WaveTable::allocateFrames(std::size_t frames) {
    m_frames = frames == 0 ? 1 : frames;
    m_data.allocate(m_frames * kWaveTableLevels * kStride);
}

void WaveTable::synthesise(std::span<Complex> spectrum, std::size_t frame, const Fft& fft) {
    // `spectrum` holds the full-band spectrum. Each level zeroes what it cannot carry
    // and inverts; level by level, so one forward transform serves all of them.
    std::vector<Complex> work(kWaveTableSize);
    for (std::size_t level = 0; level < kWaveTableLevels; ++level) {
        const std::size_t top = harmonicsAtLevel(level);
        for (std::size_t k = 0; k < kWaveTableSize; ++k) {
            const std::size_t harmonic = k <= kWaveTableSize / 2 ? k : kWaveTableSize - k;
            work[k] = harmonic != 0 && harmonic <= top ? spectrum[k] : Complex{};
        }
        fft.inverse(work);
        float* table = m_data.view().data() + (((frame * kWaveTableLevels) + level) * kStride);
        for (std::size_t i = 0; i < kWaveTableSize; ++i) {
            table[i] = work[i].real();
        }
        table[kWaveTableSize] = table[0];
    }
}

void WaveTable::buildFromHarmonics(std::span<const float> amplitudes, std::size_t frames,
                                   std::size_t harmonics) {
    allocateFrames(frames);
    Fft fft;
    fft.prepare(kWaveTableSize);
    std::vector<Complex> spectrum(kWaveTableSize);
    const float half = static_cast<float>(kWaveTableSize) / 2.0F;
    for (std::size_t f = 0; f < m_frames; ++f) {
        std::ranges::fill(spectrum, Complex{});
        for (std::size_t k = 1; k <= harmonics && k <= kWaveTableTopHarmonic; ++k) {
            const std::size_t index = (f * harmonics) + (k - 1);
            const float amplitude = index < amplitudes.size() ? amplitudes[index] : 0.0F;
            // A sine of amplitude a at bin k is X[k] = -i N a / 2, X[N - k] = +i N a / 2.
            spectrum[k] = Complex{0.0F, -half * amplitude};
            spectrum[kWaveTableSize - k] = Complex{0.0F, half * amplitude};
        }
        synthesise(spectrum, f, fft);
    }
}

namespace {

[[nodiscard]] float magnitudeOf(Complex c) noexcept {
    return std::sqrt((c.real() * c.real()) + (c.imag() * c.imag()));
}

/// The spectrum of cycle `f`, resampled to the table size linearly before analysis.
/// A user cycle of 2048 samples maps to 4096 exactly two to one, so this is
/// interpolation between known samples, not invention.
void cycleSpectrum(std::span<const float> samples, std::size_t frameLength, std::size_t f,
                   std::span<Complex> spectrum, const Fft& fft) {
    for (std::size_t i = 0; i < kWaveTableSize; ++i) {
        const double position = static_cast<double>(i) * static_cast<double>(frameLength) /
                                static_cast<double>(kWaveTableSize);
        const auto index = static_cast<std::size_t>(position);
        const auto fraction = static_cast<float>(position - static_cast<double>(index));
        const float a = samples[(f * frameLength) + (index % frameLength)];
        const float b = samples[(f * frameLength) + ((index + 1) % frameLength)];
        spectrum[i] = Complex{a + ((b - a) * fraction), 0.0F};
    }
    fft.forward(spectrum);
}

} // namespace

void WaveTable::buildFromCycles(std::span<const float> samples, std::size_t frameLength) {
    if (frameLength == 0) {
        allocateFrames(1);
        return;
    }
    const std::size_t frames = samples.size() / frameLength;
    allocateFrames(frames);
    Fft fft;
    fft.prepare(kWaveTableSize);
    std::vector<Complex> spectrum(kWaveTableSize);
    for (std::size_t f = 0; f < m_frames && f < frames; ++f) {
        cycleSpectrum(samples, frameLength, f, spectrum, fft);
        synthesise(spectrum, f, fft);
    }
}

void WaveTable::buildSpectralMorph(std::span<const float> samples, std::size_t frameLength,
                                   std::size_t steps) {
    const std::size_t sources = frameLength == 0 ? 0 : samples.size() / frameLength;
    steps = std::max<std::size_t>(steps, 1);
    if (sources < 2) {
        buildFromCycles(samples, frameLength);
        return;
    }
    allocateFrames(((sources - 1) * steps) + 1);
    Fft fft;
    fft.prepare(kWaveTableSize);
    std::vector<Complex> a(kWaveTableSize);
    std::vector<Complex> b(kWaveTableSize);
    std::vector<Complex> mixed(kWaveTableSize);
    constexpr float kFloor = 1e-9F;
    cycleSpectrum(samples, frameLength, 0, b, fft);
    for (std::size_t f = 0; f + 1 < sources; ++f) {
        a.swap(b);
        cycleSpectrum(samples, frameLength, f + 1, b, fft);
        const std::size_t last = f + 2 == sources ? steps : steps - 1;
        for (std::size_t s = 0; s <= last; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(steps);
            for (std::size_t k = 0; k <= kWaveTableSize / 2; ++k) {
                const float magA = magnitudeOf(a[k]);
                const float magB = magnitudeOf(b[k]);
                const auto magnitude =
                    static_cast<float>(dsp::exp(((1.0 - t) * dsp::log(magA + kFloor)) +
                                                (t * dsp::log(magB + kFloor))) -
                                       kFloor);
                // The phase along the shorter arc, without atan2: a normalised lerp of
                // the two unit vectors (exact at the ends, monotone between).
                const Complex unitA = magA > 0.0F ? a[k] / magA : Complex{1.0F, 0.0F};
                const Complex unitB = magB > 0.0F ? b[k] / magB : unitA;
                const Complex direction = (unitA * (1.0F - t)) + (unitB * t);
                const float length = magnitudeOf(direction);
                mixed[k] =
                    length > 0.0F ? direction * (std::max(0.0F, magnitude) / length) : Complex{};
                if (k != 0 && k != kWaveTableSize / 2) {
                    mixed[kWaveTableSize - k] = std::conj(mixed[k]);
                }
            }
            synthesise(mixed, (f * steps) + s, fft);
        }
    }
}

namespace {

[[nodiscard]] WaveTable buildShape(Waveform shape) {
    std::vector<float> amplitudes(kWaveTableTopHarmonic, 0.0F);
    for (std::size_t k = 1; k <= kWaveTableTopHarmonic; ++k) {
        const auto kk = static_cast<double>(k);
        double a = 0.0;
        switch (shape) {
        case Waveform::Sine:
            a = k == 1 ? 1.0 : 0.0;
            break;
        case Waveform::Saw:
            // A ramp from -1 to 1 over the cycle, its jump at phase 0 - the polyBLEP
            // saw's shape: -(2/pi) sum sin(2 pi k phase) / k.
            a = -2.0 / (kPi * kk);
            break;
        case Waveform::Square:
            // +1 for the first half cycle.
            a = (k % 2 == 1) ? 4.0 / (kPi * kk) : 0.0;
            break;
        case Waveform::Triangle:
            // 0 at phase 0, rising to 1 at a quarter cycle.
            if (k % 2 == 1) {
                const double sign = ((k - 1) / 2) % 2 == 0 ? 1.0 : -1.0;
                a = sign * 8.0 / (kPi * kPi * kk * kk);
            }
            break;
        }
        amplitudes[k - 1] = static_cast<float>(a);
    }
    WaveTable table;
    table.buildFromHarmonics(amplitudes, 1, kWaveTableTopHarmonic);
    return table;
}

} // namespace

const WaveTable& builtinWaveTable(Waveform shape) {
    static const std::array<WaveTable, 4> kTables = [] {
        std::array<WaveTable, 4> tables;
        tables[0] = buildShape(Waveform::Sine);
        tables[1] = buildShape(Waveform::Saw);
        tables[2] = buildShape(Waveform::Square);
        tables[3] = buildShape(Waveform::Triangle);
        return tables;
    }();
    return kTables[static_cast<std::size_t>(shape)];
}

} // namespace adx::dsp
