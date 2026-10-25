# Phase 7 — Text-first layer · M

| | |
|---|---|
| **Status** | Built 2026-10-21: every automated §7 box observed locally on Debug, RelWithDebInfo and Release; the human gate (§7 box 3, P7-1) closed 2026-10-25. CI (P7-0) is open: the first push failed and was fixed (§11, 17-19) - see plans/STATE.md |
| **Governs** | bidirectional GUI↔text sync, filesystem hot reload, the editor panel, mini-notation, the Python scripting API |
| **FINAL_PLAN refs** | §1 (the second half — *the reason the project exists*), §3.1 (PatternCompiler), §3.2 (hot reload, live-code editor), §5.8, §7 Phase 7 |
| **Entry criteria** | [phase_6.md](phase_6.md) §6 complete |
| **Next** | [phase_8.md](phase_8.md) |
| **§5 coverage owned** | §5.8 all except `adx render` (Phase 8) — bidirectional sync, hot reload, in-app editor, mini-notation live, scripting API, console |

---

## 1. Objective

Deliver the half of adX that no other DAW has.

FINAL_PLAN §1 is explicit that this is the point of the project:

> No DAW does the second half properly. That is the reason for adX to exist. The
> first half is the price of admission.

Phases 2 through 6 built the price of admission. This phase builds the reason.

The gate is a genuinely demanding one and is worth restating up front:

> **Done when** a project can be edited in the GUI and an external editor
> simultaneously, with changes flowing both ways and no dropouts on reload.

"Simultaneously" and "no dropouts" are the hard words. A reload that stops
playback, cuts voices, or clobbers the user's other window is a failure of this
phase even if the file round-trips perfectly.

---

## 2. Deliverables — exact file manifest

```
engine/format/adx/
  Patch.h/.cpp            Document diff -> command list (§4.2)
  Incremental.h/.cpp      reparse only the sections a text edit touched
  Tokenize.h/.cpp         token stream + semantic kinds for the highlighter

engine/format/mininotation/
  PatternCompiler.h/.cpp  PORTED from _archive (762 lines) — §4.5
  MiniDiagnostics.h       parse errors with column, reusing Phase 2 Diagnostics

engine/sync/
  TextSync.h/.cpp         the bidirectional coordinator (§4.1)
  FileWatcher.h/.cpp      ReadDirectoryChangesW + debounce + atomic-write handling
  ReloadPolicy.h          what a reload may and may not disturb (§4.4)

engine/script/
  ScriptApi.h/.cpp        the curated command surface exposed to Python

bindings/
  sync.cpp                watch/unwatch, sync mode, conflict signals
  mini.cpp                compile, diagnostics, step spans
  script.cpp              the scripting surface

app/adx/panels/editor/
  panel.py                the code editor dock
  highlighter.py          QSyntaxHighlighter over engine tokens
  diagnostics.py          inline squiggles, gutter marks, problem list
  stepmarks.py            playhead-synced step highlighting
  completion.py           key/section/param-path completion from ParamRegistry

app/adx/scripting/
  console.py              embedded REPL dock
  api.py                  the user-facing `adx` scripting module
  examples/               documented scripts that are also tests

docs/
  scripting.md            the scripting API reference
  mininotation.md         the mini-notation grammar (extracted from the archived header)

tests/cpp/sync/           test_patch, test_incremental, test_watcher
tests/cpp/mini/           test_compiler (the ported test corpus)
tests/python/             test_bidirectional.py, test_hot_reload.py,
                          test_editor_panel.py, test_scripting.py
```

---

## 3. The three sync directions

There are three, not two, and conflating them is how this goes wrong:

| Direction | Trigger | Path |
|---|---|---|
| **Model → text** | any command executes | `CommandStack::revision()` changes → `Writer` produces canonical text → editor buffer updated → file written (debounced) |
| **Text (in-app) → model** | typing in the editor panel | incremental reparse → `Document` diff → command list → `CommandStack::execute` |
| **Text (external) → model** | filesystem watcher fires | full reparse → `Document` diff → command list → `CommandStack::execute` |

