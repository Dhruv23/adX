// Measurement helpers shared by the DSP tests.
//
// FINAL_PLAN §9: "Every DSP primitive has a numerical test - impulse response,
// frequency response, or known-value comparison." These are the instruments those
// tests measure with. Double precision throughout, so the measurement is never the
// noise floor the test is reading.
#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <functional>
#include <numbers>
#include <vector>

#include "engine/dsp/Fft.h"
#include "engine/dsp/Window.h"

namespace adx::tests {

/// Power spectrum (|X|^2) of `samples` under a 4-term Blackman-Harris window, the
/// first n/2 + 1 bins. `samples.size()` must be a power of two.
inline std::vector<double> powerSpectrum(const std::vector<float>& samples) {
    const std::size_t n = samples.size();
    dsp::Fft fft;
    fft.prepare(n);
    std::vector<dsp::Complex> work(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double w = dsp::windowValue(dsp::WindowKind::BlackmanHarris, i, n);
        work[i] = dsp::Complex{static_cast<float>(samples[i] * w), 0.0F};
    }
    fft.forward(work);
    std::vector<double> power(n / 2 + 1);
    for (std::size_t k = 0; k <= n / 2; ++k) {
        const double re = work[k].real();
        const double im = work[k].imag();
        power[k] = (re * re) + (im * im);
    }
    return power;
}

/// Energy at bins that are not within `guard` bins of a harmonic of `fundamental`
/// (nor of DC), relative to the energy around the fundamental, in dB. What "aliasing
/// in dBc" means in phase_4.md §4.1.
inline double aliasingDbc(const std::vector<float>& samples, double fundamental, double sampleRate,
                          std::size_t guard = 8) {
    const std::vector<double> power = powerSpectrum(samples);
    const double binHz = sampleRate / static_cast<double>(samples.size());
    std::vector<bool> harmonic(power.size(), false);
    const auto mark = [&](double frequency) {
        const auto centre = static_cast<std::ptrdiff_t>(std::llround(frequency / binHz));
        for (std::ptrdiff_t b = centre - static_cast<std::ptrdiff_t>(guard);
             b <= centre + static_cast<std::ptrdiff_t>(guard); ++b) {
            if (b >= 0 && static_cast<std::size_t>(b) < harmonic.size()) {
                harmonic[static_cast<std::size_t>(b)] = true;
            }
        }
    };
    mark(0.0);
    for (double f = fundamental; f < sampleRate / 2.0; f += fundamental) {
        mark(f);
    }
    double alias = 0.0;
    for (std::size_t k = 0; k < power.size(); ++k) {
        if (!harmonic[k]) {
            alias += power[k];
        }
    }
    double carrier = 0.0;
    const auto centre = static_cast<std::ptrdiff_t>(std::llround(fundamental / binHz));
    for (std::ptrdiff_t b = centre - static_cast<std::ptrdiff_t>(guard);
         b <= centre + static_cast<std::ptrdiff_t>(guard); ++b) {
        if (b >= 0 && static_cast<std::size_t>(b) < power.size()) {
            carrier += power[static_cast<std::size_t>(b)];
        }
    }
    return 10.0 * std::log10(std::max(alias, 1e-300) / carrier);
}

/// The amplitude of a sinusoid at `frequency` in `samples`, read from the windowed
/// spectrum by summing the power in its main lobe.
inline double toneAmplitude(const std::vector<float>& samples, double frequency, double sampleRate,
                            std::size_t guard = 5) {
    const std::vector<double> power = powerSpectrum(samples);
    const std::size_t n = samples.size();
    const double binHz = sampleRate / static_cast<double>(n);
    const auto centre = static_cast<std::ptrdiff_t>(std::llround(frequency / binHz));
    double sum = 0.0;
    for (std::ptrdiff_t b = centre - static_cast<std::ptrdiff_t>(guard);
         b <= centre + static_cast<std::ptrdiff_t>(guard); ++b) {
        if (b >= 0 && static_cast<std::size_t>(b) < power.size()) {
            sum += power[static_cast<std::size_t>(b)];
        }
    }
    // Parseval for the window: sum over the lobe of |X|^2 = (A/2)^2 * sum(w^2) * ... -
    // normalised by the window's energy.
    double windowEnergy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = dsp::windowValue(dsp::WindowKind::BlackmanHarris, i, n);
        windowEnergy += w * w;
    }
    return 2.0 * std::sqrt(sum / (windowEnergy * static_cast<double>(n)));
}

/// The magnitude response, in dB, of a single-in single-out process measured by its
/// impulse response: `tick` is called once per sample with the input and returns the
/// output. `length` samples (a power of two) must be long enough for the response to
/// have decayed. Returns n/2 + 1 bins.
inline std::vector<double> impulseResponseDb(const std::function<float(float)>& tick,
                                             std::size_t length) {
    dsp::Fft fft;
    fft.prepare(length);
    std::vector<dsp::Complex> work(length);
    for (std::size_t i = 0; i < length; ++i) {
        work[i] = dsp::Complex{tick(i == 0 ? 1.0F : 0.0F), 0.0F};
    }
    fft.forward(work);
    std::vector<double> db(length / 2 + 1);
    for (std::size_t k = 0; k <= length / 2; ++k) {
        const double re = work[k].real();
        const double im = work[k].imag();
        db[k] = 10.0 * std::log10(std::max((re * re) + (im * im), 1e-300));
    }
    return db;
}

inline double toDb(double magnitude) {
    return 20.0 * std::log10(std::max(magnitude, 1e-300));
}

} // namespace adx::tests
