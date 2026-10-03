// adx-thread: main
//
// Building a Sampler node from a channel: zones resolved to pool samples and pinned.
#pragma once

#include <memory>

#include "engine/graph/nodes/ChannelNode.h"
#include "engine/project/Channel.h"

namespace adx::format {
class SamplePool;
}
namespace adx::project {
struct Resources;
}

namespace adx::instruments {

class SamplerInstrument;

/// Requests every zone's sample from `pool` (resolved against `resources`), and bakes
/// the zones into `node`. Decoding is asynchronous; the node plays silence for a zone
/// whose sample is still loading.
void configureSampler(SamplerInstrument& node, const project::Channel& channel,
                      const project::Resources* resources, format::SamplePool* pool);

/// True when `node` was built from these zones and these resolved files.
[[nodiscard]] bool samplerMatches(const SamplerInstrument& node, const project::Channel& channel,
                                  const project::Resources* resources, format::SamplePool* pool);

} // namespace adx::instruments