All three converge on the same pivot — Phase 2's `Document` — and all three end
in **commands**, never in direct model mutation. That is what guarantees undo
keeps working, that the GUI updates, and that a text edit and a knob turn are
genuinely the same operation rather than two code paths that agree by luck.

---

## 4. Design

### 4.1 `TextSync`

```cpp
enum class SyncMode : uint8_t {
    Off,          // text is written on save only
    OnSave,       // default: model->text on save; text->model on external change
    Live,         // model->text debounced ~300 ms; text->model on every edit
};

class TextSync {
public:
    void attach(Project&, CommandStack&, std::filesystem::path);
    void setMode(SyncMode);

    // Main thread. Called when the editor buffer changes.
    ApplyResult applyText(std::string_view newText);

    // Called by FileWatcher. Returns Conflict if the model has unsaved changes
    // that the incoming text does not contain.
    ApplyResult applyExternal(std::string_view newText);

    // Called on CommandStack::revision() change.
    std::string renderText() const;

    Signal<ConflictInfo> onConflict;
    Signal<DiagnosticList> onDiagnostics;
};
```

**Loop prevention** is the first thing to get right. Writing the file on a model
change fires the watcher, which parses the file, which produces commands, which
change the model, which writes the file. Three guards, all needed:

1. **Origin tagging.** Every write records `(path, contentHash, timestamp)` in a
   recent-writes set; the watcher ignores an event whose file content hashes to
   a recent self-write.
2. **No-op diffing.** A diff producing zero commands produces zero revision
   change, so the loop terminates even if guard 1 misses.
3. **A re-entrancy flag** on `TextSync`, asserted rather than silently
   tolerated — if it ever trips, that is a bug to fix, not a condition to absorb.

### 4.2 `Patch` — diff to commands

