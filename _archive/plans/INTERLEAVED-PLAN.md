# Interleaved Execution Plan: `live-PLAN.md` + `UI-Refactor.md`

**Purpose:** `live-PLAN.md` (RondoCode-style live-coding arc) and `UI-Refactor.md`
(FL-Studio-style visual arc) are independent goals that can both make
continuous progress without either blocking the other — *except* at three
points where they silently target the same code. This file is the merge:
one ordered milestone list (`M0`-`M11`) that interleaves both plans' phases,
resolves the collisions once, and leaves everything else free to proceed on
its own track. It does not replace either source plan — phase numbers below
(`L1`-`L5`, `P1`-`P6`) refer back to their sections in `live-PLAN.md` and
`UI-Refactor.md` respectively.

---

## The three collision points (why this file exists)

| Collision | live-PLAN | UI-Refactor | Resolution |
|---|---|---|---|
| **Master-output ring buffer tap** | L4.1: SPSC ring buffer in `AudioEngine::process` for the SCOPE panel's waveform + meters | P3.1: *the same* SPSC ring buffer for the SPECTRUM panel's FFT input | Build **one** tap (`M0`), generalized for both raw-waveform and FFT consumers. Neither plan's panel work is blocked waiting for the other's copy. |
| **Arranger ruler region** (`SequencerUI.cpp`) | L1.5: loop toggle + draggable loop-region brackets on the timeline ruler | P6.1: new bar:beat ruler strip with tick marks above the grid | Land L1's brackets first (`M2`), the bar:beat strip second (`M10`), as one coordinated pass on the *existing* ruler code rather than two blind, possibly-conflicting edits months apart. |
| **`BuildDefaultDockLayout` in `main.cpp`** | L3 adds `LIVE CODE`, L4 adds `SCOPE` | P3 adds `SPECTRUM`, P4 adds `VISUALIZER`, P5 adds `WATERFALL` | Each phase adds its own one-line dock entry when it lands (cheap, additive, no real conflict) — but the *final* layout/spacing/polish pass (P6.2-P6.3) is deferred to `M10`, after all five new panels exist, so polish happens once instead of five times. |

Everything else in both plans touches disjoint files or disjoint regions of
shared files (e.g. UI-Refactor's clip-drawing loop at `SequencerUI.cpp:466-484`
vs. live-PLAN's `PatternCompiler`, which never touches `SequencerUI.cpp` at
all) and needs no coordination beyond normal sequential commits.

---

## Milestone order

### M0 — Shared Audio Tap (new, synthesized; supersedes L4.1 and P3.1) — DONE
Build **one** fixed-size SPSC ring buffer (same `moodycamel`-style tool as
`AudioEvent`) that `AudioEngine::process` writes the post-mix master block
into every callback — additive, droppable-if-UI-falls-behind, never
blocking. This single tap later feeds: SCOPE's raw waveform (`M7`),
SPECTRUM's FFT input (`M6`), the particle visualizer's RMS onset detection
(`M8`), and per-track level meters (`M7`). Building it once up front means
`M6`-`M8` never wait on each other for this piece.
- **Files:** `AudioEngine.h`, `AudioEngine.cpp` (process() write only).
- **Acceptance:** tap fills every callback with no audio-thread allocation
  or blocking; a throwaway UI probe can read it back correctly.
- **Status:** Implemented as `AudioTap`/`AudioTapFrame` in `AudioEngine.h` —
  a fixed 8192-frame overwrite-style ring (release-store on write,
  acquire-load on read; masked power-of-two indexing, no allocation) rather
  than a literal `moodycamel::ReaderWriterQueue` instance, since every
  planned consumer (waveform, FFT window, RMS follower) needs to re-read the
  same rolling history each UI frame instead of draining discrete items —
  a consuming FIFO doesn't fit that access pattern. `AudioEngine::process()`
  writes the post-mix master frame via `m_masterTap.Write(...)` right after
  the hard clamp, immediately before the sample is handed to RtAudio.
  `AudioEngine::GetMasterTap()` exposes a read-only reference for `M6`-`M8`'s
  panels. Verified with a standalone throwaway probe (partial-fill,
  most-recent-window, and post-wraparound reads all matched expected
  content) and a full incremental build of `AudioSequencer` with no new
  warnings.

### M1 — UI-Refactor Phase 1: Theme & Chrome Pass — DONE
Pure cosmetic, zero dependencies, highest visible payoff for lowest risk.
Do this first so the app "looks like a DAW" immediately.
- **Files:** `main.cpp` (`SetupImGuiStyle`, transport bar), `SequencerUI.cpp`
  (track header gutter restyle — visual only, no data-model change).
- **Status:** `SetupImGuiStyle()` (`main.cpp`) now sets `FramePadding`
  (4,2), `ItemSpacing` (6,4), `FrameRounding`/`TabRounding` 2px/0px (buttons
  get the reference's subtle pill shape; windows/children/popups/tabs stay
  hard-edged), and a low-alpha white `Border`/`Separator` color with
  `WindowBorderSize`/`ChildBorderSize`/`FrameBorderSize`/`PopupBorderSize`/
  `TabBarBorderSize` all at 1px for the hairline dividers. The transport bar's
  old `"PLAY"`/`"STOP"` text button is now a 32x32 `InvisibleButton` with a
  hand-drawn triangle/square glyph (`ImDrawList::AddTriangleFilled`/
  `AddRectFilled`), followed by a monospace playhead readout, matching the
  reference's icon-first transport look. The monospace font is imgui's own
  bundled `misc/fonts/Cousine-Regular.ttf` — zero new third-party dependency
  — copied next to the executable by a new `CMakeLists.txt` post-build step
  (same pattern as `nmp.onnx`) and resolved at runtime via
  `GetMonoFontPath()` (exe-relative, cwd-relative fallback, same reasoning as
  `MelodyExtractor::GetModelPath()`); it's pushed only around the playhead
  text via `PushFont`/`PopFont`, not swapped in globally. `SequencerUI.cpp`'s
  track-header tab row (`BeginTabBar("Tracks")`) is re-skinned with
  monochrome `ImGuiCol_Tab*` colors — the add/close/select behavior
  underneath is untouched.
  - **Deviations from the plan text, and why:** Only Play/Stop got an icon
    glyph — Record doesn't exist as a feature anywhere in this codebase, and
    Loop transport is `M2`'s job (`L1`), so there was nothing real to skin
    yet for either. LOAD/SAVE/EXTRACT MELODY/IMPORT MIDI stayed as text
    buttons (they still pick up the new padding/rounding/border styling for
    free) rather than inventing hand-drawn glyphs for four unrelated file
    actions with no real icon font asset available — done at hand, four ad
    hoc squiggles would read as noise, not chrome. No icon font (Forkawesome/
    Material) was added via `FetchContent`; a self-contained `ImDrawList`
    glyph avoids a new network-fetched dependency for two icons and matches
    the plan's own "e.g." framing of that bullet. The tab row's "disclosure
    caret + Mix level sub-label" mini-row was left for `M3`/Phase 2: Arranger
    mode shows one track at a time (not stacked lanes), so there's no
    natural second line to put a waveform sliver into until the Phase 2 peak
    cache exists to feed it (`UI-Refactor.md` Phase 2 item 3 already plans
    this explicitly). Verified with a full incremental build (no new
    warnings under `/W4`) and a manual launch of `AudioSequencer.exe`
    (stayed running, no crash) to confirm the new chrome renders.

