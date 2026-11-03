// adx-thread: main
#include "engine/clip/ClipEdit.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <variant>

#include "engine/clip/ClipRenderCache.h"
#include "engine/format/audio/SamplePool.h"
#include "engine/project/Automation.h"
#include "engine/project/CurveCompile.h"
#include "engine/project/Project.h"

namespace adx::clip {

namespace {

using project::Knot;
using Chain = std::vector<Knot>;

/// A region whose end is not known yet - its file is still decoding - runs to here and
/// is cut by the end of its source instead.
constexpr std::int64_t kOpenEnd = std::numeric_limits<std::int64_t>::max() / 4;

/// Pieces of an equal-power crossfade's sine and cosine.
constexpr int kCrossfadePieces = 32;

/// Seconds at `tick`, extended before zero at the first tempo, so a clip slipped
/// further than its start still has a clip origin to measure from.
double secondsExt(const core::TempoMap& tempo, std::int64_t tick) {
    if (tick >= 0) {
        return tempo.secondsAt(core::Ticks{tick});
    }
    const double bpm = std::max(tempo.bpmAt(core::Ticks{0}), 1.0);
    return static_cast<double>(tick) / static_cast<double>(core::kPpq) * 60.0 / bpm;
}

/// The chain's value approaching `tick` from the right (knotValueAt's rule: the later
/// knot where two share a tick) or from the left (the earlier).
float valueAt(const Chain& chain, std::int64_t tick, bool fromLeft) {
    if (chain.empty()) {
        return 1.0F;
    }
    if (tick < chain.front().tick || (fromLeft && tick == chain.front().tick)) {
        return chain.front().value;
    }
    if (tick > chain.back().tick || (!fromLeft && tick == chain.back().tick)) {
        return chain.back().value;
    }
    const auto after = fromLeft ? std::ranges::lower_bound(chain, tick, {}, &Knot::tick)
                                : std::ranges::upper_bound(chain, tick, {}, &Knot::tick);
    const Knot& b = *after;
    const Knot& a = *(after - 1);
    if (b.tick == a.tick) {
        return b.value;
    }
    const double t = static_cast<double>(tick - a.tick) / static_cast<double>(b.tick - a.tick);
    return static_cast<float>(a.value + ((b.value - a.value) * t));
}

/// The product of several chains, exact at every knot of every one of them and linear
/// between: a jump in any chain is a jump in the product.
Chain multiply(const std::vector<Chain>& chains) {
    std::vector<std::int64_t> at;
    for (const Chain& chain : chains) {
        for (const Knot& knot : chain) {
            at.push_back(knot.tick);
        }
    }
    std::ranges::sort(at);
    at.erase(std::ranges::unique(at).begin(), at.end());
    Chain out;
    out.reserve(at.size());
    for (const std::int64_t tick : at) {
        float left = 1.0F;
        float right = 1.0F;
        for (const Chain& chain : chains) {
            left *= valueAt(chain, tick, true);
            right *= valueAt(chain, tick, false);
        }
        out.push_back(Knot{.tick = tick, .value = left});
        if (right != left) {
            out.push_back(Knot{.tick = tick, .value = right});
        }
    }
    return out;
}

/// One curved segment from (t0, v0) to (t1, v1), subdivided: what a fade is, and what
/// each segment of a gain envelope is.
void appendSegment(Chain& chain, std::int64_t t0, float v0, std::int64_t t1, float v1,
                   const core::Curve& curve) {
    if (chain.empty() || chain.back().tick != t0 || chain.back().value != v0) {
        chain.push_back(Knot{.tick = t0, .value = v0});
    }
    project::appendCurveSegment(chain, t0, v0, t1, v1, curve, kClipGainTolerance, kClipCurvePieces);
}

/// A breakpoint envelope at absolute ticks, points after `last` not played.
Chain envelopeChain(const std::vector<project::Breakpoint>& points, std::int64_t origin,
                    std::int64_t last) {
    Chain chain;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const std::int64_t tick = origin + points[i].at.value;
        if (tick > last) {
            break;
        }
        if (i + 1 < points.size() && origin + points[i + 1].at.value <= last) {
            appendSegment(chain, tick, points[i].value, origin + points[i + 1].at.value,
                          points[i + 1].value, points[i].curve);
        } else if (chain.empty() || chain.back().tick != tick) {
            chain.push_back(Knot{.tick = tick, .value = points[i].value});
        }
    }
    return chain;
}

Chain crossfadeChain(std::int64_t from, std::int64_t to, bool fadeIn, project::CrossfadeKind kind) {
    Chain chain;
    if (kind == project::CrossfadeKind::Linear) {
        chain.push_back(Knot{.tick = from, .value = fadeIn ? 0.0F : 1.0F});
        chain.push_back(Knot{.tick = to, .value = fadeIn ? 1.0F : 0.0F});
        return chain;
    }
    for (int k = 0; k <= kCrossfadePieces; ++k) {
        const double x = static_cast<double>(k) / kCrossfadePieces;
        const double angle = x * std::numbers::pi / 2.0;
        const auto tick =
            from + static_cast<std::int64_t>(std::llround(static_cast<double>(to - from) * x));
        chain.push_back(Knot{
            .tick = tick, .value = static_cast<float>(fadeIn ? std::sin(angle) : std::cos(angle))});
    }
    return chain;
}

/// A warped clip's markers in output time, and where source 0 lands.
struct Warp {
    std::vector<std::pair<double, double>> pairs;
    double head{0.0};
};

Warp warpOf(const project::Project& project, const project::PlaylistItem& item,
            const project::AudioClipRef& audio) {
    Warp warp;
    if (audio.warps.size() < 2) {
        return warp;
    }
    // Marker positions are measured from the clip's own start - the item's start minus
    // its offset - so slipping a warped clip slides its window over the warped audio.
    const std::int64_t origin = item.start.value - item.sourceOffset.value;
    const double originSeconds = secondsExt(project.tempo, origin);
    for (const project::WarpMarker& marker : audio.warps) {
        const double out = secondsExt(project.tempo, origin + marker.at.value) - originSeconds;
        if (!warp.pairs.empty() &&
            (marker.source <= warp.pairs.back().first || out <= warp.pairs.back().second)) {
            continue; // not increasing in both: ignored rather than folded back
        }
        warp.pairs.emplace_back(marker.source, out);
    }
    if (warp.pairs.size() < 2) {
        warp.pairs.clear();
        return warp;
    }
    // The clip's start is a marker of its own: source 0 plays at clip time 0, and the
    // stretch before the first marker is whatever gets it there - as a warped clip
    // behaves in every DAW that has them. Only a first marker on source 0 itself moves
    // where the audio starts.
    const auto& [s0, o0] = warp.pairs[0];
    warp.head = s0 > 0.0 ? 0.0 : o0;
    for (auto& [source, out] : warp.pairs) {
        out -= warp.head;
    }
    return warp;
}

/// One item while its track is being compiled.
struct Pending {
    graph::AudioRegion region{};
    std::int64_t startTick{0};
    std::int64_t endTick{0};
    bool openEnd{false};
    project::CrossfadeKind crossfade{project::CrossfadeKind::EqualPower};
    std::vector<Chain> gain;
    Chain pan;
    Chain pitch;
};

struct Ranges {
    std::size_t gain{0};
    std::size_t gainCount{0};
    std::size_t pan{0};
    std::size_t panCount{0};
    std::size_t source{0};
    std::size_t sourceCount{0};
};

std::int64_t samplesAt(const core::TempoMap& tempo, std::int64_t tick, std::uint32_t rate) {
    return tempo.toSamples(core::Ticks{tick}, rate).value;
}

void appendSamples(std::vector<Knot>& out, const Chain& chain, const core::TempoMap& tempo,
                   std::uint32_t rate) {
    for (const Knot& knot : chain) {
        out.push_back(Knot{.tick = samplesAt(tempo, knot.tick, rate), .value = knot.value});
    }
}

/// A pitch envelope (cents) as a varispeed map from arrangement samples to source
/// frames. Integrated in steps through every segment that moves, one knot per flat run.
void appendSourceMap(std::vector<graph::SourceKnot>& out, const Chain& centsTicks,
                     const graph::AudioRegion& region, const core::TempoMap& tempo,
                     std::uint32_t rate) {
    constexpr std::int64_t kStep = 128;
    Chain cents;
    appendSamples(cents, centsTicks, tempo, rate);
    const auto ratioAt = [&cents](std::int64_t at) {
        return std::exp2(static_cast<double>(valueAt(cents, at, false)) / 1200.0);
    };
    double source = region.sourceAt;
    std::int64_t at = region.start;
    out.push_back(graph::SourceKnot{.at = at, .source = source});
    while (at < region.end) {
        const auto next = std::ranges::upper_bound(cents, at, {}, &Knot::tick);
        const std::int64_t boundary =
            next == cents.end() ? region.end : std::min(next->tick, region.end);
        const bool flat =
            next == cents.end() || next == cents.begin() || (next - 1)->value == next->value;
        const std::int64_t to = flat ? boundary : std::min(at + kStep, boundary);
        const std::int64_t mid = at + ((to - at) / 2);
        source += region.rate * ratioAt(mid) * static_cast<double>(to - at);
        at = to;
        out.push_back(graph::SourceKnot{.at = at, .source = source});
    }
}

/// Builds one item's region: what plays, from where, and its own gain, pan and pitch.
/// Crossfades with its neighbours are added once the whole track is known.
bool prepareItem(const project::Project& project, const project::PlaylistItem& item,
                 const project::AudioClipRef& audio, std::uint32_t rate, format::SamplePool& pool,
                 ClipRenderCache& cache, CompiledAudio& out, Pending& pending) {
    if (project.resources.find(audio.sample) == nullptr) {
        return false;
    }
    const ClipRenderKey key = renderKeyFor(project, item, rate, pool);
    const std::shared_ptr<const ClipRenderEntry> entry = cache.request(key, pool);
    out.pins.push_back(entry);
    const core::TempoMap& tempo = project.tempo;

    const std::int64_t start = item.start.value;
    const double offsetSeconds =
        secondsExt(tempo, start + item.sourceOffset.value) - secondsExt(tempo, start);

    graph::AudioRegion& region = pending.region;
    region.start = samplesAt(tempo, start, rate);
    double frames = -1.0;
    if (entry->handle().ready()) {
        // The rendered buffer is at the project's rate and already stretched: it plays
        // at unit rate from wherever the mapping lands at the item's start.
        region.handle = &entry->handle();
        region.rate = 1.0;
        region.sourceAt = renderedStartSeconds(project, item) * rate;
        frames = static_cast<double>(entry->handle().view().frames);
    } else {
        // Until it is: the decoded file, unstretched, at its own rate (phase_8.md §9).
        out.pending = true;
        out.waiting.push_back(entry);
        const format::SampleHandle& raw = entry->source()->handle();
        region.handle = raw.ready() ? &raw : &entry->handle();
        if (raw.ready()) {
            const double sourceRate = raw.view().sampleRate;
            region.rate = sourceRate / rate;
            region.sourceAt = offsetSeconds * sourceRate;
            frames = static_cast<double>(raw.view().frames);
        } else {
            region.rate = 1.0;
            region.sourceAt = 0.0;
        }
    }
    if (region.rate == 1.0) {
        // A sub-sample slip is not an edit anyone made; a whole-frame start is a copy.
        region.sourceAt = std::round(region.sourceAt);
    }

    std::int64_t end = kOpenEnd;
    if (frames >= 0.0) {
        end = region.start + static_cast<std::int64_t>(
                                 std::ceil(std::max(0.0, frames - region.sourceAt) / region.rate));
    }
    if (item.length.value > 0) {
        end = std::min(end, samplesAt(tempo, start + item.length.value, rate));
    }
    region.end = end;
    pending.openEnd = end == kOpenEnd;
    pending.startTick = start;
    if (pending.openEnd) {
        pending.endTick = kOpenEnd;
    } else if (item.length.value > 0) {
        pending.endTick = start + item.length.value;
    } else {
        pending.endTick = tempo.toTicks(core::Samples{end}, rate).value;
    }
    pending.crossfade = audio.crossfade;

    if (audio.fadeIn.length.value > 0) {
        Chain fade;
        appendSegment(fade, start, 0.0F, start + audio.fadeIn.length.value, 1.0F,
                      audio.fadeIn.curve);
        pending.gain.push_back(std::move(fade));
    }
    if (audio.fadeOut.length.value > 0 && !pending.openEnd) {
        Chain fade;
        appendSegment(fade, std::max(start, pending.endTick - audio.fadeOut.length.value), 1.0F,
                      pending.endTick, 0.0F, audio.fadeOut.curve);
        pending.gain.push_back(std::move(fade));
    }
    for (const project::ClipEnvelope& envelope : item.envelopes) {
        const std::int64_t last = pending.openEnd ? kOpenEnd : pending.endTick;
        if (envelope.local == project::ClipTarget::Gain) {
            pending.gain.push_back(envelopeChain(envelope.points, start, last));
        } else if (envelope.local == project::ClipTarget::Pan) {
            pending.pan = envelopeChain(envelope.points, start, last);
        } else if (envelope.local == project::ClipTarget::PitchCents) {
            pending.pitch = envelopeChain(envelope.points, start, last);
        }
    }
    return true;
}

/// A frozen channel's render: from sample 0 of the arrangement, unit rate when it was
/// rendered at this rate - which is every case but a render-rate change since.
Pending frozenRegion(const project::Project& project, const project::Channel& channel,
                     std::uint32_t rate, format::SamplePool& pool, CompiledAudio& out) {
    Pending pending;
    const std::shared_ptr<const format::SampleEntry> entry =
        pool.request(pool.resolve(project.resources, channel.frozen));
    out.pins.push_back(entry);
    graph::AudioRegion& region = pending.region;
    region.start = 0;
    region.handle = &entry->handle();
    region.sourceAt = 0.0;
    region.rate = 1.0;
    region.end = kOpenEnd;
    if (entry->handle().ready()) {
        const format::SampleView& view = entry->handle().view();
        region.rate = static_cast<double>(view.sampleRate) / rate;
        region.end =
            static_cast<std::int64_t>(std::ceil(static_cast<double>(view.frames) / region.rate));
    } else {
        out.pending = true;
    }
    pending.openEnd = region.end == kOpenEnd;
    pending.endTick =
        pending.openEnd ? kOpenEnd : project.tempo.toTicks(core::Samples{region.end}, rate).value;
    return pending;
}

void addCrossfades(std::vector<Pending>& items) {
    for (std::size_t later = 0; later < items.size(); ++later) {
        for (std::size_t earlier = 0; earlier < later; ++earlier) {
            Pending& a = items[earlier];
            Pending& b = items[later];
            if (a.openEnd || b.startTick <= a.startTick || b.startTick >= a.endTick) {
                continue;
            }
            const std::int64_t to = b.openEnd ? a.endTick : std::min(a.endTick, b.endTick);
            a.gain.push_back(crossfadeChain(b.startTick, to, false, b.crossfade));
            b.gain.push_back(crossfadeChain(b.startTick, to, true, b.crossfade));
        }
    }
}

} // namespace

