// adx-thread: main
//
// Instrument type name -> node. The one place a channel's `INSTRUMENT=` becomes a
// class, so the graph builder never names an instrument.
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "engine/graph/nodes/ChannelNode.h"
#include "engine/project/Channel.h"
#include "engine/project/VoiceStealMode.h"

namespace adx::format {
class SamplePool;
}
namespace adx::project {
struct Resources;
}

namespace adx::instruments {

/// What building an instrument may need beyond its channel: the project's sample
/// references and the pool that decodes them (a Sampler's zones, phase_4.md §4.5).
struct InstrumentContext {
    const project::Resources* resources{nullptr};
    /// Null means SamplePool::global().
    format::SamplePool* pool{nullptr};
};

/// A new, unprepared node for `type`. An unknown type gets a SilentChannelNode.
[[nodiscard]] std::shared_ptr<graph::ChannelNode> makeInstrument(std::string_view type,
                                                                 std::uint32_t channelId,
                                                                 std::uint16_t maxPolyphony,
                                                                 project::VoiceStealMode stealMode);

/// A new, unprepared node for `channel`, with whatever structural configuration its
/// instrument carries (a Sampler's zones) already applied.
[[nodiscard]] std::shared_ptr<graph::ChannelNode> makeInstrument(const project::Channel& channel,
                                                                 const InstrumentContext& context);

/// True when `node` was built as `channel` now asks - same type, polyphony, steal mode
/// and structural configuration - so a rebuild may keep it, voices and all.
[[nodiscard]] bool configMatches(const graph::ChannelNode& node, const project::Channel& channel,
                                 const InstrumentContext& context);

/// What makeInstrument's node reports as typeName() for `type`: the type itself when
/// this build knows it, empty otherwise.
[[nodiscard]] std::string_view canonicalType(std::string_view type) noexcept;

} // namespace adx::instruments
