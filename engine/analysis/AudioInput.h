// adx-thread: main
//
// What every analysis of a file starts with: decode, fold to mono, convert the rate.
// The detectors take samples; this is where a path becomes samples, for the bindings
// and `adx analyze`.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace adx::analysis {

struct MonoAudio {
    std::vector<float> samples;
    std::uint32_t sampleRate{0};
};

/// The average of the channels at `sampleRate`, or at the file's own rate when it is 0.
/// False, with the reason in `error`, when the file cannot be decoded.
[[nodiscard]] bool loadMono(const std::filesystem::path& path, std::uint32_t sampleRate,
                            MonoAudio& out, std::string& error);

/// `mono` from `from` Hz to `to` Hz; a copy when they are equal.
[[nodiscard]] std::vector<float> resampleMono(std::span<const float> mono, std::uint32_t from,
                                              std::uint32_t to);

} // namespace adx::analysis
