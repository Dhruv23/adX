// Audio files for the clip, export and recording suites: written into a scratch
// directory that removes itself, from signals whose every sample a test can predict.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace adx::tests {

/// A fresh directory under the system temp directory, deleted with this object.
class ClipDir {
public:
    ClipDir();
    ~ClipDir();
    ClipDir(const ClipDir&) = delete;
    ClipDir& operator=(const ClipDir&) = delete;
    ClipDir(ClipDir&&) = delete;
    ClipDir& operator=(ClipDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return m_path;
    }
    /// Writes interleaved stereo as a 32-bit float WAV and returns its path.
    std::filesystem::path write(const std::string& name, std::span<const float> stereo,
                                std::uint32_t rate) const;

private:
    std::filesystem::path m_path;
};

/// Interleaved stereo, `frames` long. Noise is deterministic in `seed` and below full
/// scale, so a unity path reproduces it exactly.
[[nodiscard]] std::vector<float> stereoNoise(std::size_t frames, std::uint32_t seed);
[[nodiscard]] std::vector<float> stereoConstant(std::size_t frames, float value);
[[nodiscard]] std::vector<float> stereoSides(std::size_t frames, float left, float right);
[[nodiscard]] std::vector<float> stereoSine(std::size_t frames, std::uint32_t rate,
                                            double frequency, float amplitude = 0.5F);

/// The left channel's frequency over [from, to) frames, from the first and last
/// upward zero crossings, each interpolated to a fraction of a sample. Precise to well
/// under a cent on a clean tone of a few hundred cycles.
[[nodiscard]] double measuredFrequency(std::span<const float> stereo, std::uint32_t rate,
                                       std::size_t from, std::size_t to);

} // namespace adx::tests
