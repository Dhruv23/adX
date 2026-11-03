# Architecture

A map for contributors: how adX is split, the rules that keep the split working, and
where each part lives. The reasoning behind each decision is in
[FINAL_PLAN.md](../FINAL_PLAN.md) and the phase files in [plans/](../plans/); this page
points at them rather than repeating them.

## The split

The engine is C++20 and the app is Python 3.12 with PySide6 (Qt 6). They meet in one
pybind11 module, `adx_engine`, and on the Python side in one file,
`app/adx/engine_bridge.py`, the only module allowed to import it.

```
app/        Python: windows, panels, menus, tools, the scripting console
  └── engine_bridge.py   the one door into the engine
bindings/   pybind11: thin, one file per area, GIL released for real work
engine/     C++: the project model, the audio graph, DSP, file formats, geometry
```

The hot surfaces (piano roll, playlist, mixer meters, waveforms, analyzers) are drawn
on the GPU by Qt's scene graph from vertex buffers the engine builds. Python decides
*what* to show; C++ builds it in one call, and Python hands the buffer to Qt without
copying.

## The three rules

FINAL_PLAN §2.2. They are what makes Python a safe choice for a DAW's UI, and
`tools/lint.py rules` checks them on every push.

1. **The audio thread never touches Python.** Nothing below the bindings calls back
   into Python, and no Python object is reachable from `process()`.
2. **The boundary is crossed per interaction, never per note.** Dragging a note is one
   call; drawing 10,000 notes is one call returning a numpy view. A binding that returns
   a list of per-note objects is a bug.
3. **Every engine call that does real work releases the GIL** (`py::gil_scoped_release`),
   and a binding that does not says why in a `// GIL: trivial` comment.

The audio thread has rules of its own (phase_1.md): no allocation, no locks, no system
calls. Files that run on it start with `// adx-thread: realtime`, which puts them under
a stricter clang-tidy profile, and in Debug an allocator hook counts any violation at
run time.

## The engine, by folder

| Folder | What lives there | Plan |
|---|---|---|
| `rt/` | the realtime toolkit: rings, the block arena, the reaper, the allocator guard | phase_1 |
| `audio/` | device backends (WASAPI via RtAudio, exclusive, null, offline), the callback, the taps | phase_1, 8 |
| `core/` | ids, ticks, units shared by everything | phase_2 |
| `project/` | the project model and its commands; every edit is a command, every command undoes | phase_2 |
| `format/` | the `.adx` parser and writer (v2, and the v1 shim), audio file I/O | phase_2, 8 |
| `graph/` | the compiled audio graph, scheduling, snapshots swapped between blocks | phase_3 |
| `transport/` | time: tempo maps, loops, the playback position (the only owner of one) | phase_3 |
| `instruments/`, `effects/`, `dsp/` | the built-in sounds; `dsp/Math.h` keeps renders identical across builds | phase_4 |
| `clip/`, `record/` | audio clips, warping, recording and comping | phase_8 |
| `render/` | offline render and export, the render engine | phase_3, 8 |
| `geometry/` | vertex builders for every GPU-drawn surface, and the analyzer hub | phase_5, 10 |
| `plugin/` | VST3 and CLAP hosting, out of process, with the scanner and watchdog | phase_9 |
| `midi/` | MIDI input, learn, routing | phase_9 |
| `sync/`, `script/` | the text layer: file watching, incremental parse, mini-notation | phase_7 |
| `analysis/` | key, chords, tempo, timbre, form, melody (ONNX, optional) | phase_10 |
| `generate/` | the seeded genre suites | phase_10 |
| `preset/` | presets and the bundled packs | phase_4 |

## The app, by part

| Part | What it does |
|---|---|
| `application.py`, `mainwindow.py` | startup, the window, the dock layout |
| `session.py` | the open project and its undo stack, as Qt signals |
| `menus.py`, `actions.py` | the action registry: every menu item and shortcut, by id |
| `frame_clock.py` | the 60 Hz frame: one engine call that returns everything a frame draws |
| `panels/` | one folder per panel; each is a `DockPanel` |
| `text_sync.py`, `text_layer.py` | the file on disk and the model kept in step |
| `scripting/` | the Python API the Console and scripts use ([scripting.md](scripting.md)) |
| `control/`, `testing/` | the control channel and the driver that runs the app from outside ([agent-testing.md](agent-testing.md)) |
| `cli.py` | `adx`: validate, format, diff, render, analyze, and the rest, without Qt where it can |

## How a change reaches the speakers

1. A gesture in a panel (or a script, or a save from another editor) builds a
   **command** through the session.
2. The command edits the project model on the main thread and goes on the undo stack.
3. The engine compiles a new **snapshot** of what plays, off the audio thread.
4. The audio thread swaps to it between two blocks. Nothing it uses is ever freed under
   it: the reaper frees the old snapshot later, on the main thread.
5. The next frame-clock tick reads the new state back in one call, and the panels
   redraw.

## Tests

- `tests/cpp/`: Catch2, by area. `ctest --preset windows-x64-debug` runs them; the slow
  ones (`[.slow]`: the golden corpus in every format, long renders) run in the full check.
- `tests/golden/`: projects whose renders are hashed. A change that alters a hash is
  audible and has to be re-blessed on purpose (`ADX_UPDATE_GOLDEN=1`).
- `tests/python/`: pytest, the app headless on Qt's offscreen platform.
- `tests/scenarios/`: the app launched through `adX.cmd` and driven from outside.
- `bench/`: the optimization pass's benchmark (phase_10.md §4.5).

Builds use CMake presets (`windows-x64-debug`, `-relwithdebinfo`, `-release`); the
Python package builds the same tree through scikit-build (`pip install -e .[dev]`).