double renderedStartSeconds(const project::Project& project, const project::PlaylistItem& item) {
    const auto* audio = std::get_if<project::AudioClipRef>(&item.content);
    if (audio == nullptr) {
        return 0.0;
    }
    const core::TempoMap& tempo = project.tempo;
    const std::int64_t start = item.start.value;
    const double startSeconds = secondsExt(tempo, start);
    const Warp warp = warpOf(project, item, *audio);
    if (warp.pairs.empty()) {
        const double offsetSeconds =
            secondsExt(tempo, start + item.sourceOffset.value) - startSeconds;
        return offsetSeconds * std::max(0.01, static_cast<double>(audio->stretch));
    }
    return startSeconds - secondsExt(tempo, start - item.sourceOffset.value) - warp.head;
}

ClipRenderKey renderKeyFor(const project::Project& project, const project::PlaylistItem& item,
                           std::uint32_t sampleRate, const format::SamplePool& pool) {
    ClipRenderKey key;
    const auto* audio = std::get_if<project::AudioClipRef>(&item.content);
    if (audio == nullptr) {
        return key;
    }
    if (const project::SampleRef* sample = project.resources.find(audio->sample)) {
        key.path = pool.resolve(project.resources, sample->path);
    }
    key.edits = audio->edits;
    key.reverse = audio->reverse;
    key.stretch = audio->stretch;
    key.pitchSemitones = audio->pitchSemitones;
    key.sampleRate = sampleRate;
    key.warp = warpOf(project, item, *audio).pairs;
    return key;
}