The single most important algorithm in this phase, because it is what makes
reload *patch* rather than *rebuild* (FINAL_PLAN §3.2: *"make reload
diff-and-patch rather than rebuild-and-replace so playing voices are not cut"*).

```cpp
struct DocumentDiff {
    std::vector<SectionChange> added, removed, modified;
};

// Produces the minimal command list that transforms `from` into `to`.
std::vector<std::unique_ptr<Command>> computePatch(const Document& from,
                                                   const Document& to,
                                                   const Project& current);
```

Algorithm:

1. **Match sections by identity, not position.** A section's identity is its type
   plus its name (`[CHANNEL Lead]`). Moving a section in the file is a no-op, not
   a delete-plus-add — this matters because reordering sections in a text editor
   must not restart every voice.
2. **Within a matched section, match lines by content hash**, then align the
   unmatched remainder by position (a Myers diff over line hashes). Notes match
   by `(start, pitch)` so an edited velocity is a modify, not a delete+add.
3. **Emit the narrowest command** for each change. A changed `PARAM` line is
   `SetParam`, not `ReplaceChannel`. A changed note velocity is `SetNoteVelocity`.
4. **Group** the whole patch in one `beginGroup`/`endGroup`, so an external
   reload is a single undo entry.

Correctness property, tested by fuzzing: `apply(computePatch(a, b)) == b` for
arbitrary valid document pairs.

Performance property: editing one note in `suffocation.adx` produces exactly
**one** command. Not one-per-note, not a wholesale replace. A test asserts the
command count for a catalogue of single edits.

### 4.3 Incremental reparse

Full reparse of a 551-line file is sub-millisecond, so incremental parsing is
**not** needed for correctness or for file reloads. It is needed for the editor
panel, where reparsing on every keystroke also re-runs diagnostics, the
highlighter and completion.

Scope: re-lex only the edited line range, and re-parse only the section
containing it, unless the edit touched a section header (in which case fall back
to a full reparse). Simple, bounded, and correct by construction — an
incremental parser that tries to be clever about cross-section references is a
bug farm.

Budget: **< 5 ms from keystroke to updated diagnostics on a 10,000-line file.**

### 4.4 Reload policy — the "no dropouts" requirement

What a reload may and may not disturb:

| Change in the file | Effect on playback |
|---|---|
| Note added/removed/modified in a *future* tick | none; scheduler cursor picks it up |
| Note modified at the *current* tick | takes effect next block |
| Parameter value | smoothed to the new value (Phase 4's `Smooth.h`); no discontinuity |
| Effect parameter, same effect instance | same — `isEquivalent()` returns false, but the instance persists |
| Effect replaced with a different type | new instance; **old instance's tail is faded out over 20 ms** rather than cut |
| Channel instrument changed | voices on the old instrument release; new notes use the new one |
| Channel removed | its voices release, not cut |
| Tempo changed | takes effect at the next block, positions recomputed from the new map |
| Anything structural (routing, inserts) | one snapshot rebuild, swapped between blocks |

The unifying rule: **a reload never cuts a sounding voice.** It releases it. The
archived engine's note-off handling already does the right thing here, and this
is one of the places FINAL_PLAN §7 Phase 11 notes it can be reused.

The watcher must also handle how editors actually write files: most write to a
temporary and rename over the target, which produces a delete+create rather than
a modify. `FileWatcher` debounces 150 ms, coalesces events, and always re-reads
the whole file rather than trusting the event kind.

**Conflict.** If the file changes externally while the model has unsaved changes
that the new text does not contain, do not guess. Raise `onConflict` with three
offered resolutions: keep mine, take theirs, or open a diff. Silently picking
one is how a user loses work, and this project's entire premise is that people
will genuinely edit the file in two places.

### 4.5 Mini-notation — the port

`_archive/src-cpp/src/PatternCompiler.cpp` is 762 lines of working
Tidal/Strudel-lineage mini-notation. FINAL_PLAN §3.1: *"This is a differentiator;
do not rewrite it from scratch."* The port is deliberate and near-verbatim.

The archived header documents the supported grammar precisely; it is preserved
in full and becomes `docs/mininotation.md`:

- subdivision and nesting (`[a b] c`), alternation (`<a b>`)
- `*n` repeat, `/n` slow, `!n` replicate, `@n` weight
- Euclidean rhythms `(k,n,r)`
- stacking (comma-separated layers)
- `?` per-event probability, driven by a **seeded xorshift32**, never a
  time-based PRNG
- scale degrees via `scale=<root>_<mode>` for 8 modes, as a trailing token or a
  pipe stage
- pipe transform stages: `rev`, `fast n`, `every n rev`
- `cycleIndex` selecting which cycle of `<>`, `/n` and `every n` to realize
- `StepSpan` provenance per compiled note — the byte range in the source that
  produced it

Changes made on port, each with a reason:

| Change | Reason |
|---|---|
| Output ticks, not float beats | Phase 2 §4.1; deterministic rounding `(ticks*i + n/2)/n` |
| Diagnostics use Phase 2's `Diagnostic` with line **and** column | the archived version returns one `errorColumn`; the editor needs a span to underline |
| Compiles into a `Pattern`, not a `Track` | Phase 2's model |
| `cycleIndex` is driven by the live cycle counter | the archived `LiveCodeEditor` tracked this itself for `every n`; it belongs in the engine so offline export sees the same cycles |
| Keeps the FNV-1a-of-text default seed | determinism across hot reload and export — the archived header states this requirement explicitly and it is correct |

**The `StepSpan` array is what makes playhead-synced step highlighting possible**
(§4.6), and it is already there. That is a good example of why the port is
near-verbatim rather than a rewrite.

### 4.6 The editor panel

`QPlainTextEdit` + `QSyntaxHighlighter`, not a new dependency. The highlighter
consumes `engine/format/adx/Tokenize.h`'s token stream, so the editor's idea of
the grammar is the parser's idea of the grammar. A hand-written regex
highlighter drifts from the parser within weeks — which is exactly what the
archived `LiveCodeEditor` did, scanning for `PATTERN=` lines by string prefix.

Features:

- syntax highlighting from engine tokens (sections, keys, values, note names,
  mini-notation operators, comments, strings)
- inline diagnostics: squiggles with hover text, gutter marks, and a problem
  list that navigates on click
- **playhead-synced step highlighting**: at 60 Hz, the engine reports the active
  step index per playing mini-notation pattern; the editor highlights the
  corresponding `StepSpan`. This is the feature that makes live coding feel
  live, and it is the archived `LiveCodeEditor`'s best idea
- completion from `ParamRegistry` (Phase 2 §4.7) — section names, keys, and full
  parameter paths, which are otherwise impossible to remember
- bidirectional cursor link: selecting a note in the piano roll scrolls the
  editor to its line and vice versa
- "revert region" to undo text changes in one section only

### 4.7 The scripting API

```python
import adx

p = adx.project()                       # the open project
with p.undo_group("build a riff"):
    ch = p.add_channel("Lead", instrument="va", preset="c418/Cave Pad")
    pat = p.add_pattern("Verse", bars=8)
    pat.notes(ch).add_mini("0 3 5 7 | scale=a_minor | fast 2")
    p.playlist.track(1).place(pat, at="1:1:0")
    p.mixer.insert(2).add_effect("Reverb", mix=0.3)

adx.transport.play()
adx.on_bar(lambda n: ... )              # live-coding callbacks, main thread only
```

Rules:

1. **The API issues commands.** It has no privileged mutation path. FINAL_PLAN §4
   requires this and it is what keeps the three surfaces from drifting.
2. **Main thread only.** Rule 1 is absolute: user scripts never touch the audio
   thread. `adx.on_bar` callbacks are dispatched on the Qt event loop from a
   main-thread timer reading a transport counter — never from the audio callback.
3. **Every script action is undoable**, grouped by `undo_group`.
4. **The console is a real REPL** in a dock, with history, completion and
   tracebacks, running in the app's own interpreter.

`docs/scripting.md` is written as part of this phase, and the examples in
`app/adx/scripting/examples/` are executed by the test suite — so documentation
that stops working fails CI.

---

## 5. Port map

| Archive source | Destination | Fidelity |
|---|---|---|
| `src/PatternCompiler.cpp` (762 lines) | `engine/format/mininotation/` | **Near-verbatim.** Grammar, transforms, seeding and `StepSpan` preserved; output type and diagnostics retargeted (§4.5) |
| `include/PatternCompiler.h` documentation | `docs/mininotation.md` | The archived header is the most complete grammar description that exists; it becomes the spec |
| `src/LiveCodeEditor.cpp` | `app/adx/panels/editor/` | **Concept kept, implementation replaced.** The ImGui text box scanning for `PATTERN=` by prefix becomes a real editor over an engine tokenizer, as FINAL_PLAN §3.2 requires |
| `src/main.cpp::CheckForHotReload` (500 ms `stat()` poll) | `engine/sync/FileWatcher.cpp` | **Behavior kept, mechanism replaced.** Real `ReadDirectoryChangesW` watcher; diff-and-patch instead of rebuild-and-replace |
| archived `every n` cycle tracking in `LiveCodeEditor` | `PatternCompiler` + transport cycle counter | Moved into the engine so offline export realizes the same cycles as playback |

---

## 6. Tests

| Test | Asserts |
|---|---|
| `patch_apply_equals_target` | `apply(computePatch(a,b)) == b` for the corpus and 10k fuzzed document pairs |
| `patch_minimal_single_note` | editing one note's velocity in `suffocation.adx` yields exactly 1 command |
| `patch_minimal_catalogue` | a table of 25 single edits each yields the documented command count |
| `patch_section_move_is_noop` | reordering sections in the text yields 0 commands |
| `patch_is_one_undo_group` | an external reload is one history entry regardless of size |
| `incremental_matches_full` | incremental reparse yields the same `Document` as a full reparse, over 10k random edits |
| `incremental_budget` | < 5 ms keystroke→diagnostics on a 10,000-line file |
| `sync_no_loop` | 1000 alternating GUI and text edits terminate; write count is bounded |
| `sync_conflict_detected` | simultaneous divergent edits raise `onConflict` and never silently drop either side |
| `watcher_handles_rename_write` | a temp-file-plus-rename save (how most editors write) is detected as one change |
| `watcher_debounces` | 50 rapid writes produce at most 2 reloads |
| **`reload_no_voice_cut`** | **the gate.** A sustained pad plays while the file is rewritten 100 times with unrelated edits: zero voice restarts, zero output discontinuities > 0.01, zero allocator-hook violations |
| `reload_effect_swap_fades` | replacing an effect type fades the old instance over 20 ms rather than cutting |
| `reload_preserves_equivalent_effects` | an unchanged effect keeps its DSP state (reverb tail survives) |
| `reload_tempo_change_no_glitch` | a tempo edit mid-playback produces no discontinuity and correct subsequent timing |
| `mini_grammar_corpus` | every construct in `docs/mininotation.md` compiles to hand-verified ticks (ported from the archived behavior, extended) |
| `mini_deterministic` | identical output across 1000 recompiles, across processes, and between realtime and offline |
| `mini_probability_seeded` | `?` events are identical for the same seed and differ for different seeds |
| `mini_euclid_table` | `(k,n,r)` matches the canonical Euclidean rhythm table for all k≤n≤16 |
| `mini_step_spans` | every compiled note's `StepSpan` points at the exact source token |
| `mini_cycle_index` | `<a b>` and `every 4 rev` realize correctly across 16 cycles, offline and live |
| `editor_highlight_from_engine_tokens` | the highlighter's spans equal the tokenizer's, over the corpus |
| `editor_diagnostics_positions` | squiggles land on the exact reported column range |
| `editor_step_highlight_tracks` | during playback the highlighted span matches the engine-reported active step at 60 Hz |
| `script_examples_run` | every file in `scripting/examples/` executes and produces its documented result |
| `script_is_undoable` | each example's `undo_group` undoes cleanly to the prior state |
| `script_never_touches_audio_thread` | `on_bar` callbacks are observed on the main thread only |
| **`test_bidirectional`** | **the phase gate.** GUI edits and external-editor edits interleaved for 200 operations while playing: both windows converge, no dropouts, no conflicts lost, final file round-trips |

---

## 7. Definition of done

Evidence recorded 2026-10-21 on the development machine (Windows 11, MSVC 18, Qt and
PySide6 6.11.2). "Locally" is a different claim from "green in CI"; the CI half is P7-0
in plans/STATE.md, and nothing here has been pushed yet.

- [x] `test_bidirectional` passes (`tests/python/test_bidirectional.py`, ~13 s): the
      whole application in Live sync with the transport playing; 200 operations
      alternating GUI edits through the session and external saves (half
      temp-and-rename, half in place); after every one the file's canonical text is
      the project's. Ten deliberate collisions - a GUI edit adX has not written yet and
      an external save - are each raised as a conflict, never guessed, and resolved
      alternately "take theirs" and "keep mine"; no other conflict is raised. The
      transport plays throughout, the slowest callback stays inside the block
      deadline, and the final file round-trips. Passed five runs of five. It found two
      data-loss bugs before it passed (§11, corrections 9 and 10).
- [x] `reload_no_voice_cut` passes (`tests/cpp/sync/test_reload.cpp`) - more strictly
      than stated: a sustained pad plays while its file is rewritten 100 times with
      inaudible edits, narrow and structural (a channel, a pattern, a slot added and
      removed; sections moved), and the output is **bit-identical** to the same render
      with no reloads; zero allocator-hook violations in Debug and RelWithDebInfo.
      Beside it: `reload_effect_swap_fades`, `reload_instrument_change_releases`,
      `reload_channel_removed_releases`, `reload_preserves_equivalent_effects`,
      `reload_tempo_change_no_glitch`.
- [x] **A human has kept adX and VS Code open on the same `.adx` file, edited in both
      while it played, and reported no glitches.** Closed 2026-10-25 by the user, in
      their words: "i personally checked the live edit barrier." (P7-1)
- [x] Mini-notation is live, *automated half*: a MINI block plays through the
      snapshot at the song's cycles (`mini_plays_through_the_snapshot`); text typed in
      the editor panel is applied as commands as soon as it parses
      (`test_typing_applies_as_one_undoable_edit`), and the snapshot is rebuilt from it
      at the next block - which is no later than the next cycle boundary (§11,
      correction 3); the lit step follows the playhead at every frame tested over
      several cycles (`test_editor_step_highlight_tracks`). Whether it *feels* live is
      the human gate's to say.
- [x] `docs/mininotation.md` and `docs/scripting.md` exist and their examples run
      in the test suite: every row of mininotation.md's tables is compiled and compared
      (`test_mininotation_tables_are_true`, 29 rows), every Python block of
      scripting.md runs in order (`test_scripting_doc_blocks_run`), and every script in
      `app/adx/scripting/examples/` runs, asserts its documented result and undoes in
      one step (`test_script_examples_run`, `test_script_is_undoable`). In CI once
      pushed (P7-0).
- [x] All FINAL_PLAN §5.8 items except `adx render`: bidirectional GUI-text sync
      (this phase), filesystem-watch hot reload that patches (`Patch.h`, `FileWatcher.h`,
      `TextSync.h`), the in-app editor with highlighting from engine tokens, inline
      diagnostics and playhead-synced step highlighting (`panels/editor/`),
      mini-notation (`engine/format/mininotation/`), the Python scripting API and console
      (`app/adx/scripting/`), and `adx validate` / `fmt` / `diff` (Phase 2; `validate`
      now reports invariant findings at their lines, P2-2).
- [x] FINAL_PLAN.md §10 Phase 7 row updated.

Everything else observed locally, 2026-10-21: C++ - 370 cases on each of Debug,
RelWithDebInfo and Release (4-5 skipped: the local Teto tests, and one Release-only
geometry case, as before); clang-tidy clean over all 251 files on all three compile
databases (99 findings fixed in the new code); clang-format, headers, format-safety,
positions, dsp-math and `rules` clean. Python - 200 tests green without the GPU tests,
both GPU tests green; ruff, ruff format and mypy `--strict` (125 files) clean. The
incremental budget in Release: worst keystroke 3.9 ms on a 10,000-line file (budget 5;
a full parse is 10.4 ms). The patch fuzz: 10,000 pairs per run, plus 80,000 more over
eight further seeds (`ADX_PATCH_SEED`), all green. Phase completion, locally in Debug:
`ADX_FULL_EVIDENCE=1 ctest --preset windows-x64-debug -L slow` passed in 972.41 s.
Launcher: `check_launcher: adX.cmd opened adX 0.1.0 - 13 panels, audio device open`.

---

## 8. Out of scope (and who owns it)

| Thing | Owner |
|---|---|
| `adx render` | Phase 8 |
| Collaborative/multi-user editing | **never** — FINAL_PLAN §1 non-goal |
| Mini-notation clips as *launchable* session clips | Phase 11 (§5.11 says so explicitly) |
| A general-purpose plugin/extension system for scripts | not planned; the scripting API is the extension point |

---

## 9. Handoff to Phase 8

Phase 8 inherits:

- `adx` as a Python module with a complete command surface — `adx render` is a
  thin CLI over it rather than new engine work.
- `Patch` and `Document` diffing, which the project-bundle feature reuses to
  rewrite sample paths without disturbing anything else in the file.
- A scripting API that makes batch export scriptable on day one.
- The mini-notation compiler, which the golden-render corpus can use to generate
  deterministic test material compactly.

---

## 10. Risks

| Risk | Mitigation |
|---|---|
| The sync loop is subtly non-terminating under some edit pattern | Three independent guards (§4.1), any one of which breaks the loop; `sync_no_loop` fuzzes 1000 alternating edits |
| Diff-to-command produces a correct but *huge* command list, causing a reload stall | `patch_minimal_catalogue` asserts command counts for 25 edit shapes; a regression shows up as a count, not as a feeling |
| Conflict resolution UI is the kind of thing that gets deferred and then bites | It is in the gate (`sync_conflict_detected`) and in §6; conflicts are inevitable given the premise, so they are designed for rather than hoped against |
| The ported compiler's 762 lines carry latent bugs into the new system | `mini_grammar_corpus` and `mini_euclid_table` are written against the documented grammar rather than against the implementation, so they can find existing bugs rather than enshrine them |
| Live mini-notation recompiling mid-playback causes dropouts | Compilation is main-thread and produces a snapshot; the audio thread only ever swaps a pointer — the same mechanism as every other edit (Phase 3 §4.2) |

---

## 11. Corrections made while executing this plan

What changed against the plan, and what the tests found:

1. **Patch diffs projects, and applies as it goes.** §4.2 sketches a Document diff
   returning a command list. `format::applyPatch` parses the new text into a project
   of its own, remaps its ids into the open project's (channels, patterns and
   controllers by name; inserts, slots, sends and tracks by the numbers the file
   spells; notes by (start, pitch); items by what they place), emits the narrowest
   existing command for each value that changed, and covers any remainder with one
   `ReplaceProject`. The parser already knows the grammar; a second, document-level
   interpretation of it would be the drift FINAL_PLAN §4 forbids. Executing as it goes
   lets later commands find what earlier ones created; nothing needed the list. Two
   commands are new (`PatchCommands.h`): `SetNoteClip` and `ReplaceProject`.
2. **Orders.** Moving a section is zero commands (`patch_section_move_is_noop`), and the
   next canonical write puts it back - which is what canonical means. Every *other*
   order the writer preserves is content: the fuzz found a track's items and then the
   mixer's routes compared sorted, leaving patched files that wrote differently (pairs
   2404 and 4497 of 10,000 in the optimised builds; the Debug run of 1,000 never met
   them). `sameContent` now ignores only section order and what the writer sorts.