### M2 — live-PLAN Phase L1: Cycle-Based Loop Transport — DONE
Foundational primitive every later live-coding phase (`L2`-`L5`) assumes.
Touches the ruler once here; `M10` will build UI-Refactor's ruler strip
*around* what lands in this step rather than redoing the region.
- **Files:** `AudioData.h` (loop fields), `AudioEngine.cpp` (sample-accurate
  wrap in the existing block-split loop, ~line 212-213 — a different edit to
  `process()` than `M0`'s tap write, sequenced after it so both diffs stay
  clean), `AdxParser` (`LOOP=` key), `SequencerUI.cpp` (loop toggle +
  brackets on the ruler).
- **Status:** `SequencerState` (`AudioData.h`) gained
  `loopEnabled`/`loopStartBeat`/`loopEndBeat` atomics, and `AudioEvent`
  gained a `LoopChange` type carrying all three together (so the audio
  thread never observes an enabled loop with a stale bound from a prior
  event). `AudioEngine::process()`'s note/arp scheduling section (previously
  one pass over `[blockStartBeat, blockEndBeat)`) is now a `while` loop over
  sub-ranges: each iteration schedules against its own sub-block, and if
  `m_loopEnabled` and the sub-block would cross `m_loopEndBeat`, the
  sub-range is clipped to the wrap point, `m_currentSamplePosition` is reset
  to `m_loopStartBeat * samplesPerBeat`, and the loop continues with the
  remaining frames — so a wrap can span multiple sub-ranges within one
  callback if the loop region is shorter than one buffer, and can never
  skip or double-fire a note exactly at the seam. Per the plan, only
  scheduling is rewound; voices/delay/reverb already in flight are
  untouched and ring out naturally past the seam (nothing in the per-sample
  DSP loop below reads `m_currentSamplePosition` directly). `AdxParser`
  parses `LOOP=StartBeat,EndBeat` (`GLOBAL`) — presence of the key implies
  `loopEnabled = true`, matching the format's existing "absent key = default"
  discipline rather than inventing an extra enabled sub-field the plan's own
  key spec didn't call for; `SaveProject` emits the line only when enabled.
  `SequencerUI.cpp` adds a "Loop" checkbox next to Automation, plus a new
  14px ruler strip carved out above the note/clip grid (shrinks `gridSize`
  rather than overlapping it, so a bracket drag can never be mistaken for a
  note click at the top pitch row) holding two independently draggable,
  16th-note-snapped brackets with a shaded region between them — exactly
  the reserved area `M10`'s bar:beat ticks will build into later. Loop
  toggle/bracket edits dispatch `LoopChange` immediately (same
  store-then-enqueue pattern as the existing BPM slider); `main.cpp` mirrors
  the pattern for file load (`LoadProjectAndSync`) and hot-reload diffing
  (`CheckForHotReload`). Verified with a full build (no new warnings) and a
  standalone throwaway probe that reproduced just the sub-block wrap
  arithmetic outside the engine: a note re-triggered every 2-beat cycle
  across 10 wraps landed at exactly beat 0.0 every time (11 NoteOns for 10
  wraps — 1 initial + 1 per wrap — with no drift, skip, or double-fire at
  the seam).

### M3 — UI-Refactor Phase 2: Real Waveform Rendering for Clips — DONE
Independent of `M1`/`M2` — different `SequencerUI.cpp` region (clip-drawing
loop, not the ruler/header). Safe to run right after `M2`.
- **Files:** new peak-cache (keyed like `AudioClip::operator==`),
  `SequencerUI.cpp:466-484`.
