// What an audio track plays: placed clips, already resolved to sample positions.
//
// The snapshot builder turns each audio item into one of these on the main thread -
// ticks to arrangement samples, fades and crossfades and the gain envelope into one
// knot chain, the source mapping into a start and a rate - so the node that plays them
// does arithmetic and nothing else (phase_8.md §3.2).
//
// Rendering a region is a pure function of the arrangement's sample position. Nothing
// carries from one block to the next, which is what makes a seek, a loop wrap and a
// change of block size all trivially exact: the same position always produces the same
// sample.
//
// POD only: read by realtime code, carried in the render snapshot.
#pragma once

#include <cstdint>
#include <span>
#include <type_traits>

#include "engine/format/audio/SampleView.h"
#include "engine/project/Knot.h"

namespace adx::graph {

/// One point of a varispeed map: arrangement sample `at` reads source frame `source`.
/// Linear between points. What a pitch envelope compiles to.
struct SourceKnot {
    std::int64_t at;
    double source;
};

static_assert(std::is_trivially_copyable_v<SourceKnot>);

struct AudioRegion {
    /// Arrangement samples, [start, end).
    std::int64_t start;
    std::int64_t end;
    /// What plays. Never null; the node plays silence while it is not ready().
    const format::SampleHandle* handle;
    /// Source frame at `start`, and source frames per output frame. A region whose rate
    /// is exactly 1 and whose start is a whole frame is a straight copy - which is what
    /// keeps a frozen channel bit-identical.
    double sourceAt;
    double rate;
    /// When non-empty, replaces sourceAt/rate: the varispeed map of a pitch envelope.
    std::span<const SourceKnot> sourceMap;
    /// Linear gain over arrangement samples: fades, crossfades and the clip's gain
    /// envelope multiplied into one chain. Empty is unity.
    std::span<const project::Knot> gain;
    /// -1..1, the strip's balance law (nodes/Gain.h). Empty is centre.
    std::span<const project::Knot> pan;
};

static_assert(std::is_trivially_copyable_v<AudioRegion>);

/// One audio track's regions, sorted by start.
struct AudioTrackTable {
    std::span<const AudioRegion> regions;
};

} // namespace adx::graph
