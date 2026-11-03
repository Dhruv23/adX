# Phase 10.5 — Agent-run testing · L

| | |
|---|---|
| **Status** | Built 2026-11-03 (every §5 test observed locally); CI and the full check open (STATE.md P10.5-0) |
| **Governs** | `app/adx/control/` (the app's control channel), `app/adx/testing/` (the driver library), the new `adx` CLI commands below, the engine's capture tap and xrun counter, `tests/scenarios/`, `tools/fetch_test_plugins.py`, `tools/lint.py plans` |
| **FINAL_PLAN refs** | §9 "Every phase-ending check is agent-run" and "Hardware testing is done at the end"; §7 Phase 10.5 |
| **Entry criteria** | Phase 9 built (the plugin sandbox exists). It does not wait for Phase 10 and may run beside Phase 10's remaining work |
| **Next** | [phase_11.md](phase_11.md), which needs this phase done as well as Phase 10 |
| **§5 coverage owned** | none. This phase adds no user-facing feature; it makes the existing ones testable without the user |

---

## 1. Objective

Until now some phase-ending checks could only be done by the user sitting at the
app: load three real plugins, crash one live during playback, edit the same file in
adX and VS Code while it plays, perform a set on the emulated pad. These checks test
the software, not anyone's taste, so under FINAL_PLAN §9's new rule (2026-11-01)
an agent has to be able to run them alone.

This phase builds what that needs:

- a **control channel** into the running app, so an agent can launch adX the way the
  user does (`adX.cmd`), click, drag, type, trigger actions and read back state;
- a **driver library** (`adx.testing`) and a thin **`adx drive`** CLI over it;
- **audio observation**: a master capture tap, the xrun count exposed to Python, and a
  glitch detector, so "no glitches" is a measurement rather than a report;
- **headless CLI commands** for things that only had a GUI path;
- **scenarios** in `tests/scenarios/`, one per check that used to need the user, each
  closing or standing in for that check;
- a **lint gate** that fails if a phase's definition of done asks the user for a
  software check.

Judgement stays with the user. Listening sessions, approving a pack, "does this
sound like a hardstyle kick", the A/B verdicts: these are not tests of the software
and remain human gates (FINAL_PLAN §9). Hardware halves stay deferred to the final
review. What changes is that the software underneath those gates is checked by an
agent before the user is asked anything.

---

## 2. Deliverables — exact file manifest

```
app/adx/control/
  server.py         QLocalServer on a per-launch pipe name; JSON lines; off unless asked for
  protocol.py       request/response types, protocol version, error codes
  inputs.py         synthesized mouse/keyboard/drag events at a widget's coordinates
  inspect.py        the widget tree: objectName, class, geometry, visible, enabled, text, value
  probes.py         state readers: transport, project text hash, dirty flag, undo depth,
                    notices, panels open, plugin instances, MIDI ports

app/adx/testing/
  driver.py         Driver: launch() through adX.cmd or `adx gui`, connect(), click(), drag(),
                    type(), key(), trigger(), wait_for(), screenshot(), capture(), close()
  glitch.py         click / dropout / repeated-block / discontinuity detector
  describe.py       audio descriptors: third-octave bands, LUFS, peak, crest, centroid,
                    attack/decay times, f0 track
  scenario.py       pytest fixtures: a launched app, a temp project, a capture, a report

app/adx/cli.py      new commands (§4.4): drive, plugins, analyze, describe, compare,
                    glitch, midi

engine/audio/       CaptureTap: the master bus into an OverwriteRing, drained on a worker
bindings/audio.cpp  Engine.xruns(), Engine.capture_start/stop

tests/scenarios/    test_build_track_through_gui.py      (P6-1 stand-in)
                    test_external_edit_while_playing.py  (P7-1 stand-in)
                    test_real_plugins.py                 (P9-2)
                    test_live_plugin_crash.py            (P9-3)
                    test_launcher_driven.py
tests/python/       test_control_channel.py, test_driver.py, test_glitch.py,
                    test_describe.py, test_cli_headless.py
tests/cpp/audio/    capture_tap tests

tests/scenarios/wannacry.py   the WannaCry corpus fixture (§4.8); test_wannacry_matches_reference.py
tools/extract_reference.py    the reference audio out of the .mp4 with ffmpeg, into output/reference/
tools/fetch_test_plugins.py   pinned open-source plugins, SHA-256 checked, cached outside git
tools/test_plugins.toml       the pin list
tools/lint.py                 new `plans` gate
tools/check_launcher.py       also connects the driver through the launcher

docs/agent-testing.md         how to drive adX: launching, the protocol, scenarios, evidence
```

---

## 3. Order of work

1. The capture tap and `Engine.xruns()`. The xrun count is also STATE.md P8-1's
   leftover ("a shift after calibration goes unnoticed").
2. `glitch.py` with its positive controls, before anything relies on it.
3. The control channel and `inspect.py`, then `inputs.py`.
4. `Driver`, and `check_launcher.py` driving through `adX.cmd`.
5. The headless CLI commands.
6. `fetch_test_plugins.py` and the real-plugin scenario (P9-2).
7. The live-crash scenario (P9-3), then the P6-1 and P7-1 stand-ins.
8. `tools/lint.py plans`, then the phase files it flags are fixed.
9. `docs/agent-testing.md`.

---

## 4. Design

### 4.1 What used to need the user, and what replaces it

| Check | Where | Was | Becomes |
|---|---|---|---|
| Three real third-party plugins load, automate, save, reload; the editor embeds or falls back | phase_9.md §6, P9-2 | the user names and tries them | `test_real_plugins.py` over pinned open-source plugins (§4.6) |
| A plugin crashing during playback leaves the rest playing, shown live | phase_9.md §6, P9-3 | the user sets `mode` to 1 while playing | `test_live_plugin_crash.py`: the app launched through `adX.cmd`, playing, the knob turned through the GUI, the notice read back, Restart clicked, the capture checked (§4.5) |
| A track can be made in the app | phase_6.md §6, P6-1 (closed by the user) | the user made `wanna_cry_human.adx` | a stand-in, `test_build_track_through_gui.py`, so the final review can re-check it without the user. The user's own verdict stays the record of the original close |
| Edit in adX and an external editor while it plays, no glitches | phase_7.md §6, P7-1 (closed by the user) | the user edited in VS Code | a stand-in, `test_external_edit_while_playing.py` (§4.5) |
| A 20-minute set on the emulated pad; the performance walkthrough | phase_11.md §6 | the user performs | scripted in Phase 11 on this phase's driver and `adx midi play` (phase_11.md §6) |
| The MSI on a clean VM | phase_10.md §7 | unspecified | a fresh GitHub-hosted Windows runner in the full check (this machine is Windows 11 Home: no Windows Sandbox or Hyper-V) |
| Library fetch verified | phase_12.md §6 | "on the user's machine" | run by the agent on this machine |

Not on this list, on purpose: the listening sessions and the pack gates of Phases
12-19, and the judgement half of Phase 4's gates. Those are the user's (FINAL_PLAN §9).
Neither are the hardware halves in STATE.md's "Final review".

### 4.2 The control channel

`adx gui --control` (or `ADX_CONTROL=1` in the environment, which is how a launch
through `adX.cmd` asks for it) starts a `QLocalServer` on a pipe named
`adx-control-<pid>-<random>`. The name and a token are written to a file the launcher
passes back (`--control-file PATH`). With neither flag nor variable set there is no
server at all; `control_off_by_default` checks that. The pipe is the current user's
only (Qt's `UserAccessOption`), and the token is checked on connect.

The protocol is one JSON object per line, request and response, with a version number
in the handshake. It runs on the main thread from the event loop, so it sees what the
user would see and never touches the audio thread. Requests:

- `tree`, `find` — widgets by `objectName` path or by class and text;
- `click`, `double_click`, `drag`, `type`, `key`, `scroll` — synthesized input events
  delivered to the widget at its coordinates (`inputs.py`). These go through the same
  handlers a mouse does. A gesture never becomes a direct command call, because the
  wiring between panels is part of what is being tested;
- `trigger` — an action from the registry by id (`actions.py`), as a menu or shortcut would;
- `state` — the probes in `probes.py`;
- `wait_for` — a probe predicate with a timeout, so scenarios never sleep;
- `screenshot` — a panel or the window to PNG, recorded as evidence. Assertions use the
  tree and the probes, not pixels, except "the plugin editor drew something" (§4.6);
- `script` — run code in the scripting API (`adx.scripting`). Allowed only with the
  token, and only for setup and readback that has no GUI path;
- `capture_start`, `capture_stop` — the master tap to a WAV;
- `quit` — close as the user would, with the save prompt answered by the request.

Every panel's interactive widgets get stable `objectName`s. That is most of the work
in this phase and the reason it is sized L. A widget without one fails
`control_tree_names_stable` once it is reachable from a menu.

### 4.3 The driver library

`adx.testing.Driver` wraps the protocol for pytest and scripts:

```python
with Driver.launch(project=tmp / "song.adx", via="adX.cmd") as app:
    app.trigger("transport.play")
    app.wait_for("transport.playing")
    app.drag("browser/Presets/Delay", "mixer/insert:1/slot:0")
    app.wait_for(lambda s: s.undo_depth == 1)
    wav = app.capture(seconds=4)
```

`adx drive` is the same thing from a shell, one request per call against a running
app (`adx drive --control-file F click mixer/insert:1/mute`), for an agent poking at
a live session. Launch modes: through `adX.cmd` (what the user double-clicks, so
pathing and environment are covered), or `adx gui` directly. Audio is the real default
device or `--null-audio`. Locally scenarios run against the real device. VB-Cable can
be the output so nothing plays into headphones. In CI they run with null audio.

### 4.4 Headless CLI commands

| Command | Does |
|---|---|
| `adx drive …` | §4.3 |
| `adx plugins scan / list / probe PATH / render PROJECT` | scan into the cache, list it, load one in the sandbox and report its parameters, latency and whether it has an editor; render a project with plugins (already `adx render`, which this reuses) |
| `adx analyze key / chords / bpm FILE` | Phase 10's analyses, headless, JSON out. Phase 10 owns the analysis; this only exposes it |
| `adx describe FILE` | `describe.py`'s descriptors, JSON |
| `adx compare A B` | per-band and per-descriptor differences, exit 1 over a tolerance |
| `adx glitch FILE [--reference REF]` | §4.5; exit 1 on any finding, with sample positions |
| `adx midi ports / send / play SCRIPT` | list ports, send a message, play a timed script of MIDI messages into a port (loopMIDI locally). Phase 11's pad scenarios build on `play` |

Each has a test in `test_cli_headless.py`, exit codes included. None imports Qt
(the existing `cli_render_no_qt` rule extends to them).

### 4.5 "No glitches" as a measurement

A user reporting no glitches is listening for clicks, dropouts and stutter. The
software equivalents:

- **xruns**: `Engine.xruns()`, read before and after; any increase fails;
- **the capture**: the master bus copied into an `OverwriteRing` on the audio thread
  (no allocation; `capture_tap_no_alloc`) and drained to disk on a worker;
- **the detector** (`glitch.py`), on the capture:
  - a run of exact zeros inside non-silent material longer than 1 ms (a dropout);
  - a block repeated sample for sample (a stuck buffer);
  - a first-difference spike more than 8× the local RMS of the first difference (a click);
  - with `--reference`, the capture aligned to an offline render of the same project and
    compared. Bit-identical where the scenario allows it; otherwise the residual must sit
    below -90 dBFS outside the edit points the scenario names.

The detector is a gate, so it is seen to fire first: `glitch_positive_controls` plants
each defect in a clean render and requires each to be found at the right position, and
a clean render must yield nothing.

`test_external_edit_while_playing.py` is the model: the app plays a project. A separate
process saves the file 50 times, alternating an atomic rename (VS Code's way) and an
in-place truncate-and-write. It waits for each reload through `wait_for`. Then: zero
xruns, zero detector findings, the GUI's project text equal to the file after every
save, and one undo step per reload.

### 4.6 Real plugins without the user

`tools/test_plugins.toml` pins at least three open-source plugins by version, download
URL and SHA-256, chosen so that between them they cover a VST3 instrument, a VST3
effect, a CLAP instrument and a CLAP effect, and at least one has an editor. Starting
candidates (versions and hashes are fixed when the tool is written, and the list is
recorded in phase_9.md §10): Surge XT (instrument, VST3 and CLAP, GPL-3), Dexed
(instrument, VST3), Airwindows Consolidated (effect, VST3 and CLAP, MIT), and
ChowTapeModel (effect, VST3 and CLAP, BSD-3). `fetch_test_plugins.py` downloads them
into a cache outside the repository and refuses a hash mismatch. They are test fixtures,
never shipped or linked. A FINAL_PLAN §8 row records that.

For each plugin and format, `test_real_plugins.py`, through the GUI:

- scan, and find it in the Plugins panel;
- add it to a channel or insert by drag;
- automate one parameter with a lane;
- render twice and check the renders are non-silent and equal (where a plugin is not
  deterministic, the test says why and compares descriptors instead);
- save, close, reopen through `adX.cmd`, and check the state blob and named parameters
  survive and the render matches;
- open the editor. If it embeds, the plugin's child window exists and the screenshot of
  its area is not a single colour. If it doesn't, the generic editor shows a control for
  every parameter.

Marked `[realplugin]`: run locally and in the full check, which caches the fetch.

`test_live_plugin_crash.py` loads `adx_test_plugin` and one real plugin on separate
channels and plays. It turns the fixture's `mode` knob to 1 through the generic editor
with synthesized input, then checks:

- the crash notice appears (probe);
- the other channel's stem in the capture matches its offline render throughout (§4.5);
- xruns stay at zero;
- after Restart is clicked the fixture's path is audible again.

### 4.7 The `plans` lint gate

`tools/lint.py plans` reads every unchecked `- [ ]` box in each phase file's
definition of done. It fails on a box that asks the user to perform a check
("the user", "a human", "needs the user", "demonstrated live", "on the user's
machine") unless the box carries one of two tags:

- `(hardware half)` — deferred under §9;
- `(judgement)` — a listening, approval or taste gate.

Boxes already checked are history and are left alone. The gate is seen to fire on a
planted box before it is trusted. The first run fixes, or tags, every box it flags in
phases 9-19.

### 4.8 The music the agent writes: WannaCry

Any test where the agent writes music uses *WannaCry* (Ninajirachi & Porter Robinson).
The user set this on 2026-11-01. There are three sources, all in the repository root's
working tree and all excluded from git (`.git/info/exclude`). They are never committed,
and neither is anything rendered or extracted from them:

| Source | Path | What it is |
|---|---|---|
| the user's MIDI | `wanna_cry_human.adx` | the user's own entry: one `Lead` channel, 4 bars, 28 notes, 120 bpm |
| the agent's piano transcription | `output/transcription/wannacry_piano.adx` | `RightHand` and `LeftHand`, 1,715 notes, 3:58 at 126 bpm |
| the reference audio | `Ninajirachi & Porter Robinson - WannaCry (Piano Tutorial).mp4` | a piano-tutorial video; `tools/extract_reference.py` pulls its audio with ffmpeg (installed through winget) into `output/reference/wannacry.wav` |

`tests/scenarios/wannacry.py` is the fixture that finds them. `ADX_WANNACRY_DIR`
overrides the root. Each scenario takes the corpus as its material:

- **`scenario_build_track_through_gui`** (P6-1's stand-in) writes WannaCry into an empty
  project through the GUI:
  - the user's 4-bar melody, note by note in the piano roll by synthesized input;
  - an 8-bar section of the transcription, both hands on two rack channels, by the same
    route;
  - the rest of the transcription by Edit > Paste of its `.adx` text, which exercises
    the clipboard path.

  Then it arranges, mixes and exports. `adx diff` against the sources must show no note
  differences, and the render must be bit-identical after save and reopen.
- **`scenario_external_edit_while_playing`** (P7-1) plays `wannacry_piano.adx`. The
  external writer's 50 edits are musical ones on that file: transpose a bar, move a
  phrase, rewrite a left-hand pattern, change the tempo.
- **`scenario_real_plugins`** (P9-2) plays the transcription's two hands through each
  instrument plugin, and through each effect plugin on an insert.
- **`scenario_live_plugin_crash`** (P9-3): WannaCry plays on the channels that do not
  crash. Their stem is what must match its offline render.
- **Phase 11's `scenario_scripted_set_20min`** cuts its launchable clips from the
  transcription's sections. The user's melody is one more clip.
- **`scenario_wannacry_matches_reference`** checks that what the agent wrote is the song,
  by comparing the render with the reference audio:
  - BPM from `adx analyze bpm` within 1 of 126 on both;
  - the same key from `adx analyze key` on both;
  - per-bar chroma correlation, after aligning the render to the reference, above a
    threshold fixed when the test is first run and recorded in §10.

  This is a software check of the agent's transcription and entry, not a listening
  verdict.

The corpus lives only on this machine, so CI can't run these scenarios. Each one is
therefore parametrized:
- **in CI:** a committed generated project (`docs/examples/` or a Phase 10 genre suite
  track), so the GUI and audio paths are still exercised there;
- **locally:** the WannaCry corpus, with the label `[wannacry]`.

A `[wannacry]` scenario that skips because a file is missing counts as not run. Phase
completion needs the local WannaCry run, recorded in §10.

---

## 5. Tests

| Test | Asserts |
|---|---|
| `capture_tap_no_alloc` | 60 s of capture under the allocator hook: 0 violations |
| `capture_tap_matches_offline` | a capture with null audio equals the offline render of the same project, sample for sample |
| `xruns_exposed` | an induced overload raises `Engine.xruns()`; a clean run leaves it at 0 |
| `glitch_positive_controls` | each planted defect (dropout, repeated block, click, residual over the threshold) is found at its position; a clean render yields nothing |
| `describe_known_signals` | a sine's centroid and f0; LUFS against EBU TECH 3341's cases; the attack and decay of a known envelope |
| `control_off_by_default` | no pipe exists without `--control` or `ADX_CONTROL` |
| `control_handshake` | version and token checked; a wrong token is refused |
| `control_input_is_real` | a driver click and drag produce the same undo entries as the in-process pytest-qt test of the same gesture |
| `control_actions_cover_registry` | every registry action is listed and triggerable |
| `control_tree_names_stable` | every interactive widget reachable from a menu has an `objectName`, and the names survive a relaunch |
| `control_wait_for_times_out` | an unmet predicate fails at its timeout with the last state in the message |
| `cli_headless_commands` | each §4.4 command, exit codes included; none imports Qt |
| `launcher_driven` | `adX.cmd` with `ADX_CONTROL=1`: the driver connects, opens a project, plays, quits cleanly |
| `scenario_build_track_through_gui` | P6-1's stand-in: WannaCry written into an empty project through the rack, piano roll, playlist, mixer and export, all by synthesized input (§4.8); no note differences from the sources; save, reopen, render bit-identical |
| `scenario_external_edit_while_playing` | P7-1's stand-in, §4.5, on `wannacry_piano.adx` (§4.8) |
| `scenario_real_plugins[plugin-format]` | P9-2, §4.6, playing WannaCry (§4.8) |
| `scenario_live_plugin_crash` | P9-3, §4.6, with WannaCry on the surviving channels (§4.8) |
| `scenario_wannacry_matches_reference` | the agent's WannaCry against the reference audio: BPM, key, aligned per-bar chroma (§4.8) |
| `plans_gate_fires` | `tools/lint.py plans` fails on a planted untagged box and passes on the tree |

---

## 6. Definition of done

- [ ] Every §5 test passes locally on Debug, RelWithDebInfo and Release; the
      scenarios also pass in the full check (null audio, plugins from the cache, the
      committed stand-in project), and locally on the WannaCry corpus (`[wannacry]`,
      §4.8) with nothing skipped.
- [ ] P9-2 and P9-3 closed by `scenario_real_plugins` and `scenario_live_plugin_crash`,
      the plugin list and output recorded in phase_9.md §10.
- [ ] The P6-1 and P7-1 stand-ins pass, so the final review can re-check those phases
      with an agent alone.
- [ ] `tools/lint.py plans` is a CI step, seen to fire, and clean on the tree.
- [ ] `docs/agent-testing.md` written; its examples run in the test suite.
- [ ] `check_launcher.py` drives the app through `adX.cmd`.
- [ ] FINAL_PLAN §8 row for the fetched test plugins; FINAL_PLAN §10 and STATE.md updated;
      launcher rebuilt and checked.

---

## 7. Out of scope (and who owns it)

| Thing | Owner |
|---|---|
| The pad-controller scenarios (20-minute set, walkthrough) | Phase 11, on this phase's driver and `adx midi play` |
| The analyses behind `adx analyze` | Phase 10 |
| The installer's clean-machine job | Phase 10 (a fresh hosted runner, §4.1) |
| Listening sessions, pack approval, A/B verdicts | the user (FINAL_PLAN §9, "judgement") |
| Hardware halves | the final review |
| Pixel-golden screenshot tests | not planned: brittle across DPI and themes; the tree and probes are the assertions |

---

## 8. Handoff to Phase 11

Phase 11 gets a driver that can launch the app, play, click a session grid and capture
the result; `adx midi play` to feed an emulated pad from a script; and the glitch
detector to turn "performed with no glitches" into a number. Its §6 boxes are written
against them.

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| Synthesized input misses what real input exercises (OS drag-and-drop, native plugin windows) | browser drags go through Qt's drag machinery, not a shortcut; native plugin windows are checked by window handle and screenshot only; what is not covered is written in each scenario's docstring |
| Scenarios are slow and flaky | `wait_for` with timeouts instead of sleeps; the slow ones labelled and run in the full check, not per push; a flake is an open issue, never a retry loop |
| A real plugin's download disappears or changes | pinned hashes; the cache in CI; a replacement is a recorded change to the pin list |
| The control channel becomes a backdoor | off by default, per-user pipe, token, no network socket |
| The plans lint gate gets tagged around | a `(judgement)` tag on a box that tests software is a review finding; the tag list is small on purpose |

---

## 10. Evidence and progress

### What exists (built 2026-11-01/02, recorded 2026-11-03)

Every §2 file is in the working tree, uncommitted: the control channel
(`app/adx/control/`), the driver library (`app/adx/testing/`, plus `midi.py` and
`wav.py`), the §4.4 CLI commands, the capture tap (`engine/audio/CaptureTap`,
`bindings/capture.cpp`) and `Engine.xruns()`, the six scenarios and the WannaCry fixture
(`tests/scenarios/`), `tools/fetch_test_plugins.py` with `tools/test_plugins.toml`,
`tools/extract_reference.py`, the `plans` gate (a CI step), `docs/agent-testing.md`, and
`check_launcher.py` driving the app through `adX.cmd`. The session that built them left
this section empty; the 2026-11-03 session below verified them and fixed what failed.

The pinned plugins (also FINAL_PLAN §8): Dexed 1.1.0 (VST3, CLAP), Surge XT 1.3.4 (VST3,
CLAP, and its Effects bundle in both), Airwindows Consolidated 2026-10-31-d013e10 (VST3,
CLAP).

### Session of 2026-11-03

Observed locally (Windows 11, this machine):

| Check | Result |
|---|---|
| ctest `-LE slow`, Debug / RelWithDebInfo / Release | 489 / 489 each |
| `ruff check`, `ruff format --check` (CI's paths), `mypy --strict` (176 files) | clean |
| `tools/lint.py` format, headers, format-safety, positions, dsp-math, rules, plans | clean |
| clang-tidy on the changed C++ (`ClapModule.cpp`), all three databases | clean |
| `scenario_live_plugin_crash`, stand-in and WannaCry | pass |
| `test_real_plugins[Airwindows Consolidated-clap-*]` before the knob fix | 1 hang in 4 runs, 2 failures |
| `pytest tests/scenarios` after the fixes (stand-in and WannaCry) | 26 passed, 0 skipped |
| `CI=1 pytest tests/python` | all pass (3 skips: ONNX and hardware) |
| `ADX_FULL_EVIDENCE=1 ctest --preset windows-x64-debug -L slow`, idle | passed, 1797 s (STATE.md P10.5-4) |
| `check_launcher.py` through `adX.cmd`, driven | 24 panels; played and captured 48,384 frames, 0 xruns; quit cleanly |
| per-push CI, the full check (with its new `scenarios` job) | not run: nothing is committed (STATE.md P10.5-0) |

What failed and was fixed:

- **The configure was broken.** The licence check added for the installer (phase_10.md
  §4.8) wrote its regex as `"FetchContent_Declare\(..."`; a quoted CMake argument unescapes
  `\(` to `(`, so the regex did not compile and every configure stopped at
  `AdxDependencies.cmake:459`. The builds since 2026-11-03 01:04 had been run from stale
  build trees. Now `\\(` and `\\|`; seen to pass a declared licence and fail a missing one.
- **A plugin could stop the app behind a dialog.** Airwindows Consolidated reports
  uninitialised defaults for its unused parameter slots (3.1e32, 1.8e25, and different
  values on each run). `Knob.__init__` stored the value unclamped, so `fraction()` came out
  near 3e32 and `paintEvent`'s `drawArc` raised `OverflowError`. The excepthook's modal box
  then held the app until the scenario timed out. That was the intermittent hang and
  "closed the control channel". The knob now clamps on construction and in `fraction()`,
  and plugin descriptors clamp a default into range (a non-finite one to the minimum).
  Tests: `test_knob_holds_an_out_of_range_value` (seen to fail without the fix) and
  `test_plugin_descriptor_defaults_stay_in_range`.
- **Surge XT and Dexed are not deterministic**, so `test_real_plugins` compares their
  renders by descriptors (§4.6 allows this when the test says why). Measured on two renders
  of one saved project: Surge XT differs from 1.7 ms onward, by up to 0.64, because of its
  random oscillator phase and drift. Dexed differs by up to 2.4e-5 (-92 dBFS), in under
  0.2 % of the samples. Their descriptors are equal.
- **The crash scenario's survivor** was Dexed, whose jitter crossed the -90 dBFS residual
  (-86.5 dBFS in one block). It is now the channel's own instrument with Airwindows
  Consolidated (CLAP) on its insert, running Density through its state. The default,
  Galactic, is a modulated reverb whose LFO keeps running while the transport is stopped,
  so a live take never matches an offline render (measured -30 dBFS apart from the first
  block). A processor named without its knobs gets zeros: PurestGain and Tube2 went silent,
  Galactic transparent.
- **The control server started before the window's first layout pass.** `showEvent`
  queues `_size_panels`; a driver that connected sooner, under load, read a tree whose
  tab bars had not settled yet (`test_control_tree_names_stable` failed in the full run).
  The server now starts from the event loop, after that pass.
- **Stale tests.** The widget names became full parameter paths (`param:insert.1.slot.2.feedback`)
  for the control channel, and three generic-editor tests still looked up the short
  names. They now read the editors' `controls`.
- **Lint.** 36 ruff findings and 41 `mypy --strict` errors in the uncommitted Phase 8-10.5
  code, each of which would fail per-push CI. Among them, three real ones: `QComboBox`
  attributes named `window`, `size` and `style` replaced those QWidget methods on the
  scope, spectrum and genre-suite panels (renamed `window_box`, `fft_size`, `style_box`).
  The JSON-valued control-channel signatures use `protocol.Json`.
- **The full check gains a `scenarios` job** (§6's "in the full check"): Release, null
  audio, the plugin fetch cached on `tools/test_plugins.toml`'s hash, `-m "not wannacry"`.

Tried and reverted: calling the CLAP `params.flush()` only while the plugin is inactive
(CLAP says `[active ? audio-thread : main-thread]`; Airwindows logs the violation). With
the flush left to `process()`, Airwindows' post-load report of all 22 parameter values
reached the main thread as echoes, and the app recorded each one as a "Set effect value"
edit on open, so a reopened project no longer equalled its file. See P10.5-2.

## 11. Corrections made while executing this plan

- §4.6 named Surge XT, Dexed, Airwindows and ChowTapeModel as candidates. ChowTapeModel is
  not pinned; Surge XT's Effects bundle covers a second effect in both formats.
- §4.6 holds the surviving channel to its offline render. That works only for a plugin
  with no free-running state: a modulated effect (Airwindows' Galactic) or a randomized
  synth (Surge XT, Dexed) cannot match a live take sample for sample. The scenario
  docstring says which plugin it uses and why.
