# Project Implementation Plan: adX Live Coding Editor

**Project Context:** `plan.md` (native-synthesis reconstruction, Phases 1-5) is
the engine-capability arc. This plan is a **parallel, editor/workflow arc**:
turn adX's existing text-format + hot-reload loop into an actual live-coding
instrument — RondoCode-style mini-notation syntax, an in-app code editor,
seamless cycle looping, and live visualization — **on top of** the current
engine, not instead of it.

**What "RondoCode-style" means here (researched, not assumed):** RondoCode
(rondocode.com, MIT-licensed, github.com/vijaypemmaraju/rondocode) is a
browser-based live-coding environment "heavily inspired by Strudel" that
combines two things: (1) **synths** — DSP graphs authored as code
(`synth(({note, gate, adsr, saw, ladder}) => ...)`), and (2) **patterns** —
Tidal/Strudel-lineage **mini-notation** sequencing those synths
(`n("0 0 3 5 0 0 7 5").scale("a minor").sound("acid").every(4, x => x.rev())`).
It also ships a terser bracket-free "rondo" language that round-trips with the
JS form, inline draggable widgets (knobs/envelopes/step-grids) laid directly
over the source, an audio-driven WGSL shader visualizer (`visual()`), and a
cycle-based scheduler so patterns loop indefinitely until edited. Full
research notes and sources at the bottom of this file.

**Key finding: adX already has half of this.** `main.cpp`'s
`CheckForHotReload` polls the loaded `.adx` file and — per its own comment —
implements *"Strudel-style live coding: edit the file in a text editor, save,
hear it update without stopping playback."* `AutomationLane`/`Breakpoint`
(plan.md Phase 2) is already a real-time-safe, atomic, per-block-evaluated
modulation-signal system — the same role RondoCode's `.ctrl()` streams play.
`DispatchSequenceUpdate` already ships a full tracks+patches snapshot to the
audio thread lock-free on every edit. **This plan is a compiler front-end and
an editor UI targeting that existing pipeline** — it does not touch the
real-time voice/mixing code except where noted (loop transport, a read-only
scope tap).

**Architectural strictness (unchanged, still binding — see `plan.md`):**
1. **The Audio Thread is Sacred:** no `std::mutex`, no allocation, no file
   I/O, no string work inside `AudioEngine::process`.
2. **Lock-Free Communication:** all Main→Audio state changes go through the
   existing `moodycamel::ReaderWriterQueue<AudioEvent>`.
3. **New rule for this plan:** all pattern/DSL parsing — mini-notation,
   `.adxl` text, syntax highlighting, diagnostics — happens on the **Main
   Thread only**, exactly like `AdxParser::LoadProject` today. The compiler
   never runs per-sample or per-block; it runs once per edit and hands the
   audio thread a finished `SequenceSnapshot`, same as now.

---

## Scope decision: what we are *not* copying from RondoCode

RondoCode's headline feature is letting users author arbitrary **DSP graphs**
in code (`saw(f).mix(square(f/2), 0.3)` piped through a `ladder` filter). adX's
`Patch` (`AudioData.h`) is a **fixed-parameter** synth model — additive
harmonic stack + polyBLEP unison osc + noise + resonant filter + formant bank,
each with dedicated struct fields (`RESFILTER=`, `FORMANT=`, `OSC=`, etc. in
`AdxParser`). Exposing those fields through a friendlier text syntax is
in-scope (Phase L2/L3 below); letting users *compose new signal graphs from
primitives* is a rewrite of `AudioEngine`'s voice architecture and is **out of
scope** for this plan. Also out of scope, and why:

- **No WGSL/GPU shader visuals, no phone/touch-first design, no MCP/agentic
  authoring** — real RondoCode features, none requested, none needed to hit
  "live visualization + looping + RondoCode-like syntax."
- **No embedded JS/scripting engine.** Everything stays a small hand-rolled
  recursive-descent parser in C++, matching `AdxParser`'s existing style and
  the "Main-Thread-only" parsing rule above. This is the single biggest
  simplification versus RondoCode's actual architecture (which is a
  TypeScript monorepo with a full lexer/parser/codegen/decompiler package)
  and is the right trade for a single-user native app rather than a
  browser-hosted, phone-first product.

