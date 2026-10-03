// The mixer, and the routing graph.
//
// Two things here are not what iteration one had, and both matter. An Insert is a
// separate object from a Channel, so many channels can feed one strip - which is
// how drum-bus processing and grouped sidechaining work at all. And routing is an
// arbitrary DAG of Route edges rather than a fixed "send to the master delay or
// the master reverb"; v1's two hardcoded global buses are one special case of this
// (FINAL_PLAN §4).
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/project/Color.h"

namespace adx::project {

/// One named effect parameter. Same shape and same reason as ParamValue on a
/// channel: Phase 4 owns what these mean, Phase 2 owns that they are named.
struct SlotParam {
    std::string name;
    double value{0.0};

    [[nodiscard]] friend bool operator==(const SlotParam&, const SlotParam&) noexcept = default;
};

struct Slot {
    core::SlotId id;
    /// Opaque to Phase 2. Slots process in ascending id order.
    std::string type;
    float mix{1.0F};
    bool bypass{false};
    std::vector<SlotParam> params;
    /// The insert whose post-fader signal keys this slot's effect - a ducker's trigger,
    /// a compressor's external key - or invalid for none. An explicit connection
    /// (FINAL_PLAN §5.2), so it is part of the routing graph: sorted, delay-compensated
    /// and cycle-checked like a route (phase_3.md §4.4).
    core::InsertId sidechain;

    [[nodiscard]] const SlotParam* find(std::string_view name) const noexcept;
    [[nodiscard]] SlotParam* find(std::string_view name) noexcept;

    [[nodiscard]] friend bool operator==(const Slot&, const Slot&) noexcept = default;
};

struct Send {
    core::SendId id;
    core::InsertId target;
    float level{0.0F};
    bool preFader{false};

    [[nodiscard]] friend bool operator==(const Send&, const Send&) noexcept = default;
};

struct Insert {
    core::InsertId id;
    std::string name;
    Color color;

    float gain{1.0F};
    float pan{0.0F};
    /// 0 collapses to mono, 1 is unchanged, above 1 widens.
    float stereoSeparation{1.0F};

    bool muted{false};
    bool soloed{false};
    bool polarityInvert{false};

    std::vector<Slot> slots;
    std::vector<Send> sends;

    [[nodiscard]] friend bool operator==(const Insert&, const Insert&) noexcept = default;
};

/// One edge of the routing DAG. The master is an insert like any other; what makes
/// it the master is that nothing routes out of it.
struct Route {
    core::RouteId id;
    core::InsertId from;
    core::InsertId to;

    [[nodiscard]] friend bool operator==(const Route&, const Route&) noexcept = default;
};

struct Mixer {
    std::vector<Insert> inserts;
    std::vector<Route> routes;
    /// Which insert is the master. Always valid in a valid project; Validate
    /// reports it when it is not.
    core::InsertId master;

    [[nodiscard]] const Insert* find(core::InsertId id) const noexcept;
    [[nodiscard]] Insert* find(core::InsertId id) noexcept;

    /// Slots and sends carry project-wide ids so a ParamRef can address one in
    /// eight bytes. These find them without the caller having to know which insert
    /// they belong to.
    [[nodiscard]] const Slot* findSlot(core::SlotId id) const noexcept;
    [[nodiscard]] Slot* findSlot(core::SlotId id) noexcept;
    [[nodiscard]] const Send* findSend(core::SendId id) const noexcept;
    [[nodiscard]] Send* findSend(core::SendId id) noexcept;

    /// The insert a slot or send belongs to, for building a path back out.
    [[nodiscard]] core::InsertId ownerOfSlot(core::SlotId id) const noexcept;
    [[nodiscard]] core::InsertId ownerOfSend(core::SendId id) const noexcept;

    [[nodiscard]] friend bool operator==(const Mixer&, const Mixer&) noexcept = default;
};

} // namespace adx::project
