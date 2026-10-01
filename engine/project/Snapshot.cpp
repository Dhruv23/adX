#include "engine/project/Snapshot.h"

#include <algorithm>

namespace adx::project {

bool paramRefLess(const ParamRef& lhs, const ParamRef& rhs) noexcept {
    if (lhs.kind != rhs.kind) {
        return lhs.kind < rhs.kind;
    }
    if (lhs.owner != rhs.owner) {
        return lhs.owner < rhs.owner;
    }
    return lhs.index < rhs.index;
}

std::uint32_t Snapshot::findParam(ParamRef ref) const noexcept {
    const auto position = std::ranges::lower_bound(
        paramIndex, ref, [](const ParamRef& a, const ParamRef& b) { return paramRefLess(a, b); },
        &ParamSlot::ref);
    if (position == paramIndex.end() || !(position->ref == ref)) {
        return graph::kNone;
    }
    return position->index;
}

float AutomationLane::valueAt(std::int64_t tick) const noexcept {
    if (points.empty()) {
        return 0.0F;
    }
    if (tick <= points.front().tick) {
        return points.front().value;
    }
    if (tick >= points.back().tick) {
        return points.back().value;
    }
    // The segment whose start is at or before `tick`.
    const auto next = std::ranges::upper_bound(points, tick, {}, &AutomationPoint::tick);
    const AutomationPoint& to = *next;
    const AutomationPoint& from = *(next - 1);
    if (to.tick <= from.tick) {
        return from.value;
    }
    const auto span = static_cast<double>(to.tick - from.tick);
    const auto t = static_cast<float>(static_cast<double>(tick - from.tick) / span);
    return core::interpolate(from.curve, from.value, to.value, t);
}

} // namespace adx::project
