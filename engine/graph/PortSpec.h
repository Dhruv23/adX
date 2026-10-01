// What a node consumes and produces.
//
// Audio ports are stereo, planar, block-sized. There are at most two of each kind,
// and the two are named because they mean different things:
//
//   input 0   main      everything routed to the node is summed here
//   input 1   sidechain a key signal the node listens to but does not pass through
//   output 0  post      the node's result
//   output 1  pre       an insert's signal before its fader, for pre-fader sends
//
// Sidechain is a port rather than a side channel on purpose. An edge into it takes
// part in the topological sort like any other, so the key signal is always *this*
// block's. A sidechain read from outside the sort order reads last block's data, and
// that one-block skew is audible on a fast ducker (phase_3.md §4.4).
#pragma once

#include <cstdint>

namespace adx::graph {

inline constexpr std::uint32_t kPortMain = 0;
inline constexpr std::uint32_t kPortSidechain = 1;
inline constexpr std::uint32_t kPortPost = 0;
inline constexpr std::uint32_t kPortPre = 1;

inline constexpr std::uint32_t kMaxInputPorts = 2;
inline constexpr std::uint32_t kMaxOutputPorts = 2;

/// Channels per audio port. Stereo throughout; a mono source is a stereo buffer with
/// two equal halves. A second width would double every port's bookkeeping for a
/// case the mixer never has.
inline constexpr std::uint32_t kPortChannels = 2;

/// What kind of data a port carries. Only Audio exists as a buffer in Phase 3: events
/// reach a node through ProcessContext::events and control values through
/// ProcessContext::params, both already resolved - but the kind is part of the spec so
/// a Phase 4 control-rate port does not need a new field.
enum class BufferKind : std::uint8_t { Audio, Control, Event };

struct PortSpec {
    /// Audio inputs, 0..kMaxInputPorts. 2 means main + sidechain.
    std::uint8_t inputs{0};
    /// Audio outputs, 0..kMaxOutputPorts. A meter has none.
    std::uint8_t outputs{1};
    /// Whether the node reads note events. Instruments do; effects do not.
    bool acceptsEvents{false};
    BufferKind kind{BufferKind::Audio};
};

} // namespace adx::graph
