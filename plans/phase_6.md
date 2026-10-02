# Phase 6 — The DAW proper · XL

| | |
|---|---|
| **Status** | Not started |
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

- [ ] `test_workflow_end_to_end` passes.
- [ ] A human has actually made a piece of music with it, start to finish,
      without opening a text editor, and the result is recorded in the phase log.
      The automated test proves the operations exist; a person proves they are
      *usable*, and those are different claims.
- [ ] Every instrument and effect from Phase 4 is editable.
- [ ] Mixer holds 60 fps with 200 inserts metering (measured on real hardware,
      same protocol as Phase 5 §6).
- [ ] Undo works from every panel, verified by a randomized UI-driven
      undo-to-empty test in the spirit of Phase 2's.
- [ ] Layout persists and resets correctly.
- [ ] All Phase 5 CI checks still pass with ~20 more panels in the tree.
- [ ] FINAL_PLAN.md §10 Phase 6 row updated.

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
