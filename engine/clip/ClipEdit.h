// adx-thread: main
//
// Non-destructive clip editing, compiled: slip, trim, fades, crossfades, and the gain,
// pan and pitch envelopes of an audio clip (phase_8.md §3.2).
//
// All of it is properties of the placement - sourceOffset, length, the fades, the
// envelopes - and none of it touches audio. When a snapshot is built each audio item
// becomes one AudioRegion: where it plays in samples, which buffer, from which frame,
// and one gain chain that is the product of its fades, its crossfades and its gain
// envelope. Fades are the first and last segments of that chain and are compiled by the
// same code a drawn gain envelope is (clip_fade_equals_gain_envelope).
//
// The source mapping is the one the playlist draws the waveform with
// (PlaylistGeometry::previewAudio): source seconds = offset seconds + elapsed / stretch,
// offset seconds measured at the item's start. A warped clip maps through its markers
// instead.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/graph/AudioRegion.h"
#include "engine/graph/GraphBuilder.h"
#include "engine/project/Knot.h"
#include "engine/project/Playlist.h"

namespace adx::format {
class SamplePool;
}
namespace adx::project {
class Project;
} // namespace adx::project

namespace adx::clip {

class ClipRenderCache;
class ClipRenderEntry;
struct ClipRenderKey;

/// Everything a snapshot's audio tables point into.
struct CompiledAudio {
    std::vector<graph::AudioTrackTable> tables;
    std::vector<graph::AudioRegion> regions;
    std::vector<project::Knot> knots;
    std::vector<graph::SourceKnot> sourceKnots;
    /// The entries whose handles the regions point at, kept alive with the snapshot.
    std::vector<std::shared_ptr<const void>> pins;
    /// Something a region plays is still decoding or rendering. A rebuild once it is
    /// ready replaces the stand-in (RenderEngine::commit).
    bool pending{false};
    /// The entries that were not ready, so a caller can ask whether they are now.
    std::vector<std::shared_ptr<const ClipRenderEntry>> waiting;
};

/// The tolerance a fade or clip envelope is subdivided to, in linear gain, and the most
/// pieces one segment may take to get there. More than automation's 32: a fade is one
/// segment however long it is, and a coarse exponential fade-in is audible as steps in
/// the first milliseconds of a sustained sound.
inline constexpr float kClipGainTolerance = 1e-4F;
inline constexpr int kClipCurvePieces = 256;

/// Compiles `sources` (GraphBuild::audioSources, in order) at `sampleRate`: a playlist
/// track's clips, or a frozen channel's render as one region from the start of the
/// arrangement at unit rate - a straight copy (freeze_is_transparent).
void compileAudio(const project::Project& project, std::span<const graph::AudioSource> sources,
                  std::uint32_t sampleRate, format::SamplePool& pool, ClipRenderCache& cache,
                  CompiledAudio& out);

/// The render key for one audio item, as compileAudio builds it. What the waveform view
/// asks the peak cache for, so it draws what plays.
[[nodiscard]] ClipRenderKey renderKeyFor(const project::Project& project,
                                         const project::PlaylistItem& item,
                                         std::uint32_t sampleRate, const format::SamplePool& pool);

/// Where in its rendered buffer an audio item starts playing, in seconds of that buffer:
/// the offset seconds times the stretch for a plain clip, or where the warp map puts the
/// item's start. From there the buffer plays at unit rate, so the frame at arrangement
/// tick t is (this + seconds(t) - seconds(item.start)) * rate. One rule for the audio
/// and the waveform drawn over it.
[[nodiscard]] double renderedStartSeconds(const project::Project& project,
                                          const project::PlaylistItem& item);

/// Where the last audio clip ends, in samples at `sampleRate`: the arrangement's end
/// as far as audio is concerned, which the model cannot know without the files. Call
/// after prefetchAudio has been waited on.
[[nodiscard]] std::int64_t audioEndSamples(const project::Project& project,
                                           std::uint32_t sampleRate, format::SamplePool& pool,
                                           ClipRenderCache& cache);

/// Every sample the project's audio clips need, requested and pinned: what an offline
/// render calls before it waits, so the first snapshot already has them.
[[nodiscard]] std::vector<std::shared_ptr<const void>>
prefetchAudio(const project::Project& project, std::uint32_t sampleRate, format::SamplePool& pool,
              ClipRenderCache& cache);

/// An item cut in two: the playlist's slice. `left` keeps the item's id; `right` has
/// none yet. Everything about the item carries over so that the two play what the one
/// did: the right half starts where the source had got to (through the stretch), each
/// envelope is cut at the same instant with a breakpoint either side of it holding the
/// value there, the fade-in stays on the left and the fade-out on the right, and warp
/// markers carry over unchanged - they are measured from the clip's origin, which the
/// halves share.
struct SplitItem {
    project::PlaylistItem left;
    project::PlaylistItem right;
};

/// Cuts `item`, `length` long as the playlist shows it, at arrangement tick `at`;
/// nullopt unless `at` is strictly inside it. A stretched, unwarped clip's right-hand
/// offset is rounded to the tick, which is the offset's resolution.
[[nodiscard]] std::optional<SplitItem> splitItem(const project::Project& project,
                                                 const project::PlaylistItem& item,
                                                 core::Ticks length, core::Ticks at);

} // namespace adx::clip
