// adx-thread: main
//
// Effect type name -> slot node. The one place a slot's type becomes a class.
#pragma once

#include <memory>
#include <string_view>

#include "engine/graph/nodes/SlotNode.h"
#include "engine/project/Mixer.h"

namespace adx::effects {

/// A new, unprepared node for `slot`'s type, configured from its structural
/// parameters (a lookahead, which fixes the node's latency). An unknown type is the
/// identity slot: it keeps its place, its mix and its parameters, and passes audio
/// through untouched.
[[nodiscard]] std::shared_ptr<graph::SlotNode> makeEffect(const project::Slot& slot);

/// The same for a bare type with default configuration.
[[nodiscard]] std::shared_ptr<graph::SlotNode> makeEffect(std::string_view type);

/// True when `node` was configured as `slot` asks - same type, same lookahead - so a
/// rebuild may keep it.
[[nodiscard]] bool configMatches(const graph::SlotNode& node, const project::Slot& slot) noexcept;

/// A fresh instance of `effect`'s type and configuration, with pristine DSP state and
/// unprepared: what an offline render processes so that it never touches the instance
/// the audio thread is running (AudioEffect.h's clone(), kept; phase_4.md §4.2).
[[nodiscard]] std::shared_ptr<graph::SlotNode> clone(const graph::SlotNode& effect);

/// What makeEffect's node reports as typeName() for `type`.
[[nodiscard]] std::string_view canonicalType(std::string_view type) noexcept;

} // namespace adx::effects
