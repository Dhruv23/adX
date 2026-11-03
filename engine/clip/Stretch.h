// adx-thread: main
//
// Offline time-stretch and pitch-shift through RubberBand, ported from
// _archive/src-cpp/src/AudioClipProcessor.cpp (phase_8.md §3.3, §4).
//
// Offline because quality matters more than latency here: the whole clip is studied
// first, then processed in one pass, and the result is cached (ClipRenderCache.h), so
// the cost is paid once per change rather than per playback. Time and pitch are
// independent (FINAL_PLAN §5.5).
//
// What changed from the archive: planar in and out rather than a clip struct; a key
// frame map for warp markers; and the output is trimmed or padded to exactly the
// length the ratio asks for, so a stretched clip's end lands on the sample the
// arrangement computed rather than wherever the stretcher's last hop fell.
#pragma once

#include <cstdint>
#include <span>
#include <utility>

#include "engine/clip/Destructive.h"

namespace adx::clip {

struct StretchRequest {
    /// Output duration over input duration: 2 is twice as long.
    double timeRatio{1.0};
    double pitchSemitones{0.0};
    /// Warp: (input frame, output frame) pairs, increasing in both. When non-empty the
    /// map shapes the stretch inside the material and `timeRatio` is its overall ratio.
    std::span<const std::pair<std::size_t, std::size_t>> keyFrames;
};

/// True when the request changes nothing, so the input can be used as it is.
[[nodiscard]] bool isIdentity(const StretchRequest& request) noexcept;

/// `in` stretched. The output has the input's rate and channel count and exactly
/// round(frames * timeRatio) frames.
[[nodiscard]] Planar stretch(const Planar& in, const StretchRequest& request);

} // namespace adx::clip
