// adx-thread: main
#include "engine/geometry/SessionGeometry.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "engine/geometry/ColorPalette.h"

namespace adx::geometry {
namespace {

float* vertex(float* out, float x, float y, float role) noexcept {
    *out++ = x;
    *out++ = y;
    *out++ = role;
    return out;
}

float* quad(float* out, float x0, float y0, float x1, float y1, ColorRole role) noexcept {
    const float r = roleValue(role);
    out = vertex(out, x0, y0, r);
    out = vertex(out, x1, y0, r);
    out = vertex(out, x1, y1, r);
    out = vertex(out, x0, y0, r);
    out = vertex(out, x1, y1, r);
    return vertex(out, x0, y1, r);
}

constexpr float kInset = 0.06F;

ColorRole roleOf(session::ClipState state, bool filled, bool recording) noexcept {
    if (!filled) {
        return ColorRole::kSessionEmpty;
    }
    switch (state) {
    case session::ClipState::Queued:
    case session::ClipState::Retriggered:
        return ColorRole::kSessionQueued;
    case session::ClipState::Playing:
        return recording ? ColorRole::kSessionRecording : ColorRole::kSessionPlaying;
    case session::ClipState::QueuedStop:
        return ColorRole::kSessionQueuedStop;
    case session::ClipState::Stopped:
        break;
    }
    return ColorRole::kSessionStopped;
}

} // namespace

void SessionGeometry::build(const session::SessionEngine& session, const SessionGridSpec& spec) {
    const std::uint32_t tracks = std::min<std::uint32_t>(spec.tracks, 64);
    const std::uint32_t rows = std::min<std::uint32_t>(spec.rows, 64);
    const std::size_t cells = static_cast<std::size_t>(tracks) * rows;
    float* write = m_cells.resize(cells * 6, kFloatsPerVertex).data();
    m_playing = 0;
    m_queued = 0;
    m_states.assign(cells, 0);
    // A first pass for the counts, so the marks buffer is sized once.
    for (std::uint32_t t = 0; t < tracks; ++t) {
        for (std::uint32_t r = 0; r < rows; ++r) {
            const std::size_t index = (static_cast<std::size_t>(t) * rows) + r;
            const bool filled = index < spec.filled.size() && spec.filled[index] != 0;
            const session::ClipState state = session.state(t, r);
            m_states[index] = static_cast<std::uint8_t>(state);
            write = quad(write, static_cast<float>(t) + kInset, static_cast<float>(r) + kInset,
                         static_cast<float>(t + 1) - kInset, static_cast<float>(r + 1) - kInset,
                         roleOf(state, filled, spec.recording));
            if (!filled) {
                continue;
            }
            m_playing += state == session::ClipState::Playing ? 1U : 0U;
            m_queued += state == session::ClipState::Queued ||
                                state == session::ClipState::Retriggered ||
                                state == session::ClipState::QueuedStop
                            ? 1U
                            : 0U;
        }
    }
    // A ring is kRingSegments quads, its lit share the clip's progress; a queue mark
    // is one triangle in the cell's corner.
    const std::size_t marks = (static_cast<std::size_t>(m_playing) * kRingSegments * 6) +
                              (static_cast<std::size_t>(m_queued) * 3);
    float* mark = m_marks.resize(marks, kFloatsPerVertex).data();
    float* const end = mark + (marks * kFloatsPerVertex);
    for (std::uint32_t t = 0; t < tracks; ++t) {
        const float progress = session.progress(t);
        for (std::uint32_t r = 0; r < rows; ++r) {
            const std::size_t index = (static_cast<std::size_t>(t) * rows) + r;
            if (index >= spec.filled.size() || spec.filled[index] == 0) {
                continue;
            }
            const auto state = static_cast<session::ClipState>(m_states[index]);
            const auto x = static_cast<float>(t);
            const auto y = static_cast<float>(r);
            if (state == session::ClipState::Playing &&
                mark + (static_cast<std::size_t>(kRingSegments) * 18) <= end) {
                const float cx = x + 0.82F;
                const float cy = y + 0.5F;
                const float outer = 0.13F;
                const float inner = 0.08F;
                const auto lit = static_cast<std::uint32_t>(
                    std::lround(std::clamp(progress, 0.0F, 1.0F) * kRingSegments));
                for (std::uint32_t s = 0; s < kRingSegments; ++s) {
                    const float a0 = (2.0F * std::numbers::pi_v<float> * static_cast<float>(s) /
                                      static_cast<float>(kRingSegments)) -
                                     (std::numbers::pi_v<float> / 2.0F);
                    const float a1 =
                        a0 + (2.0F * std::numbers::pi_v<float> / static_cast<float>(kRingSegments));
                    const float role = roleValue(s < lit ? ColorRole::kSessionProgress
                                                         : ColorRole::kSessionStopped);
                    const float ox0 = cx + (outer * std::cos(a0));
                    const float oy0 = cy + (outer * std::sin(a0));
                    const float ox1 = cx + (outer * std::cos(a1));
                    const float oy1 = cy + (outer * std::sin(a1));
                    const float ix0 = cx + (inner * std::cos(a0));
                    const float iy0 = cy + (inner * std::sin(a0));
                    const float ix1 = cx + (inner * std::cos(a1));
                    const float iy1 = cy + (inner * std::sin(a1));
                    mark = vertex(mark, ix0, iy0, role);
                    mark = vertex(mark, ox0, oy0, role);
                    mark = vertex(mark, ox1, oy1, role);
                    mark = vertex(mark, ix0, iy0, role);
                    mark = vertex(mark, ox1, oy1, role);
                    mark = vertex(mark, ix1, iy1, role);
                }
            } else if ((state == session::ClipState::Queued ||
                        state == session::ClipState::Retriggered ||
                        state == session::ClipState::QueuedStop) &&
                       mark + 9 <= end) {
                const float role = roleValue(ColorRole::kSessionProgress);
                mark = vertex(mark, x + 0.70F, y + kInset + 0.04F, role);
                mark = vertex(mark, x + 1.0F - kInset - 0.04F, y + kInset + 0.04F, role);
                mark = vertex(mark, x + 1.0F - kInset - 0.04F, y + 0.36F, role);
            }
        }
    }
}

} // namespace adx::geometry
