#include "engine/geometry/PianoRollGeometry.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/geometry/ColorPalette.h"

namespace adx::geometry {
namespace {

constexpr std::size_t kQuadVertices = 6;
/// Notes are drawn this far inside their row, top and bottom, so stacked rows read as
/// separate notes.
constexpr float kRowInset = 0.06F;
/// The grid thins out until its lines are at least this far apart.
constexpr float kMinGridSpacingPixels = 8.0F;

/// Writes one axis-aligned quad as two triangles. Returns the next write position.
float* quad(float* out, float x0, float y0, float x1, float y1, ColorRole role) noexcept {
    const float c = roleValue(role);
    const std::array<float, kQuadVertices * kFloatsPerVertex> v{
        x0, y0, c, x1, y0, c, x0, y1, c, x1, y0, c, x1, y1, c, x0, y1, c,
    };
    return std::ranges::copy(v, out).out;
}

float* line(float* out, float x0, float y0, float x1, float y1, ColorRole role) noexcept {
    const float c = roleValue(role);
    const std::array<float, 2 * kFloatsPerVertex> v{x0, y0, c, x1, y1, c};
    return std::ranges::copy(v, out).out;
}

[[nodiscard]] bool isBlackKey(int pitch) noexcept {
    switch (pitch % 12) {
    case 1:
    case 3:
    case 6:
    case 8:
    case 10:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool selectedIn(std::span<const core::NoteId> selected, core::NoteId id) noexcept {
    return std::ranges::binary_search(selected, id);
}

[[nodiscard]] ColorRole noteRole(bool selected, bool muted) noexcept {
    if (selected) {
        return ColorRole::kNoteSelected;
    }
    return muted ? ColorRole::kNoteMuted : ColorRole::kNote;
}

/// Shrinks a buffer to what was actually written. `end` is one past the last float.
void finish(GeometryBuffer& buffer, const float* begin, const float* end) {
    const auto floats = static_cast<std::size_t>(end - begin);
    static_cast<void>(buffer.resize(floats / kFloatsPerVertex, kFloatsPerVertex));
}

/// World y of the middle of row `pitch`, offset by `cents`.
[[nodiscard]] float curveY(int pitch, float cents) noexcept {
    return worldRowTop(pitch) + 0.5F - (cents / 100.0F);
}

} // namespace

float laneValue(const project::Note& note, LaneKind lane) noexcept {
    switch (lane) {
    case LaneKind::kVelocity:
        return static_cast<float>(note.velocity) / 127.0F;
    case LaneKind::kReleaseVelocity:
        return std::clamp(static_cast<float>(note.releaseVelocity) / 127.0F, 0.0F, 1.0F);
    case LaneKind::kPan:
        return std::clamp((note.pan + 1.0F) * 0.5F, 0.0F, 1.0F);
    case LaneKind::kCutoff:
        return std::clamp((note.cutoff + 4.0F) / 8.0F, 0.0F, 1.0F);
    case LaneKind::kResonance:
        return std::clamp(note.resonance, 0.0F, 1.0F);
    case LaneKind::kFinePitch:
        return std::clamp((static_cast<float>(note.fineTuneCents) + 100.0F) / 200.0F, 0.0F, 1.0F);
    }
    return 0.0F;
}

bool laneIsBipolar(LaneKind lane) noexcept {
    return lane == LaneKind::kPan || lane == LaneKind::kCutoff || lane == LaneKind::kFinePitch;
}

GeometryBuffer& PianoRollGeometry::buffer(std::size_t index) noexcept {
    switch (index) {
    case 0:
        return m_rows;
    case 1:
        return m_grid;
    case 2:
        return m_ghosts;
    case 3:
        return m_notes;
    case 4:
        return m_lanes;
    default:
        return m_curves;
    }
}

void PianoRollGeometry::build(const project::Project& project, const Viewport& viewport,
                              const BuildOptions& options) {
    m_density = densityLod(viewport);
    m_visibleIds.clear();
    m_visibleIndex.clear();
    m_visibleMuted.clear();

    const project::Pattern* pattern = project.find(options.pattern);
    const project::NoteClip* clip =
        pattern == nullptr ? nullptr : pattern->clipFor(options.channel);
    sortIfStale(clip, options);

    buildRows(viewport, options);
    buildGrid(project, viewport, options);
    if (pattern != nullptr && options.ghosts) {
        buildGhosts(*pattern, viewport, options);
    } else {
        static_cast<void>(m_ghosts.resize(0, kFloatsPerVertex));
    }
    if (clip == nullptr) {
        static_cast<void>(m_notes.resize(0, kFloatsPerVertex));
        static_cast<void>(m_lanes.resize(0, kFloatsPerVertex));
        static_cast<void>(m_curves.resize(0, kFloatsPerVertex));
        return;
    }
    if (m_density) {
        buildDensity(*clip, viewport);
        buildDensityLanes(*clip, viewport, options);
        static_cast<void>(m_curves.resize(0, kFloatsPerVertex));
        return;
    }
    buildNotes(*clip, viewport, options);
    buildLanes(*clip, viewport, options);
    buildCurves(*clip);
}

void PianoRollGeometry::sortIfStale(const project::NoteClip* clip, const BuildOptions& options) {
    const std::size_t count = clip == nullptr ? 0 : clip->notes.size();
    if (m_sortedValid && m_sortedPattern == options.pattern && m_sortedChannel == options.channel &&
        m_sortedRevision == options.dataRevision && m_order.size() == count) {
        return;
    }
    m_sortedValid = true;
    m_sortedPattern = options.pattern;
    m_sortedChannel = options.channel;
    m_sortedRevision = options.dataRevision;
    m_order.resize(count);
    m_maxLength = 0;
    if (clip == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        m_order[i] = static_cast<std::uint32_t>(i);
        m_maxLength = std::max(m_maxLength, clip->notes[i].length.value);
    }
    const auto& notes = clip->notes;
    std::ranges::sort(m_order, [&notes](std::uint32_t lhs, std::uint32_t rhs) {
        return project::noteOrderBefore(notes[lhs], notes[rhs]);
    });
}

std::pair<std::size_t, std::size_t>
PianoRollGeometry::visibleRange(const project::NoteClip& clip,
                                const Viewport& viewport) const noexcept {
    // Anything starting before tickStart - maxLength has ended before the view.
    const double earliest = viewport.tickStart - static_cast<double>(m_maxLength);
    const auto first = std::ranges::partition_point(m_order, [&](std::uint32_t i) {
        return static_cast<double>(clip.notes[i].start.value) < earliest;
    });
    const auto last = std::ranges::partition_point(m_order, [&](std::uint32_t i) {
        return static_cast<double>(clip.notes[i].start.value) <= viewport.tickEnd;
    });
    return {static_cast<std::size_t>(first - m_order.begin()),
            static_cast<std::size_t>(std::max(first, last) - m_order.begin())};
}

void PianoRollGeometry::buildRows(const Viewport& viewport, const BuildOptions& options) {
    const int low = std::clamp(static_cast<int>(std::floor(viewport.pitchLow)), 0, 127);
    const int high = std::clamp(static_cast<int>(std::ceil(viewport.pitchHigh)) - 1, 0, 127);
    const std::size_t rows = high >= low ? static_cast<std::size_t>(high - low + 1) : 0;
    const std::span<float> out = m_rows.resize(rows * kQuadVertices, kFloatsPerVertex);
    float* w = out.data();
    const auto x0 = static_cast<float>(viewport.tickStart / static_cast<double>(core::kPpq));
    const auto x1 = static_cast<float>(viewport.tickEnd / static_cast<double>(core::kPpq));
    for (int pitch = low; pitch <= high && rows > 0; ++pitch) {
        ColorRole role = isBlackKey(pitch) ? ColorRole::kRowBlack : ColorRole::kRowWhite;
        if (options.scale.mask != 0) {
            if (((pitch - options.scale.root) % 12 + 12) % 12 == 0) {
                role = ColorRole::kRowRoot;
            } else {
                role =
                    options.scale.contains(pitch) ? ColorRole::kRowInScale : ColorRole::kRowBlack;
            }
        }
        w = quad(w, x0, worldRowTop(pitch), x1, worldRowTop(pitch) + 1.0F, role);
    }
}

void PianoRollGeometry::buildGrid(const project::Project& project, const Viewport& viewport,
                                  const BuildOptions& options) {
    const auto start = static_cast<std::int64_t>(std::floor(std::max(0.0, viewport.tickStart)));
    const auto end = static_cast<std::int64_t>(std::ceil(std::max(0.0, viewport.tickEnd)));
    const float yTop = std::clamp(128.0F - viewport.pitchHigh, 0.0F, 128.0F);
    const float yBottom = std::clamp(128.0F - viewport.pitchLow, 0.0F, 128.0F);
    const auto meters = project.tempo.meterEvents();
    const auto minStep = static_cast<std::int64_t>(
        std::ceil(kMinGridSpacingPixels / std::max(viewport.pixelsPerTick, 1e-9F)));

    // Two passes over the meter segments: count, then write.
    std::size_t lines = 0;
    for (int pass = 0; pass < 2; ++pass) {
        float* w = pass == 0 ? nullptr : m_grid.resize(lines * 2, kFloatsPerVertex).data();
        for (std::size_t m = 0; m < std::max<std::size_t>(meters.size(), 1); ++m) {
            const core::MeterEvent meter = meters.empty() ? core::MeterEvent{} : meters[m];
            const std::int64_t segStart = meter.at.value;
            const std::int64_t segEnd =
                m + 1 < meters.size() ? meters[m + 1].at.value : std::max(end, segStart) + 1;
            const std::int64_t beat = core::ticksPerBeat(meter.denominator);
            const std::int64_t bar = beat * std::max<std::int64_t>(1, meter.numerator);
            std::int64_t step = options.gridDivision > 0 ? options.gridDivision : beat;
            for (const std::int64_t candidate : {step, beat, bar}) {
                step = candidate;
                if (step >= minStep) {
                    break;
                }
            }
            while (step < minStep) {
                step += bar;
            }
            const std::int64_t from = std::max(start, segStart);
            const std::int64_t to = std::min(end, segEnd - 1);
            std::int64_t tick =
                segStart +
                (((std::max<std::int64_t>(0, from - segStart) + step - 1) / step) * step);
            for (; tick <= to; tick += step) {
                if (pass == 0) {
                    ++lines;
                    continue;
                }
                const std::int64_t local = tick - segStart;
                ColorRole role = ColorRole::kGridSub;
                if (local % bar == 0) {
                    role = ColorRole::kGridBar;
                } else if (local % beat == 0) {
                    role = ColorRole::kGridBeat;
                }
                w = line(w, worldX(tick), yTop, worldX(tick), yBottom, role);
            }
        }
    }
}

void PianoRollGeometry::buildGhosts(const project::Pattern& pattern, const Viewport& viewport,
                                    const BuildOptions& options) {
    std::size_t bound = 0;
    for (const project::NoteClip& clip : pattern.noteClips) {
        if (clip.channel != options.channel) {
            bound += clip.notes.size();
        }
    }
    const std::span<float> out = m_ghosts.resize(bound * kQuadVertices, kFloatsPerVertex);
    float* w = out.data();
    for (const project::NoteClip& clip : pattern.noteClips) {
        if (clip.channel == options.channel ||
            (!options.ghostChannels.empty() &&
             std::ranges::find(options.ghostChannels, clip.channel) ==
                 options.ghostChannels.end())) {
            continue;
        }
        for (const project::Note& note : clip.notes) {
            const auto s = static_cast<double>(note.start.value);
            const auto e = static_cast<double>(note.end().value);
            const auto p = static_cast<float>(note.pitch);
            if (e < viewport.tickStart || s > viewport.tickEnd || p + 1.0F < viewport.pitchLow ||
                p > viewport.pitchHigh) {
                continue;
            }
            const float top = worldRowTop(note.pitch);
            w = quad(w, worldX(note.start.value), top + kRowInset, worldX(note.end().value),
                     top + 1.0F - kRowInset, ColorRole::kGhost);
        }
    }
    finish(m_ghosts, out.data(), w);
}

void PianoRollGeometry::buildNotes(const project::NoteClip& clip, const Viewport& viewport,
                                   const BuildOptions& options) {
    const auto [first, last] = visibleRange(clip, viewport);
    const std::span<float> out = m_notes.resize((last - first) * kQuadVertices, kFloatsPerVertex);
    float* w = out.data();
    // A one-pixel gap at the build's zoom between a note and the next on its row.
    const float gap = 1.0F / (viewport.pixelsPerTick * static_cast<float>(core::kPpq));
    for (std::size_t k = first; k < last; ++k) {
        const std::uint32_t index = m_order[k];
        const project::Note& note = clip.notes[index];
        const auto p = static_cast<float>(note.pitch);
        if (static_cast<double>(note.end().value) < viewport.tickStart ||
            p + 1.0F < viewport.pitchLow || p > viewport.pitchHigh) {
            continue;
        }
        const float x0 = worldX(note.start.value);
        const float x1 = std::max(x0 + gap, worldX(note.end().value) - gap);
        const float top = worldRowTop(note.pitch);
        w = quad(w, x0, top + kRowInset, x1, top + 1.0F - kRowInset,
                 noteRole(selectedIn(options.selected, note.id), note.muted));
        m_visibleIds.push_back(note.id);
        m_visibleIndex.push_back(index);
        m_visibleMuted.push_back(note.muted ? 1 : 0);
    }
    finish(m_notes, out.data(), w);
}

void PianoRollGeometry::buildDensity(const project::NoteClip& clip, const Viewport& viewport) {
    const auto [first, last] = visibleRange(clip, viewport);
    const std::int64_t bucket = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(kDensityPixelsPerSixteenth / viewport.pixelsPerTick));
    m_runStart.fill(-1);
    m_runEnd.fill(-1);
    const std::span<float> out = m_notes.resize((last - first) * kQuadVertices, kFloatsPerVertex);
    float* w = out.data();
    const auto flush = [&](int pitch) {
        const auto p = static_cast<std::size_t>(pitch);
        if (m_runStart[p] < 0) {
            return;
        }
        const float top = worldRowTop(pitch);
        w = quad(w, worldX(m_runStart[p] * bucket), top + kRowInset,
                 worldX((m_runEnd[p] + 1) * bucket), top + 1.0F - kRowInset, ColorRole::kDensity);
        m_runStart[p] = -1;
    };
    for (std::size_t k = first; k < last; ++k) {
        const project::Note& note = clip.notes[m_order[k]];
        const auto p = static_cast<std::size_t>(note.pitch);
        if (static_cast<double>(note.end().value) < viewport.tickStart ||
            static_cast<float>(note.pitch) + 1.0F < viewport.pitchLow ||
            static_cast<float>(note.pitch) > viewport.pitchHigh) {
            continue;
        }
        const std::int64_t b0 = note.start.value / bucket;
        const std::int64_t b1 = (note.end().value - 1) / bucket;
        if (m_runStart[p] >= 0 && b0 <= m_runEnd[p] + 1) {
            m_runEnd[p] = std::max(m_runEnd[p], b1);
            continue;
        }
        flush(note.pitch);
        m_runStart[p] = b0;
        m_runEnd[p] = b1;
    }
    for (int pitch = 0; pitch < 128; ++pitch) {
        flush(pitch);
    }
    finish(m_notes, out.data(), w);
}

void PianoRollGeometry::buildDensityLanes(const project::NoteClip& clip, const Viewport& viewport,
                                          const BuildOptions& options) {
    // One bar per density bucket, at the bucket's largest value: zoomed out, the lane
    // still shows the shape of the velocities without a bar per note.
    const auto [first, last] = visibleRange(clip, viewport);
    const std::int64_t bucket = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(kDensityPixelsPerSixteenth / viewport.pixelsPerTick));
    const std::span<float> out = m_lanes.resize((last - first) * kQuadVertices, kFloatsPerVertex);
    float* w = out.data();
    const bool bipolar = laneIsBipolar(options.lane);
    const float base = bipolar ? 0.5F : 1.0F;
    std::int64_t current = -1;
    float extreme = 0.0F;
    const auto flush = [&] {
        if (current >= 0) {
            const float y = 1.0F - extreme;
            w = quad(w, worldX(current * bucket), std::min(y, base), worldX((current + 1) * bucket),
                     std::max(y, base), ColorRole::kLane);
        }
    };
    for (std::size_t k = first; k < last; ++k) {
        const project::Note& note = clip.notes[m_order[k]];
        if (static_cast<double>(note.end().value) < viewport.tickStart) {
            continue;
        }
        const std::int64_t b = note.start.value / bucket;
        const float value = laneValue(note, options.lane);
        if (b != current) {
            flush();
            current = b;
            extreme = value;
        } else if (std::abs(value - base) > std::abs(extreme - base)) {
            extreme = value;
        }
    }
    flush();
    finish(m_lanes, out.data(), w);
}

void PianoRollGeometry::buildLanes(const project::NoteClip& clip, const Viewport& viewport,
                                   const BuildOptions& options) {
    const std::span<float> out =
        m_lanes.resize(m_visibleIndex.size() * kQuadVertices, kFloatsPerVertex);
    float* w = out.data();
    const float barBeats =
        kLaneBarPixels / (viewport.pixelsPerTick * static_cast<float>(core::kPpq));
    const bool bipolar = laneIsBipolar(options.lane);
    for (const std::uint32_t index : m_visibleIndex) {
        const project::Note& note = clip.notes[index];
        const float x0 = worldX(note.start.value);
        const float x1 =
            x0 + std::min(barBeats, std::max(worldX(note.end().value) - x0, barBeats * 0.4F));
        const float y = 1.0F - laneValue(note, options.lane);
        const float base = bipolar ? 0.5F : 1.0F;
        const ColorRole role =
            selectedIn(options.selected, note.id) ? ColorRole::kLaneSelected : ColorRole::kLane;
        w = quad(w, x0, std::min(y, base), x1, std::max(y, base), role);
    }
    finish(m_lanes, out.data(), w);
}

void PianoRollGeometry::buildCurves(const project::NoteClip& clip) {
    std::size_t segments = 0;
    for (const std::uint32_t index : m_visibleIndex) {
        if (const project::NoteExtras* extras = clip.extrasFor(clip.notes[index].id)) {
            segments += (extras->slide ? 3 : 0) + extras->pitchCurve.size() + 1;
        }
    }
    const std::span<float> out = m_curves.resize(segments * 2, kFloatsPerVertex);
    float* w = out.data();
    for (const std::uint32_t index : m_visibleIndex) {
        const project::Note& note = clip.notes[index];
        const project::NoteExtras* extras = clip.extrasFor(note.id);
        if (extras == nullptr) {
            continue;
        }
        const std::int64_t s = note.start.value;
        const std::int64_t e = note.end().value;
        if (extras->slide) {
            const project::NoteSlide& slide = *extras->slide;
            const std::int64_t a = std::min(e, s + slide.start.value);
            const std::int64_t b = std::min(e, a + slide.length.value);
            const float target = curveY(note.pitch, static_cast<float>(slide.targetCents));
            w = line(w, worldX(s), curveY(note.pitch, 0.0F), worldX(a), curveY(note.pitch, 0.0F),
                     ColorRole::kCurve);
            w = line(w, worldX(a), curveY(note.pitch, 0.0F), worldX(b), target, ColorRole::kCurve);
            w = line(w, worldX(b), target, worldX(e), target, ColorRole::kCurve);
        }
        if (!extras->pitchCurve.empty()) {
            float px = worldX(s);
            float py = curveY(note.pitch, 0.0F);
            for (const project::PitchPoint& point : extras->pitchCurve) {
                const float x = worldX(std::min(e, s + point.at.value));
                const float y = curveY(note.pitch, static_cast<float>(point.cents));
                w = line(w, px, py, x, y, ColorRole::kCurve);
                px = x;
                py = y;
            }
            w = line(w, px, py, worldX(e), py, ColorRole::kCurve);
        }
    }
    finish(m_curves, out.data(), w);
}

void PianoRollGeometry::updateSelection(std::span<const core::NoteId> selected) {
    if (m_density || m_visibleIds.empty()) {
        return;
    }
    const std::span<float> notes = m_notes.patch();
    const std::span<float> lanes = m_lanes.patch();
    const std::size_t laneCount = lanes.size() / (kQuadVertices * kFloatsPerVertex);
    for (std::size_t i = 0; i < m_visibleIds.size(); ++i) {
        const bool isSelected = selectedIn(selected, m_visibleIds[i]);
        const std::size_t base = i * kQuadVertices * kFloatsPerVertex;
        const float noteColor = roleValue(noteRole(isSelected, m_visibleMuted[i] != 0));
        const float laneColor = roleValue(isSelected ? ColorRole::kLaneSelected : ColorRole::kLane);
        for (std::size_t v = 0; v < kQuadVertices; ++v) {
            notes[base + (v * kFloatsPerVertex) + 2] = noteColor;
            if (i < laneCount) {
                lanes[base + (v * kFloatsPerVertex) + 2] = laneColor;
            }
        }
    }
}

} // namespace adx::geometry
