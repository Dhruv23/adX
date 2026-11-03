# Handoff: build and validate Phases 10, 10.5 and 11

## 0. Where things stand (2026-11-03): read this first

Phase 10.5 is **built and passes locally**. It isn't done, because CI hasn't seen it. The
evidence is in [plans/phase_10.5.md](plans/phase_10.5.md) §10 and §11 and in
[plans/STATE.md](plans/STATE.md). Nothing is committed since `be487fe`.

### Open bug: OverflowError dialog with Airwindows Consolidated (not fixed)

**What the user saw.** They ran the app through `adX.cmd`. A modal dialog appeared,
twice: `OverflowError: int too big to convert  Details are on stderr.` It came while
Airwindows Consolidated (CLAP) was on an insert with the Galactic processor loaded. The
generic editor's "Main" group showed:
- knobs for replace, brightness, detune, bigness and dry_wet;
- three knobs labelled `p2944917345` and so on;
- one blank knob;
- a combo showing `0`.

**What's known:**
- Airwindows Consolidated exposes 10 processor slots. Galactic uses five. The rest are
  named `-` and have no key, so `plugin_descriptors` labels them `p<id>`.
- Those CLAP param ids are 2944917340-2944917349, above 2^31-1. The plugin's own
  `bypass_` (2920836328) and `output_level` (3289450511) are above it too.
- Passing a value like that to a Qt API typed signed `int` raises OverflowError in
  PySide6. I checked this for `QSpinBox.setValue`, `QComboBox.setCurrentIndex` and
  `Signal(int).emit`.
- So the leading suspect is a plugin param id (a `uint32` in CLAP) reaching a signed-int
  Qt or pybind11 parameter. Places to look:
  - the echo path (plugin output param values → main thread → project edits);
  - the plugin window;
  - `Plugins.param_text` / `param_values`;
  - anything keyed by param id in a `Signal(int)`, item data or a spin box.
- The exact message, "int too big to convert", is the one CPython's `_PyLong_AsByteArray`
  produces (`int.to_bytes`, some pybind11 or numpy casts). PySide's own message in this
  build was empty. That points more at a pybind11 binding taking `int` or `int32_t`
  where it should take `uint32_t`. `grep` bindings/plugin.cpp and the param/echo code
  for the id type.
- **Not reproduced yet.** On null audio, `aw_repro.py` (below) did this with no error:
  1. added Airwindows to an insert;
  2. opened the slot in the Effect panel;
  3. turned every knob, the spare slots included.

  It probably needs the plugin live, on real audio, with its own editor or processor
  switch sending param echoes. Next step: the same repro through
  `Driver.launch(via="gui")` without `--null-audio`. Or load Galactic through the state
  blob (`airwindows_state` in tests/scenarios/test_live_plugin_crash.py) and open the
  plugin window.
- **A second problem:** under `pyw` (adX.cmd) there is no stderr, so the dialog's
  "Details are on stderr" sends the user nowhere. The excepthook in
  `app/adx/application.py:91` should also write the traceback to a log file, for example
  in the settings folder, and give that path in the dialog.
- **Cosmetic:** unused Airwindows slots (name `-`) should probably be hidden, or labelled
  by their name rather than `p<id>`.
- Earlier, related, and fixed: Knob and `plugin_descriptors` now clamp out-of-range
  defaults like the 3e32 Airwindows once reported (`test_knob_holds_an_out_of_range_value`).
  That fix only covered the paint path.
