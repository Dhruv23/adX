// adx-thread: main
//
// The Slicer's main-thread half (Slicer.h): cutting a sample into slice zones and laying
// the slices out as notes, in beats of the source loop.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/core/Time.h"
#include "engine/project/SampleZone.h"

namespace adx::instruments {

/// Main thread. `count` equal slices of a `frames`-long sample, on consecutive keys
/// from `baseKey`.
[[nodiscard]] std::vector<project::SampleZone>
evenSlices(core::SampleId sample, std::uint32_t frames, std::uint32_t count, std::uint8_t baseKey);

/// Main thread. Slices starting at `points` (frames, ascending; the first is usually
/// 0), each ending where the next starts and the last at `frames`.
[[nodiscard]] std::vector<project::SampleZone> slicesAt(core::SampleId sample,
                                                        std::span<const std::uint32_t> points,
                                                        std::uint32_t frames, std::uint8_t baseKey);

struct SliceNote {
    core::Ticks start;
    core::Ticks length;
    std::uint8_t pitch{60};
};

/// Main thread. One note per slice, at the slice's position in beats of a loop
/// recorded at `sourceBpm` and sampled at `sampleRate`: the pattern that plays the
/// loop back as it was, at any project tempo.
[[nodiscard]] std::vector<SliceNote> slicePattern(std::span<const project::SampleZone> slices,
                                                  std::uint32_t sampleRate, double sourceBpm);

} // namespace adx::instruments