3. **Mini-notation takes effect at the next block, not the next cycle boundary.**
   §7's wording reads as a quantised swap. The snapshot is rebuilt on the edit and
   swapped at the next block, phase-aligned to the song's cycles - which is how Tidal
   and Strudel behave, and is never later than the boundary. Holding the old pattern
   until the boundary would need the snapshot to carry two versions of a clip and a
   switch tick that survives loops; nothing asked for that. A cycle is one bar of the
   meter at tick 0, counted from the start of the song, so an export plays the same
   cycles as playback.
4. **The archived Euclid formula was a rotation of Bjorklund's** - E(5,8) came out
   `x.x.xx.x`, not `x.xx.xx.`. `mini_euclid_table` was written against the canonical
   table (Tidal's algorithm, transcribed independently), as §10 hoped, and found it.
   `?` is now seeded per cycle (FNV-1a of the text, mixed with the cycle number): the
   archive's one seed per pattern dropped the same hits every bar. Unknown steps warn
   (`ADX1007`); a syntax error is `ADX2015`, mapped to the file's line and column.
5. **A section header starts at the margin.** The lexer took any line whose content
   began with `[` as a header, indented or not - so `  [bd sn] hh`, the commonest
   shape of a MINI line, became an unknown section. Recorded in
   docs/adx-format-v2.md §4.1.
6. **Duplicate numbered ids loaded silently.** `SLOT 1` on two inserts (natural to type
   - the numbers look per-insert) gave two slots one id, and so one engine node. The
   parser now reports `ADX2010` at the id; a track's id is per arrangement. Found by
   `reload_no_voice_cut`.
7. **Handovers.** §4.4's two rows a new node cannot avoid - a new effect type, a new
   instrument - are `graph/Handover.h`: the new node runs the old one with the
   parameters it last processed (every slot and channel now remembers them each block)
   and crossfades (effects, 20 ms) or rings out its released voices (instruments, at
   most 4 s). This needed `SlotNode::process` to be final, with effects implementing
   `processSlot`. A removed channel is `DepartingNode`: its node keeps feeding the insert
   it fed until its voices are silent. A node handing over renders only its own block,
   so a second swap within the first's fade cuts the first's tail
   (`misc-no-recursion`; recorded rather than chained).
8. **Text with errors - or invariant errors - is never applied.** `TextSync` validates
   as well as parses, so a routing cycle typed into the text is refused at its ROUTE
   line rather than found by the engine after the model took it. With P2-2 closed, the
   editor shows validation findings at their lines too.
9. **A Live write could overwrite an external save** (found by `test_bidirectional`).
   An edit that changed nothing (a value set to what it was) left the file matching the
   project, so the external save landed while adX's Live write was still pending - and
   was overwritten 16 ms later. Fixed twice over: the Live writer re-reads the file and
   treats a change it has not seen as an external edit; and a conflict is decided on
   content (`modelDiffers`), not on the revision counter.
10. **A late watcher event raised a false conflict** (found by `test_bidirectional`).
    The Live writer's re-read applies an external save before the watcher reports it;
    if the user edits in between, the watcher's late report looked like a rival edit.
    Text identical to what the file held when the two last agreed is now `Unchanged`.
11. **`textChanged` fires on formatting.** The highlighter's deferred first pass marked
    the editor's text as edited and started the apply timer, which wrote stale text
    over an edit made in another panel. Found as a 1-in-5 flake in
    `test_unapplied_text_is_never_overwritten`; the document's revision moves on
    formatting too, so the panel compares the text itself
    (`test_highlighting_is_not_an_edit`).
12. **Step highlighting is computed in Python from the engine's compile**, not reported
    by the audio thread. The notes each MINI placement plays, with their StepSpans,
    come from `mini_realize` - the function the snapshot builder plays - when the text
    or the project changes; per frame it is a numpy comparison, so the frame keeps its
    one engine call (phase_5.md 4.9).
13. **The editor applies as you type, whatever the file's sync mode** (which governs
    the file, not the panel), and re-renders only after edits elsewhere settle (120 ms)
    and only while shown: re-rendering a long file on every frame of a drag would make
    the drag stutter.
