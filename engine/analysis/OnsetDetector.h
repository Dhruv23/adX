// adx-thread: main
//
// Onsets by spectral flux (phase_8.md §2): where new energy arrives across the
// spectrum. Feeds warp-marker seeding and, in Phase 10, the slicer's auto-chop.
//
//   frames      1024-point Hann windows every 256 samples (5.3 ms at 48 kHz)
//   flux        per frame, the sum over bins of the rise in log-compressed magnitude,
//               log(1 + 100|X|) - compression keeps a quiet hi-hat visible beside a kick
//   peaks       a frame is an onset when it is the largest within +-6 frames (32 ms)
//               and above the local mean (+-16 frames) by `sensitivity` times, and
//               further than `minimumGapSeconds` from the last onset
//   time        the frame's start plus three quarters of the window: an attack enters
//               a sliding window at its far end, and raises the flux most where the
//               Hann window climbs fastest - a quarter of the way in from that end
//
// Deterministic and offline: a whole buffer in, a list of times out.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace adx::analysis {

struct OnsetOptions {
    float sensitivity{1.5F};
    /// Absolute floor on the flux, so dither in a silent passage is not a drum roll.
    float floor{0.5F};
    double minimumGapSeconds{0.05};
};

/// Onset times in seconds from the start of `mono`.
[[nodiscard]] std::vector<double> detectOnsets(std::span<const float> mono,
                                               std::uint32_t sampleRate,
                                               const OnsetOptions& options = {});

} // namespace adx::analysis
