#include "engine/project/Automation.h"

#include <algorithm>

namespace adx::project {

float AutomationClip::valueAt(core::Ticks at) const noexcept {
    if (points.empty()) {
        return 0.0F;
    }
    if (at <= points.front().at) {
        return points.front().value;
    }
    if (at >= points.back().at) {
        return points.back().value;
    }

    // Points are sorted on load, which is what makes this a binary search rather
    // than the linear scan iteration one used. `upper_bound` lands on the first
    // point strictly after `at`, so the segment we want starts one before it.
    const auto after = std::ranges::upper_bound(points, at, {}, &Breakpoint::at);
    const auto before = std::prev(after);

    const std::int64_t span = (after->at - before->at).value;
    if (span <= 0) {
        return before->value;
    }
    const auto t = static_cast<float>(static_cast<double>((at - before->at).value) /
                                      static_cast<double>(span));
    return interpolate(before->curve, before->value, after->value, t);
}

} // namespace adx::project
