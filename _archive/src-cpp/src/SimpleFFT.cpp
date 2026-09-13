#include "SimpleFFT.h"

#include <algorithm>
#include <cmath>

namespace SimpleFFT {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

void Transform(std::vector<std::complex<float>>& data) {
    size_t n = data.size();
    if (n <= 1) return;

    // Bit-reversal permutation.
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }

    // Iterative Cooley-Tukey butterflies.
    for (size_t len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * kPi / static_cast<float>(len);
        std::complex<float> wlen(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k) {
                std::complex<float> u = data[i + k];
                std::complex<float> v = data[i + k + len / 2] * w;
                data[i + k] = u + v;
                data[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

void ApplyHannWindow(const float* samples, size_t count, std::vector<std::complex<float>>& out) {
    out.resize(count);
    if (count <= 1) {
        if (count == 1) out[0] = std::complex<float>(samples[0], 0.0f);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        float w = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) / static_cast<float>(count - 1));
        out[i] = std::complex<float>(samples[i] * w, 0.0f);
    }
}

void ComputeLogMagnitudeSpectrum(const float* samples, size_t count, std::vector<float>& outMagnitudes) {
    std::vector<std::complex<float>> buf;
    ApplyHannWindow(samples, count, buf);
    Transform(buf);

    size_t half = count / 2;
    outMagnitudes.resize(half);
    for (size_t i = 0; i < half; ++i) {
        float mag = std::abs(buf[i]) / static_cast<float>(count);
        float db = 20.0f * std::log10(std::max(mag, 1e-6f));
        outMagnitudes[i] = std::clamp((db + 100.0f) / 100.0f, 0.0f, 1.0f); // -100..0 dB -> 0..1
    }
}

} // namespace SimpleFFT
