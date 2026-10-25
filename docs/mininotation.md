# Mini-notation

A pattern written as a line of text — `bd*2 [~ sn] hh(3,8)` — in the Tidal/Strudel
lineage. It is how a `MINI` block in a `.adx` file describes notes
([adx-format-v2.md](adx-format-v2.md) §7.3), what the scripting API's
`add_mini` compiles ([scripting.md](scripting.md)), and what the editor panel lights
step by step while it plays.

This document is the specification. It began as the header of the archived compiler
(`_archive/src-cpp/include/PatternCompiler.h`), which was the most complete description
of the grammar there was; the compiler was ported near-verbatim to
`engine/format/mininotation/PatternCompiler.cpp` (FINAL_PLAN §3.1: *"This is a
differentiator; do not rewrite it from scratch"*). Where the port changed behaviour, the
change is listed in [§9](#9-changes-from-the-archived-compiler).

**Every example in the tables below is executed by the test suite**
(`tests/python/test_docs_examples.py`): the pattern is compiled and its notes compared
with the table. A table that stops being true fails CI.

---

## 1. Cycles

A pattern describes one **cycle**, and a cycle is one bar of the meter at tick 0 (in
4/4, four beats). Everything in the pattern divides that span: `bd sn` is two steps of
two beats each; `bd sn hh cp` is four steps of a beat each.

Cycles are counted from the start of the **song**, not of the pattern: a pattern placed
at bar 9 plays cycle 8. So `<a b>` (§3) alternates bar by bar across the whole
arrangement, and an offline export plays exactly the cycles playback does.

Positions come out in ticks (3840 per quarter note). A span divided `n` ways is cut at
`start + (span × i + n/2) / n` in integer arithmetic — the format's rounding — so a
pattern compiles to the same ticks on every machine.

## 2. Steps

A step is one word. It becomes one note, whose pitch is, in order of preference:

| A step that is… | means | example |
|---|---|---|
| an integer, when a scale is set (§5) | a degree of the scale | `0` `3` `-1` |
| a drum name | a General MIDI drum | `bd` = 36 |
| a note name | that note, octave 4 if none is given | `c` `F#3` `eb5` |
| anything else | middle C, and warning `ADX1007` | `zz` |

Drum names: `bd` 36, `rim` 37, `sn` `sd` 38, `cp` 39, `hh` 42, `lt` 45, `oh` 46,
`mt` 47, `cy` `cr` 49, `ht` 50, `rd` 51, `perc` 60. Note names are case-insensitive,
take one `#` or `b`, and C4 is MIDI 60. A step's velocity is 100.

`~` is a rest: it takes a step's time and plays nothing.

| Pattern | Cycle | Plays (note@beat) |
|---|---|---|
| `bd sn` | 0 | `C2@0 D2@2` |
| `bd sn hh cp` | 0 | `C2@0 D2@1 F#2@2 D#2@3` |
| `~ sn` | 0 | `D2@2` |
| `c e g` | 0 | `C4@0 E4@1.3333 G4@2.6667` |
| `c#5 eb` | 0 | `C#5@0 D#4@2` |

## 3. Structure

| Syntax | Meaning |
|---|---|
| `[a b]` | a group: its steps share one step's time (subdivision; groups nest) |
| `<a b c>` | alternation: one of them per cycle, in turn |
| `a, b` | stacking: layers separated by commas play at once |

| Pattern | Cycle | Plays (note@beat) |
|---|---|---|
| `[a b] c` | 0 | `A4@0 B4@1 C4@2` |
| `bd [sn sn]` | 0 | `C2@0 D2@2 D2@3` |
| `<a b>` | 0 | `A4@0` |
| `<a b>` | 1 | `B4@0` |
| `<a b c>` | 7 | `B4@0` |
| `bd, hh*2` | 0 | `C2@0 F#2@0 F#2@2` |

## 4. Modifiers

Glued to the step or group they modify — no space before them.

| Syntax | Meaning |
|---|---|
| `a*n` | repeat `n` times within its own step |
| `a/n` | slow down: play only on cycles that are multiples of `n` |
| `a!n` | replicate: `n` copies, each a step of its own; a bare `!` repeats the previous step |
| `a@w` | weight: take `w` shares of the sequence's time instead of one (`w` may be decimal) |
| `a?` `a?p` | play with probability 0.5, or `p` (§6) |
| `a(k,n)` `a(k,n,r)` | Euclidean rhythm: `k` onsets spread over `n` steps, rotated left by `r` |

| Pattern | Cycle | Plays (note@beat) |
|---|---|---|
| `bd*4` | 0 | `C2@0 C2@1 C2@2 C2@3` |
| `bd/2` | 0 | `C2@0` |
| `bd/2` | 1 | `` |
| `bd!3 sn` | 0 | `C2@0 C2@1 C2@2 D2@3` |
| `bd ! sn` | 0 | `C2@0 C2@1.3333 D2@2.6667` |
| `bd@3 sn` | 0 | `C2@0 D2@3` |
| `bd(3,8)` | 0 | `C2@0 C2@1.5 C2@3` |
| `bd(3,8,2)` | 0 | `C2@0.5 C2@2 C2@3` |
| `bd*8?0` | 0 | `` |

Euclidean rhythms are Bjorklund's algorithm, spelled as Tidal and Strudel spell them:
E(3,8) is `x..x..x.`, E(5,8) is `x.xx.xx.`. A negative rotation turns to the right.

## 5. Scales

With a scale set, integer steps are degrees of it: `0` is the root, `7` is an octave up
in a seven-note scale, `-1` is the degree below the root. The root is in octave 4.

The scale is `scale=<root>_<mode>` — root `c`, `d#`, `eb`…; mode one of `major`,
`minor` (natural), `dorian`, `phrygian`, `lydian`, `mixolydian`, `locrian`,
`chromatic` (an unknown mode is major). It is written either as the last word of the
pattern, or as a pipe stage (§7). A pipe stage wins over a trailing word.

| Pattern | Cycle | Plays (note@beat) |
|---|---|---|
| `0 2 4 scale=c_major` | 0 | `C4@0 E4@1.3333 G4@2.6667` |
| `0 1 \| scale=a_minor` | 0 | `A4@0 B4@2` |
| `-1 7 scale=c_major` | 0 | `B3@0 C5@2` |
| `0 3 5 7 \| scale=a_minor` | 0 | `A4@0 D5@1 F5@2 A5@3` |

## 6. Probability

`?` keeps a step with probability one half; `?0.3` with probability 0.3. The dice are a
seeded xorshift32 — never the clock — so the same pattern always plays the same hits in
the same cycle, in playback and in an export. The seed is the FNV-1a hash of the
pattern's text, mixed with the cycle number, so the hits vary from cycle to cycle but
never from run to run.

## 7. Transforms

After the pattern, `|`-separated stages transform the whole compiled cycle, left to
right:

| Stage | Meaning |
|---|---|
| `scale=<name>` | set the scale (§5) — resolved while compiling, before the others |
| `rev` | reverse: the cycle is mirrored in time |
| `fast <n>` | play the cycle `n` times (rounded, 1..64) within one cycle |
| `every <n> rev` | reverse only on cycles that are multiples of `n` |

Anything else after a `|` is an error (`ADX2015`): the stages are typed syntax, not a
format that skips what it does not know.

| Pattern | Cycle | Plays (note@beat) |
|---|---|---|
| `bd sn \| rev` | 0 | `D2@0 C2@2` |
| `bd sn \| fast 2` | 0 | `C2@0 D2@1 C2@2 D2@3` |
| `bd sn \| every 2 rev` | 0 | `D2@0 C2@2` |
| `bd sn \| every 2 rev` | 1 | `C2@0 D2@2` |
| `0 3 \| scale=c_major \| fast 2` | 0 | `C4@0 F4@1 C4@2 F4@3` |

## 8. Errors

A pattern that does not parse is `ADX2015`, reported at the character where reading
stopped (`[bd sn` — at the end, where `]` was expected) or over the whole bad stage
(`| wobble 3`). It plays nothing until it is fixed; the rest of the file is unaffected.
An unknown step is the warning `ADX1007` and plays middle C. In a `.adx` file both are
reported at their line and column in the file, so the editor underlines the exact
characters.

A `MINI` block may span lines: they are joined by one space and compiled as one
pattern. A line of a block may begin with `[` — `  [bd sn] hh` — because only a line at
the left margin is a section header.

## 9. Changes from the archived compiler

| Change | Why |
|---|---|
| Ticks, not float beats | the format's deterministic rounding; the same notes everywhere |
| A problem has a byte range, mapped to the file's line and column | the archive returned one `errorColumn`; the editor needs a span to underline |
| `(k,n,r)` is Bjorklund's algorithm | the archive's bucket formula was a *rotation* of it: E(5,8) came out `x.x.xx.x`. Found by `mini_euclid_table`, which was written against the canonical table rather than the implementation |
| Cycles are the song's, counted by the engine | the archive's LiveCodeEditor counted loops itself; in the engine, an export sees the same cycles playback does |
| `?` is seeded per cycle | the archive used one seed per pattern, so `bd?` dropped the same hits every bar |
| Unknown steps warn (`ADX1007`) | the archive fell back to middle C silently |
| Integer `*`, `/`, `!` and `(k,n)` are bounded | `bd*99999999` cannot exhaust memory |