---

## Phase L1: Cycle-Based Loop Transport
**Goal:** Give the engine a native, sample-accurate loop region — the
foundational primitive every live-coding tool assumes (a pattern repeats
every "cycle" until you change it; today adX's playhead runs forward once and
resets to 0 only on Stop).

1. **Data:** Add `std::atomic<float> loopStartBeat{0.0f}, loopEndBeat{4.0f};
   std::atomic<bool> loopEnabled{false};` to `SequencerState` (`AudioData.h`).
2. **Engine (`AudioEngine.cpp::process`):** the block-scheduling loop already
   splits each callback into `[blockStartBeat, blockEndBeat)` to schedule
   notes/automation correctly at buffer boundaries (~line 212-213). Extend
   that same split: if `loopEnabled` and the block crosses `loopEndBeat`,
   process up to the wrap sample-accurately, reset `m_currentSamplePosition`
   to `loopStartBeat * samplesPerBeat`, and continue processing the
   remainder of the callback from the new position — so a wrap can never
   skip or double-fire a note/automation event exactly at the seam.
3. **Engine:** wrapping only rewinds *scheduling* (what new `NoteOn`s fire).
   Already-active voices, delay lines, and the reverb tail are untouched by
   the wrap and ring out naturally — this is what makes the loop sound
   seamless instead of hard-cut.
4. **Parser:** `LOOP=StartBeat,EndBeat` (`GLOBAL` scope) in `.adx`.
5. **UI (`SequencerUI.cpp`):** loop toggle + draggable loop-region brackets on
   the timeline ruler, built on the same ruler-drawing code Phase 2's markers
   already added.
6. **Acceptance:** a 4-beat pattern loops indefinitely with no click or gap
   at the seam; a note released just before the loop end is still audibly
   decaying (reverb tail) into the next cycle's downbeat.

---

## Phase L2: Mini-Notation Pattern Compiler
**Goal:** The actual "RondoCode-like syntax" deliverable — a Tidal/Strudel-
lineage mini-notation parser that compiles a pattern string into the
`Note`s adX's engine already knows how to play, so nothing downstream (voice
allocation, automation, export) has to change.

