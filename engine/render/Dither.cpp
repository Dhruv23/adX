// adx-thread: main
#include "engine/render/Dither.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace adx::render {

namespace {

/// xorshift32: a few instructions, the same sequence on every compiler - which
/// std::uniform_real_distribution does not promise.
class Noise {
public:
    explicit Noise(std::uint32_t seed) noexcept : m_state(seed == 0 ? 1U : seed) {}

    /// Uniform on [0, 1).
    [[nodiscard]] double next() noexcept {
        m_state ^= m_state << 13U;
        m_state ^= m_state >> 17U;
        m_state ^= m_state << 5U;
        return static_cast<double>(m_state) / 4294967296.0;
    }

private:
    std::uint32_t m_state;
};

/// Wannamaker's three-tap E-weighted shaping filter (Lipshitz, Vanderkooy & Wannamaker,
/// "Minimally audible noise shaping", JAES 1991).
constexpr std::array<double, 3> kShape{1.623, -0.982, 0.109};

} // namespace

const char* toString(DitherKind kind) noexcept {
    switch (kind) {
    case DitherKind::Off:
        return "off";
    case DitherKind::Tpdf:
        return "tpdf";
    case DitherKind::Shaped:
        return "shaped";
    }
    return "";
}

void quantize(std::span<float> audio, std::uint32_t channels, std::uint32_t bits, DitherKind kind,
              std::uint32_t seed) {
    if (bits == 0 || bits > 24 || channels == 0) {
        return;
    }
    const double scale = std::ldexp(1.0, static_cast<int>(bits) - 1);
    const double low = -scale;
    const double high = scale - 1.0;
    std::vector<Noise> noise;
    noise.reserve(channels);
    std::vector<std::array<double, 3>> error(channels, {0.0, 0.0, 0.0});
    for (std::uint32_t c = 0; c < channels; ++c) {
        noise.emplace_back(seed + (c * 0x9E3779B9U));
    }
    const std::size_t frames = audio.size() / channels;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::uint32_t c = 0; c < channels; ++c) {
            float& sample = audio[(frame * channels) + c];
            double value = static_cast<double>(sample) * scale;
            std::array<double, 3>& e = error[c];
            if (kind == DitherKind::Shaped) {
                value -= (kShape[0] * e[0]) + (kShape[1] * e[1]) + (kShape[2] * e[2]);
            }
            const double d =
                kind == DitherKind::Off ? 0.0 : noise[c].next() + noise[c].next() - 1.0;
            const double q = std::clamp(std::nearbyint(value + d), low, high);
            if (kind == DitherKind::Shaped) {
                e = {q - value, e[0], e[1]};
            }
            sample = static_cast<float>(q / scale);
        }
    }
}

} // namespace adx::render