std::vector<std::shared_ptr<const void>> prefetchAudio(const project::Project& project,
                                                       std::uint32_t sampleRate,
                                                       format::SamplePool& pool,
                                                       ClipRenderCache& cache) {
    std::vector<std::shared_ptr<const void>> pins;
    for (const project::PlaylistTrack& track : project.playlist.tracks) {
        for (const project::PlaylistItem& item : track.items) {
            const auto* audio = std::get_if<project::AudioClipRef>(&item.content);
            if (audio != nullptr && project.resources.find(audio->sample) != nullptr) {
                pins.push_back(cache.request(renderKeyFor(project, item, sampleRate, pool), pool));
            }
        }
    }
    return pins;
}

std::int64_t audioEndSamples(const project::Project& project, std::uint32_t sampleRate,
                             format::SamplePool& pool, ClipRenderCache& cache) {
    std::vector<graph::AudioSource> tracks;
    tracks.reserve(project.playlist.tracks.size());
    for (const project::PlaylistTrack& track : project.playlist.tracks) {
        tracks.push_back(graph::AudioSource{.track = track.id, .frozen = {}});
    }
    CompiledAudio compiled;
    compileAudio(project, tracks, sampleRate, pool, cache, compiled);
    std::int64_t end = 0;
    for (const graph::AudioRegion& region : compiled.regions) {
        if (region.end != kOpenEnd) {
            end = std::max(end, region.end);
        }
    }
    return end;
}

