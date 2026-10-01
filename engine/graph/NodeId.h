// Identifies a node in a graph description.
//
// Deliberately not an entity id: one insert becomes several nodes (its slots, its
// fader, its meter), and several of them have no entity of their own. The builder
// hands these out in an order that keeps each channel next to the strip it feeds,
// which is what the topological sort's ascending-id tie-break then preserves - and
// what keeps the number of simultaneously live buffers small (phase_3.md §4.4).
#pragma once

#include <compare>
#include <cstdint>

namespace adx::graph {

struct NodeId {
    std::uint32_t v{0};

    friend constexpr auto operator<=>(const NodeId&, const NodeId&) noexcept = default;
};

/// "No such thing" for every index a render graph stores: buffers, edges, tracks.
inline constexpr std::uint32_t kNone = 0xFFFFFFFFU;

} // namespace adx::graph