14. **Incremental reparse is per pattern body** (§4.3's scope, made concrete): an edit
    inside one `[PATTERN]` re-parses it against the rest of the file with every other
    pattern's body left out; anything else is a full parse. 1.2 ms worst keystroke on
    10,000 lines in Release (correction 17).
15. **`adx` the scripting module is `adx.scripting.api`** - `adx` is the application's
    package. The console provides it as `adx`; the examples import it. Positions are the
    file's spelling, from `0:0:0` (§4.7's `at="1:1:0"` was a 1-based sketch).
    `add_mini` compiles into ordinary notes over the pattern's own bars; `pattern.mini`
    writes a live MINI block.
16. **Not in §2's manifest:** `app/adx/text_sync.py` (the Qt side of the file's sync),
    `app/adx/text_layer.py` (the window's wiring), `panels/editor/code_editor.py` (the
    text widget), `engine/project/SourceMap.h` (P2-2), `engine/graph/Handover.h` and
    `nodes/DepartingNode.*`, `bindings` for the incremental parse and tokens, and the
    Python tests `test_docs_examples.py`. The watcher tests are in `test_watcher.cpp`
    as listed; the sync tests in `test_sync.cpp`.

The first push (CI run 40174263448, 2026-10-25) failed, and its Python step hung until
GitHub's six-hour limit. Three fixes followed:

17. **The keystroke budget had no headroom.** `incremental_budget` measured 6.1 and
    6.6 ms on the Release and RelWithDebInfo runners (3.9 ms here). The other patterns'
    bodies were blanked to empty lines to keep line numbers, so the "cheap" parse still
    walked all 10,000 lines - about 3 ms of the 3.9. They are now left out, and a table
    maps each reduced line back to its line in the file: 1.2 ms worst keystroke locally.
    `incremental_matches_full` (10,000 random edits against a full parse) still agrees.
18. **`test_external_tempo_edit_shows_in_the_transport` opened a modal dialog.** Its
    first pass ends by undoing a reload, which leaves the project unsaved (correctly).
    The second pass's `open_project` then asked "Save changes?" in a message box that
    nothing answers offscreen, so it waited forever. It hangs the same way here
    (reproduced 2026-10-25); why the 2026-10-21 local run did not is not known. The test
    now discards the open project first, as the `studio` fixture's teardown does.
19. **A hang can no longer be silent.** Every test now runs with modal dialogs
    replaced by an error naming the dialog (`no_modal_dialogs` in `conftest.py`). A
    watchdog ends the run after 300 s on one test, printing the test and its stack as
    an `::error` annotation (the public API returns these). pytest's
    `faulthandler_timeout` dumps all threads at 240 s. The pytest step has a 20-minute
    limit and the job 120. Also: `adx.Playlist.track` looped forever when the engine
    refused to add a track. It now raises `ScriptError`.
