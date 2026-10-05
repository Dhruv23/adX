// adx-thread: main
//
// Building a Convolution from its slot: the IR file decoded, or the synthetic room.
#pragma once

#include <cstdint>
#include <vector>

#include "engine/effects/Convolution.h"
#include "engine/project/Mixer.h"

namespace adx::project {
struct Resources;
}

namespace adx::effects {

/// Gives `node` the IR `slot` asks for: its `ir=` file, resolved against `resources`
/// and decoded now, or - with no file, or one that does not decode - the synthetic
/// room from its `size` and `damping`.
void configureConvolution(Convolution& node, const project::Slot& slot,
                          const project::Resources* resources);

/// True when `node` was built from what `slot` now asks for.
[[nodiscard]] bool convolutionMatches(const Convolution& node, const project::Slot& slot,
                                      const project::Resources* resources);

/// The synthetic room: `seconds` of RT60 at `sampleRate`, one channel per seed, scaled
/// to unit energy.
[[nodiscard]] std::vector<float> syntheticRoom(double seconds, double damping,
                                               std::uint32_t sampleRate, std::uint32_t seed);

} // namespace adx::effects