1. **New module:** `include/PatternCompiler.h` / `src/PatternCompiler.cpp`.
   Grammar (standard Tidal/Strudel mini-notation, scoped to what a single
   track needs):
   - space-separated steps fill one cycle equally: `c4 e4 g4 c5`
   - `~` rest
   - `[ ]` subdivision (nested groups share their parent step's duration)
   - `< >` alternation (one element per **cycle**, not per step)
   - `*n` / `/n` speed up / slow down a step or group
   - `!n` replicate without speeding up; `@n` elongate a step's duration
   - `(k,n,r)` Euclidean rhythm (`bd(3,8,0)` = 3 hits over 8 steps)
   - `,` stacks parallel layers (chords) within one step
   - `?` / `?0.3` per-event probability, driven by a **seeded** xorshift PRNG
     (same generator style as `Voice::noiseRng`) so re-evaluating the same
     source text with the same seed is deterministic — required for hot
     reload and offline export to agree.
2. **Step resolution:** a step is either a bare note name (`c4`, `a3`,
   reusing the engine's existing MIDI-pitch model), a scale degree consumed
   by a trailing `scale=<name>` directive, or a patch-local token name
   resolved against the track's `Patch` registry (drum-hit shorthand).
3. **Output:** `std::vector<Note>` (`startBeat`/`lengthBeats`/`pitch`/
   `velocity`) tiled across `[loopStartBeat, loopEndBeat)` from Phase L1 —
   this plugs directly into `Track::notes`. No engine or `AudioData.h`
   changes needed beyond Phase L1's loop fields.
4. **Parser:** extend `AdxParser` with a `PATTERN=<mini-notation string>` key
   inside `[TRACK]`, compiled into `Track::notes` at load and at every
   hot-reload — same "unrecognized keys are skipped" fallback discipline as
   `plan.md`'s Phase 1-5 keys, so hand-authored `NOTE` lines keep working
   for anything the mini-notation can't express yet.
5. **Acceptance:** a drum track's `PATTERN="bd ~ bd ~ [~ bd] ~ bd(3,8,0) ~"`
   compiles to the expected `Note` vector (verified by a small unit test
   comparing compiled beats/pitches against hand-computed values) and is
   audibly a four-on-the-floor groove with an Euclidean fill.

---

## Phase L3: In-App Live-Code Editor Panel
**Goal:** Close the loop entirely inside adX — type, `Ctrl+Enter`, hear it —
instead of alt-tabbing to an external text editor and saving, which is what
today's file-watch hot reload requires.

1. **Dependency:** vendor an ImGui-compatible multi-line text-edit widget via
   `FetchContent` (matching every existing dependency in `CMakeLists.txt`) —
   candidate: `BalazsJako/ImGuiColorTextEdit`. If it proves unmaintained or
   awkward to integrate, fall back to a minimal hand-rolled editor built on
   `ImGui::InputTextMultiline` with a manual syntax-color pre-pass; decide
   during a short implementation spike before committing to the dependency.
2. **New module:** `include/LiveCodeEditor.h` / `src/LiveCodeEditor.cpp`.
   Owns the in-memory buffer, mirrored to/from the watched `.adx` file on
   disk (so external editors and the in-app panel both work against the same
   file — additive, not a fork of state). Syntax highlighting covers section
   headers, `PATTERN=` mini-notation operators, and comments.
3. **Evaluate model** (standard live-coding convention, Tidal/Strudel/
   FoxDot): **Ctrl+Enter** compiles the buffer and dispatches immediately via
   `PatternCompiler → SequenceSnapshot → DispatchSequenceUpdate` — the exact
   path Phase L2 built, no new IPC. **Ctrl+S** additionally persists to disk
   through the existing `AdxParser::SaveProject`, which keeps the file-watch
   hot-reload path (and any external editor) in sync for free.
4. **Failure handling:** a parse error must never apply a partial/broken
   snapshot — on failure, keep the last-good state playing and surface the
   error inline at its source line, mirroring `CheckForHotReload`'s existing
   "keep previous state on parse failure" guarantee for malformed `.adx`.
5. **UI:** dock a "LIVE CODE" panel into `BuildDefaultDockLayout`, next to
   SEQUENCER.
6. **Acceptance:** editing a `PATTERN=` line and pressing Ctrl+Enter audibly
   changes the sound within one loop cycle, with zero dropouts, while
   transport keeps running and other tracks are unaffected.

---

## Phase L4: Live Visualization
**Goal:** RondoCode's "the code is playing" feedback — see the cycle
position and the sound, not just hear it. Native ImGui equivalents of
RondoCode's oscilloscope-style feedback and step-highlighting, no shader
language needed.

1. **Engine:** add a small lock-free SPSC ring buffer (same
   `moodycamel`-style tool already used for `AudioEvent`) that `process()`
   pushes the mixed master-output block into every callback — a read-only
   tap, allocated once up front, never blocking the audio thread.
2. **UI — "SCOPE" panel:** oscilloscope (raw waveform, drawn via
   `ImDrawList`, no extra dependency) plus a lightweight bucketed-magnitude
   spectrum bar view.
3. **UI — playhead-synced pattern highlighting:** the LIVE CODE panel
   underlines the mini-notation step currently sounding. Requires
   `PatternCompiler` to retain a step → source-column span alongside each
   compiled `Note`, then map `state.playheadPositionBeats` back to the
   active step each frame.
4. **UI — per-track level meters:** cheap atomic peak-hold updated at the
   same per-bus mix point `MIX=Volume,Pan` already applies (`plan.md` Phase
   1), read by the UI thread for decaying meter bars next to each track.
5. **Acceptance:** playing a pattern visibly highlights the active step in
   sync with what's audible; the scope shows a stable, correctly-scaled
   waveform for a sustained tone; meters visibly respond to note velocity.

---

## Phase L5 (stretch, optional): Pattern-Transform Vocabulary
**Goal:** A small chainable transform layer echoing RondoCode/Strudel's
`.every()`, `.rev()`, `.fast()/.slow()`, `.scale()` — without embedding a
scripting language.

1. **Parser:** pipe-suffix syntax on `PATTERN=`, e.g.
   `PATTERN="bd sn" | every 4 rev` or `PATTERN="0 3 5 7 | scale=a_minor | fast 2"`.
2. **Compiler:** each transform is a pure `vector<Note> -> vector<Note>`
   function over one compiled cycle. `every N` requires `PatternCompiler` to
   be called per-cycle-index (using Phase L1's loop transport to know the
   current cycle count) and recompile only at cycle boundaries — not
   per-sample, preserving the Main-Thread-only parsing rule.
3. **Acceptance:** `PATTERN="bd sn" | every 4 rev` audibly reverses step
   order every 4th loop; `fast 2` audibly doubles pattern speed without
   changing pitch.

---

## Extended Format Reference (new keys this plan introduces)

| Key | Scope | Phase | Meaning |
|---|---|---|---|
| `LOOP=StartBeat,EndBeat` | `GLOBAL` | L1 | Sample-accurate loop/cycle region |
| `PATTERN=<mini-notation>` | `TRACK` | L2 | Compiles to `Track::notes`; coexists with hand-authored `NOTE` lines |
| `PATTERN="..." \| every N rev \| fast n \| scale=name` | `TRACK` | L5 | Chainable per-cycle pattern transforms |

Unrecognized keys/sections must be skipped (not fatal), same discipline as
`plan.md`'s extended-format table, so existing `.adx` projects keep loading
unchanged through every phase of this plan.

---

## Non-goals (explicit)

- Arbitrary DSP-graph authoring (RondoCode's `synth(...)` closures) — would
  require rearchitecting `AudioEngine`'s fixed-parameter `Voice`/`Patch`
  model; not attempted here.
- WGSL/GPU shader visuals, phone/touch-first input, MCP/agentic composition
  workflows — real RondoCode features, not requested, not needed for the
  "syntax + live visualization + looping" ask this plan targets.
- No embedded scripting language (JS or otherwise); the mini-notation
  compiler is a small hand-rolled C++ parser, consistent with `AdxParser`.

---

## Research Notes & Sources

RondoCode (MIT-licensed, TypeScript monorepo: `@rondocode/pattern` — pure
pattern engine/scheduler; `@rondocode/engine` — AudioWorklet DSP/offline
render; `@rondocode/rondo` — lexer/parser/codegen for the terse "rondo"
language with full JS round-trip; `@rondocode/app` — CodeMirror editor +
inline widgets; `@rondocode/server` — MCP/CLI tooling) is "heavily inspired
by Strudel," which is itself a JavaScript/browser port of TidalCycles' pattern
language. Its mini-notation, cycle-based looping semantics, and pattern
combinators (`.every()`, `.rev()`, `.fast()/.slow()`, `.scale()`) are the
direct model for Phases L2/L5 above; TidalCycles/Strudel is the original
source of that notation (RondoCode itself credits it), so Phase L2's grammar
is cross-checked against Strudel's own mini-notation docs, not RondoCode
alone.

- [rondocode — introducing the platform (Threads)](https://www.threads.com/@hi.im.vijay/post/DbBP1LdgTAp)
- [rondocode is now open source, MIT-licensed (Threads)](https://www.threads.com/@hi.im.vijay/post/DbCX1IajTvJ/rondocode-is-now-open-source-and-mit-licensed-feel-free-to-contribute/)
- [rondocode.com](https://rondocode.com/)
- [github.com/vijaypemmaraju/rondocode](https://github.com/vijaypemmaraju/rondocode) (README, monorepo layout, `packages/app/src/examples/index.ts`)
- [Strudel — mini-notation reference](https://strudel.cc/learn/mini-notation/)
- [Strudel REPL](https://strudel.cc/)
- [awesome-live-coding-music](https://github.com/pjagielski/awesome-live-coding-music)
- [awesome-livecoding (TOPLAP)](https://github.com/toplap/awesome-livecoding)