- Repro scripts, outside the repo, in the session scratchpad
  `C:\Users\dhruv\AppData\Local\Temp\claude\d--VS-Code-adX\632835d8-a990-4a94-8e71-bbfa8299e42e\scratchpad\`:
  - `aw_params.py` dumps the plugin's descriptors from the scan cache;
  - `aw_repro.py` is the in-process GUI repro. It sets `sys.excepthook` after
    `create()`, because the app's hook opens a modal dialog that hangs a script.

### Other open items
- **P10.5-0 (blocker):** CI and the full check for Phases 8-10.5 need a commit and push.
  **Ask the user first.** P8-0 and P9-0 wait on the same push. The new `scenarios` job in
  full-check.yml has never run in CI.
- **P10.5-2:** `ClapModule::idle()` calls `params.flush()` on the main thread between
  `activate()` and the first `process()`. CLAP wants it on the audio thread. The
  spec-correct move to `!m_active` was tried and reverted: on reopen it turned the
  plugin's post-load value report into 22 "Set effect value" undo steps. It needs echoes
  told apart from editor gestures first. This may be related to the bug above.
- **P10.5-3:** two fresh launches sometimes arrange the docks differently, which changes
  Qt's `window.tabs.*` tab bars. `test_control_tree_names_stable` now compares every
  interactive widget and leaves those tab bars out. The cause is unknown.
- **P10.5-4:** the Debug slow ctest suite took 1797 s against an 1800 s timeout. Under
  load it timed out and missed realtime deadlines. It needs a longer timeout or a split.
- **P10-5:** the CPU targets for `bench_full_project` are the user's decision. It blocks
  Phase 10 §3 step 7 and Phase 11.
- **P10-1:** key accuracy is 70 % against an 85 % target (see Phase 10 below).

### Verified locally on 2026-11-03, before the bug report
- ctest `-LE slow` passes 489/489 in Debug, RelWithDebInfo and Release.
- The Debug slow suite passes on an idle machine.
- `CI=1 pytest tests/python` passes.
- All 26 scenarios pass with none skipped, `[wannacry]` included.
- ruff, `mypy --strict` and all lint gates are clean, and clang-tidy is clean on
  ClapModule.cpp in all three databases.
- `check_launcher` passes: 24 panels, 48,896 captured frames, 0 xruns.

Also fixed this session: the CMake license-gate regex that broke every configure
(`cmake/AdxDependencies.cmake`).

---

Updated 2026-11-01. Nothing is committed since `be487fe` (Phase 7). The working tree holds:
- Phase 8, built but not done;
- Phase 9, built but not done;
- the start of Phase 10 (harmony analysis in the engine);
- this session's plan amendments (below);
- the user's own planning edits (FINAL_PLAN.md, plans/STATE.md, plans/phase_7.md,
  plans/phase_12-19.md).

Build on all of it and revert none of it. Read [plans/STATE.md](plans/STATE.md) top to
bottom before touching code; it's the ledger, and this file is only the way in.

---

## 1. What changed in the plan on 2026-11-01

Two rules from the user, both now in [FINAL_PLAN.md](FINAL_PLAN.md) §9.

### Every phase-ending check is agent-run

A check that tests the software has to be one you can run alone, end to end, with no one
at the keyboard. That includes checks that need:
- the GUI up;
- a launch through `adX.cmd`;
- panels and paths wired together;
- real plugins loaded;
- playback free of glitches.

Where a check used to need the user, build a software path and test through that. The
path can be a CLI command, the Phase 10.5 driver, or a capture plus a glitch measurement.

Two exemptions:
- **Hardware halves** are still deferred to the final review after Phase 19. Don't ask
  the user to run them.
- **Judgement gates** stay the user's, recorded in their own words: listening sessions,
  pack approvals, A/B verdicts. The user's words: "listening sessions or things that
  arent testing the software can remain human driven."

In a definition of done, a box that needs the user carries `(hardware half)` or
`(judgement)`. Any other box you can't run yourself is a plan bug. `tools/lint.py plans`
(built in 10.5) enforces this.

### Music-writing tests use WannaCry

Any test where you write music uses *WannaCry* (Ninajirachi & Porter Robinson):

| Source | Path |
|---|---|
| the user's MIDI | `wanna_cry_human.adx` (1 channel, 4 bars, 28 notes, 120 bpm) |
| your piano transcription | `output/transcription/wannacry_piano.adx` (2 hands, 1,715 notes, 3:58, 126 bpm) |
| reference audio | `Ninajirachi & Porter Robinson - WannaCry (Piano Tutorial).mp4`; extract with ffmpeg (installed through winget, on PATH) |

All three are excluded in `.git/info/exclude`. **Never commit them, or anything rendered or
extracted from them.** Their scenarios run locally under the `[wannacry]` label; in CI the
same scenarios run on a committed stand-in project. A `[wannacry]` skip counts as not run.
Details: [plans/phase_10.5.md](plans/phase_10.5.md) §4.8.

### Files edited

| File | Change |
|---|---|
| `plans/phase_10.5.md` | **new**: the agent-run testing phase (control channel, `adx.testing` driver, `adx drive`, capture tap, `Engine.xruns()`, glitch detector, headless CLI commands, scenarios, real-plugin fetch, `plans` lint gate, the WannaCry corpus) |
| `FINAL_PLAN.md` | §7: twenty-one phases, a Phase 10.5 summary, Phase 10's clean VM as a fresh GitHub-hosted runner (this box is Windows 11 Home: no Windows Sandbox or Hyper-V). §9: the two rules above, and the plan family now includes `phase_10.5.md`. §10: a 10.5 row; Phase 9 and 11 notes |
| `plans/STATE.md` | finish-a-phase step 1 states the rule; progress row and section for 10.5; P9-2 and P9-3 are agent-run blockers closed by 10.5's scenarios; Phase 11's entry needs 10.5; the final review re-checks P6-1 and P7-1 through stand-ins |
| `plans/phase_9.md` | P9-2 and P9-3 boxes point at `scenario_real_plugins` and `scenario_live_plugin_crash`; latency and ASIO boxes tagged `(hardware half)` |
| `plans/phase_10.md` | the installer box runs on a fresh hosted runner |
| `plans/phase_11.md` | entry needs 10.5; the 20-minute set and the `docs/performance.md` walkthrough become `scenario_scripted_set_20min` and `scenario_performance_walkthrough`, with clips cut from WannaCry; two rows added to §5 |
| `plans/phase_12-19.md` | listening and approval boxes tagged `(judgement)`; Phase 12's library fetch is run by the agent on this machine |

---

## 2. The order to work in

The phases overlap. This order lets each one use what the one before it built:

1. **Phase 10, §3 steps 3-6.** Timbre and cleanup ports, melody behind ONNX, the
   visualizers, the genre suites. Also P10-2: bind the harmony analysis and put it in the
   app as jobs.
2. **Phase 10.5, all of it.** It needs Phase 10's analyses for `adx analyze` and
   `scenario_wannacry_matches_reference`. It closes P9-2 and P9-3.
3. **Phase 10, §3 steps 7-9.** The optimization pass (`bench_full_project`, which needs
   "all analyzers open" — open them through the driver), docs (including
   `docs/agent-testing.md` from 10.5), and the installer.
4. **Phase 11.** Its entry needs Phase 10 §7 **and** Phase 10.5 §6. Start with the
   week-one spike in phase_11.md §3, before anything else is built on the transport.

For each phase:
- **at the start:** follow STATE.md "When you start a phase" (status, inherited issues);
- **at the end:** follow "When you finish a phase". Every box must be observed, not
  assumed.

### Phase 10 — what's left
Plan: [plans/phase_10.md](plans/phase_10.md). Progress so far is in its §11:
- Chroma, KeyDetector, ChordDetector and ChordSuggest are in `engine/analysis/`, with
  tests;
- key accuracy is 70 % against an 85 % target (P10-1). Every miss is a minor key read as
  its relative major;
- nothing is bound to Python or in the app yet (P10-2).

For P10-1, either reach the target on a labelled recorded corpus or revise it in
phase_10.md with the reason. WannaCry is one more real case for key and BPM; it doesn't
replace a corpus. `bench_full_project` is a hard gate for Phase 11: don't hand off with
it unmet.

### Phase 10.5 — build all of it
Plan: [plans/phase_10.5.md](plans/phase_10.5.md). Its §3 gives the order, §4.1 the
inventory of checks it replaces, and §5 and §6 the tests and the done-criteria. Points
the plan stresses:
- The control channel is **off by default**: per-user pipe, token, no network socket.
- Driver gestures are synthesized input events on real widgets, never direct command
  calls. Testing the wiring between panels is the point.
- Give every interactive widget an `objectName`. That's most of the work.
- Prove the glitch detector catches planted faults (`glitch_positive_controls`) before
  any scenario relies on it.
- Real plugins come from `tools/test_plugins.toml`, pinned by version and SHA-256,
  cached outside git, never shipped. Candidates: Surge XT, Dexed, Airwindows
  Consolidated, ChowTapeModel. Record the final list and output in phase_9.md §10.
- `tools/lint.py plans` becomes a CI step. Make it fire on a planted box first.
- Extend `tools/check_launcher.py` to connect the driver through `adX.cmd`.

### Phase 11
Plan: [plans/phase_11.md](plans/phase_11.md).
- `tools/virtual_pad.py` needs a `--headless` script mode beside its on-screen grid. The
  scripted set is fed through `adx midi play` into loopMIDI, which is installed.
- "No glitches" is measured: zero xruns, zero detector findings, LEDs matching the clip
  state ring, and the recorded performance bit-identical to the capture.
- The physical pad controller is a hardware half; it goes in STATE.md's "Final review".

---

## 3. Validating all three

Do this per phase and record each result in that phase's evidence section and in
STATE.md:

```
py -3.12 -m pip install -e .[dev]
ctest --preset windows-x64-debug -LE slow          # also relwithdebinfo, release
ADX_FULL_EVIDENCE=1 ctest --preset windows-x64-debug -L slow
CI=1 py -3.12 -m pytest tests/python               # and tests/scenarios once 10.5 exists
py -3.12 -m pytest tests/scenarios -m wannacry     # local WannaCry runs: nothing skipped
py -3.12 tools/lint.py tidy --build-dir <each>     # ALL THREE compile databases (debug, relwithdebinfo, release)
py -3.12 tools/lint.py <each gate>                 # format, headers, format-safety, positions, dsp-math, rules, plans
ruff check / ruff format --check / mypy --strict   # pass explicit paths; never bare tools/ (tools/msst)
py -3.12 tools/check_launcher.py
```

Then:
- **CI.** Per-push CI and the full check (a `phase-N` tag) must be seen green, with run
  ids recorded. That needs a commit and a push. **Ask the user before you commit or
  push**, and never add a Claude co-author trailer. P8-0 and P9-0 are waiting on the same
  push. There's no `gh` CLI: read
  `api.github.com/repos/Dhruv23/adX/actions/runs`. Failure annotations are under
  `check-runs/{id}/annotations`.
- **Phase 9 can be marked Done (hardware deferred)** once 10.5's scenarios close P9-2 and
  P9-3, CI is green, P9-4's emulated half passes, and P9-1's emulated half passes.
  - P9-1 needs P9-9 fixed: a WASAPI exclusive-mode backend, or ASIO.
  - P9-4 needs FlexASIO installed, which takes one UAC approval from the user. Ask for
    it once, when you're ready to run that half.
- **Phase 8** needs only CI (P8-0).
- **At the end,** STATE.md and FINAL_PLAN §10 must agree. Re-carry or fix every open
  issue, with a reason for each.

---

## 4. Environment and pitfalls

- Python is `py -3.12` (Python312). The repo's `venv/` is 3.11 for stem separation, so
  bare `python` or `pip` fail.
- MSVC: wrap builds in a .cmd that calls
  `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`
  first, with `set VIRTUAL_ENV=`.
- The Qt SDK for the Quick plugin is in `.qt/` (6.11.2, the same as the PySide6 pin).
  Bump both together.
- `.git/info/exclude` ignores `tools/`: new files there need `git add -f`. `tools/msst`
  is someone else's clone; never format it.
- Bash heredocs mangle `\n` and backslashes in Python. Write scripts to the scratchpad
  with the Write tool, or use Edit.
- Run long builds, test suites and CI waits in the background, then wait for the
  notification. Don't poll.
- Pytest makes modal dialogs raise, and a watchdog reports hangs. Answer the save prompt
  through the driver's `quit` request, not a real dialog.
- From the Phase 9 session:
  - review every clang-tidy auto-fix: its naming fix renamed aliases but not their uses;
  - duplicate a handle before `_dup2` onto fd 1;
  - two fixture targets with one OUTPUT_NAME collide on .lib/.exp/.pdb.