void compileAudio(const project::Project& project, std::span<const graph::AudioSource> sources,
                  std::uint32_t sampleRate, format::SamplePool& pool, ClipRenderCache& cache,
                  CompiledAudio& out) {
    out = CompiledAudio{};
    std::vector<std::pair<std::size_t, std::size_t>> tableRanges;
    std::vector<Ranges> ranges;
    for (const graph::AudioSource& source : sources) {
        std::vector<Pending> items;
        if (source.frozen.valid()) {
            if (const project::Channel* channel = project.find(source.frozen)) {
                items.push_back(frozenRegion(project, *channel, sampleRate, pool, out));
            }
        }
        const project::PlaylistTrack* track =
            source.track.valid() ? project.playlist.find(source.track) : nullptr;
        if (track != nullptr && project.playlist.audible(*track)) {
            for (const project::PlaylistItem& item : track->items) {
                const auto* audio = std::get_if<project::AudioClipRef>(&item.content);
                if (audio == nullptr || item.muted) {
                    continue;
                }
                Pending pending;
                if (prepareItem(project, item, *audio, sampleRate, pool, cache, out, pending)) {
                    items.push_back(std::move(pending));
                }
            }
        }
        std::ranges::stable_sort(items, {}, &Pending::startTick);
        addCrossfades(items);

        tableRanges.emplace_back(out.regions.size(), items.size());
        for (Pending const& item : items) {
            Ranges range{.gain = out.knots.size()};
            appendSamples(out.knots, multiply(item.gain), project.tempo, sampleRate);
            range.gainCount = out.knots.size() - range.gain;
            range.pan = out.knots.size();
            appendSamples(out.knots, item.pan, project.tempo, sampleRate);
            range.panCount = out.knots.size() - range.pan;
            range.source = out.sourceKnots.size();
            if (!item.pitch.empty() && item.region.end != kOpenEnd) {
                appendSourceMap(out.sourceKnots, item.pitch, item.region, project.tempo,
                                sampleRate);
            }
            range.sourceCount = out.sourceKnots.size() - range.source;
            out.regions.push_back(item.region);
            ranges.push_back(range);
        }
    }
    // Spans last: every vector has stopped growing.
    for (std::size_t r = 0; r < out.regions.size(); ++r) {
        graph::AudioRegion& region = out.regions[r];
        const Ranges& range = ranges[r];
        region.gain = std::span<const Knot>{out.knots}.subspan(range.gain, range.gainCount);
        region.pan = std::span<const Knot>{out.knots}.subspan(range.pan, range.panCount);
        region.sourceMap = std::span<const graph::SourceKnot>{out.sourceKnots}.subspan(
            range.source, range.sourceCount);
    }
    for (const auto& [first, count] : tableRanges) {
        out.tables.push_back(graph::AudioTrackTable{
            .regions = std::span<const graph::AudioRegion>{out.regions}.subspan(first, count)});
    }
}

