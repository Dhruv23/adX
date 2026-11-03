// adx-thread: main
//
// Tempo estimation from audio, ported as-is from _archive/src-cpp/src/BpmDetector.cpp
// (phase_8.md §4): an RMS energy envelope, its half-wave rectified difference as onset
// strength, autocorrelation over 60-200 BPM with a confidence gate, parabolic
// refinement, and octave folding into 70-200. It seeds warp markers (phase_8.md §3.3);
// a person corrects what it gets wrong.
//
// What changed on port: mono samples arrive as a span, and the result says when the
// detector declined (a one-shot, no pulse) instead of returning 0 for both.
#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace adx::analysis {

/// The tempo of `mono` at `sampleRate`, or nullopt for audio with no usable pulse:
/// shorter than two seconds, or no autocorrelation peak clearly above the average.
[[nodiscard]] std::optional<double> estimateBpm(std::span<const float> mono,
                                                std::uint32_t sampleRate);

} // namespace adx::analysis
