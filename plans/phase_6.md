# Phase 6 — The DAW proper · XL

| | |
|---|---|
| **Status** | Built 2026-10-20; every automated §6 box observed locally; **the human gate (§6 box 2) is open** and CI is pending - see plans/STATE.md (P6-0, P6-1) |
| **Governs** | every remaining panel in `app/adx/panels/`, their geometry builders, the generic parameter-editor system |
| **FINAL_PLAN refs** | §3.3.10, §5.1 (playlist, automation, history), §5.2 mixer UI, §5.10 meters, §7 Phase 6 |
| **Entry criteria** | [phase_5.md](phase_5.md) §6 complete, **including the frame-rate acceptance step** |
| **Next** | [phase_7.md](phase_7.md) |
| **§5 coverage owned** | §5.1 playlist, channel rack, automation editing, envelope/LFO/peak controllers, undo history panel · §5.2 mixer UI + live metering display · §5.3/§5.4 all instrument and effect editor UIs · §5.10 per-track and master meter *display* |

---

## 1. Objective

Build the rest of the application. Phase 5 proved one hot surface; this phase
multiplies the proven pattern across every remaining panel until the
FINAL_PLAN §7 gate is met:

> **Done when** a complete track can be written, arranged, mixed and exported
> without ever opening a text editor.

This is the largest phase in the project by volume and the least architecturally
risky, because Phase 5 already answered the hard question. The risk here is
**sprawl**, and §3 is the answer to it.

---

## 2. Deliverables — panel manifest

```
app/adx/panels/
  playlist/           panel.py  view.qml  PlaylistItem.h/.cpp  tools.py  tracks.py
  channel_rack/       panel.py  step_grid.py  channel_row.py
  mixer/              panel.py  strip.py  routing_matrix.py  send_editor.py
  instrument/         panel.py  generic_editor.py  editors/{additive,va,sampler,
                      slicer,drumsynth,granular,fm,wavetable}.py
  effect/             panel.py  generic_editor.py  editors/{eq,comp,reverb,delay,
                      limiter,multiband,convolution,grossbeat,...}.py
  automation/         panel.py  lane.py  AutomationItem.h/.cpp  controllers.py
  browser/            panel.py  model.py  drag.py  preview.py
  history/            panel.py
  preferences/        dialog.py  pages/{audio,midi,paths,appearance,editing}.py
  inspector/          panel.py         context-sensitive properties for the selection

engine/geometry/
  PlaylistGeometry.h/.cpp     clip rects, patterns preview, audio waveforms
  AutomationGeometry.h/.cpp   breakpoints + curve tessellation
  StepGridGeometry.h/.cpp     channel-rack step matrix
  MeterGeometry.h/.cpp        multi-channel meter bars from one ring read
  EqCurveGeometry.h/.cpp      EQ response curve + live spectrum overlay

app/adx/widgets/
  (Phase 5 stubs completed) knob, fader, meter, xy_pad, curve_editor,
  step_button, combo, numeric_drag, color_swatch, ruler, timeline

app/adx/
  actions.py          every menu/keyboard action, in one registry
  shortcuts.py        keymap, user-remappable, persisted
  dnd.py              drag-and-drop payload types and handlers
  selection.py        the global selection model (§4.7)

tests/python/panels/  one test module per panel
tests/python/test_workflow_end_to_end.py    the §6 gate
```

---

## 3. Discipline — how this phase avoids becoming iteration one

FINAL_PLAN §3.3.10 records what went wrong last time: `DrawSequencerUI()` was a
single 1,219-line function and `main.cpp` was 1,871 lines of mutable globals
interleaved with draw calls. Volume is what caused that, and this phase has more
volume than iteration one ever had. So the rules are mechanical:

1. **One panel, one directory, one `DockPanel` subclass.** A panel over ~400
   lines splits into sub-widgets. Ruff enforces the function-length limit.
2. **No panel owns project state.** Panels hold view state (scroll, zoom,
   selection) only. Everything else is read from the engine and mutated by
   commands. There is no cache of project data in a panel, because two caches
   disagree.
3. **Every mutation is a command.** No panel calls a setter. If a needed command
   does not exist, it is added in `engine/project/commands/` with a test — not
   worked around.
