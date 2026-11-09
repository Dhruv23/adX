# Phase 11.6 — Editor polish: scale parity, a piano-roll cut tool, a shortcuts page · S

| | |
|---|---|
| **Status** | Done 2026-11-08 (evidence in §10; CI runs recorded in plans/STATE.md) |
| **Governs** | `engine/core/TempoMath.h/.cpp`, `engine/geometry/PianoRollGeometry.cpp`, `app/adx/panels/piano_roll/{scales.py,tools.py,panel.py}`, `app/adx/menus.py`, `app/adx/shortcuts.py`, `app/adx/panels/preferences/`, `tools/gen_keyboard.py` |
| **FINAL_PLAN refs** | §5.1 (amended), §5.14 (new), §7 Phase 11.6 |
| **Entry criteria** | None functional — [phase_5.md](phase_5.md) (piano roll, scale highlighting) and [phase_6.md](phase_6.md) (`ActionRegistry`, `Keymap`, the Preferences dialog) are both **Done** already; this phase is follow-on polish against gaps found reviewing that work after the fact, not something either phase was waiting on |
| **Next** | none required. Independent of Phase 11, 11.5 and 12–19 |
| **§5 coverage owned** | §5.1 amendment (cut tool, custom scales) · §5.14 **all** |

---

## 1. Objective

Three small, independent FL Studio-parity gaps, found doing the research rather than
reported as bugs:

1. **Scale highlighting** (phase_5.md §4.7) shipped with 14 presets, root/scale
   dropdowns, root/in-scale/out-of-scale row colouring
   (`engine/geometry/PianoRollGeometry.h`'s `ScaleHighlight`), and snap-on-draw
   (`scales.py`'s `snap_pitch`/`snap_pitches`). Against FL Studio's version, three
   things are missing: a user-defined custom scale (toggle each of the twelve
   semitones and save it under a name — FL's "User" scale), roughly 16 more named
   presets toward FL's ~30, and a way to re-snap notes already on the grid when the
   scale changes, not only notes being drawn (FL's scale lock). adX already has all
   seven diatonic modes, chromatic, both pentatonics, blues, whole tone, and harmonic
   and melodic minor — the gap is breadth of the preset list and the two missing
   behaviours, not the highlighting mechanism itself.

2. **The piano roll has nine tools** (Draw, Paint, Select, Slice, Glue, Strum, Mute,
   Slide, Pitch curve — `tools.py`) and nothing that discards. `SliceTool` always
   keeps both halves (`tools.py:341`). There is no tool that cuts a row off at a
   point and throws the remainder away. Add a **Cut** tool: click a bar inside a note
   row, and every note in that pitch row at or after that bar is removed outright; a
   note straddling the bar is shortened to end exactly at the bar line, not split
   into a kept second note (which is what would make it Slice instead of Cut).

3. **The rebinding system already exists end to end and has no UI.**
   `shortcuts.py`'s `Keymap` (declared defaults, scopes, persisted per-user
   overrides, `conflicts()`) and `actions.py`'s `ActionRegistry` (`remap()`,
   `actions_registry_complete`, `test_shortcut_remap_persists`) were built in Phase 6
   §4.10 and are fully exercised by tests — headlessly. `actions.py`'s own docstring
   says "menus, toolbars, context menus **and the keymap editor** are all built from
   this registry." The keymap editor was never built. There is no page anywhere a
   user can see a shortcut, let alone change one.

A fourth finding, surfaced doing #2 and #3 together, is folded into this phase's
scope rather than filed separately, because it's what makes both of the others
possible to finish honestly:

- **Playlist's tool-switch keys are real `ActionSpec`s** — `menus.py`'s
  `register_actions` registers one `playlist.tool.<name>` action per tool, scope
  `"playlist"`, straight from each `Tool.shortcut` (`menus.py:161-172`). They are in
  the `Keymap`, remappable, and `actions_registry_complete` checks them for
  conflicts.
- **Piano-roll's tool-switch keys are not.** `panel.py:223` builds a bare
  `QActionGroup` with `action.setShortcut(QKeySequence(str(index + 1)) if index < 9
  else QKeySequence())` — a 1-9 index into tool-declaration order, entirely outside
  the registry: not remappable, not conflict-checked, invisible to
  `actions_registry_complete`.
- A tenth piano-roll tool has nowhere to go in a scheme that stops at 9, and a
  shortcuts page built only from the registry would show every playlist tool key and
  none of the piano roll's. **Cut's own default key is therefore what forces this
  fix**, not scope creep next to it: migrate piano-roll tool switching onto the same
  pattern playlist already uses, so Cut gets a real, visible, remappable key and the
  nine tools already there stop being the one shortcut surface the new page can't
  show.
- `tools/gen_keyboard.py:42` already special-cases this exact situation once:
  `menu.replace("&", "") or "Playlist tools"`, with the comment *"the playlist's
  toolbar, no menu"* — a single hardcoded fallback label for the one scope-only
  action group that exists today. Adding a second (`"piano_roll"`) breaks it
  silently (both groups would render under the heading "Playlist tools"). Keyed by
  `spec.scope` instead, titled, this is a two-line fix the migration above makes
  necessary, not optional.

---

## 2. Deliverables — exact file manifest

```
engine/core/
  TempoMath.h/.cpp        + TempoMap::barStart(Ticks at) -> Ticks: the start of the
                          bar containing `at`, from the same meterEvents() segment
                          walk and bar = ticksPerBeat(denominator) * numerator math
                          PianoRollGeometry.cpp already computes inline for bar grid
                          lines — factored out so the grid and the Cut tool read one
                          implementation and cannot disagree (the precedent already
                          set for ScaleHighlight, scales.py:5)

engine/geometry/
  PianoRollGeometry.cpp   its grid-line pass (the bar/beat/sub modulo block, ~line
                          273) calls TempoMap::barStart instead of repeating the math

bindings/
  project.cpp (or wherever meter_at/bpm_at live)   + bar_start(ticks) -> int,
                          bound the same way as the existing meter_at/bpm_at pair

app/adx/
  engine_bridge.py        + Project.bar_start(ticks: int) -> int, mirroring
                           meter_at/bpm_at's @_ffi wrapper
  menus.py                + one piano_roll.tool.<name> ActionSpec per tool (mirrors
                           the existing playlist.tool.* block at menus.py:161-172),
                           scope "piano_roll"; + edit.snap_selection_to_scale (the
                           lock action, §4.1)
  shortcuts.py             unchanged — Keymap already supports everything this
                           phase needs; no new storage format

app/adx/panels/piano_roll/
  scales.py               SCALES gains the presets listed in §4.1; + CustomScales:
                           load/save named (root omitted - a mask is root-relative)
                           user scales via QSettings, same persistence shape as
                           Keymap (group "custom_scales", one key per name)
  tools.py                + CutTool (§4.2); DrawTool/PaintTool/.../PitchCurveTool
                           unchanged
  panel.py                tool switching goes through registry actions instead of
                           the local QActionGroup/QKeySequence(str(index+1)); a Lock
                           toggle button beside the root/scale combos wired to
                           edit.snap_selection_to_scale; scale combo's new trailing
                           "Custom..." entry opens custom_scale_dialog
  custom_scale_dialog.py  new — twelve semitone toggle buttons labelled from
                           scales.NOTE_NAMES, a name field, Save/Delete, built over
                           CustomScales

app/adx/panels/preferences/
  dialog.py                + ShortcutsPage in self.pages
  pages/shortcuts.py       new — one row per registry.specs(): label, scope, a
                           QKeySequenceEdit bound through registry.remap(), the
                           default (grey, for comparison), a conflict marker driven
                           by keymap.conflicts(), a search field, Reset (per-row and
                           "Reset all")

tools/
  gen_keyboard.py          group rows by spec.scope (titled, e.g. "playlist" ->
                           "Playlist tools") instead of the single hardcoded
                           "Playlist tools" fallback (line 42); the generated
                           "## Piano roll tools" section follows for free once the
                           migration above registers those actions

docs/
  keyboard.md              regenerated (tools/gen_keyboard.py): gains "## Piano
                           roll tools"; its hand-written line "the piano roll's
                           tools are 1-9" is now wrong and is dropped
  user-guide.md            short mentions added: the Cut tool, the scale Lock
                           toggle and custom scales, and "Preferences > Shortcuts"
                           in place of whatever it currently says about remapping
                           (check before editing — likely nothing, since there was
                           no page to point to)

tests/python/
  test_piano_roll_tools.py        + test_cut_tool_removes_notes_after_bar,
                                   test_cut_tool_truncates_straddling_note,
                                   test_cut_tool_scoped_to_clicked_row
  test_scales.py (new)             + custom-scale save/load round trip,
                                   mask_of/degrees for every added preset,
                                   test_lock_resnaps_existing_notes
  panels/test_panel_actions.py     + test_piano_roll_tool_shortcuts_are_registered
                                   (today's gap, made a permanent regression test)
  panels/test_panel_preferences.py + test_shortcuts_page_lists_every_action,
                                   test_shortcuts_page_rebind_persists,
                                   test_shortcuts_page_flags_conflicts
  test_docs.py                     existing `gen_keyboard.py --check` gate now also
                                   covers the piano-roll section (no new test file)

tests/cpp/geometry/
  test_tempo_math.cpp (existing or new)   + test_bar_start_matches_grid_lines: for
                                   a project with a mid-pattern meter change,
                                   TempoMap::barStart agrees with every tick
                                   PianoRollGeometry tags ColorRole::kGridBar
```

---

## 3. Order of work

1. `TempoMap::barStart` first, with its own test against the existing meter-change
   fixture used by `PianoRollGeometry`'s own tests — before anything calls it, so a
   wrong bar boundary is caught by one focused test, not three features at once.
2. Refactor `PianoRollGeometry.cpp`'s grid pass onto it; its existing visual tests
   (`geometry_scale_highlight_rows`, the grid-line checks) must still pass unchanged
   — this step should produce no behaviour change, only one fewer place the math
   lives.
3. `bindings`/`engine_bridge.py` exposure, then `CutTool` in Python, which is now a
   small, pure function over `model.notes()` and `model.bar_start()`-shaped data,
   same pattern as `SliceTool`.
4. Piano-roll tool-switch migration into `menus.py` (this is what gives Cut a key),
   with `test_piano_roll_tool_shortcuts_are_registered` written first so the
   migration has a failing test to turn green rather than being "trust me, it
   works."
5. `gen_keyboard.py`'s scope-label fix, regenerate `docs/keyboard.md`, confirm the
   `--check` gate is green with both "Playlist tools" and "Piano roll tools"
   sections present and distinct.
6. Scale presets + `CustomScales` + the Lock action — independent of 1-5, can be
   done in parallel by a second pass.
7. The Shortcuts preferences page last, since it is the one piece that needs the
   registry to already contain everything worth showing (steps 4 and 6's new
   actions included) to be worth building at all.

---

## 4. Design

### 4.1 Scale presets, custom scales, and the lock

**Presets.** `SCALES` (`scales.py:14`) gains, toward FL Studio's list: Harmonic
Major, Diminished (Whole-Half), Diminished (Half-Whole), Hungarian Minor, Hungarian
Major, Neapolitan Major, Neapolitan Minor, Enigmatic, Persian, 8-Tone Spanish, Gypsy
(Romani), Arabic (Double Harmonic / Byzantine), Ahava Rabba (Phrygian Dominant),
Hirajoshi, Iwato, Kumoi, Pelog — each just another `tuple[int, ...]` of intervals;
`mask_of`, `in_scale`, `snap_pitch` and `degrees` need no change, since they were
already written generically over any 12-bit mask.

**Custom scales.** A `CustomScales` class in `scales.py`, shaped like `Keymap`: a
`QSettings` group (`"custom_scales"`), one key per user-given name holding the
12-bit mask as a string (root is never stored — a mask is relative to whatever root
the combo picks later). `panel.py`'s scale combo gains a trailing `"Custom..."`
entry; choosing it (or an existing saved custom scale, listed above it) opens
`custom_scale_dialog.py`: twelve toggle buttons labelled `scales.NOTE_NAMES`, a name
field, Save (writes through `CustomScales`, adds/updates the combo entry) and
Delete. The dialog edits a mask, not a root — exactly the shape `ScaleHighlight`
already expects, so nothing downstream (row colouring, snapping) needs to know a
scale is custom at all.

**Lock.** A toggle button beside the root/scale combos. Off (default): unchanged —
`DrawTool`/`PaintTool` snap only what's being drawn, via `model.snap_pitch`, exactly
as today. On: every scale or root change, and the toggle's own rising edge, issues
one `edit.snap_selection_to_scale` command (new `ActionSpec`, `menus.py`) that runs
`scales.snap_pitches` — already written, already vectorized, never previously called
from anywhere but the per-note draw path — over every note pitch in the current
pattern (not just the selection, despite the action's name tracking the general
"apply an edit to notes" naming already used elsewhere — rename if that reads
wrong) and commits it as one `RollModel.edit("Snap notes to scale", update=...)`,
one undo step. A click that changes nothing (the scale already fits) issues none, as
every other tool in this file already does (`tools.py:4-6`'s stated rule).

### 4.2 The piano-roll Cut tool

```python
@dataclass(slots=True)
class CutTool(Tool):
    """Click a bar in a note row: every note at/after it in that row is discarded;
    one straddling it is truncated to end there, not kept as a second note."""

    name: str = "cut"
    label: str = "Cut"

    def release(self, model: RollModel, p: Pointer) -> None:
        hit, _part = model.hit_note(p.x, p.y)
        if not hit:
            return
        bar = model.bar_start(model.view.tick_at(p.x))
        notes = model.notes()
        row = notes[notes["pitch"] == notes[notes["id"] == hit]["pitch"][0]]
        starts = row["start"].astype(np.int64)
        ends = starts + row["length"].astype(np.int64)
        remove = row[starts >= bar]["id"]
        straddling = row[(starts < bar) & (ends > bar)].copy()
        straddling["length"] = bar - straddling["start"].astype(np.int64)
        if not len(remove) and not len(straddling):
            return
        if len(remove):
            model.select(remove, "remove")
        model.edit("Cut notes", update=straddling, remove=remove)
```

Row scope mirrors `GlueTool` (`tools.py:399-408`): the gesture acts on the clicked
note's own pitch row in the pattern, not the current selection — a cut is "this
row, from here," not "these notes," so there is nothing to resolve the way
`_targets` resolves selection-vs-click for Draw, Select and Slice. `model.bar_start`
is a thin Python wrapper over the new `Project.bar_start` binding (§2), matching
`model.snap_tick`/`snap_pitch`'s existing shape. Clicking empty space (no `hit`)
does nothing, same as `SliceTool`.

### 4.3 Registering piano-roll tools, and the `gen_keyboard.py` fix

`menus.py:161-172`'s playlist block is copied almost verbatim:

```python
for tool in w.piano_roll.tools.values():
    specs.append(
        ActionSpec(
            f"piano_roll.tool.{tool.name}",
            f"{tool.label} tool",
            partial(w.piano_roll.set_tool, tool.name),
            "",
            PIANO_ROLL_TOOL_KEYS.get(tool.name, ""),
            scope="piano_roll",
        )
    )
```

with an explicit `PIANO_ROLL_TOOL_KEYS` dict (`1`-`9` for the existing nine, in
their current order, `0` for Cut) rather than reusing `panel.py`'s old index-based
`QKeySequence(str(index + 1))` — the dict makes Cut's key a decision this file
states once, not an emergent property of dict-insertion order the next tool added
would silently reshuffle. `panel.py:219-229`'s `QActionGroup` loop is deleted; the
checked/unchecked toolbar buttons it built are reattached to the registry's
`QAction`s (`registry.action(f"piano_roll.tool.{name}")`) the same way the playlist
toolbar already consumes `panel.tool_actions()`.

`tools/gen_keyboard.py:42` changes from

```python
menu = spec.menu.replace("&", "") or "Playlist tools"  # the playlist's toolbar, no menu
```

to

```python
menu = spec.menu.replace("&", "") or f"{spec.scope.replace('_', ' ').title()} tools"
```

which reproduces "Playlist tools" for `scope="playlist"` unchanged (so
`docs/keyboard.md`'s existing section is byte-identical) and produces "Piano roll
tools" for the new `scope="piano_roll"` actions for free. `docs/keyboard.md`'s
hand-written sentence *"Inside a panel its own keys apply too: the piano roll's
tools are 1-9..."* (line 5-6) is deleted once the generated section says the same
thing correctly, including Cut's `0`.

### 4.4 The Shortcuts preferences page

`pages/shortcuts.py`, added to `dialog.py`'s page list like the other four. One row
per `registry.specs()`: label, scope (blank for global), a `QKeySequenceEdit`
pre-filled from `keymap.shortcut(spec.id)`, the shipped default shown greyed out
beside it for comparison, and a small warning icon when the key appears in
`keymap.conflicts()`. Editing a row calls `registry.remap(spec.id, new_text)`
directly — the same call `test_shortcut_remap_persists` already exercises
headlessly, so the page is a thin view over code already proven correct, not a new
code path to trust from scratch. A search field filters rows by label or scope
(piano-roll and playlist's twenty-odd tool keys make an unfiltered list long enough
to need one). "Reset" clears one row's override (`keymap.set(id, "")`, which
`Keymap.set` already treats as "back to default," `shortcuts.py:50-57`); "Reset
all" calls `keymap.reset()`.

---

## 5. Tests

| Test | Asserts |
|---|---|
| `test_bar_start_matches_grid_lines` | for a project with a mid-pattern meter change, `TempoMap::barStart(t)` equals the tick of every grid line `PianoRollGeometry` colours `ColorRole::kGridBar`, at every bar in the fixture |
| `test_cut_tool_removes_notes_after_bar` | three notes on one pitch, click a bar before the second: the second and third are removed, the first is untouched |
| `test_cut_tool_truncates_straddling_note` | a note spanning the clicked bar is shortened to end exactly at the bar, never split into two notes |
| `test_cut_tool_scoped_to_clicked_row` | a note on a different pitch starting after the same bar is untouched — the cut is per-row, not per-pattern |
| custom-scale round trip | a saved custom scale's mask survives a `CustomScales` save/reload cycle; an unnamed/duplicate-name save overwrites, not duplicates, like `Keymap.set` |
| preset mask coverage | `mask_of`/`degrees` return the expected pitch classes for every newly added scale name |
| `test_lock_resnaps_existing_notes` | with Lock on, changing the scale combo re-snaps every out-of-scale note already in the pattern in one undo step; with Lock off, it touches nothing |
| `test_piano_roll_tool_shortcuts_are_registered` | every `piano_roll.tool.*` id appears in `registry.specs()`, is in `keymap.conflicts()`'s input set, and its `QAction.shortcut()` matches `PIANO_ROLL_TOOL_KEYS` |
| `test_shortcuts_page_lists_every_action` | the page's row count equals `len(list(registry.specs()))`; every row's shown shortcut equals `keymap.shortcut(spec.id)` |
| `test_shortcuts_page_rebind_persists` | editing a row's `QKeySequenceEdit`, closing and reopening Preferences shows the new key, and `keymap.shortcut(id)` agrees |
| `test_shortcuts_page_flags_conflicts` | two actions in the same scope remapped to the same key both show the warning; resolving one clears both |
| `tools/gen_keyboard.py --check` (existing gate) | still green with two distinct scope sections present; `docs/keyboard.md` has no stale hand-written line about 1-9 |

---

## 6. Definition of done

- [x] `TempoMap::barStart` matches every `kGridBar` line in a meter-change fixture;
      `PianoRollGeometry.cpp`'s inline bar math is gone, not duplicated.
- [x] The Cut tool ships as the piano roll's eleventh tool (§9), with its own key (`0`),
      doing exactly the three cut-tool tests above and nothing to a different pitch
      row.
- [x] Every piano-roll tool switch is a registered, remappable, conflict-checked
      `ActionSpec`; `test_piano_roll_tool_shortcuts_are_registered` is green.
- [x] `docs/keyboard.md` regenerates clean under `--check`, with a correct "Piano
      roll tools" section and no leftover hand-written 1-9 sentence.
- [x] 31 named scales total (14 existing + 17 added; §9), a working custom-scale editor
      that survives a restart, and a Lock toggle that re-snaps existing notes in one
      undo step and does nothing when off.
- [x] A Shortcuts page in Preferences lists every registered action, rebinds and
      persists through `ActionRegistry.remap`, flags conflicts via
      `Keymap.conflicts()`, and resets per-row or all at once.
- [x] FINAL_PLAN.md §10 gains a row for Phase 11.6; §5.1 is amended for the cut tool
      and custom scales; §5.14 is added for the shortcuts page.

---

## 7. Out of scope (and why)

| Thing | Status |
|---|---|
| A playlist Cut tool (truncate a clip's tail instead of splitting it) | **not this phase** — the user's own disambiguation scoped the cut tool to the piano roll only; the playlist already has Slice, Slip and Stretch for clip-length editing, and a playlist-side truncate is a separate, smaller follow-on if wanted later |
| Chord-stamp / scale-stamp palettes (FL's separate "Stamp" tool) | out of scope — a different feature from scale highlighting, not requested |
| MIDI-controller or on-device key rebinding | out of scope — this phase is the in-app keyboard only, scoped by the user's own question to "keyboard shortcuts and rebinding" |
| A chorded/multi-key rebinding scheme (e.g. a leader key) | not needed — `Keymap`/`QKeySequence` already support multi-key sequences like `Ctrl+Shift+D`; the page is a view over that, not a new binding grammar |
| Re-deriving every other panel's ad hoc keys (channel rack, mixer, if any exist outside the registry) | out of scope for this phase, **but note for a future pass**: the same audit that found the piano roll's keys outside the registry should eventually be run over every panel, not assumed clean elsewhere |

---

## 8. Risks

| Risk | Mitigation |
|---|---|
| `TempoMap::barStart` disagrees with `PianoRollGeometry`'s grid after the refactor | Step 2 of §3 requires the existing grid-line tests to pass unchanged before anything else in the phase touches the Cut tool; `test_bar_start_matches_grid_lines` pins the two together going forward |
| Moving piano-roll tool keys into the registry changes a key a user already has muscle memory for | `PIANO_ROLL_TOOL_KEYS` keeps the existing 1-9 order and meaning; only Cut (`0`, previously unused) is new — no existing tool's key moves |
| A tenth tool makes the piano-roll toolbar too crowded | UI layout only; not a design risk this plan owns — flagged for whoever builds `panel.py`'s toolbar row to check at normal width, phone-width n/a (desktop app) |
| The Shortcuts page is built before the piano-roll migration lands, and ships looking "complete" while missing a whole panel's keys | §3's order (migration in step 4, the page last in step 7) exists specifically to prevent this |
| 16 new scale presets include a naming or interval mistake (easy to transpose a mode's intervals wrong by hand) | every preset is checked by the mask-coverage test against its published interval set, not eyeballed |

---

## 9. Corrections

Found building it, 2026-11-08:

- **Cut is the eleventh tool, not the tenth.** The roll already had ten: the nine of
  `TOOLS` plus the Chord tool (`chords.ChordTool`), which had no key because the old
  index scheme stopped at 9. Cut still gets `0`; Chord still has none. The 1-9 keys
  did not move.
- **The pitch-curve tool's name is `bend`**, so its action is `piano_roll.tool.bend`
  and `PIANO_ROLL_TOOL_KEYS` uses that key.
- **§4.1 lists 17 presets, not 16**, so there are 31 scales, not 30. All 17 were kept;
  `test_preset_list` checks that no two presets share a mask, so none is a duplicate
  under another name. "Gypsy (Romani)" is 0 2 3 6 7 8 10, the one common reading that
  is not already Hungarian minor or Phrygian dominant.
- **The lock action is `piano_roll.snap_notes_to_scale`**, scope `piano_roll`, not
  `edit.snap_selection_to_scale`: it acts on every note in the pattern, and §4.1
  invited the rename. It is enabled only while a scale is chosen.
- **`barStart` is on `TempoMap` (TempoMap.h/.cpp)**, the class's real home; TempoMath.h
  holds the two free functions both callers share (`barTicks`, `barStartIn`), so the
  grid pass calls `barStartIn` per line without a meter lookup each time. Its test is
  `tests/cpp/geometry/test_bar_start.cpp` (no test_tempo_math.cpp existed).
- **The Shortcuts page applies a rebind at once.** `Keymap.set` persists on every call,
  so the page is a direct view over it; Cancel does not undo a rebind. The page says so.
- **"Custom…" edits the scale in use** when that is a custom scale (so it can be
  renamed or deleted), and makes a new one otherwise. Choosing a saved custom scale
  just uses it; the plan had that open the editor, which would make using one
  impossible without a dialog every time.
- **Piano-roll tool keys now work while the piano roll has focus**, as the playlist's
  do (`WidgetWithChildrenShortcut`); before, as bare QActions, they fired anywhere in
  the dock's window.
- `docs/keyboard.md`: only the 1-9 half of the hand-written sentence went; Delete and
  Ctrl+A stay, and the line now points to Preferences > Shortcuts.
- `register_actions` passed the 150-line limit (`lint.py rules`) with the new specs, so
  they are in `_piano_roll_specs` and `_attach_piano_roll`.

## 10. Evidence

Observed locally 2026-11-08:

- C++: `adx_tests "[tempomap],[geometry]"`: 47 cases, 46,693 assertions, all pass,
  including `bar_start_within_a_meter`, `test_bar_start_matches_grid_lines` and the
  unchanged `geometry_scale_highlight_rows` and grid tests.
- Python: the whole suite (`pytest -m "not gpu and not serial" -n auto`) green after the
  function-length fix; new tests `test_cut_tool_*` (four), the `cut` gesture in
  `test_piano_roll_tools_emit_one_command`/`_undo_restores`, `test_scales.py` (24),
  `test_piano_roll_tool_shortcuts_are_registered`, `test_panel_preferences.py` (three).
- `tools/gen_keyboard.py --check` green with distinct "Playlist tools" and "Piano roll
  tools" sections; ruff, ruff format, mypy --strict, clang-format, `lint.py rules`,
  `lint.py headers` clean.