namespace {

/// `points` (item ticks) before and after `cut`, each ending or starting with a
/// breakpoint at the cut that holds the value there, the right's shifted to start at 0.
std::pair<std::vector<project::Breakpoint>, std::vector<project::Breakpoint>>
cutPoints(const std::vector<project::Breakpoint>& points, core::Ticks cut) {
    if (points.empty()) {
        return {points, points};
    }
    project::AutomationClip lane;
    lane.points = points;
    const float value = lane.valueAt(cut);
    // The shape of the segment the cut falls in, carried by its first point.
    const auto after = std::ranges::upper_bound(points, cut, {}, &project::Breakpoint::at);
    const core::Curve curve = after == points.begin() ? core::Curve{} : std::prev(after)->curve;
    std::vector<project::Breakpoint> left;
    std::vector<project::Breakpoint> right{
        project::Breakpoint{.at = core::Ticks{0}, .value = value, .curve = curve}};
    for (const project::Breakpoint& point : points) {
        if (point.at < cut) {
            left.push_back(point);
        } else if (cut < point.at) {
            right.push_back(project::Breakpoint{.at = core::Ticks{point.at.value - cut.value},
                                                .value = point.value,
                                                .curve = point.curve});
        }
    }
    left.push_back(project::Breakpoint{.at = cut, .value = value, .curve = curve});
    return {std::move(left), std::move(right)};
}

} // namespace

