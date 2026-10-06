#include "engine/geometry/HitTest.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <utility>

namespace adx::geometry {
namespace {

[[nodiscard]] const project::NoteClip*
clipOf(const project::Project& project, core::PatternId pattern, core::ChannelId channel) noexcept {
    const project::Pattern* p = project.find(pattern);
    return p == nullptr ? nullptr : p->clipFor(channel);
}

} // namespace

NoteHit hitTestNote(const project::Project& project, core::PatternId pattern,
                    core::ChannelId channel, const Viewport& viewport, float x, float y) {
    const project::NoteClip* clip = clipOf(project, pattern, channel);
    if (clip == nullptr) {
        return {};
    }
    const double tick = viewport.tickAt(x);
    const int row = static_cast<int>(std::floor(viewport.pitchAt(y)));
    for (const project::Note& note : std::views::reverse(clip->notes)) {
        if (std::cmp_not_equal(note.pitch, row)) {
            continue;
        }
        const auto start = static_cast<double>(note.start.value);
        const auto end = static_cast<double>(note.end().value);
        if (tick < start || tick >= end) {
            continue;
        }
        const float x0 = viewport.xOf(start);
        const float x1 = viewport.xOf(end);
        const float edge = std::min(kEdgePixels, (x1 - x0) / 3.0F);
        NotePart part = NotePart::kBody;
        if (x >= x1 - edge) {
            part = NotePart::kRightEdge;
        } else if (x < x0 + edge) {
            part = NotePart::kLeftEdge;
        }
        return NoteHit{.note = note.id, .part = part};
    }
    return {};
}

std::size_t hitTestRect(const project::Project& project, core::PatternId pattern,
                        core::ChannelId channel, const Viewport& viewport, Rect rect,
                        std::span<core::NoteId> out) {
    const project::NoteClip* clip = clipOf(project, pattern, channel);
    if (clip == nullptr) {
        return 0;
    }
    const double t0 = viewport.tickAt(std::min(rect.x0, rect.x1));
    const double t1 = viewport.tickAt(std::max(rect.x0, rect.x1));
    // y grows downward, pitch upward: the top of the rectangle is the higher pitch.
    const float pHigh = viewport.pitchAt(std::min(rect.y0, rect.y1));
    const float pLow = viewport.pitchAt(std::max(rect.y0, rect.y1));
    std::size_t count = 0;
    for (const project::Note& note : clip->notes) {
        const auto p = static_cast<float>(note.pitch);
        if (p + 1.0F <= pLow || p >= pHigh) {
            continue;
        }
        if (static_cast<double>(note.end().value) <= t0 ||
            static_cast<double>(note.start.value) > t1) {
            continue;
        }
        if (count < out.size()) {
            out[count] = note.id;
        }
        ++count;
    }
    return count;
}

LaneHit hitTestLane(const project::Project& project, core::PatternId pattern,
                    core::ChannelId channel, const Viewport& viewport, float laneHeightPx, float x,
                    float y) {
    const project::NoteClip* clip = clipOf(project, pattern, channel);
    if (clip == nullptr || laneHeightPx <= 0.0F) {
        return {};
    }
    const float value = std::clamp(1.0F - (y / laneHeightPx), 0.0F, 1.0F);
    LaneHit best{.note = {}, .value = value};
    float bestDistance = 0.0F;
    for (const project::Note& note : clip->notes) {
        const float x0 = viewport.xOf(static_cast<double>(note.start.value));
        const float width = std::min(
            kLaneBarPixels, std::max(viewport.xOf(static_cast<double>(note.end().value)) - x0,
                                     kLaneBarPixels * 0.4F));
        if (x < x0 - 1.0F || x > x0 + width + 1.0F) {
            continue;
        }
        const float distance = std::abs(x - x0);
        if (!best.note.valid() || distance < bestDistance) {
            best.note = note.id;
            bestDistance = distance;
        }
    }
    return best;
}

} // namespace adx::geometry
