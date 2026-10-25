// adx-thread: main
//
// Tidal/Strudel-lineage mini-notation, ported from the archived engine
// (_archive/src-cpp/src/PatternCompiler.cpp). FINAL_PLAN §3.1: "This is a
// differentiator; do not rewrite it from scratch." The grammar, the transforms, the
// seeding and the per-note StepSpan are the archive's; docs/mininotation.md is its
// header, preserved as the spec. What changed on the port, and why (phase_7.md §4.5):
//
//   - output is ticks, not float beats: every boundary is cut with the deterministic
//     rounding of docs/adx-format-v2.md, `(span * i + n / 2) / n`, so a compiled
//     pattern is the same on every machine and in every build;
//   - problems carry a byte range, not one column (MiniDiagnostics.h);
//   - `(k,n,r)` is Bjorklund's algorithm. The archive used a bucket formula it
//     believed equivalent; it is only equivalent up to rotation, and E(5,8) came out
//     x.x.xx.x instead of the canonical x.xx.xx. (phase_7.md §10);
//   - the cycle index is the song's: cycle k spans [k * cycle, (k + 1) * cycle) of the
//     arrangement, so `<a b>`, `/n` and `every n` realise the same cycles in an export
//     as in playback, without the editor counting loops (the archive's LiveCodeEditor
//     did);
//   - `?` rolls are seeded per cycle: the archive's FNV-1a-of-text seed, mixed with
//     the cycle index. Still a pure function of (text, cycle), so hot reload and export
//     agree - but `bd?` no longer drops the same hits in every bar.
//
// Main thread only. Nothing here runs per block: the snapshot builder compiles a
// placement's cycles when it flattens the channel's events.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/TempoMap.h"
#include "engine/core/Time.h"
#include "engine/format/mininotation/MiniDiagnostics.h"

namespace adx::mini {

/// Provenance of one compiled note: the byte range of the step token that produced
/// it, in the text passed to compile(). What playhead-synced step highlighting reads.
struct StepSpan {
    std::uint32_t start{0};
    std::uint32_t length{0};

    [[nodiscard]] friend bool operator==(const StepSpan&, const StepSpan&) noexcept = default;
};

struct MiniNote {
    core::Ticks start;
    core::Ticks length;
    std::uint8_t pitch{60};
    std::uint8_t velocity{100};

    [[nodiscard]] friend bool operator==(const MiniNote&, const MiniNote&) noexcept = default;
};

struct CompileResult {
    /// Sorted by start; stable for simultaneous notes (stack layers in order).
    std::vector<MiniNote> notes;
    /// Parallel to `notes`.
    std::vector<StepSpan> spans;
    /// Errors stop the compile (no notes); warnings do not.
    std::vector<Problem> problems;
    bool ok{true};
};

/// Compiles one cycle of `text` into [cycleStart, cycleStart + cycleLength).
///
/// `cycleIndex` selects which cycle of `<a b>` alternation, `/n` slow-down and
/// `every n` to realise. `seed` 0 (the default) derives the `?` seed from the text.
[[nodiscard]] CompileResult compile(std::string_view text, core::Ticks cycleStart,
                                    core::Ticks cycleLength, std::int64_t cycleIndex,
                                    std::uint32_t seed = 0);

/// A compiled note placed in the arrangement, with the cycle it came from.
struct RealizedNote {
    MiniNote note;
    StepSpan span;
    std::int64_t cycle{0};
    /// Its index within its cycle's notes - with the cycle, a stable identity, so an
    /// unrelated edit does not renumber a sounding note.
    std::uint32_t index{0};
};

/// Every note `text` plays in the arrangement range [from, to), cycles being
/// `cycleLength` long from tick 0. A note belongs to the range when it starts in it.
/// Problems are reported once (from the first cycle compiled), not once per cycle.
[[nodiscard]] std::vector<RealizedNote> realize(std::string_view text, core::Ticks from,
                                                core::Ticks to, core::Ticks cycleLength,
                                                std::vector<Problem>* problems = nullptr);

/// The length of one cycle: one bar of the meter in effect at tick 0.
[[nodiscard]] core::Ticks cycleLength(const core::TempoMap& tempo) noexcept;

/// The 32-bit FNV-1a hash the default seed is derived from. Exposed for tests.
[[nodiscard]] std::uint32_t fnv1a(std::string_view text) noexcept;

/// Bjorklund's Euclidean rhythm: `k` onsets spread over `n` steps, rotated left by
/// `r`. Exposed for mini_euclid_table.
[[nodiscard]] std::vector<bool> euclid(int k, int n, int r);

} // namespace adx::mini