std::optional<SplitItem> splitItem(const project::Project& project,
                                   const project::PlaylistItem& item, core::Ticks length,
                                   core::Ticks at) {
    const std::int64_t start = item.start.value;
    if (at.value <= start || at.value >= start + length.value) {
        return std::nullopt;
    }
    const core::Ticks cut{at.value - start};
    SplitItem out{.left = item, .right = item};
    out.left.length = cut;
    out.right.id = core::ItemId{};
    out.right.start = at;
    out.right.length = core::Ticks{length.value - cut.value};
    out.left.envelopes.clear();
    out.right.envelopes.clear();
    for (const project::ClipEnvelope& envelope : item.envelopes) {
        auto [left, right] = cutPoints(envelope.points, cut);
        project::ClipEnvelope& l = out.left.envelopes.emplace_back(envelope);
        l.points = std::move(left);
        project::ClipEnvelope& r = out.right.envelopes.emplace_back(envelope);
        r.points = std::move(right);
    }

    // The source position at the cut. Unstretched (or warped: the markers keep their
    // origin only if the origin, start - offset, does not move) that is the cut itself.
    std::int64_t offset = item.sourceOffset.value + cut.value;
    auto* leftAudio = std::get_if<project::AudioClipRef>(&out.left.content);
    auto* rightAudio = std::get_if<project::AudioClipRef>(&out.right.content);
    if (leftAudio != nullptr && rightAudio != nullptr) {
        leftAudio->fadeIn.length = std::min(leftAudio->fadeIn.length, out.left.length);
        leftAudio->fadeOut = project::ClipFade{};
        rightAudio->fadeIn = project::ClipFade{};
        rightAudio->fadeOut.length = std::min(rightAudio->fadeOut.length, out.right.length);
        if (rightAudio->warps.size() < 2 && rightAudio->stretch != 1.0F) {
            // Source seconds = offset seconds + elapsed / stretch (this file's header),
            // so the right half's offset is where that had got to at the cut.
            const core::TempoMap& tempo = project.tempo;
            const double startSeconds = secondsExt(tempo, start);
            const double offsetSeconds =
                secondsExt(tempo, start + item.sourceOffset.value) - startSeconds;
            const double atSeconds = secondsExt(tempo, at.value);
            const double reached =
                offsetSeconds + ((atSeconds - startSeconds) / rightAudio->stretch);
            offset = tempo.ticksAtSeconds(atSeconds + reached).value - at.value;
        }
    }
    out.right.sourceOffset = core::Ticks{std::max<std::int64_t>(0, offset)};
    return out;
}

} // namespace adx::clip
