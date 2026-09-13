# Phase 7 — Text-first layer · M

| | |
|---|---|
| **Status** | Not started |
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

- [ ] `test_bidirectional` passes.
- [ ] `reload_no_voice_cut` passes.
- [ ] A human has kept adX and VS Code open on the same `.adx` file, edited in
      both while it played, and reported no glitches. Recorded in the phase log —
      this is the phase's actual claim and an automated test cannot fully make it.
- [ ] Mini-notation is live: typing a pattern in the editor changes what is
      playing at the next cycle boundary, with step highlighting following the
      playhead.
- [ ] `docs/mininotation.md` and `docs/scripting.md` exist and their examples are
      executed by CI.
- [ ] All FINAL_PLAN §5.8 items except `adx render` are complete.
- [ ] FINAL_PLAN.md §10 Phase 7 row updated.

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