4. **Every FFI call goes through `engine_bridge`** (Phase 5 §4.8).
5. **Generic before specific** (§4.4). Twelve instruments and thirty effects get
   *one* auto-generated editor, plus hand-built editors only where the generic
   one is genuinely inadequate.
6. **Every panel added in a PR brings its tests and, if it is a hot surface, its
   perf budget** — added to `perf_geometry_budgets`, which already exists.

### Build order

Ordered so that something is usable as early as possible, and so that each panel
unblocks the next:

1. **Channel rack** — needed to create channels at all.
2. **Playlist** — the arrangement; the largest single panel.
3. **Mixer** — routing and levels.
4. **Generic parameter editor** — unlocks all instruments and effects at once.
5. **Automation** — depends on the playlist and on parameter discovery.
6. **Browser** + drag-and-drop — makes the rest ergonomic.
7. **History panel, inspector, preferences.**
8. **Specific instrument/effect editors** — the long tail; strictly last,
   because the generic editor already makes everything usable.

---

## 4. Design

### 4.1 Playlist

The second hot surface. Same pipeline as the piano roll (Phase 5 §4.3):
`PlaylistGeometry` → `GeometryBuffer` → `PlaylistItem` (`QQuickItem`) →
`QSGGeometryNode`.

Content per track lane: pattern clips (with a miniature note preview built at
low LOD), audio clips (reusing the Phase 5 waveform geometry and `ClipPeakCache`
unchanged), and automation clips (reusing `AutomationGeometry`).

Tools: draw, paint, select, slice, slip (drag content within a clip), stretch
(drag edge with time-stretch), mute, group.

Track features: mute/solo, height resize, color, grouping (a collapsible parent
lane whose operations apply to its children), freeze placeholder (Phase 8 makes
freeze real).

Time markers live on a ruler with drag, rename and jump-to. Arrangement
snapshots (§5.1) are named playlist states stored in the project and switched by
one command — implemented as a full playlist swap, which is small data and makes
undo trivial.

**LOD matters more here than in the piano roll**: a fully zoomed-out 10-minute
project shows hundreds of clips, each of which would otherwise draw its own note
preview or waveform. Below ~30 px of clip width, clips draw as flat colored
rects with a name, nothing else.

### 4.2 Channel rack

The step sequencer. A grid of channels × steps with per-step velocity via
drag-up/down, step groups, and a swing control.

`StepGridGeometry` builds the matrix in C++ — a 64-channel × 64-step grid is
4,096 cells, and 4,096 Python widgets is exactly the mistake this architecture
exists to prevent.

Each row: name, color, mute/solo, a level/pan pair, an output-insert selector,
a polyphony field, and a click-to-open instrument editor.

The step grid writes into the *pattern* currently selected — the channel rack
edits a pattern's note clips at step resolution while the piano roll edits the
same data at full resolution. They are two views of one model and must stay
consistent live, which falls out of neither caching state (§3 rule 2).

### 4.3 Mixer

A strip per insert, horizontally scrollable, each with: fader (with a dB scale),
pan, meter (peak + RMS + clip indicator), mute/solo/arm, polarity, stereo
separation, input/output selectors, eight effect slots with drag-reorder and
per-slot wet/dry + bypass, and a send list.

The meters read **one** ring per frame through `engine_bridge`'s 60 Hz timer, and
`MeterGeometry` turns the whole array into one vertex buffer — so 200 inserts
cost one FFI call and one upload per frame, not 200. This is the concrete reason
FINAL_PLAN §2.2 says "one FFI call per frame, total."

**Routing matrix** — a separate view showing the insert→insert DAG as a grid,
with cycle-creating cells disabled (Phase 3's cycle detection is consulted
*before* the command, so an invalid route is impossible to express rather than
rejected after the fact). Sidechain connections are shown distinctly, because a
sidechain edge that looks like an audio edge is confusing.

### 4.4 The generic parameter editor — the key leverage point

Phase 4 gave every instrument and effect a `ParamDescriptor` table (name, range,
default, unit, scale kind, group). That is enough to generate a real editor:

```python
def build_editor(descriptors: list[ParamDescriptor]) -> QWidget:
    # group -> section; unit + scale -> widget type + formatting
    #   Hz + logarithmic  -> log knob, "1.2 kHz"
    #   dB                -> fader, "-6.0 dB"
    #   ms                -> numeric drag, "250 ms"
    #   normalized        -> knob, "45 %"
    #   stepped/enum      -> combo
    #   curve             -> curve editor widget
```

Every parameter automatically gets: right-click → automate, right-click → MIDI
learn (wired in Phase 9), shift-drag for fine adjustment, double-click to reset,
value tooltip, and undo coalescing.

This is what makes thirty effects tractable. Hand-built editors are then written
only where a generic grid genuinely fails:

| Hand-built | Why the generic editor is inadequate |
|---|---|
| Parametric EQ | needs a draggable response curve with a live spectrum overlay |
| Compressor / Multiband / Limiter | needs a transfer curve + gain-reduction meter |
| Additive | needs a harmonic bar editor |
| Sampler / Slicer | needs a waveform with zone and loop-point editing |
| Wavetable | needs a 3D table view |
| FM | needs an algorithm matrix |
| Convolution | needs IR display and trimming |
| GrossBeat | needs a time/volume curve editor over a bar grid |
| Envelope/ADSR anywhere | needs the shared `curve_editor` widget (Phase 2 §4.8's `Curve`) |

Everything else ships with the generic editor and is genuinely usable.

### 4.5 Automation editing

Breakpoint curves drawn over the playlist or in a dedicated lane panel.
`AutomationGeometry` tessellates each segment using **Phase 2's `Curve::evaluate`**
— the same evaluator the engine uses. FINAL_PLAN §3.2 calls this out as a
principle: one evaluator shared by engine and UI so they cannot disagree. The UI
does not reimplement curve math, ever.

Editing: add/remove/drag points, drag a segment's curvature handle, select
multiple points and scale/offset them, snap values to a grid, and paste a
shape from a library (ramp, pulse, sine, random).

**Keyframing from the control.** Right-click any automatable knob/slider → "Create
automation" (or the keyframe button beside it) creates a lane targeting that
`ParamRef` in one command and starts it with a point at the current value. A knob
under automation shows a lane-colored ring and its live value; turning it while
playing edits the base value, and holding the record modifier writes points
(thinned on release). Baked Voice parameters carry a "re-renders" badge (Phase 4 §4.13).

**Clip envelopes.** A selected playlist item shows its envelopes drawn *on the clip*
(gain by default, switchable to pan, pitch, or an effect parameter the lane feeds).
Points edit in place and travel with the clip on move, duplicate and split (Phase 2
`PlaylistItem` envelopes, Phase 4 §4.0). One lane per target; the clip header shows which parameters
it envelopes. A clip envelope is distinct from a playlist automation clip, which spans
many items: the UI labels them differently.

**Controllers** (§5.1's envelope controller, LFO tool, peak controller) are
automation *sources* rather than drawn curves. Each is a small engine node whose
output is assignable to any `ParamRef`. They are engine-side because they must
run at audio rate; the UI configures them. A peak controller reading insert 3's
level and driving insert 7's gain is a sidechain-shaped edge in the graph and
participates in Phase 3's topological order like any other.

### 4.6 Browser and drag-and-drop

A tree over: sample directories, the preset library (Phase 4's
`PresetLibrary`, tag-filtered), project patterns, plugins (Phase 9), and
user-defined favorites. Search filters by name and tag. Audio files preview on
click through a dedicated preview channel that bypasses the mixer.

Drag-and-drop payloads (`dnd.py`) are declared once as typed objects and
accepted by the panels that make sense:

| Payload | Valid drop targets |
|---|---|
| sample file | playlist (creates an audio clip), channel rack (creates a sampler channel), sampler zone editor |
| preset | channel (replaces instrument), effect slot |
| pattern | playlist |
| effect type | mixer slot |

Drop is one command, so it is one undo step.

### 4.7 Selection model

One global selection (`selection.py`) holding a typed set: notes, clips,
automation points, channels, inserts, slots. The inspector panel renders
properties for whatever is selected. Delete/copy/paste/duplicate operate on the
current selection through commands, so they work uniformly across panels rather
than being reimplemented five times.

Clipboard is serialized as an `.adx` fragment using Phase 2's writer — which
means copy-paste between adX instances, and pasting adX text from a chat window,
both work for free. That is a small decision with a disproportionate payoff, and
it exists only because the format is text.

### 4.8 History panel

A list of `CommandStack::history()` entries with the current position marked;
click to jump to any point. Coalesced entries appear as one row (Phase 2 §4.9),
which is what makes the list readable rather than a wall of "move note".

### 4.9 Preferences

Audio (device, sample rate, buffer size, the latency readout from Phase 1's
`StreamInfo`), MIDI (Phase 9), paths (samples, presets, projects, plugins),
appearance (theme, meter scale, grid colors), editing (default note length, snap
defaults, undo limit, autosave interval).

Autosave writes a timestamped `.adx` to a project-local `.adx-autosave/` — cheap
and diffable, because the project is text.

### 4.10 Actions and shortcuts

Every user-triggerable operation is registered once in `actions.py` with an id,
label, icon, enablement predicate and default shortcut. Menus, toolbars, context
menus and the keymap editor are all built from that registry.

*Why:* a DAW accumulates hundreds of actions, and without a registry they end up
duplicated across a menu bar, a toolbar and a context menu that then disagree
about enablement — which is a class of bug that is tedious to find and trivial
to prevent.

---

## 5. Tests

Panel tests use `pytest-qt` and drive real widgets.

| Test | Asserts |
|---|---|
| `playlist_place_pattern` | dropping a pattern creates one item, one command, correct tick |
| `playlist_edit_propagates` | editing a pattern updates all placements — **the FL ergonomic advantage FINAL_PLAN §4 names, asserted** |
| `playlist_slip_and_stretch` | slip changes `sourceOffset` only; stretch changes length and stretch factor |
| `playlist_lod_switch` | below the width threshold, clip geometry drops previews |
| `channel_rack_step_roundtrip` | a step toggled in the rack appears in the piano roll at the right tick and velocity |
| `channel_rack_grid_is_cpp` | no per-cell Python widget is created for a 64×64 grid |
| `mixer_meters_one_ffi_call` | 200 inserts, 60 frames → exactly 60 FFI calls |
| `mixer_slot_reorder` | drag-reorder is one command and preserves DSP state where `isEquivalent` holds |
| `mixer_routing_prevents_cycle` | the cell that would create a cycle is disabled, and the command is never issued |
| `generic_editor_covers_all` | every instrument and effect produces a non-empty editor with every parameter present |
| `generic_editor_units` | each `Unit` renders the documented widget and formatting (table-driven) |
| `automation_uses_engine_curve` | UI tessellation matches `Curve::evaluate` to within 1e-6 at 1000 sample points |
| `automation_drag_is_one_command` | dragging N points coalesces to one history entry |
| `controller_lfo_drives_param` | an LFO controller assigned to a cutoff produces the expected modulation in a render |
| `browser_drag_creates_command` | each payload×target pair in §4.6 produces exactly the documented command |
| `history_jump` | clicking an entry undoes/redoes to exactly that point |
| `selection_uniform_ops` | delete/copy/paste/duplicate behave identically across all six selection types |
| `clipboard_is_adx_text` | copied notes paste as valid `.adx` into a text editor and back |
| `actions_registry_complete` | every menu item resolves to a registered action; no duplicate ids; every action has an enablement predicate |
| `no_panel_caches_project_state` | CI check: no panel module holds a project-derived attribute across events |
| `perf_geometry_budgets` (extended) | playlist, step grid, automation, meter and EQ-curve builders each within budget |
| **`test_workflow_end_to_end`** | **the phase gate.** Scripted through the real UI: create 4 channels, assign instruments and presets, program patterns in the rack and the piano roll, arrange 32 bars in the playlist, set levels/pans/sends, add 3 effects, draw 2 automation lanes, export a WAV, reopen it and confirm it renders identically. **No text editor is opened at any point.** |

---

## 6. Definition of done

Evidence recorded 2026-10-20 on the development machine (Windows 11, MSVC 18, Qt and
PySide6 6.11.2, Direct3D 11). "Locally" is a different claim from "green in CI"; the CI
half is P6-0 in plans/STATE.md.

- [x] `test_workflow_end_to_end` passes (6.4 s). Through the real panels and their
      input paths: four channels made in the rack (additive, VA, DrumSynth, wavetable),
      two presets dropped on channels; steps clicked in the rack and notes drawn with
      the piano roll's draw tool through its QML input; 32 bars laid out in the
      playlist with the draw tool on two tracks (14 placements); two inserts added,
      channels routed with their rows' output selectors, levels and pans turned on the
      rack's knobs and a strip's fader, a send chosen from a strip's menu; three effects
      dropped into slots (Reverb, Compressor, Limiter); two automation lanes created
      from a knob's "Create automation" and drawn with the mouse; File > Export, File >
      Save, File > Open, export again: **identical hash and identical bytes**. Every
      quarter of the song is audible. No step writes project text.
- [ ] **A human has actually made a piece of music with it.** *Open - P6-1.* No person
      has used the build yet; this box is the user's to check and cannot be closed by
      an agent. Everything it needs is in place (`adx gui`).
- [x] Every instrument and effect from Phase 4 is editable.
      `test_generic_editor_covers_all_instruments` and `..._effects`: all 11
      instrument types the catalog lists that take parameters, and all 27 effect types,
      show a control for every value parameter (`testtone` has none and says so). Nine
      instruments and nine effects add a hand-built overview tab beside the full grid
      (§4.4's table; §10 below).
- [x] Mixer holds 60 fps with 200 inserts metering, real hardware, Phase 5's protocol
      (`test_perf_mixer_gpu`, `tests/python/perf_mixer_driver.py`): 1,189 frames with the whole application up and the transport playing: frame time (the UI frame step plus the meter view's sync and render) p50 0.29 ms, p99 **0.63 ms**, max 2.1 ms; displayed frames vsync-locked at **60.0 fps**, interval p99 18.7 ms, max 20.6 ms - not one missed vsync; exactly **1 engine call per frame**; every strip's meter moved. (The first run failed this box - see §10, correction 14.)
- [x] Undo works from every panel: `test_undo_to_empty_from_every_panel` - a seeded
      walk of 80 UI operations over thirteen kinds (rack channels and steps, playlist
      placement and drags, inserts, slots, knob turns, automation from a knob,
      selection delete and duplicate, channel volume), undone to empty through Edit >
      Undo - byte-identical to the start - and redone to the end, byte-identical to
      the finish. It found two defects (§10, corrections 3 and 4).
- [x] Layout persists and resets: `test_layout_persists_and_resets` (a floated mixer,
      a hidden panel and a moved browser come back in a second window over the same
      settings; reset docks every panel in its default area).
- [ ] All Phase 5 CI checks still pass with ~20 more panels in the tree. *Locally:*
      every one does - 337 C++ cases on each of Debug, RelWithDebInfo and Release;
      clang-tidy clean over all 232 files on all three compile databases; clang-format,
      ruff, ruff format, mypy `--strict` (108 files), headers, format-safety, positions,
      dsp-math and `rules` (now 6 checks, `no_panel_caches_project_state` among them);
      pytest 156 tests green, both GPU tests included (154 without them, as CI runs). The slow suite in Debug with full evidence: passed in 944.11 s. *In
      CI:* P6-0.
- [x] FINAL_PLAN.md §10 Phase 6 row updated.

### The §5 tests, where they live

| §5 test | Where |
|---|---|
| `playlist_place_pattern`, `playlist_edit_propagates`, `playlist_slip_and_stretch`, `playlist_lod_switch` | `tests/python/panels/test_panel_playlist.py` (and the C++ `playlist_slip_and_stretch`, `playlist_lod_switch`) |
| `channel_rack_step_roundtrip`, `channel_rack_grid_is_cpp` | `test_panel_channel_rack.py` |
| `mixer_meters_one_ffi_call`, `mixer_slot_reorder`, `mixer_routing_prevents_cycle` | `test_panel_mixer.py` (and the C++ `mixer_slot_reorder_keeps_nodes`) |
| `generic_editor_covers_all`, `generic_editor_units` | `test_panel_generic_editor.py` |
| `automation_uses_engine_curve`, `automation_drag_is_one_command` | `test_panel_automation.py` and the C++ `automation_uses_engine_curve` |
| `controller_lfo_drives_param` | C++ `tests/cpp/render/test_phase6_engine.cpp` (a render), and the editor in `test_panel_automation.py` |
| `browser_drag_creates_command` | `test_panel_browser.py` - every payload x target pair, one step each, undone byte for byte |
| `history_jump` | `test_panel_history.py`, clicking rows with the mouse |
| `selection_uniform_ops` | `test_panel_selection.py`, parametrised over all six kinds |
| `clipboard_is_adx_text` | `test_panel_selection.py` |
| `actions_registry_complete` | `test_panel_actions.py` |
| `no_panel_caches_project_state` | `tools/lint.py rules` (a CI step) |
| `perf_geometry_budgets` (extended) | C++ `perf_geometry_budgets_phase6` |
| `test_workflow_end_to_end` | `tests/python/test_workflow_end_to_end.py` |

---

## 7. Out of scope (and who owns it)

| Thing | Owner |
|---|---|
| The text editor panel, hot reload, scripting console | Phase 7 |
| Recording UI, clip destructive editing, the full export dialog | Phase 8 |
| Plugin browser entries, plugin windows, MIDI learn *wiring* (the right-click entry exists and is disabled) | Phase 9 |
| Spectrum/waterfall/particle panels, LUFS meter panel, vectorscope | Phase 10 |
| Session view | Phase 11 |

---

## 8. Handoff to Phase 7

Phase 7 inherits a complete GUI, which is exactly what it needs: bidirectional
sync is only meaningful when there is a GUI to sync *with*.

Specifically:

- Every mutation already goes through a command, so Phase 7's text→model path
  can reuse the same command set and Phase 7's model→text path has a complete,
  authoritative set of changes to observe.
- `CommandStack::revision()` is the change signal the file writer debounces on.
- The `DockPanel` base and the docking shell mean the editor panel is a new
  directory, not new infrastructure.
- The selection model gives the editor somewhere to reflect cursor position
  (select a note in the piano roll → highlight its line in the text).
- The generic parameter editor's `ParamRef` plumbing is the same addressing the
  text format uses, so a text edit to `insert.2.slot.1.mix` and a knob turn are
  provably the same operation.

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| Sprawl — the phase never converges | §3's build order makes the app usable after step 3; the long tail of specific editors is strictly last and each one is independently shippable |
| Panels start caching project state for speed and then disagree | `no_panel_caches_project_state` CI check; geometry builders exist precisely so that reading from the engine is already fast |
| The generic editor is "technically complete" but unpleasant, and everything needs hand-building | Evaluated after step 4 with a real user session, before committing to the long tail. If it is inadequate, the fix is better `ParamDescriptor` metadata (groups, layout hints), not thirty bespoke editors |
| Mixer metering at 200 inserts becomes the new frame-rate problem | One ring read + one geometry build + one upload per frame, budgeted in CI, with the same call-count assertions as Phase 5 |
| Keyboard shortcuts conflict and are discovered by users, not tests | `actions_registry_complete` checks for duplicate bindings; the keymap is user-remappable from the start |

---

## 10. Corrections made while executing this plan

Most of the code was written in an earlier session (2026-10-06) that stopped before
testing or committing it; this phase's agent (2026-10-20) found it building and its C++
tests green, wrote the whole Python test layer, the phase gate and the GPU driver, and
fixed what those found. What changed against the plan:

1. **One geometry item, not one per panel.** §2 lists `PlaylistItem.h/.cpp`,
   `AutomationItem.h/.cpp` and a `view.qml` per panel. There is one `GeometryItem`
   (`bindings/qml/GeometryItem.*`) with declared layers and one `panels/geometry.qml`,
   hosted by `panels/geometry_view.py`; the playlist, step grid, automation lanes and
   meters each declare their layers. Same pipeline as Phase 5, a quarter of the C++.
2. **The meters build inside the frame's one call.** §4.3 reads a ring, then builds.
   `Engine.frame_with_meters` reads every level *and* builds all strips' vertices in
   the one engine call the frame clock makes; the mixer uploads the result. Phase 5's
   "meters" panel is folded into the mixer.
3. **A press seals coalescing.** Phase 2's coalescing is a 500 ms window, so two quick
   gestures (three quick clicks in a lane; a drag straight after another) merged into
   one undo step. `CommandStack::sealCoalescing()` (tested: `a sealed gesture does not
   coalesce into the last`) is called on every press: `GeometryView.gesture_started`
   and the piano roll's for the hot surfaces, an application event filter for every
   other widget, and before discrete toolbar operations. Within a gesture, commands
   still merge.
4. **An automation drag lost its selection at the first write.** The panel rebuilds its
   lane sources after every change and the editor compared them by identity, so a
   multi-point drag moved its points for one frame and then wrote the originals back.
   Lanes now compare by label.
5. **Attributes shadowed Qt methods**: `MainWindow.actions` (now `registry`), the rack's
   and mixer's `scroll`, the EQ view's `geometry`, the VA pad's `x`/`y`. mypy `--strict`
   (never run on this code before) found them, with 60 other typing gaps; all fixed.
6. **Rows and strips were rebuilt wholesale** on every channel or insert change -
   O(N^2) widgets when adding N, 113 s for the 64-channel test. Both sync
   incrementally now (the mixer also stopped appending a layout stretch per sync).
7. **P5-1 was only half done**: the rack and instrument panel auditioned, the piano
   roll - where the issue was raised - did not. A note pressed in with the draw tool
   sounds once, a dragged note each new pitch once, a clicked key once
   (`test_panel_piano_roll.py`); the engine side is `audition_sounds_without_the_transport`.
8. **Slot mix is not a descriptor.** `insert.N.slot.M.mix` is a slot field, shown in the
   effect panel's header, not in the generic grid; the plan's examples implied it was
   an effect parameter.
9. **The clipboard spells pitches as names** (`C4`), as the writer does; anything
   pasting adX text must expect that.
10. **Hand-built editors** (§4.4): instruments - additive (harmonics), sampler and pool
    and slicer (zones), FM (algorithm), wavetable (frames; a 2-D frame strip, not the
    3-D view the table names), VA (filter pad), DrumSynth and Granular (envelopes);
    effects - EQ and ParametricEq (curve and live spectrum), Compressor, Multiband and
    Limiter (transfer curve and gain reduction), Convolution (IR), GrossBeat (time
    map), Delay (taps), Reverb (decay). Each is an overview tab beside the full generic
    grid, so hand-building never hides a parameter.
11. **Tests layout.** No `tests/python/panels/conftest.py` (two conftests of one name
    collide without packages): the `studio` fixture is in `tests/python/conftest.py`,
    and `tests/python/ui_input.py` delivers pointer input through a view's `forward`
    object - the same entry point QML uses - so a test press is a gesture exactly as a
    user's is.
12. **clang-tidy had never run on this code**: 59 findings, fixed; the four binding
    registration functions over the 150-line limit were split by subject
    (`instrumentCommands`, `placementCommands`, `routingCommands`,
    `defineSessionQueries`, `defineDuplication`).
13. **Open items closed with tests that were missing**: P5-2 (curved slides drawn
    through `core::Curve`, `geometry_curved_slide_follows_curve`), P5-4 (an audio clip
    in the playlist draws through the peak cache and redraws when reversed,
    `test_playlist_audio_clip_draws_its_waveform`), P4-2 (a sampler preset dropped on a
    channel plays, `test_a_sampler_preset_dropped_on_a_channel_plays`).
14. **The tempo box cost a frame every half second, app-wide.** The first GPU run of
    the mixer failed: frame work p99 0.47 ms, yet 2 % of frames missed their vsync, one
    every 31 frames. Every miss was the event loop blocked for one frame before the
    step - the 500 ms caret blink of the transport bar's tempo spin box, which took
    focus at startup: in a window compositing Qt Quick views, each caret repaint is a
    full flush that waits on the vsync. The box now takes focus only on a click and
    returns it when edited, and the window starts on the playlist canvas
    (`test_the_window_does_not_start_in_a_text_field`). Phase 5's measurement could not
    see this: it drove the roll in a bare window, not the application. The mixer driver
    drives the whole application.
