#pragma once

#include "AudioData.h"
#include <cstdint>
#include <string>
#include <vector>

// live-PLAN Phase L2: a small hand-rolled recursive-descent compiler for
// Tidal/Strudel-lineage mini-notation (see live-PLAN.md Phase L2 for the
// grammar). Runs on the Main Thread only, exactly like AdxParser::LoadProject
// -- it never runs per-sample or per-block. Compiling turns one PATTERN=
// string into concrete Notes the engine already knows how to play, so
// nothing downstream (voice allocation, automation, export) has to change.
namespace PatternCompiler {

// One compiled Note's provenance: the [sourceStart, sourceStart+sourceLen)
// byte range (into the exact string passed to Compile -- callers that embed
// that string inside a larger buffer, e.g. LiveCodeEditor's PATTERN= line,
// must add their own base offset) of the mini-notation token that produced
// it. Parallel array to CompileResult::notes (same index). Feeds L4's
// playhead-synced step highlighting.
struct StepSpan {
    size_t sourceStart = 0;
    size_t sourceLen = 0;
};

struct CompileResult {
    std::vector<Note> notes;
    std::vector<StepSpan> spans; // parallel to notes
    bool ok = true;
    std::string error;      // human-readable parse error; empty when ok
    size_t errorColumn = 0; // byte offset into the pattern text, when !ok
};

// Compiles one mini-notation PATTERN string into Notes tiled across
// [loopStartBeat, loopEndBeat) -- one cycle's worth of events, matching
// live-PLAN L1's loop region. A trailing whitespace-delimited "scale=<name>"
// token (e.g. "0 3 5 7 scale=a_minor") switches bare-integer steps from
// drum/note-name resolution to scale-degree resolution against that scale;
// <name> is "<root>[#|b]_<mode>" (mode: major, minor, dorian, phrygian,
// lydian, mixolydian, locrian, chromatic), root defaults to octave 4.
//
// live-PLAN L5 (stretch): the pattern text may carry pipe-separated
// transform stages after the mini-notation body, e.g.
// `"bd sn" | every 4 rev` or `"0 3 5 7 | scale=a_minor | fast 2"`. Each
// stage is one of:
//   scale=<name>   -- same as the trailing-space form above, as a stage
//   rev            -- reverses step order (time-mirrors the compiled cycle)
//   fast <n>       -- tiles the whole compiled cycle n times into the same
//                     span (n rounded to the nearest integer, clamped to
//                     [1, 64]), doubling+ playback speed without touching
//                     pitch
//   every <n> rev  -- applies `rev` only on cycles where cycleIndex % n == 0
// Stages apply left to right over the flattened cycle; `scale=` (wherever
// it appears) always resolves at step-resolution time, before any other
// stage runs.
//
// cycleIndex selects which cycle of `< >` alternation, `/n` slow-down, and
// `every n` (L5) to realize. L2-only patterns are always compiled with
// cycleIndex 0 (this compiler runs once per edit, not once per real
// playback cycle) -- callers that want `every n` to actually vary per real
// loop repetition must track a live cycle counter themselves and pass it
// in, recompiling at cycle boundaries (see LiveCodeEditor, which does this
// for PATTERN= lines that contain an `every` stage).
//
// Deterministic: the same patternText (and seed, when explicitly passed)
// always compiles to the same notes -- required for hot reload and offline
// export to agree, since `?` per-event probability is driven by a seeded
// xorshift32 PRNG (same generator style as Voice::noiseRng) rather than a
// time-based one. seed == 0 (the default) derives a non-zero seed from an
// FNV-1a hash of patternText itself.
CompileResult Compile(const std::string& patternText, float loopStartBeat, float loopEndBeat,
                       int cycleIndex = 0, uint32_t seed = 0);

} // namespace PatternCompiler
