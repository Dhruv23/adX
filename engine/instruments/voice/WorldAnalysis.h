// adx-thread: main
//
// WORLD analysis of a voicebank WAV: F0, spectral envelope, aperiodicity (phase_4.md
// §4.13). Worker threads only - never the audio thread, never on first play.
//
// F0 comes from the bank's own UTAU `.frq` when one is there and valid (Teto ships one
// per WAV: FREQ0003, a 256-sample hop, then (f0, amplitude) pairs), resampled onto
// WORLD's 5 ms frames; otherwise from WORLD's Harvest. The envelope (CheapTrick) and
// aperiodicity (D4C) always come from WORLD. Analyses are cached per WAV - one WAV
// carries many aliases - in memory, for the life of the process.
//
// Deterministic: WORLD has no randomness in analysis, so the same file analyses to the
// same numbers every run in a given build (world_analysis_deterministic).
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace adx::instruments {

inline constexpr double kWorldFramePeriodMs = 5.0;

/// An UTAU .frq file.
struct FrqFile {
    std::int32_t hop{256};
    double averageF0{0.0};
    std::vector<double> f0;
    std::vector<double> amplitude;
};

/// Parses `bytes` as FREQ0003; nullopt when it is not one.
[[nodiscard]] std::optional<FrqFile> parseFrq(const std::vector<char>& bytes);

/// The .frq UTAU writes beside `wav`: `_あ.wav` -> `_あ_wav.frq`.
[[nodiscard]] std::filesystem::path frqPathFor(const std::filesystem::path& wav);

struct WorldAnalysis {
    int sampleRate{44100};
    /// The source samples, mono, as WORLD reads them.
    std::vector<double> samples;
    /// Per 5 ms frame. f0 0 is unvoiced.
    std::vector<double> f0;
    int fftSize{0};
    /// frames x (fftSize / 2 + 1).
    std::vector<std::vector<double>> spectrogram;
    std::vector<std::vector<double>> aperiodicity;
    /// True when f0 came from the bank's .frq.
    bool fromFrq{false};

    [[nodiscard]] double durationMs() const noexcept {
        return 1000.0 * static_cast<double>(samples.size()) / static_cast<double>(sampleRate);
    }
};

/// Analyses `wav` (cached by path). Null when it cannot be read.
[[nodiscard]] std::shared_ptr<const WorldAnalysis> analyseWav(const std::filesystem::path& wav,
                                                              bool useFrq = true);

/// WORLD's Harvest F0 of `samples`, for comparing an imported .frq against.
[[nodiscard]] std::vector<double> harvestF0(const std::vector<double>& samples, int sampleRate);

} // namespace adx::instruments
