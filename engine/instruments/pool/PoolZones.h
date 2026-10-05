// adx-thread: main
//
// The sample-pool channel's main-thread half (SamplePoolChannel.h).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/project/SampleZone.h"

namespace adx::instruments {

/// Main thread. One zone per sample, on consecutive keys from `baseKey`, each at its
/// recorded pitch on its own key.
[[nodiscard]] std::vector<project::SampleZone> poolZones(std::span<const core::SampleId> samples,
                                                         std::uint8_t baseKey);

} // namespace adx::instruments
