#include "tests/cpp/clip/ClipFixtures.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <numbers>
#include <random>
#include <system_error>

#include "engine/render/OfflineRender.h"

namespace adx::tests {

namespace {

std::atomic<std::uint32_t>& counter() {
    static std::atomic<std::uint32_t> next{0};
    return next;
}

} // namespace

ClipDir::ClipDir() {
    const auto stamp =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    m_path = std::filesystem::temp_directory_path() /
             ("adx-clip-" + std::to_string(stamp) + "-" + std::to_string(counter()++));
    std::filesystem::create_directories(m_path);
}

ClipDir::~ClipDir() {
    std::error_code ignored;
    std::filesystem::remove_all(m_path, ignored);
}

std::filesystem::path ClipDir::write(const std::string& name, std::span<const float> stereo,
                                     std::uint32_t rate) const {
    const std::filesystem::path file = m_path / name;
    static_cast<void>(render::writeWavFloat32(file.string(), stereo, 2, rate));
    return file;
}

std::vector<float> stereoNoise(std::size_t frames, std::uint32_t seed) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<float> sample(-0.5F, 0.5F);
    std::vector<float> out(frames * 2);
    for (float& value : out) {
        value = sample(random);
    }
    return out;
}

std::vector<float> stereoConstant(std::size_t frames, float value) {
    return std::vector<float>(frames * 2, value);
}

std::vector<float> stereoSides(std::size_t frames, float left, float right) {
    std::vector<float> out(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        out[i * 2] = left;
        out[(i * 2) + 1] = right;
    }
    return out;
}

std::vector<float> stereoSine(std::size_t frames, std::uint32_t rate, double frequency,
                              float amplitude) {
    std::vector<float> out(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto value =
            static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * frequency *
                                                    static_cast<double>(i) / rate));
        out[i * 2] = value;
        out[(i * 2) + 1] = value;
    }
    return out;
}

double measuredFrequency(std::span<const float> stereo, std::uint32_t rate, std::size_t from,
                         std::size_t to) {
    double first = -1.0;
    double last = -1.0;
    std::size_t crossings = 0;
    for (std::size_t i = from + 1; i < to && (i * 2) < stereo.size(); ++i) {
        const float a = stereo[(i - 1) * 2];
        const float b = stereo[i * 2];
        if (a < 0.0F && b >= 0.0F) {
            const double at = static_cast<double>(i - 1) + (a / (a - b));
            if (first < 0.0) {
                first = at;
            }
            last = at;
            ++crossings;
        }
    }
    if (crossings < 2 || last <= first) {
        return 0.0;
    }
    return static_cast<double>(crossings - 1) * rate / (last - first);
}

} // namespace adx::tests