- **Status:** New `ClipPeakCache.h`/`.cpp` module (registered in
  `CMakeLists.txt`) exposes `GetOrBuildClipPeaks(const AudioClip&)`, keyed by
  `filePath|pitchShiftSemitones|timeStretchFactor|reversed` — the same
  identity `AudioClip::operator==` uses, minus `startTimeSeconds` (moving a
  clip shouldn't invalidate its cached waveform shape). Each cache entry
  holds a mipmap-style stack of min/max tiers: tier 0 buckets the clip's
  ~4096 target columns worth of frames, and each further tier merges
  adjacent bucket pairs (min-of-mins/max-of-maxs) until only a handful of
  buckets remain — a handful of zoom levels built once, in roughly `O(2 ×
  frameCount)` work, rather than rescanning raw PCM every frame at every
  zoom. A cache hit whose `pcmData` pointer no longer matches what was cached
  (`AudioClipProcessor::ReprocessClip` re-decoding in place after a pitch/
  stretch edit) is detected via the stored source pointer and rebuilt
  automatically — no explicit invalidation hook was needed at any of the four
  clip-creation/reprocess call sites. `SequencerUI.cpp` gained a
  `DrawClipWaveform()` helper (drawn once for the full-size Arranger clip
  lane, once more for the track-tab sliver below) that picks the cache tier
  closest to on-screen pixel density and merges the 1-2 buckets covering each
  visible pixel column into a vertical tick — visible-range-only (intersected
  against the grid's scroll window), so a clip mostly scrolled off-screen
  doesn't walk its full width every frame. The clip-drawing loop
  (`SequencerUI.cpp`, `s_arrangerMode` branch) now paints a black clip body,
  the white waveform ticks, a gray filename title strip across the top, and
  keeps `COLOR_CLIP`/`COLOR_CLIP_HOVERED` as an outline plus a translucent
  hover tint and a white selection outline — a tint/outline over the
  waveform, per the plan, rather than the old solid-color fill swap. The
  track-tab "Mix level" sub-label the `M1` status note deferred to this phase
  is also in: a faint 4px sliver along each tab's bottom edge, drawn via
  `ImGui::GetItemRectMin/Max()` right after that track's `BeginTabItem` call,
  showing the track's first audio clip's envelope. Verified with a full
  clean rebuild (no new warnings) and a visual smoke test — a throwaway
  `.adx` project with two `CLIP` entries (`kick_A2.wav`, a pitch/time-
  stretched `kick_C3.wav`) loaded with Arranger mode forced on: both clips
  rendered as recognizable min/max envelopes (not flat blocks), each with its
  own title strip, and the tab sliver showed the same clip's transient shape
  at reduced scale — screenshotted, inspected, then the temporary
  force-Arranger-on edit and the throwaway project file were both reverted/
  removed.

### M4 — live-PLAN Phase L2: Mini-Notation Pattern Compiler — DONE
Entirely new files; zero collision with anything in the UI-Refactor track.
Only depends on `M2` for loop bounds to tile patterns across.
- **Files:** new `PatternCompiler.h/.cpp`, `AdxParser.cpp` (`PATTERN=` key).
- **Status:** New `PatternCompiler.h`/`.cpp` (registered in `CMakeLists.txt`)
  is a self-contained recursive-descent parser + flatten pass, matching the
  grammar in `live-PLAN.md` L2: space-separated steps, `~` rest, `[ ]`
  subdivision, `< >` alternation, `*n`/`/n` speed, `!`/`!n` replicate
  (bare `!` repeats the previous step; `token!n` expands to `n` copies),
  `@n` elongate (weighted division), `(k,n,r)` Euclidean rhythm (a
  Bresenham-bucket formula proven equivalent to Bjorklund's algorithm:
  `pattern[i] = floor(i*k/n) != floor((i-1)*k/n)`, rotated by `r`), `,`
  parallel stacks, and `?`/`?0.3` per-event probability driven by a seeded
  xorshift32 PRNG (same generator shape as `Voice::noiseRng`) — the seed
  defaults to an FNV-1a hash of the pattern text itself, so re-evaluating
  identical source is deterministic without the caller having to thread a
  seed through. Step resolution tries, in order: a scale degree (only when
  a trailing `scale=<root>_<mode>` directive is present and the token is a
  bare integer — 7 diatonic modes plus chromatic, root defaults to octave
  4), a fixed General-MIDI-ish drum shorthand table (`bd`/`sn`/`hh`/`oh`/
  `cp`/`cy`/`rd`/`lt`/`mt`/`ht`/`rim`/`perc`), then a bare note name
  (`c4`, `f#5`) reusing the same letter/octave model as
  `AdxParser::NoteNameToMidi` (duplicated locally since that method is
  private to `AdxParser` and the two parsers are otherwise independent);
  unresolvable tokens fall back to Middle C, matching
  `AdxParser::NoteNameToMidi`'s own default-on-failure discipline. Compiling
  always realizes `cycleIndex` 0 — `Compile()` takes `cycleIndex` as a
  parameter for forward compatibility, but L2 recompiles once per edit (not
  once per real playback cycle), so `<a b>` deterministically picks `a` and
  `/n` always fires at this phase; wiring a live cycle counter through so
  `<>`/`/n` actually vary per real loop repetition, and recompiling only at
  cycle boundaries, is explicitly `M11`/L5's job per the plan. Each compiled
  `Note` carries a parallel `StepSpan{sourceStart, sourceLen}` (byte offsets
  into the exact string passed to `Compile`) for `M7`'s playhead-synced step
  highlighting. `AdxParser.cpp` gained a `PATTERN=<mini-notation>` key inside
  `[TRACK]`: recorded into a `pendingPatterns` list during the parse pass
  (rather than compiled inline like `MIX=`/`SEND=`) and compiled only after
  the whole file is read, so a hand-authored file with `[TRACK]` before
  `[GLOBAL]` still sees the right `LOOP=` bounds; a file with no `LOOP=` at
  all falls back to a synthetic 4-beat span (`[0, 4)`) so `PATTERN=` still
  compiles to something audible rather than silently producing zero notes.
  Compiled notes are appended into the same `Track::notes` hand-authored
  `NOTE` lines already populate — both coexist, per the plan's "PATTERN
  augments, doesn't replace" framing — and a compile error is logged as an
  `AdxParser Warning` (never fatal to the load) rather than dropping the
  rest of the file. `SaveProject` deliberately does **not** gain a
  reciprocal `PATTERN=` writer in this milestone: `Track::notes` has no
  memory of which notes came from a compiled pattern versus a hand-authored
  line, so serializing would silently flatten every `PATTERN=` track into
  individual `NOTE` lines on the next save — a decision deferred to `M5`,
  where the live-code editor's own buffer (not `SequencerState`) becomes the
  authoritative on-disk text for `PATTERN=` tracks. Verified with a
  standalone throwaway probe (not part of the build) covering: the plan's
  own `"bd ~ bd ~ [~ bd] ~ bd(3,8,0) ~"` acceptance example (6 notes,
  hand-computed start/length/pitch matched exactly, including the
  Euclidean fill's three hits at beats 6.0/6.375/6.75); `"0 3 5 7
  scale=a_minor"` (A4/D5/F5/A5 — a root-position arpeggio through the
  octave); determinism (two compiles of identical `?0.5`-laden text
  produced byte-identical note lists); a combined `<bd sn> hh!3 cp@3`
  smoke test (5 notes, correct alternation/replicate/elongate weighting);
  and a deliberately unbalanced `"bd [sn"` correctly reported as a parse
  error instead of hanging or crashing. Also verified with a full clean
  rebuild of `AudioSequencer` (no new warnings beyond four pre-existing,
  unrelated `C4101` warnings in `AdxParser.cpp`'s older catch blocks, whose
  line numbers merely shifted).

### M5 — live-PLAN Phase L3: In-App Live-Code Editor Panel — DONE
Depends on `M4`. Adds one dock-layout line to `main.cpp` (additive, no
conflict with UI-Refactor's later dock additions).
- **Files:** new `LiveCodeEditor.h/.cpp`, `CMakeLists.txt` (FetchContent),
  `main.cpp` (`BuildDefaultDockLayout` + `AdxParser::SaveProject` hookup).
- **Status:** Implementation spike (per the plan's own instruction to
  decide before committing to a dependency) cloned `BalazsJako/
  ImGuiColorTextEdit` and compiled its `TextEditor.cpp` against this
  project's already-fetched ImGui docking source — it failed outright
  (`PushItemFlag`/`ImGuiItemFlags_NoTabStop` don't exist in this ImGui
  version; the library predates that API and is unmaintained), confirming
  the plan's own fallback: **a minimal hand-rolled editor built on
  `ImGui::InputTextMultiline`.** No `FetchContent` entry was added for it;
  the spike clone was deleted. New `LiveCodeEditor.h`/`.cpp` (registered in
  `CMakeLists.txt`) owns one `std::string` buffer that mirrors
  `g_loadedProjectPath` on disk — `SyncFromDisk()` is called from
  `LoadProjectAndSync` (LOAD button + command-line load), from "SAVE AS
  NEW", and from `CheckForHotReload` right after it applies an
  externally-changed file, so the panel and any external editor never
  silently diverge; it's a no-op while the panel has unsaved local edits,
  so an external change landing mid-edit can't clobber work in progress.
  Editing uses ImGui's own `misc/cpp/imgui_stdlib.h`/`.cpp` (already inside
  the fetched ImGui source tree, so this is imgui's own companion header,
  not a new third-party dependency — added to the `ImGui` CMake target
  alongside `Cousine-Regular.ttf`'s precedent from `M1`) for a
  `std::string`-backed `InputTextMultiline`, rather than a hand-managed
  `char*`/resize-callback buffer.
  - **Syntax highlighting deviation, and why:** the plan's phrase "manual
    syntax-color pre-pass" turned out to describe something ImGui's public
    API cannot do — `InputTextMultiline` owns its own internal glyph
    rendering with a single `ImGuiCol_Text` color and no per-glyph color
    hook, so a true inline overlay would mean reimplementing text-editing
    input handling from scratch (exactly what the abandoned ~2000-line
    `ImGuiColorTextEdit` is, which a "minimal hand-rolled editor" should not
    attempt to duplicate). Implemented instead as **live, per-line
    diagnostics** below the buffer (colored red, `Line N: <message>`,
    updated on every keystroke) rather than per-token inline color — this
    still satisfies "distinguish section headers / PATTERN= / comments at a
    glance" in spirit for the one case that actually matters functionally
    (spotting a broken `PATTERN=` line before evaluating it), without a
    fragile/impossible overlay-sync scheme.
  - **Evaluate model:** every keystroke re-scans the buffer for `[TRACK
    <name>]` / `PATTERN=` lines (a small local line-based scanner,
    deliberately not reusing `AdxParser`'s fuller grammar) and dry-runs
    `PatternCompiler::Compile` on each — cheap (UI-thread, main-thread-only
    parsing, matching the plan's architectural rule), so diagnostics are
    always current without waiting for an explicit action.
    **Ctrl+Enter** re-runs that same scan for real: each `PATTERN=` line is
    matched to an existing `Track` by `patchName` and its `.notes` are
    replaced wholesale (the live-coding model — a track carrying a
    `PATTERN=` line treats it as the single source of truth, so
    re-evaluating replaces what's playing rather than accumulating, same as
    Tidal/Strudel), then one `DispatchSequenceUpdate` ships every changed
    track together. A compile error or an unmatched track name (this panel
    edits existing tracks only, not authoring brand-new ones from scratch —
    a disclosed scope limit, not silently dropped) leaves that track's
    last-good notes untouched and is surfaced as a diagnostic instead of
    ever applying a partial/broken snapshot, mirroring
    `CheckForHotReload`'s existing guarantee.
  - **A real ImGui bug found and fixed during implementation:**
    `InputTextMultiline` inserts a newline on *any* Enter keypress,
    including Ctrl+Enter, since it has no built-in "evaluate shortcut"
    concept — left alone, every Ctrl+Enter would also splice a newline into
    the buffer at the cursor, silently corrupting whatever line the cursor
    was sitting on. Fixed with an `ImGuiInputTextFlags_CallbackAlways`
    callback that detects the just-inserted `'\n'` at the cursor and
    deletes it in the same frame. Verified interactively (see below) by
    placing the cursor mid-word and confirming Ctrl+Enter evaluates without
    splitting the word.
  - **`Ctrl+S` deviation from the plan text, and why:** the plan says
    "Ctrl+S additionally persists to disk through the existing
    `AdxParser::SaveProject`" — implemented instead as a direct write of
    this panel's own buffer text to `g_loadedProjectPath`. `SaveProject`
    only ever serializes `Track::notes` back out as individual `NOTE`
    lines; since `Track`/`SequenceSnapshot` has no memory of which notes
    came from a compiled `PATTERN=` line versus a hand-authored `NOTE`
    line, routing `Ctrl+S` through it would silently rewrite every
    `PATTERN=` track into flat `NOTE` lines on the very first save —
    destroying the live-coding source text and directly contradicting the
    plan's own stated goal ("external editors and the in-app panel both
    work against the same file — additive, not a fork of state"). Writing
    the buffer verbatim is what actually keeps that promise; no new
    dispatch call was needed for it either, since `CheckForHotReload`'s
    existing ~500ms poll (`main.cpp`) picks up the write and folds it into
    `state` on its own. The same reasoning is why `M4`'s `AdxParser.cpp`
    changes deliberately didn't add a reciprocal `PATTERN=` writer to
    `SaveProject`.
  - `BuildDefaultDockLayout` docks `"LIVE CODE"` into the same `bottom`
    node as `"SEQUENCER"` (tabbed together) rather than resplitting
    proportions — matching the plan's "next to SEQUENCER" and consistent
    with `INTERLEAVED-PLAN.md`'s own collision-point #3, which defers all
    layout/spacing polish to `M10` once every new panel exists.
  - Verified with a full clean rebuild (no new warnings from either new
    file) and an interactive smoke test: launched `AudioSequencer.exe`
    against a throwaway project with `LOOP=0,8` and one
    `PATTERN=bd ~ bd ~ [~ bd] ~ bd(3,8,0) ~` track, screenshotted the LIVE
    CODE panel showing the loaded text and "No pattern errors.", typed an
    unbalanced `[bd` suffix and watched a live diagnostic appear
    (`Line 14: TRACK Drum: expected ']'`) with the status flipping to
    "* unsaved", then — on a fresh launch — typed a recognizable marker
    token mid-pattern, positioned the cursor mid-word, and pressed
    Ctrl+Enter: the word stayed unbroken (proving the newline-strip fix)
    and the EXPORT panel's independently-computed "content length" readout
    grew from 3.4s to 4.0s, confirming the evaluate path really reached
    `state.tracks` end-to-end rather than just updating the buffer. No
    crashes across any of the four launches. Scratch project/screenshots
    removed afterward.

### M6 — UI-Refactor Phase 3: Master Output Tap + Spectrum Analyzer — DONE
The tap already exists (`M0`) — this phase is now just FFT + panel, so it's
unblocked by anything in the live-coding track.
- **Files:** new FFT header, `main.cpp` (`SPECTRUM` panel + dock line).
- **Status:** New `SimpleFFT.h`/`.cpp` (registered in `CMakeLists.txt`) is a
  ~90-line self-contained iterative radix-2 Cooley-Tukey FFT (bit-reversal
  permutation + butterfly passes, `std::complex<float>`, in place) — no
  third-party dependency, confirming the plan's own grep finding that none
  exists anywhere in the repo yet. `ApplyHannWindow` + `Transform` are
  exposed separately for reuse (`M9`'s waterfall needs the same log-
  magnitude bins); `ComputeLogMagnitudeSpectrum` composes both and maps the
  lower (Nyquist) half of bins from `20*log10(|bin|/N)` into a clamped
  `[0,1]` range assuming a -100..0 dB floor/ceiling, convenient for direct
  use as a plot Y-fraction. `main.cpp` gained `DrawSpectrumWindow`
  (alongside `DrawExportWindow` et al.): reads the last 2048 frames from
  `engine.GetMasterTap()` (mono-summed `(L+R)*0.5`), computes the log-
  magnitude spectrum every rendered frame, and applies a fast-attack/slow-
  release filter per bin across frames (0.6 attack / 0.15 release lerp
  rates) so the trace reads as the reference's smooth-but-jagged line
  rather than a jittery raw FFT, per the plan. Plotted as one
  `ImDrawList::AddPolyline` on a black background, X mapped through
  `log(bin)/log(numBins-1)` (skipping bin 0/DC) for a log-scaled frequency
  axis rather than linear, so low end isn't crushed into a handful of
  pixels. Docked into the same `right` node as `TRACK FX`/`MASTER FX`/
  `EXPORT` (tabbed) — an additive one-line dock entry, per
  `INTERLEAVED-PLAN.md`'s collision-point #3, which defers the eventual
  bottom-docked `SPECTRUM | VISUALIZER | WATERFALL` row to `M10` once all
  three panels exist. Verified three ways: (1) a standalone throwaway
  probe feeding a clean 1kHz test tone into `ComputeLogMagnitudeSpectrum`
  found its peak at bin 46 (990.5 Hz — within one FFT bin's ~21.5 Hz
  resolution of the true 1000 Hz) with magnitude 0.869 (a strong, clearly
  above-floor peak), confirmed pure silence produces a flat near-zero
  floor (max 0.0000) with no spurious peak, and confirmed the output size
  is exactly `N/2`; (2) a full clean rebuild with no new warnings from
  either new file; (3) an interactive test playing `suffocation.adx` (a
  real, ~80s multi-track project) end-to-end — two screenshots ~1.2s apart
  during playback (playhead 0005.50 -> 0006.97 beats) show a real, richly
  jagged bass-heavy trace whose shape visibly shifts between frames,
  confirming the tap -> FFT -> smoothing -> polyline pipeline is genuinely
  live and reactive to real audio, not a static placeholder. Screenshots
  removed afterward.

### M7 — live-PLAN Phase L4: Live Visualization — DONE
SCOPE panel and meters consume `M0`'s tap directly. Step-highlighting is the
one sub-feature genuinely gated on the live-coding track (`M4`'s
`PatternCompiler` span-tracking + `M5`'s editor panel), which is why this
lands after both.
- **Files:** `main.cpp` (`SCOPE` panel + dock line), `PatternCompiler`
  (step→source-column spans), `AudioEngine` (per-track peak-hold).
- **Status:** `PatternCompiler`'s `StepSpan` (source byte offsets per
  compiled `Note`) already existed from `M4` — this phase just consumes it.
  `main.cpp` gained `DrawScopeWindow` (a `"SCOPE"` dockable window,
  additive dock entry tabbed with `SPECTRUM`/`TRACK FX`/`MASTER FX`/
  `EXPORT` in `right`, same collision-point-#3 deferral as `M6`): a raw
  mono-summed oscilloscope trace (1024 tap samples, `AddPolyline` on
  black, centered on a mid-line) stacked above a coarse **bucketed**
  spectrum-bar view (24 log-spaced bars, each the peak magnitude across
  its bin range from `SimpleFFT::ComputeLogMagnitudeSpectrum`, its own
  independent fast-attack/slow-release smoothing) — deliberately coarser
  than `SPECTRUM`'s full ~1000-bin line per the plan's "lightweight
  bucketed-magnitude" wording, since this panel is about "is it making
  sound and roughly what kind," not frequency analysis. The tap-read +
  pad + mono-sum steps `SPECTRUM` (`M6`) already needed were pulled out
  into a shared `ReadMonoFromTap` helper so `SCOPE` (two more calls, one
  per sub-view) doesn't duplicate them a third time; `M6`'s `DrawSpectrumWindow`
  was refactored to use it too (behavior-preserving — same computation,
  just factored).
  `AudioEngine` gained a per-bus decaying peak-hold: `m_trackPeaks`
  (`std::array<std::atomic<float>, kMaxEngineTracks>`), written in
  `process()` at the exact post-effect, post-volume/pan point `MIX=`
  already applies (right after `l *= liveVolume * ...` / `r *= ...`),
  one-pole release (`0.9999` per-sample coefficient, a few hundred ms at
  `kEngineSampleRate`) so a UI frame reading it once sees a smoothly
  decaying value rather than near-silence between transients; exposed
  read-only via `GetTrackPeak(trackIndex)`. `SequencerUI.cpp`'s track-tab
  loop (`DrawSequencerUI`, now additionally taking `const AudioEngine&` —
  the one file-list addition beyond the plan's literal bullet, since "per-
  track meters next to each track" is structurally the track-tab code)
  draws a thin green/yellow/red fill bar along each tab's **top** edge
  (the bottom edge is already `M3`'s Mix-level waveform sliver), width
  scaled by that track's live peak.
  - **Step-highlighting deviation, and why:** same constraint already
    disclosed in `M5` — `InputTextMultiline` has no per-glyph color hook,
    so an inline underline inside the live buffer isn't achievable without
    reimplementing text-editing input handling. `LiveCodeEditor` instead
    gained a `CompiledPatternInfo` cache (notes + spans per successfully-
    compiled `PATTERN=` line, refreshed every frame alongside diagnostics —
    cheap, and it closes a real gap a `changed`-gated refresh would have
    left: a freshly-loaded project's patterns wouldn't compile for
    highlighting until the user edited something) and a status line below
    the editor: `state.playheadPositionBeats` is matched against each
    compiled `Note`'s `[startBeat, startBeat+lengthBeats)` range every
    frame, and the winning step's source text/line/beat is shown
    (`-> Line N, TRACK X: "step" @ beat B.BB`) — real, working, playhead-
    synced feedback, just as a readout rather than an overlay.
  - Verified interactively end-to-end: playing `suffocation.adx` (real
    ~80s multi-track content) showed a genuinely reactive, loud/clipped-
    looking oscilloscope trace and a sensible descending bass-to-treble
    bar chart in SCOPE; a zoomed 3x crop of the SEQUENCER track-tab row
    during playback showed live green meter bars of differing widths on
    the sustained bass/lead/pad tracks while the percussive kick/snare/hat
    tracks (correctly, since they're transient instruments caught between
    hits) showed none at that instant. A dedicated `LOOP=0,8`,
    `PATTERN="bd ~ bd ~ [~ bd] ~ bd(3,8,0) ~"` project at 90 BPM was played
    and screenshotted at three playhead positions: beat 1.15 (inside a rest
    slot) and beat 3.95 (another rest slot) both correctly showed
    "(no pattern step active)"; beat 2.54 (inside the pattern's second `bd`
    slot, `[2,3)`) correctly showed `-> Line 14, TRACK Drum: "bd" @ beat
    2.00` — an exact match against hand-computed expectations for all
    three. Also a full clean rebuild with no new warnings from any touched
    file. Screenshots and throwaway projects removed afterward.

### M8 — UI-Refactor Phase 4: Transient Particle Visualizer — DONE
Consumes `M0`'s tap for RMS/onset detection only — no FFT dependency, so it
could in principle run anywhere after `M0`; kept here to stay close to
UI-Refactor's own internal ordering.
- **Files:** new particle pool, `main.cpp` (`VISUALIZER` panel + dock line).
- **Status:** New `ParticleVisualizer.h`/`.cpp` (registered in
  `CMakeLists.txt`) is a self-contained namespace (`Update`/`Draw`) holding
  a fixed `std::array<Particle, 512>` pool (round-robin recycle cursor —
  spawning always succeeds, silently stealing the oldest slot if the pool
  is momentarily full, rather than growing or dropping the burst) and the
  onset-detection envelope-follower state. `Update(tap, dt)` reads the tap's
  latest 1024 frames (~23ms), mono-sums and computes RMS, and fires a burst
  when `rms > 0.02` (a noise-floor gate) **and** `rms > envelope * 1.5`
  (a 50% jump) — a plain asymmetric envelope follower (instant attack via
  `max(rms, ...)`, `0.9`-per-`Update`-call decay) exactly matching the
  plan's "simple, cheap, no FFT needed" framing. Burst size (8-40 particles)
  and speed scale with how far `rms` cleared the envelope. Each particle
  gets a randomized angle within ±50° of straight up, an
  `std::mt19937`-seeded random speed/lifetime (UI-thread cosmetic
  randomness — no need for the audio thread's xorshift/determinism
  discipline here), and integrates via `x/y += v*dt; vy += gravity*dt`
  each `Update` (a gentle downward pull for a fountain arc, not a straight
  jet), fading and dying at `age >= lifetime`. `Draw` maps each live
  particle's normalized `[0,1]x[0,1]` position (origin bottom-center) into
  the panel's rect and renders it via `AddCircleFilled`, colored by
  `lerp(purple, cyan, age/lifetime)` with alpha fading to 0 — the
  reference's purple-to-cyan gradient. `main.cpp` gained
  `DrawVisualizerWindow` (advances the simulation by `ImGui::GetIO().
  DeltaTime` every frame regardless of window visibility, draws only when
  the window is actually open) and a `"VISUALIZER"` dock entry, same
  additive/tabbed-in-`right` treatment as `M6`/`M7`.
  - Verified with a full clean rebuild (no new warnings) and two rounds of
    interactive testing. The first round (playing `suffocation.adx`)
    initially looked broken — a panel titled "VISUALIZER" appeared to be
    rendering SCOPE's oscilloscope/bars content — which turned out to be a
    **testing artifact, not a code bug**: `imgui.ini`, accumulated across
    every manual test session since `M1`, already had a persisted
    dockspace layout, so `main.cpp`'s `BuildDefaultDockLayout` (gated
    behind "only build if no dock node exists yet") never re-ran to place
    the newly-added `SPECTRUM`/`SCOPE`/`VISUALIZER` windows, and each fell
    back to ImGui's identical default floating position (confirmed via
    `imgui.ini`: all three at `Pos=60,60`), stacking three separate opaque
    windows pixel-for-pixel with only the topmost genuinely visible.
    Deleting `imgui.ini` and relaunching showed the real, correct
    behavior: all three properly tabbed into the `right` dock node exactly
    as declared. With that resolved, `suffocation.adx`'s own "Intro"
    section then showed a genuinely empty (correctly so) VISUALIZER panel
    — its oscilloscope trace is loud but *sustained* (pad/bass, no sharp
    attacks), so a peak-envelope onset detector correctly finds nothing to
    report there, which is expected behavior for this detection method,
    not a bug. To verify the actual mechanism unambiguously, a second,
    synthetic `LOOP=0,4`/`PATTERN="c2 ~ ~ ~"` project (one sharp, driven
    hit against three beats of true silence, 100 BPM) was played and
    sampled across a full loop: a frame at playhead 0000.24 (just after
    the hit re-fired on wrap) showed a fresh, tight purple burst; a frame
    at playhead 0000.78 (~0.54s later) showed the same particles now
    spread wide and risen (confirming the gravity-arc integration) and
    visibly shifted toward cyan (confirming the age-based color gradient).
    Screenshots and both throwaway projects removed afterward.

### M9 — UI-Refactor Phase 5: Spectrogram Waterfall — DONE
Depends on `M6`'s FFT log-magnitude bins.
- **Files:** ring buffer of bin history, `main.cpp` (`WATERFALL` panel +
  dock line).
- **Status:** New header-only `SpectrogramHistory.h` is a small
  fixed-capacity ring buffer of `std::vector<float>` bin frames (`Push`/
  `Get(ageFromNewest)`), reusing each slot's existing vector capacity after
  warm-up so steady-state pushes don't allocate — simple enough not to
  need a `.cpp`. `main.cpp` gained `DrawWaterfallWindow`: pushes one
  `SimpleFFT::ComputeLogMagnitudeSpectrum` frame (via the `M7`-added
  `ReadMonoFromTap` helper) only every 6th rendered frame (~10 pushes/sec
  at 60fps), so the 30-frame history spans ~3 seconds — "a few seconds,"
  per the plan — rather than a fraction of one. Drawn oldest-first (so
  newer, brighter traces paint over older ones) as `M` separate
  `AddPolyline` calls, no texture upload: each older trace is progressively
  dimmed (`lerp(1.0, 0.15, ageFraction)`) and offset upward
  (`ageFraction * canvasHeight * 0.35`), reproducing the reference's
  receding pseudo-3D layered-line look. Reuses `SPECTRUM`'s log-scaled
  frequency-axis mapping (skip bin 0/DC, `x = log(bin)/log(numBins-1)`) so
  the waterfall and the spectrum line agree on where frequencies land.
  Docked additively into `right`, same treatment as `M6`-`M8`.
  Verified with a full clean rebuild (no new warnings) and an interactive
  test playing `suffocation.adx`: the WATERFALL tab showed exactly the
  intended look — many overlaid jagged traces receding into the panel,
  bright/sharp for the current spectrum and fading toward faint gray
  lines for older history (including flat near-silent lines retained from
  before playback started, correctly dim/receded at the back of the
  stack) — matching the reference's "several overlaid frequency traces
  receding into the panel" description. Screenshot removed afterward.

### M10 — UI-Refactor Phase 6: Ruler, Layout Wiring & Polish — DONE
Now safe to do as **one consolidated pass**: add the bar:beat ruler strip
next to `M2`'s already-landed loop brackets (single coordinated
`SequencerUI.cpp` ruler edit instead of two independent ones), then do the
final `BuildDefaultDockLayout` arrangement/spacing pass now that all five
new panels (`LIVE CODE`, `SCOPE`, `SPECTRUM`, `VISUALIZER`, `WATERFALL`)
exist, then screenshot-compare against the reference images.
- **Files:** `SequencerUI.cpp` (ruler strip), `main.cpp` (layout polish).
- **Status:** `SequencerUI.cpp`'s ruler strip (widened 14px -> 18px to fit
  a small bar-number label alongside `M2`'s loop brackets in the same
  strip, same beat->pixel mapping, no second ruler pass) now draws a
  shrunk-font (`SetWindowFontScale(0.75)`) bar number at every bar line
  (`i % 4 == 0`, matching the existing bar/beat gridline split), plus a
  `"m:ss.s/bar"` running time readout (derived from `state.bpm` and
  `state.playheadPositionBeats`) in the gutter's top-left corner cell —
  matching the reference's `"0:08:38 / bar"` ruler-corner readout. The
  time readout needed its own small backing rect: the gutter's per-pitch-
  row key coloring (`--- Draw Gutter ---`, pre-existing code) starts its Y
  coordinate at `canvasPos.y` rather than `gridPos.y`, so the topmost
  piano-key row already extends up into the ruler-height corner —
  a pre-existing, harmless-until-now offset between the gutter's row
  coloring and the actual grid content it labels, left alone as out of
  scope for a ruler task, but it meant text drawn straight onto that
  corner had unpredictable contrast against whatever key color landed
  there; a small `AddRectFilled` behind the text fixes that locally.
  `BuildDefaultDockLayout` (`main.cpp`) now splits a bottom analysis row
  off the **full** dockspace first (`ImGuiDir_Down`, 22% height, before
  the left/center/right split), so it spans the entire window width —
  "a bottom-docked row spanning the window," per the plan — then splits
  that row into three equal thirds: `VISUALIZER` (particle fountain) |
  `SCOPE` tabbed with `SPECTRUM` (a combined "spectrum/waveform" slot —
  `UI-Refactor.md`'s Phase 6 wording predates `SCOPE` existing as its own
  panel, and `SCOPE`'s own content is literally a waveform plus a coarse
  spectrum, so tabbing them together is the natural reconciliation rather
  than inventing a fourth column) | `WATERFALL`, left to right, exactly
  mirroring the reference's bottom-strip ordering.
  - **Reference-screenshot comparison, and what actually happened:** the
    literal reference image files `UI-Refactor.md` describes were never
    saved into this repo (confirmed by a search for any `.png`/`.jpg`
    outside `build/`) — they existed only as images shown when that plan
    was written, not as on-disk assets this pass could diff against. The
    verification that follows is therefore against `UI-Refactor.md`'s own
    written description (black/white FL Studio arrangement, icon
    transport, white-on-black waveform lanes, live spectrum line,
    particle transient visualizer, receding-line waterfall — all
    individually verified in `M1`/`M3`/`M6`-`M9`), not literal pixels.
  - Verified with a full clean rebuild (no new warnings) and iterative
    interactive screenshotting: the first pass caught the corner-contrast
    issue above via a zoomed 3x crop of the ruler strip (before the fix,
    the time text was invisible, painted over by the gutter's later
    black background fill at the time it was drawn earlier in the
    function — moved to after `--- Draw Gutter ---` and given a backing
    rect, then re-verified legible). A final full-window screenshot
    playing `suffocation.adx` in Arranger mode shows the complete,
    cohesive layout: bar numbers and a live time/bar readout in the
    ruler, and the bottom row showing a live particle scatter
    (VISUALIZER), a reactive waveform trace (SCOPE, tabbed with
    SPECTRUM), and a receding multi-line spectrogram (WATERFALL) all
    spanning the window's full width side by side. Screenshots removed
    afterward.

### M11 — live-PLAN Phase L5 (stretch/optional): Pattern-Transform Vocabulary — DONE
Purely additive to `PatternCompiler`; no UI-Refactor collision at all. Left
last since both source plans mark it optional.
- **Files:** `PatternCompiler.h/.cpp`.
- **Status:** `PatternCompiler.h`/`.cpp` gained pipe-suffix transform syntax:
  `patternText` is now split on every top-level `|` (safe unconditionally —
  `|` never appears anywhere in the mini-notation grammar itself, so no
  bracket-nesting tracking is needed); stage 0 is the mini-notation body
  (still honoring L2's legacy trailing `" scale=<name>"` form), and stages
  1.. are each one of `scale=<name>` (a pipe-form alternative to the
  trailing one — resolved before flatten, same as always, since it's a
  step-resolution directive, not a post-hoc transform), `rev` (unconditional
  time-mirror of the compiled cycle), `fast <n>` (tiles the whole cycle
  `round(n)` times into the same span, clamped to `[1, 64]` so a typo like
  `fast 999999` can't blow up memory — using the *rounded* tile count for
  both the divisor and the loop count, rather than the raw possibly-
  fractional `n`, so tiles always exactly cover the cycle with no gap or
  overflow), and `every <n> rev` (applies `rev` only when
  `cycleIndex % n == 0`). An unrecognized stage is a compile error (`"unknown
  pattern transform: '...'"`), not a silently-skipped key — pipe stages are
  new, deliberately-typed syntax, unlike `.adx`'s "unrecognized keys are
  skipped" discipline. `rev`/`fast` keep `notes` and `spans` in lockstep
  (same 1:1 note correspondence, just reordered/retimed), so `M7`'s
  step-highlighting still resolves correctly against a transformed pattern.
  - **Wiring `every N` to real playback cycles (the part the plan flagged
    as needing "AudioEngine's live loop-cycle counter"):** rather than add
    a new counter to `AudioEngine` (which `M11`'s own file list doesn't
    include, and which would mean a second, redundant way of tracking
    "has the loop wrapped" alongside the sample-accurate one `AudioEngine`
    already has internally for scheduling), `LiveCodeEditor` gained a
    `CheckCycleBoundary()` UI-thread heuristic: `state.playheadPositionBeats`
    already wraps back down to `loopStartBeat` sample-accurately every
    cycle (`M2`), so a same-frame *decrease* in that value, observed from
    the UI thread, is itself proof a wrap just fired — no engine-side
    changes needed for a UI-thread cosmetic/live-coding feature that was
    never meant to run per-sample anyway. Resets to cycle 0 whenever
    playback (re)starts (an `isPlaying` transition), so "every 4" counts
    from the start of the current play session rather than some arbitrary
    wall-clock-since-launch offset; on each detected wrap, only PATTERN=
    lines that textually mention `"every"` are recompiled+reapplied (a
    cheap pre-filter, since a pattern without a cycle-dependent transform
    compiles identically at any `cycleIndex`) via the same
    match-by-`patchName`/replace-notes/`DispatchSequenceUpdate` path
    `CompileAndApply` (`M5`) already uses. `RescanDiagnostics` and
    `CompileAndApply` were both updated to compile at the live
    `m_cycleIndex` too (previously always 0), so the step-highlight readout
    and an explicit Ctrl+Enter evaluate both agree with whatever
    `CheckCycleBoundary` actually applied. This is a disclosed deviation
    from the plan's literal file list (`LiveCodeEditor.h/.cpp` beyond just
    `PatternCompiler.h/.cpp`) for the same reason `M7`'s step-highlighting
    landed there: it's where the per-frame PATTERN= scanning already lived.
  - Verified three ways: (1) a standalone throwaway probe --
    `"bd sn" | every 4 rev"`-equivalent cases at `cycleIndex` 0, 1, and 4
    matched hand-computed expectations exactly (reversed at 0 and 4, not at
    1); `"0 3 5 7 | scale=a_minor | fast 2"` produced exactly the expected
    8 notes (A4/D5/F5/A5 twice, each half-length, correctly tiled into
    `[0,2)` and `[2,4)`); an unknown stage (`"wobble 3"`) correctly errored
    instead of silently compiling; standalone `rev` (no `every`) applied
    unconditionally regardless of `cycleIndex`. (2) A full clean rebuild
    (no new warnings beyond the four pre-existing, unrelated ones already
    noted in `M4`). (3) A live interactive test: a `LOOP=0,2`,
    `PATTERN=bd sn | every 2 rev` project at 90 BPM was played, and the
    LIVE CODE panel's step-highlight readout was sampled across three real
    loop cycles -- cycle 0 showed `"sn" @ beat 0.00` (reversed, since
    `0 % 2 == 0`), the next cycle showed `"bd" @ beat 0.00` (back to
    normal order, `1 % 2 != 0`), and the cycle after that showed `"sn" @
    beat 0.00` again (`2 % 2 == 0`) -- a clean, correct alternation driven
    entirely by real playback, not just by recompiling the text. Screenshots
    and the throwaway project removed afterward.

---

## Dependency graph (within-track edges only; cross-track edges are the
## collision points already resolved above)

```
M0 (shared tap)
 ├─> M6 (spectrum) ─> M9 (waterfall)
 ├─> M7 (scope/meters)
 └─> M8 (particles)

M1 (theme) ─ independent

M2 (L1 loop) ─> M4 (L2 pattern) ─> M5 (L3 editor) ─> M7 (L4 viz, step-highlight)
                                                    └─> M11 (L5 transforms)
M2 ─────────────────────────────────────────────────> M10 (ruler merge)

M3 (waveform clips) ─ independent, after M1/M2 only for file-churn hygiene

M10 ─ waits on M5, M6, M7, M8, M9 (needs all panels to exist for the polish pass)
```

## Practical takeaway

Alternate commits roughly in the `M0…M11` order above. Every milestone
except `M10` can be worked, tested, and merged without waiting on the other
plan's progress — `M10` is the single deliberate sync point, and it's
designed to land last precisely so neither plan is blocked on it before
then.
