// adx-thread: main
#include "engine/geometry/PlaylistGeometry.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <variant>

#include "engine/geometry/AutomationGeometry.h"
#include "engine/geometry/ClipPeakCache.h"
#include "engine/geometry/ColorPalette.h"
#include "engine/project/ParamRegistry.h"

namespace adx::geometry {
namespace {

constexpr double kPpq = static_cast<double>(core::kPpq);

enum Layer : std::uint8_t { kLanes, kGrid, kClips, kPreviews, kCurves, kLayerCount };

void quad(std::vector<float>& out, float x0, float y0, float x1, float y1, ColorRole role) {
    const float r = roleValue(role);
    out.insert(out.end(), {x0, y0, r, x1, y0, r, x1, y1, r, x0, y0, r, x1, y1, r, x0, y1, r});
}

void line(std::vector<float>& out, float x0, float y0, float x1, float y1, ColorRole role) {
    const float r = roleValue(role);
    out.insert(out.end(), {x0, y0, r, x1, y1, r});
}

[[nodiscard]] float beats(std::int64_t ticks) noexcept {
    return static_cast<float>(static_cast<double>(ticks) / kPpq);
}

/// A placement's natural length when it says none: a pattern's own length; an audio
/// clip's is not known here, so a bar; an automation clip's runs to its last point.
std::int64_t naturalLength(const project::Project& project, const project::PlaylistItem& item) {
    if (item.length.value > 0) {
        return item.length.value;
    }
    if (const auto* ref = std::get_if<project::PatternRef>(&item.content)) {
        const project::Pattern* pattern = project.find(ref->pattern);
        return pattern != nullptr ? pattern->length.value : core::kPpq * 4;
    }
    if (const auto* ref = std::get_if<project::AutomationRef>(&item.content)) {
        const project::AutomationClip* clip = project.findAutomationClip(ref->clip);
        if (clip != nullptr && !clip->points.empty()) {
            return std::max<std::int64_t>(clip->points.back().at.value - item.sourceOffset.value,
                                          core::kPpq);
        }
    }
    return core::kPpq * 4;
}

} // namespace

void layoutTracks(const project::Playlist& playlist, std::vector<TrackRow>& out) {
    out.clear();
    float top = 0.0F;
    for (const project::PlaylistTrack& track : playlist.tracks) {
        const project::PlaylistTrack* group = playlist.find(track.group);
        const bool hidden = group != nullptr && group->collapsed;
        const float height =
            hidden ? 0.0F : static_cast<float>(std::max<std::uint16_t>(1, track.height));
        out.push_back(TrackRow{.track = track.id, .top = top, .height = height});
        top += height;
    }
}

GeometryBuffer& PlaylistGeometry::buffer(std::size_t index) noexcept {
    switch (index) {
    case kLanes:
        return m_lanes;
    case kGrid:
        return m_grid;
    case kClips:
        return m_clips;
    case kPreviews:
        return m_previews;
    default:
        return m_curves;
    }
}

void PlaylistGeometry::build(const project::Project& project, const PlaylistViewport& viewport,
                             const PlaylistBuildOptions& options) {
    for (auto& scratch : m_scratch) {
        scratch.clear();
    }
    m_visible.clear();
    m_pending = false;
    m_detailed = 0;
    layoutTracks(project.playlist, m_rows);

    buildLanes(viewport);
    buildGrid(project, viewport);
    for (std::size_t t = 0; t < project.playlist.tracks.size(); ++t) {
        const TrackRow& row = m_rows[t];
        if (row.height <= 0.0F || row.top + row.height < viewport.rowStart ||
            row.top > viewport.rowEnd) {
            continue;
        }
        const project::PlaylistTrack& track = project.playlist.tracks[t];
        const bool audible = project.playlist.audible(track);
        for (const project::PlaylistItem& item : track.items) {
            const bool selected = std::ranges::binary_search(
                options.selected, item.id.value, {}, [](core::ItemId id) { return id.value; });
            buildClip(project, viewport, item, row, selected, audible, options);
        }
    }

    for (std::size_t layer = 0; layer < kLayerCount; ++layer) {
        const std::vector<float>& scratch = m_scratch.at(layer);
        const std::span<float> out =
            buffer(layer).resize(scratch.size() / kFloatsPerVertex, kFloatsPerVertex);
        std::ranges::copy(scratch, out.begin());
    }
}

void PlaylistGeometry::buildLanes(const PlaylistViewport& viewport) {
    std::vector<float>& out = m_scratch.at(kLanes);
    const float x0 = beats(static_cast<std::int64_t>(viewport.tickStart));
    const float x1 = beats(static_cast<std::int64_t>(viewport.tickEnd) + 1);
    std::size_t index = 0;
    for (const TrackRow& row : m_rows) {
        if (row.height <= 0.0F) {
            continue;
        }
        if (row.top + row.height >= viewport.rowStart && row.top <= viewport.rowEnd) {
            quad(out, x0, row.top, x1, row.top + row.height,
                 index % 2 == 0 ? ColorRole::kTrackA : ColorRole::kTrackB);
        }
        ++index;
    }
}

void PlaylistGeometry::buildGrid(const project::Project& project,
                                 const PlaylistViewport& viewport) {
    std::vector<float>& out = m_scratch.at(kGrid);
    const core::MeterEvent meter = project.tempo.meterAt(core::Ticks{0});
    const std::int64_t beat = (core::kPpq * 4) / std::max<std::int64_t>(1, meter.denominator);
    const std::int64_t bar = beat * std::max<std::int64_t>(1, meter.numerator);
    const float top = viewport.rowStart;
    const float bottom =
        std::max(viewport.rowEnd, m_rows.empty() ? 0.0F : m_rows.back().top + m_rows.back().height);
    const double pixelsPerBeat =
        static_cast<double>(viewport.pixelsPerTick) * static_cast<double>(beat);
    const double pixelsPerBar =
        pixelsPerBeat * static_cast<double>(std::max<std::int64_t>(1, meter.numerator));
    // Too dense to read: every 4th, 16th... bar only.
    std::int64_t barStep = 1;
    while (pixelsPerBar * static_cast<double>(barStep) < 12.0 && barStep < (1 << 20)) {
        barStep *= 4;
    }
    const bool beatLines = pixelsPerBeat >= 10.0;
    const std::int64_t step = beatLines ? beat : bar * barStep;
    const auto first =
        static_cast<std::int64_t>(std::max(0.0, viewport.tickStart) / static_cast<double>(step)) *
        step;
    for (std::int64_t tick = first; static_cast<double>(tick) <= viewport.tickEnd; tick += step) {
        const bool isBar = tick % bar == 0;
        line(out, beats(tick), top, beats(tick), bottom,
             isBar ? ColorRole::kGridBar : ColorRole::kGridBeat);
    }
}

void PlaylistGeometry::buildClip(const project::Project& project, const PlaylistViewport& viewport,
                                 const project::PlaylistItem& item, const TrackRow& row,
                                 bool selected, bool audible, const PlaylistBuildOptions& options) {
    const std::int64_t length = naturalLength(project, item);
    const std::int64_t end = item.start.value + length;
    if (static_cast<double>(end) < viewport.tickStart ||
        static_cast<double>(item.start.value) > viewport.tickEnd) {
        return;
    }
    VisibleClip visible{.item = item.id,
                        .track = row.track,
                        .start = item.start.value,
                        .end = end,
                        .top = row.top,
                        .height = row.height};
    if (const auto* ref = std::get_if<project::PatternRef>(&item.content)) {
        visible.kind = ClipKind::Pattern;
        visible.ref = ref->pattern.value;
    } else if (const auto* audio = std::get_if<project::AudioClipRef>(&item.content)) {
        visible.kind = ClipKind::Audio;
        visible.ref = audio->sample.value;
    } else if (const auto* automation = std::get_if<project::AutomationRef>(&item.content)) {
        visible.kind = ClipKind::Automation;
        visible.ref = automation->clip.value;
    }
    m_visible.push_back(visible);

    ColorRole body = ColorRole::kClip;
    if (selected) {
        body = ColorRole::kClipSelected;
    } else if (item.muted || !audible) {
        body = ColorRole::kClipMuted;
    }
    const float x0 = beats(item.start.value);
    const float x1 = beats(end);
    const float top = row.top + 0.04F;
    const float bottom = row.top + row.height - 0.04F;
    const float widthPx = static_cast<float>(length) * viewport.pixelsPerTick;
    std::vector<float>& clips = m_scratch.at(kClips);
    if (widthPx < kClipDetailPixels) {
        // Far out: one flat rectangle, nothing else.
        quad(clips, x0, top, x1, bottom, body);
        return;
    }
    ++m_detailed;
    const float header = top + (row.height * kClipHeaderShare);
    quad(clips, x0, top, x1, header, selected ? ColorRole::kClipSelected : ColorRole::kClipHeader);
    quad(clips, x0, header, x1, bottom, body);

    const float bodyHeight = bottom - header;
    if (const auto* ref = std::get_if<project::PatternRef>(&item.content)) {
        if (const project::Pattern* pattern = project.find(ref->pattern)) {
            previewPattern(*pattern, item, end, header, bodyHeight, viewport);
        }
    } else if (std::holds_alternative<project::AudioClipRef>(item.content)) {
        if (options.peaks != nullptr) {
            previewAudio(project, item, end, header, bodyHeight, viewport, *options.peaks);
        }
    } else {
        previewAutomation(project, item, end, header, bodyHeight, viewport);
    }
}

void PlaylistGeometry::previewPattern(const project::Pattern& pattern,
                                      const project::PlaylistItem& item, std::int64_t end,
                                      float top, float height, const PlaylistViewport& viewport) {
    int low = 127;
    int high = 0;
    for (const project::NoteClip& clip : pattern.noteClips) {
        for (const project::Note& note : clip.notes) {
            low = std::min<int>(low, note.pitch);
            high = std::max<int>(high, note.pitch);
        }
    }
    if (low > high) {
        return;
    }
    // At least an octave tall, so one repeated note does not fill the clip.
    const int span = std::max(high - low + 1, 12);
    const int pad = (span - (high - low + 1)) / 2;
    const float rowHeight = height / static_cast<float>(span);
    const std::int64_t offset = item.sourceOffset.value;
    const std::int64_t windowEnd = offset + (end - item.start.value);
    const double visibleFrom = viewport.tickStart - static_cast<double>(item.start.value - offset);
    const double visibleTo = viewport.tickEnd - static_cast<double>(item.start.value - offset);
    std::vector<float>& out = m_scratch.at(kPreviews);
    std::size_t drawn = 0;
    for (const project::NoteClip& clip : pattern.noteClips) {
        for (const project::Note& note : clip.notes) {
            const std::int64_t start = note.start.value;
            if (note.muted || start < offset || start >= windowEnd ||
                static_cast<double>(note.end().value) < visibleFrom ||
                static_cast<double>(start) > visibleTo || drawn >= kMaxPreviewNotes) {
                continue;
            }
            const std::int64_t stop = std::min(note.end().value, windowEnd);
            const float x0 = beats(item.start.value + (start - offset));
            const float x1 = beats(item.start.value + (stop - offset));
            const float y0 =
                top + (static_cast<float>(span - 1 - pad - (note.pitch - low)) * rowHeight);
            quad(out, x0, y0, x1, y0 + rowHeight, ColorRole::kClipPreview);
            ++drawn;
        }
    }
}

void PlaylistGeometry::previewAudio(const project::Project& project,
                                    const project::PlaylistItem& item, std::int64_t end, float top,
                                    float height, const PlaylistViewport& viewport,
                                    ClipPeakCache& peaks) {
    const auto& audio = std::get<project::AudioClipRef>(item.content);
    const project::SampleRef* sample = project.resources.find(audio.sample);
    if (sample == nullptr) {
        return;
    }
    std::filesystem::path path(sample->path);
    if (path.is_relative() && !project.resources.baseDirectory.empty()) {
        path = std::filesystem::path(project.resources.baseDirectory) / path;
    }
    // Every field of the key that changes what the clip sounds like makes a new entry,
    // so changing one redraws the clip (P5-4).
    const std::shared_ptr<const ClipPeaks> clipPeaks =
        peaks.request(PeakKey{.path = path.generic_string(),
                              .pitchSemitones = audio.pitchSemitones,
                              .stretch = audio.stretch,
                              .reversed = audio.reverse,
                              .sourceOffset = item.sourceOffset.value,
                              .gain = 1.0F});
    if (!clipPeaks->sourceReady() || clipPeaks->frames() == 0) {
        m_pending = m_pending || !clipPeaks->failed();
        return;
    }
    const double from = std::max(viewport.tickStart, static_cast<double>(item.start.value));
    const double to = std::min(viewport.tickEnd, static_cast<double>(end));
    if (to <= from) {
        return;
    }
    // One column every two pixels.
    const auto columns = static_cast<std::uint32_t>(
        std::clamp((to - from) * static_cast<double>(viewport.pixelsPerTick) / 2.0, 1.0, 4096.0));
    const double ticksPerColumn = (to - from) / columns;
    const double startSeconds = project.tempo.secondsAt(item.start);
    const double offsetSeconds =
        project.tempo.secondsAt(item.start + item.sourceOffset) - startSeconds;
    const double rate = clipPeaks->sampleRate();
    const double stretch = std::max(0.01, static_cast<double>(audio.stretch));
    const auto frames = static_cast<double>(clipPeaks->frames());
    const auto frameAt = [&](double tick) {
        const double seconds =
            project.tempo.secondsAt(core::Ticks{static_cast<std::int64_t>(tick)}) - startSeconds;
        double frame = (offsetSeconds + (seconds / stretch)) * rate;
        if (audio.reverse) {
            frame = frames - frame;
        }
        return std::clamp(frame, 0.0, frames);
    };
    const double samplesPerColumn = std::abs(frameAt(from + ticksPerColumn) - frameAt(from)) + 1.0;
    const int tier = clipPeaks->chooseTier(samplesPerColumn);
    if (tier < 0) {
        m_pending = true;
        return;
    }
    m_pending = m_pending || !clipPeaks->complete();
    const float middle = top + (height * 0.5F);
    std::vector<float>& out = m_scratch.at(kPreviews);
    for (std::uint32_t c = 0; c < columns; ++c) {
        const double t0 = from + (ticksPerColumn * c);
        const double t1 = t0 + ticksPerColumn;
        const double f0 = frameAt(t0);
        const double f1 = frameAt(t1);
        const auto begin = static_cast<std::uint64_t>(std::min(f0, f1));
        const auto stop = std::max(begin + 1, static_cast<std::uint64_t>(std::max(f0, f1)));
        if (begin >= clipPeaks->frames()) {
            continue;
        }
        const auto [low, high] = clipPeaks->range(tier, begin, stop);
        const float y0 = middle - (static_cast<float>(high) / 32767.0F * height * 0.5F);
        const float y1 = middle - (static_cast<float>(low) / 32767.0F * height * 0.5F);
        quad(out, static_cast<float>(t0 / kPpq), y0, static_cast<float>(t1 / kPpq),
             std::max(y1, y0 + 0.01F), ColorRole::kWaveFill);
    }
}

void PlaylistGeometry::previewAutomation(const project::Project& project,
                                         const project::PlaylistItem& item, std::int64_t end,
                                         float top, float height,
                                         const PlaylistViewport& viewport) {
    const auto& ref = std::get<project::AutomationRef>(item.content);
    const project::AutomationClip* clip = project.findAutomationClip(ref.clip);
    if (clip == nullptr || clip->points.empty()) {
        return;
    }
    const project::ParamDescriptor* descriptor =
        project::ParamRegistry::descriptorFor(clip->target, project);
    LaneRange range;
    if (descriptor != nullptr && descriptor->maximum > descriptor->minimum) {
        range = LaneRange{.minimum = descriptor->minimum,
                          .maximum = descriptor->maximum,
                          .logarithmic = descriptor->scale == project::ScaleKind::Logarithmic &&
                                         descriptor->minimum > 0.0F};
    } else {
        const auto [low, high] =
            std::ranges::minmax_element(clip->points, {}, &project::Breakpoint::value);
        range =
            LaneRange{.minimum = low->value, .maximum = std::max(high->value, low->value + 1.0F)};
    }
    std::vector<LanePoint> points;
    points.reserve(clip->points.size());
    for (const project::Breakpoint& point : clip->points) {
        points.push_back(
            LanePoint{.tick = point.at.value, .value = point.value, .curve = point.curve});
    }
    const LaneView view{.tickStart =
                            std::max(viewport.tickStart, static_cast<double>(item.start.value)),
                        .tickEnd = std::min(viewport.tickEnd, static_cast<double>(end)),
                        .pixelsPerTick = viewport.pixelsPerTick,
                        .heightPx = height * viewport.pixelsPerRow,
                        .segments = 0};
    // Clip-local points: tick 0 of the clip is its source offset, at item.start. The view
    // is absolute, and so are the ticks tessellateLane culls against it.
    const auto offset = static_cast<double>(item.start.value - item.sourceOffset.value);
    tessellateLane(m_scratch.at(kCurves), points, range, view, offset, top + (height * 0.08F),
                   height * 0.84F, roleValue(ColorRole::kAutomationLine));
}

core::PlaylistTrackId PlaylistGeometry::trackAt(const PlaylistViewport& viewport,
                                                float y) const noexcept {
    const float row = viewport.rowAt(y);
    for (const TrackRow& track : m_rows) {
        if (track.height > 0.0F && row >= track.top && row < track.top + track.height) {
            return track.track;
        }
    }
    return {};
}

ClipHit PlaylistGeometry::hitTest(const PlaylistViewport& viewport, float x,
                                  float y) const noexcept {
    const double tick = viewport.tickAt(x);
    const float row = viewport.rowAt(y);
    const auto edge = static_cast<double>(kClipEdgePixels / viewport.pixelsPerTick);
    // Last drawn is on top.
    for (std::size_t i = m_visible.size(); i-- > 0;) {
        const VisibleClip& clip = m_visible[i];
        if (row < clip.top || row >= clip.top + clip.height ||
            tick < static_cast<double>(clip.start) || tick > static_cast<double>(clip.end)) {
            continue;
        }
        ClipPart part = ClipPart::Body;
        const auto widthTicks = static_cast<double>(clip.end - clip.start);
        if (widthTicks > edge * 3.0) {
            if (tick - static_cast<double>(clip.start) <= edge) {
                part = ClipPart::LeftEdge;
            } else if (static_cast<double>(clip.end) - tick <= edge) {
                part = ClipPart::RightEdge;
            } else if (row < clip.top + (clip.height * kClipHeaderShare) &&
                       widthTicks * static_cast<double>(viewport.pixelsPerTick) >=
                           kClipDetailPixels) {
                part = ClipPart::Header;
            }
        }
        return ClipHit{.track = clip.track, .item = clip.item, .part = part};
    }
    return ClipHit{.track = trackAt(viewport, y), .item = {}, .part = ClipPart::None};
}

} // namespace adx::geometry
