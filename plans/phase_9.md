# Phase 9 — Plugins & MIDI · L

| | |
|---|---|
| **Status** | Done (hardware deferred) 2026-11-05. Every §6 box observed, the two hardware boxes by their emulated halves (`[~]`): MIDI latency under 10 ms in WASAPI exclusive mode (P9-9 fixed), and ASIO through FlexASIO over VB-Audio Virtual Cable (P9-4). Real plugins and the live crash (P9-2, P9-3) pass in CI's scenarios job. CI: per-push CI green, run 41991668112; full check green, run 42017826073 on tag `phase-10.5-r4`. Hardware halves are in the final review |
| **Governs** | VST3 and CLAP hosting, the plugin sandbox, MIDI device I/O, MIDI learn, clock sync, ASIO |
| **FINAL_PLAN refs** | §5.6, §7 Phase 9, §8 (plugin hosting row), §10 ("the single largest risk") |
| **Entry criteria** | [phase_8.md](phase_8.md) §6 complete |
| **Next** | [phase_10.md](phase_10.md) |
| **§5 coverage owned** | §5.6 **all** except MIDI file import/export (Phase 8) · §5.1 parameter linking with MIDI learn |

---

## 1. Objective

Let adX host other people's code without letting other people's code take adX
down.

FINAL_PLAN §10 names this phase the single largest risk in the project, and the
reason is specific: every other phase fails in ways adX controls. This one fails
in ways a third party controls. A plugin will crash, hang, allocate on the audio
thread, open a modal dialog during a scan, report a wrong latency, and lie about
its state format. The architecture has to assume all of that.

FINAL_PLAN §10 also anticipated that this phase might need its own plan document
as the one permitted exception to the no-parallel-plans rule. **This file is that
document.** No further plan document is created for Phase 9.

---

## 2. Deliverables — exact file manifest

```
engine/plugin/
  PluginId.h              stable identity: {format, uid, path, name, version}
  PluginDescriptor.h      what a scan yields; serialized into the cache
  PluginHost.h            format-agnostic interface the graph sees
  PluginNode.h/.cpp       graph::Node wrapper; owns a PluginHost
  ParamMap.h/.cpp         plugin params <-> Phase 2 ParamRef
  StateBlob.h             opaque plugin state, base64 in .adx (§4.7)
  Bridge/
    Protocol.h            the IPC wire format (§4.3)
    SharedAudio.h         shared-memory audio + event rings
    HostProcess.h/.cpp    adX side: spawn, handshake, supervise, restart
    ChildMain.cpp         the adx_plugin_host.exe entry point
    Watchdog.h/.cpp       deadline detection, kill, restart, bypass
  vst3/Vst3Module.h/.cpp  VST3 loading, IComponent/IEditController plumbing
  clap/ClapModule.h/.cpp  CLAP loading
  Scanner.h/.cpp          out-of-process scan + cache (§4.2)
  PluginCache.h/.cpp      on-disk descriptor cache, invalidated by file mtime+size

engine/midi/
  MidiBackend.h           abstract; RtMidi implementation
  MidiInput.h/.cpp        device open, timestamping, thread -> lock-free ring
  MidiOutput.h/.cpp
  MidiRouter.h/.cpp       device -> channel routing, channel filtering, transforms
  MidiLearn.h/.cpp        CC/note -> ParamRef bindings, persisted in the project
  MidiClock.h/.cpp        clock + MTC send/receive, tempo follow
  MidiMap.h               controller profile definitions

engine/audio/
  AsioBackend.cpp         RtAudio's ASIO API path + buffer/latency handling

app/adx/panels/
  plugins/browser.py      scan status, list, rescan, blacklist, per-plugin info
  plugins/window.py       plugin editor window host (§4.6)
  midi/settings.py        devices, routing, channel filters, clock
  midi/learn.py           learn mode overlay + binding list

tools/
  adx_plugin_host.exe     the sandbox child (built from ChildMain.cpp)

tests/cpp/plugin/         test_protocol, test_watchdog, test_parammap, test_cache
tests/cpp/midi/           test_router, test_learn, test_clock
tests/python/             test_plugin_lifecycle.py, test_midi.py
tests/fixtures/plugins/   adx_test_plugin (a VST3 and a CLAP we control) (§4.8)
```

---

## 3. Order of work

MIDI first, plugins second. MIDI is lower risk, delivers user value immediately,
and its input path (device thread → lock-free ring → scheduler) is a smaller
rehearsal of the same pattern the plugin bridge needs.

1. MIDI input, routing, channel filtering.
2. MIDI learn over the Phase 6 generic parameter editor (the right-click entry
   already exists and is disabled — this enables it).
3. MIDI clock and MTC.
4. ASIO.
5. Plugin scanning, out-of-process (§4.2).
6. The IPC bridge and the sandbox (§4.3).
7. VST3 hosting.
8. CLAP hosting.
9. Plugin windows, state persistence, automation.

---

## 4. Design

### 4.1 MIDI input

```cpp
struct MidiEvent {                    // trivially copyable
    uint64_t timestampSamples;        // converted from device time at capture
    uint8_t  status, data1, data2;
    uint8_t  deviceIndex;
};

class MidiInput {
    // RtMidi callback thread. Converts device time to the audio clock and pushes.
    void onMessage(double deviceTime, std::span<const uint8_t>) noexcept;
    rt::SpscRing<MidiEvent, 4096> m_ring;
};
```

The audio thread drains the ring at the top of each block, converts timestamps
to block-relative sample offsets, and merges them into the same sorted event
stream the scheduler already consumes (Phase 3 §4.5). A live note and a
sequenced note are then *the same kind of event*, which is what makes them
behave identically through voice allocation, per-channel polyphony and PDC.

Events arriving mid-block are timestamped to their true offset rather than
quantized to the block start. This is the difference between 0 ms and up to
5.3 ms of jitter, and it is free given the infrastructure.

`MidiRouter` handles device→channel routing, channel filtering, key/velocity
range splits, transpose, and velocity curves. Every transform is a project
setting, so a routing setup is saved and diffable like everything else.

**Pitch bend and note slides.** MIDI pitch-bend (and per-note bend from MPE
channels) maps to `Note.pitchCurve` on input recording and on file import, using the
bend range the device reports (default ±2 semitones, RPN 0 honored). Export writes
`slide`/`pitchCurve` as pitch-bend on a per-note channel (MPE-style rotation) so a
slide survives a round trip into another DAW; this is lossy for overlapping notes
and the exporter warns. Tests: `midi_bend_import_roundtrip`, `midi_export_slide_mpe`.

**Singing-voice imports.** UST and USTX (OpenUtau) import to `Note` + `lyric` +
`pitchCurve` for the Voice instrument (Phase 4 §4.13). VSQX (Vocaloid XML) may be
imported as notes + lyrics only; adX cannot render Vocaloid voices natively, so a
VSQX channel is assigned a Voice or VST3 instrument by the user.

### 4.2 Plugin scanning — out of process, always

A plugin scan loads arbitrary code. Some of it crashes on load. Some opens a
dialog. Some takes 30 seconds. In-process scanning means one bad plugin makes
adX unable to start, and the user cannot even get to the setting that would
disable it.

So `Scanner` spawns `adx_plugin_host.exe --scan <path>`, which loads the plugin,
writes a `PluginDescriptor` as JSON to stdout, and exits. adX applies a 30-second
deadline. Crash, hang, or malformed output → the plugin is recorded as **failed**
with the reason and is skipped next time unless explicitly rescanned.

Results land in `PluginCache`, keyed by `(path, mtime, size)`, so startup does
not rescan. Scanning is incremental, parallel across N children, and shows
progress. The blacklist is visible and editable, because a plugin that fails
once may work after the user updates it.

### 4.3 The sandbox

**Every plugin instance runs in a child process.** This is the decision the phase
turns on, and it is not the cheap option.

*Why pay for it:* an in-process plugin crash takes down the host, and with it
whatever the user had not saved. Out-of-process, a crash bypasses one insert,
raises a notification, and playback continues. For a project whose premise is
that people leave it open next to a text editor and edit live, host stability is
not a nicety.

*What it costs:* one shared-memory round trip per block per plugin — realistically
20–60 µs — plus the engineering in this section. Mitigations: multiple plugin
instances share one child process by default (per-project, configurable to
one-child-per-plugin for known-bad ones), and a frozen channel (Phase 8 §3.4)
costs nothing at all.

**Wire protocol.** Shared memory for audio, a named pipe for control.

```
Shared memory region per instance:
  [ control block ]  atomics: sequence, frames, state, watchdogTick
  [ input audio  ]  float, maxChannels * kMaxBlockFrames
  [ output audio ]  float, maxChannels * kMaxBlockFrames
  [ event in     ]  SpscRing<MidiEvent | ParamChange>
  [ event out    ]  SpscRing<MidiEvent | ParamChange>

Per block:
  host writes input + events, release-stores sequence
  child acquire-loads sequence, processes, release-stores sequence+1
  host acquire-loads sequence+1 with a deadline; on miss -> §4.4
```

A futex-style wait is not available portably and a spin on the audio thread is
unacceptable, so the host **never blocks** on the child. It reads whatever is
ready; a child that has not finished by the deadline yields a silent block from
that plugin and a recorded late-block. This keeps Rule 1 intact: the audio
thread has no unbounded wait anywhere in it.

### 4.4 `Watchdog`

Three failure modes, three responses:

| Failure | Detection | Response |
|---|---|---|
| Child crashed | process handle signalled | bypass the insert, notify, offer restart; project keeps playing |
| Child hung | watchdog tick stale > 2 s | kill, restart, restore state from the last `StateBlob`, notify |
| Child late | missed the block deadline | output silence for that block, count it; after 50 in 1 s, bypass and notify |

A restarted plugin reloads its saved state, so a crash costs a click and not the
session. The late-block counter is surfaced per plugin, because "which plugin is
making it stutter" is otherwise an afternoon of bisecting.

### 4.5 `PluginNode` and PDC

`PluginNode` is an ordinary `graph::Node` (Phase 3 §4.3). It declares
`latencySamples()` from the plugin's report, and Phase 3's PDC compensates
automatically — this is exactly why PDC was built in Phase 3 with no plugins in
sight.

Plugins that **change** their latency at runtime (some do, on preset change) send
a latency-changed notification; adX rebuilds the PDC plan on the main thread and
publishes a new snapshot. A latency change during playback causes one
recompensation, which is audible as a brief discontinuity and is unavoidable —
but it is correct, and it is logged so it is explicable.

**Plugins do not get to violate Rule 1**, because they are not on our audio
thread at all. That is a genuine, under-appreciated benefit of the sandbox: a
plugin that allocates in `process()` degrades only its own child process's
timing, and the watchdog handles it.

### 4.6 Plugin editor windows

The plugin's own GUI, parented into a Qt window. Out-of-process makes this
harder: the child creates the window and adX reparents it via native window
handle embedding (`QWindow::fromWinId` + `setParent`).

Fallback, always available: the **generic editor** from Phase 6 §4.4, driven by
the plugin's parameter list. Some plugins have unusable embedding behavior, and
the generic editor means those plugins are still usable rather than unusable.
It also means a headless render never needs a GUI at all.

### 4.7 Parameters, automation, and state

`ParamMap` maps plugin parameters to Phase 2 `ParamRef`s, so plugin parameters
are automatable, MIDI-learnable and text-addressable **exactly like native
ones**. In the `.adx` file:

```
[INSERT 3]
  SLOT 2 vst3 uid=56535458... name="Pro-Q 4"
    PARAM band1.freq=440
    PARAM band1.gain=-3.5
    STATE=base64:UklGRi4AAABXQVZF...
```

Named parameters are written out for the ones the plugin exposes by name, so a
diff is readable and automation targets are stable. `STATE` is the plugin's own
opaque blob, base64'd, for everything the parameter list does not capture.

This is a compromise and worth naming: a binary blob in a text format is not
diffable. The alternative — refusing to store plugin state — makes plugin
hosting useless. So: parameters are text and diffable; the blob is the
irreducible remainder, kept on its own line so it never obscures anything else
in a diff, and optionally written to a sidecar file for users who would rather
keep their `.adx` free of it.

### 4.8 `adx_test_plugin`

A VST3 and a CLAP plugin that adX itself builds, in `tests/fixtures/plugins/`,
with deliberate misbehavior modes selectable by parameter:

crash on load · crash on process · hang for 10 s · allocate in process · report
a wrong latency · change latency mid-playback · return malformed state · open a
modal dialog on scan · produce NaN.

CI cannot depend on commercial plugins being installed, so without this fixture
the entire failure-handling design would be untested — which would mean it does
not work, since untested error paths generally do not. This fixture is a
first-class deliverable of the phase, not test scaffolding.

### 4.9 ASIO

RtAudio already supports ASIO; enabling it is a build flag plus the ASIO SDK
(which cannot be redistributed, so it is an opt-in local path in
`AdxDependencies.cmake` with a clear message when absent). The work is in the
device panel: ASIO exposes buffer sizes the device dictates rather than any
value, has a control-panel button that must be launched, and reports latency
differently. Phase 1's `AudioBackend` abstraction absorbs all of it.

### 4.10 MIDI clock and MTC

Send and receive. As a receiver, adX slaves its tempo to incoming clock with a
PLL smoothing the jitter — raw MIDI clock is jittery enough that following it
naively produces audible wow.

**This is where the Phase 3 checkpoint pays off a second time:** an external
clock is just another time source's rate and position driver. Because nothing
assumes a global position, sync is contained rather than invasive.

---

## 5. Tests

| Test | Asserts |
|---|---|
| `midi_input_sample_accurate` | events land at their true block offset, not the block start, at 3 buffer sizes |
| `midi_input_no_alloc` | drain path under the allocator hook: 0 violations |
| `midi_router_filters` | channel filter, key split, velocity curve, transpose — table-driven |
| `midi_learn_binds_and_persists` | learn a CC, save, reload, the binding still drives the parameter |
| `midi_learn_survives_rename` | renaming the target channel keeps the binding (it is a `ParamRef`, not a path) |
| `midi_clock_pll_stability` | following a jittered clock, tempo deviation stays < 0.1 % |
| `midi_roundtrip_latency` | `[.device]` hardware keyboard → audible note, measured < 10 ms — **the FINAL_PLAN gate** |
| `scanner_survives_crash_on_load` | `adx_test_plugin --crash-on-load` is recorded failed; the scan completes |
| `scanner_survives_hang` | a 60 s hang is killed at the 30 s deadline; the scan completes |
| `scanner_cache_invalidation` | touching the plugin file triggers a rescan; not touching it does not |
| `protocol_roundtrip` | audio and events survive the shared-memory round trip bit-identically |
| `watchdog_crash_bypasses` | `--crash-on-process` mid-playback → insert bypassed, playback continues, zero dropouts elsewhere |
| `watchdog_hang_restarts` | `--hang` → killed, restarted, state restored, audio resumes |
| `watchdog_late_block_silences` | a slow child produces silence for that block and increments the counter, never a stall in the host |
| `plugin_alloc_does_not_affect_host` | `--allocate-in-process` → host allocator hook still reports 0 violations |
| `plugin_nan_is_contained` | `--produce-nan` → NaN is detected and the insert muted, not propagated to the master |
| `plugin_latency_compensated` | reported latency is compensated by PDC; measured impulse alignment within 1 sample |
| `plugin_wrong_latency_detected` | `--wrong-latency` is detected by measurement and reported as a warning |
| `plugin_latency_change_midplay` | one recompensation, correct after, logged |
| `plugin_state_roundtrip` | state saves to `.adx`, reloads, and the plugin renders identically |
| `plugin_malformed_state_handled` | `--malformed-state` loads to defaults with a warning, never a crash |
| `plugin_params_automatable` | a plugin parameter automates, MIDI-learns, and is text-addressable exactly like a native one |
| `plugin_generic_editor_fallback` | with embedding disabled, the generic editor drives every parameter |
| `plugin_shared_child_isolation` | two instances in one child: one crashing takes down both, both recover, and the option to isolate works |
| `asio_device_enumeration` | `[.device]` ASIO devices enumerate with correct buffer-size constraints |
| `headless_render_with_plugins` | `adx render` on a project with plugins works with no GUI and no display |

---

## 6. Definition of done

- [~] `midi_roundtrip_latency` measured under 10 ms on real hardware, recorded. (hardware half)
      **Emulated half passed 2026-11-05** in WASAPI exclusive mode
      (`tools/measure_midi_latency.py --exclusive`, loopMIDI in, the device's own clock
      out): median 5.91 ms at block 64 and 6.99 ms at block 128, 30 notes each, none
      missed, 0 xruns (§10). Block 256 measures 12.49 ms: a 256-frame block alone is
      5.3 ms, so a 10 ms note needs a small block. **Before (2026-10-30, P9-1):** loopMIDI into the engine, measured to the
      output mix, 67.75 to 74.24 ms median (§10); MIDI in is about 5 ms, the rest is
      shared-mode output (P9-9). It becomes `[~]` when that tool measures under 10 ms;
      it does not cover a keyboard's USB link or the device buffer after the mix. The
      hardware keyboard is deferred to the final review (FINAL_PLAN §9).
- [x] A VST3 instrument and a VST3 effect load, automate, save and reload
      (FINAL_PLAN's stated gate), verified with at least three real third-party
      plugins as well as the fixture. **The fixture half is observed**
      (`plugin_state_roundtrip`, `plugin_instrument_plays`,
      `plugin_instrument_round_trips_through_text`, `plugin_latency_compensated`, all
      for VST3). **The three real plugins (P9-2)** are agent-run since 2026-11-01
      (FINAL_PLAN §9): pinned open-source plugins, fetched by hash, driven through the
      GUI by `scenario_real_plugins` (phase_10.5.md §4.6). **Passed:** 16 of 16 locally
      (Dexed, Surge XT and its Effects bundle, Airwindows Consolidated; VST3 and CLAP),
      and in CI's `agent-run scenarios (Release, null audio, real plugins)` job, full
      check run 42017826073.
- [x] A CLAP instrument and effect do the same. **The fixture half is observed**
      (the same tests for CLAP, plus `plugin_params_automatable`); real plugins with
      P9-2.
- [x] Every `adx_test_plugin` misbehavior mode is handled without taking down the
      host, and each has a passing test. Crash on load (`scanner_survives_crash_on_load`),
      crash on process (`watchdog_crash_bypasses`), hang (`watchdog_hang_restarts`;
      at scan, `scanner_survives_hang`), allocate in process
      (`plugin_alloc_does_not_affect_host`), wrong latency
      (`plugin_wrong_latency_detected`), latency change mid-playback
      (`plugin_latency_change_midplay`), malformed state
      (`plugin_malformed_state_handled`), a modal dialog on scan
      (`scanner_survives_dialog_on_scan`), NaN (`plugin_nan_is_contained`) - and a slow
      child (`watchdog_late_block_silences`).
- [x] A crashing plugin during playback does not interrupt playback of the rest
      of the project — shown in the running app, not only in a unit test. **In a test**
      (`watchdog_crash_bypasses`: the other channel bit-identical throughout, the
      hosted path dry after the crash). **Live (P9-3)** is agent-run since 2026-11-01:
      `scenario_live_plugin_crash` launches the app through `adX.cmd`, plays, turns
      `mode` to 1 through the GUI, and checks the notice, Restart, the capture and the
      xrun count (phase_10.5.md §4.6). **Passed** locally (stand-in and WannaCry) and in
      the full check's scenarios job, run 42017826073.
- [~] ASIO works with a real interface, with the latency readout matching a
      measured loopback. (hardware half) **Built and enumerating** (`asio_device_enumeration`
      `[.device]`, `rtaudio_default_api_is_not_asio`). **Emulated half not yet run
      (P9-4):** FlexASIO over VB-Audio Virtual Cable, the readout against
      `tools/measure_latency.py` through `Engine(asio=True)`; it would not cover a real
      interface's own ASIO driver. The real interface is deferred to the final review
      (FINAL_PLAN §9). **Emulated half passed 2026-11-05** (`tools/measure_latency.py
      --asio`, FlexASIO 1.10b in shared mode over VB-Cable): takes land 0 samples from
      the click at 64, 256 and 1024, and adX's readout is the driver's own split
      (in 4096 / out 6144 at 1024, as FlexASIO's log returns them). It first showed
      twice the round trip; fixed (§11). FlexASIO's own report undershoots the measured
      loopback (its PortAudio estimate), which is why recording uses the calibration.
- [x] MIDI learn works from the Phase 6 generic editor on both native and plugin
      parameters (`test_panel_midi_learn.py`: the menu entry, enabled, arms learn and
      binds the next control, on a Delay's parameter and on the fixture's `gain`; the
      engine side in `midi_learn_binds_and_persists`).
- [x] `headless_render_with_plugins` passes in CI. **Passes locally**
      (`test_plugin_lifecycle.py::test_headless_render_with_plugins`, `adx render` with
      no display), and in CI run 41991668112 (the job builds the host and the fixture
      before pytest, so the test runs rather than skips).
- [x] FINAL_PLAN.md §8 gains rows for the VST3 SDK, CLAP and RtMidi (and ASIO); §10
      Phase 9 row updated; the "may need its own plan document" note already points at
      this file.

---

## 7. Out of scope (and who owns it)

| Thing | Owner |
|---|---|
| AU, LV2, VST2 | not planned; FINAL_PLAN §5.6 names VST3 and CLAP only |
| adX as a plugin | **never** — FINAL_PLAN §1 non-goal ("never a VST/AU/CLAP guest") |
| Pad-controller mapping with LED feedback | Phase 11 (§5.11) — MIDI learn and device I/O land here; the *performance* mapping is Phase 11 |
| Analysis, visualizers | Phase 10 |

---

## 8. Handoff to Phase 10

Phase 10 inherits a feature-complete DAW. What matters to it specifically:

- The optimization pass now has plugins in scope — they are the dominant cost in
  a real project, and measuring without them would measure the wrong thing.
- The installer must handle the plugin host executable, the plugin cache
  location and first-run scanning.
- Freeze (Phase 8) plus the sandbox gives a real answer to "a plugin is making
  it stutter", which Phase 10's polish work should surface in the UI.

And to Phase 11, which depends on this phase directly: `MidiInput`,
`MidiRouter`, `MidiLearn` and `MidiOutput` are exactly what pad triggering and
LED feedback are built on.

---

## 9. Risks

FINAL_PLAN §10 calls this the largest risk in the project. Specifically:

| Risk | Mitigation |
|---|---|
| Out-of-process hosting is more work than in-process and could sink the schedule | It is scoped as its own sub-project (§4.3–§4.4) with a controlled test fixture (§4.8). The fallback — in-process with a crash handler — is strictly worse and is not adopted, but it is a known escape hatch if the bridge proves unworkable |
| The IPC round trip is too slow and limits usable plugin counts | Measured early, before VST3 hosting is written, using a null child. If a block round trip exceeds ~100 µs the design is revisited before it is built on. Shared children and freeze are the mitigations |
| VST3 SDK licensing/redistribution complicates the build | The SDK is the anticipated FetchContent escape hatch from Phase 0 §3.4; vendoring or a local path is acceptable and pre-approved |
| Real-world plugins misbehave in ways the fixture does not model | The fixture covers the known classes; the blacklist, watchdog and per-plugin isolation option handle the unknown ones without a code change |
| Window embedding across processes is unreliable for some plugins | The generic editor fallback (§4.6) means those plugins remain fully usable |
| Plugin state as a base64 blob undermines the text-first premise | Acknowledged explicitly (§4.7); named parameters carry everything that can be named, the blob is isolated to one line, and a sidecar option exists |

---

## 10. Evidence and progress

Started 2026-10-29 (MIDI input), built 2026-10-30. Nothing is committed: the tree also
holds Phase 8, which is built but not done either (P8-0, P8-1).

### What exists

**The sandbox (§4.2-§4.4).** `adx_plugin_host.exe` (`engine/plugin/Bridge/ChildMain.cpp`)
scans (`--scan`, JSON to stdout, one child per file, parallel, under a 30 s deadline) and
hosts (`--serve`). Control is a length-prefixed binary protocol on the child's stdin and
stdout (`Protocol.h`); the child duplicates those handles and points the CRT's at stderr,
so a plugin that prints cannot corrupt it. Audio is one shared region per instance
(`SharedAudio.h`) exchanged a block at a time under a request/reply sequence pair and two
named events; the audio thread's side (`AudioLink.cpp`) never waits past a deadline -
half the block in realtime, ten seconds in an offline render - and waits in 20 ms slices
so a child the watchdog found dead ends the wait at once. Each instance owns its region
and events, so a restarted child reattaches to the same memory and the audio thread's
pointers never change. The watchdog (`HostProcess::watch`, 20 ms) turns a dead child into
`Crashed` (dry, a notice, a restart on request), a block outstanding for 2 s into a kill
and an automatic restart with the last state, and the audio thread's own counters into
`LateBypassed` (50 late in a second) and `Muted` (NaN or infinity, never propagated).
Instances share one child; one marked isolated gets a child of its own at its next
restart. `PluginHost` owns the children, the cache, and the notices the app shows.

**Formats.** CLAP 1.2.6 (`clap/ClapModule.cpp`: params, state, latency, audio ports,
GUI, restart, main-thread callbacks; note and parameter events merged by time) and VST3
3.8.0 (`vst3/Vst3Module.cpp`, over the SDK's hosting helpers: single or split
component/controller, connection points, `IComponentHandler` edits fed back to the
processor and echoed to adX, `restartComponent`, state as one blob holding component and
controller state, `IPlugView` editors). Both load effects and instruments.

**In the graph (§4.5, §4.7).** `PluginEffect` is a slot node whose latency is the
plugin's, PDC-compensated like any node's, with its own dry path following the plugin's
live latency so a mix below 1 does not comb. `PluginInstrument` renders whole blocks
through a new `ChannelNode::renderWhole` hook (notes at their offsets; note-offs by the key
their note-on had; release-all and loop wrap handled). Parameters are declared by the node
(`DeclaresParams`) from the scan cache and laid out by the graph like a type's; one the
project does not name gets NaN, read as "leave the plugin's value", so a saved state is
never overwritten by defaults nobody chose. Parameter changes from a plugin's own editor
come back (`ProjectSync::applyPluginEchoes`) as value commands; live state goes into the
project before every save and export (`syncPluginStates`). A latency change is a notice
and a rebuild. `bridge::measureLatency` probes with an impulse and reports a plugin whose
report is wrong.

**Format.** `SLOT n clap|vst3 plugin=.. name=.. path=.. [isolated=yes] key=value...` with
a quoted `STATE "base64:..."` line beneath it; `INSTRUMENT=clap|vst3` with `PLUGIN`,
`PLUGIN_NAME`, `PLUGIN_PATH`, `PLUGIN_ISOLATED` and a last `STATE=` key; the `[MIDI]`
section (`ROUTE`, `LEARN`). docs/adx-format-v2.md §7.2, §7.5, §7.5.1.

**MIDI (§3 steps 1-3).** Input, routing and the clock from the first session (below).
Now also: MIDI learn - bindings in the project (`project::MidiSettings`, commands, the
format), compiled into the snapshot, matched on the audio thread and written as knob
turns, their values sent back so the project and the knob follow
(`RenderEngine::applyLearned`), and the last control published for learn mode; and the
project's own routes replacing the omni route when there are any. Device names map to
engine slots through `midi/DeviceNames`.

**ASIO (§4.9).** RtAudio's ASIO path compiled in (`ADX_ENABLE_ASIO`, on); the backend
names its API, WASAPI unless ASIO is asked for (`Engine(asio=True)`,
`enumerate_devices(asio=True)`).

**The app.** A Plugins panel (scan on a worker thread, installed and failed lists,
rescan and forget a file, add an effect to the selected insert or an instrument to the
selected channel), a plugin window (the plugin's editor embedded by handle, the generic
editor below it always), the effect and instrument panels' generic editors built from a
plugin's own parameter list, a MIDI panel (learned controls, learn status, routes), "MIDI
learn" enabled in every parameter's menu, and `PluginRuntime` (learned values, editor
echoes, notices to the status bar, rebuild on latency change, learn mode, state sync).

### Evidence (2026-10-30, the final tree)

| Check | Result |
|---|---|
| ctest Debug (`-LE slow`) | 466 / 466 (Phase 10's six harmony tests included) |
| ctest RelWithDebInfo | 467 / 467, slow suite included |
| ctest Release | 467 / 467, slow suite included |
| Debug `[.slow]`, `ADX_FULL_EVIDENCE=1` | passed, 1419.69 s |
| `[plugin]` in Release, three runs back to back | 28 / 28 each |
| pytest, `CI=1` | 248 passed (GPU tests deselected), none skipped |
| clang-tidy | clean, all 331 files, on all three compile databases |
| clang-format, headers, format-safety, positions, dsp-math, rules | clean |
| ruff, ruff format, mypy `--strict` | clean |
| Launcher | `check_launcher: adX.cmd opened adX 0.1.0 - 16 panels, audio device open` |

Phase 9 tests: `[plugin]` - `protocol_byte_roundtrip`, `plugin_scan_json_roundtrip`,
`plugin_state_base64`, `plugin_param_keys`, `protocol_roundtrip`,
`scanner_cache_invalidation`, `plugin_cache_saves_and_reloads`,
`scanner_finds_both_formats`, `scanner_survives_crash_on_load`, `scanner_survives_hang`,
`scanner_survives_dialog_on_scan`, `scanner_survives_hang_at_the_real_deadline`
(`[.slow]`), `watchdog_crash_bypasses`, `watchdog_hang_restarts`,
`watchdog_late_block_silences`, `plugin_shared_child_isolation`,
`plugin_alloc_does_not_affect_host`, `plugin_nan_is_contained`,
`plugin_malformed_state_handled`, `plugin_slot_round_trips_through_text`,
`plugin_params_absent_keep_the_plugins_own`, `plugin_params_automatable`,
`plugin_latency_compensated`, `plugin_wrong_latency_detected`,
`plugin_latency_change_midplay`, `plugin_state_roundtrip`, `plugin_instrument_plays`,
`plugin_instrument_round_trips_through_text`; `[midi][learn]` - `midi_learn_controls_normalise`,
`midi_learn_binds_and_persists`, `midi_learn_survives_rename`, `midi_learn_relearn_replaces`,
`midi_project_routes_play_their_channel`; `rtaudio_default_api_is_not_asio`,
`asio_device_enumeration` (`[.device]`). Python: `test_plugin_lifecycle.py` (scan, render,
state sync and save, crash notice and restart, plugin descriptors, headless `adx render`),
`test_midi.py`, `panels/test_panel_midi_learn.py`.

From the first session (MIDI input, 2026-10-29): `midi_input_sample_accurate`,
`midi_input_plays_at_its_offset_through_the_engine`, `midi_input_no_alloc`,
`midi_router_filters`, `midi_router_note_off_follows_its_note_on`,
`midi_clock_pll_stability`, `midi_clock_follows_position`, `midi_clock_sends_24_a_beat`,
and the live-MIDI and MIDI-preferences Python tests.

### MIDI latency without a keyboard (2026-10-30, observed locally)

The user has no MIDI keyboard, so `midi_roundtrip_latency` ran in software: loopMIDI
1.0.16.27 as the port, `tools/measure_midi_latency.py` + `tools/midi_latency_probe.cpp`
(notes sent into loopMIDI, onset found in WASAPI loopback of the default output, both
stamped on QPC). Default output: 48000 Hz float stereo, Release pyd, 30 notes each:

```
block    64: to the mix median  67.75 ms, p95  68.30, max  68.36 (30 notes, 0 missed); endpoint 10.00 ms; est. audible  77.75 ms
block   128: to the mix median  69.45 ms, p95  71.04, max  71.69 (30 notes, 0 missed); endpoint 10.00 ms; est. audible  79.45 ms
block   256: to the mix median  74.24 ms, p95  76.26, max  76.81 (30 notes, 0 missed); endpoint 10.00 ms; est. audible  84.24 ms
```

**Far over the 10 ms gate.** Splitting the path: `Engine.audition` (no MIDI at all) to the
mix measured 62.85 ms median at block 64 and 68.50 at 256 (15 notes each), so MIDI in
(loopMIDI + RtMidi + the input thread) is about 5 ms and the rest is audio output. The
device's driver offers one shared-mode period, 480 frames (default = fundamental = min =
max), and a 1056-frame buffer at it. RtAudio 6.0.1's WASAPI thread keeps that 22 ms buffer
full and its own ring (block + device buffer) full behind it, then the Windows mix adds
its 10 ms period: about 55 to 60 ms before any block size. Shared mode on this device can
never get under 10 ms (its period alone is 10 ms); the gate needs WASAPI exclusive mode or
ASIO. Recorded as P9-9.

### Not built

MIDI output and sending clock to a device; clock follow wired to the arrangement's time
source; MTC; pitch bend and MPE into `pitchCurve` (`midi_bend_import_roundtrip`,
`midi_export_slide_mpe`); UST/USTX/VSQX import; recording a plugin parameter's automation
from its own editor (echoes become values, not lanes); a stored per-plugin "embedding
off" preference (the window always tries to embed and always shows the generic editor).
All are open issues (STATE.md P9-5 to P9-8).

### The first session (2026-10-29)

Started 2026-10-29, in the session that finished building Phase 8 and before Phase 8's
hardware gate (P8-1) closed. §3 puts MIDI first because it is the low-risk half; none
of it depends on the latency measurement except `midi_roundtrip_latency`, which is a
hardware gate of its own.

### 2026-11-05: closing the phase

**CI (P9-0 closed).** per-push CI green, run 41991668112; full check green, run 42017826073 on tag `phase-10.5-r4`; the scenarios job ran `scenario_real_plugins` and `scenario_live_plugin_crash` against the pinned plugins of `tools/test_plugins.toml`.

**MIDI latency, exclusive mode (P9-9 fixed, P9-1 emulated half `[~]`).** loopMIDI running, `py -3.12 tools/measure_midi_latency.py --exclusive --notes 30`:

```
block    64: to the device median   5.91 ms, p95   7.02, max   7.48 (30 notes, 0 missed) [exclusive pcm24, device block 144, reported 6.00 ms, 0 xruns]
block   128: to the device median   6.99 ms, p95   7.47, max   7.52 (30 notes, 0 missed) [exclusive pcm24, device block 144, reported 6.00 ms, 0 xruns]
block   256: to the device median  12.49 ms, p95  13.09, max  13.18 (30 notes, 0 missed) [exclusive pcm24, device block 256, reported 10.67 ms, 0 xruns]
```

Shared mode measured 68-74 ms on the same device; exclusive mode is in Preferences > Audio. Not covered: a keyboard's USB link, and the DAC after the device clock.

**ASIO (P9-4 emulated half `[~]`).** FlexASIO 1.10b installed (winget `dechamps.FlexASIO`), `%USERPROFILE%\FlexASIO.toml` with `backend = "Windows WASAPI"`, input `CABLE Output (VB-Audio Virtual Cable)`, output `CABLE Input (VB-Audio Virtual Cable)`, shared mode. `py -3.12 tools/measure_latency.py --asio --attempts 5`:

```
MOVED block    64: the loopback moved during the take (4768 then 5248 samples); not a judgement of adX
PASS  block    64: reported  2239 samples (in 1056, out 1183), measured  5116, take offset +0 samples (+0.000 ms)
MOVED block   256: the loopback moved during the take (4852 then 5332 samples); not a judgement of adX
PASS  block   256: reported  2624 samples (in 1056, out 1568), measured  5344, take offset +0 samples (+0.000 ms)
PASS  block  1024: reported 10240 samples (in 4096, out 6144), measured  8124, take offset +0 samples (+0.000 ms)
```

The virtual chain's delay moves while a stream runs (FlexASIO's PortAudio buffers resync to VB-Cable's clock: one 480-frame step in shared mode, several smaller ones in exclusive mode, with adX counting 0 xruns throughout), so the tool now calibrates again after each take and judges only a take whose loopback held still; a moved one is printed `MOVED` and retried. In exclusive mode every take moved, so the recorded run is shared mode. A real interface's driver has a fixed delay.

**Checks on this tree (2026-11-05, local).** ctest `-LE slow`: Release 494/494; Debug
493/494 under `-j 8`, the miss `snapshot_swap_between_blocks` (a Null-backend timing case
that never touches RtAudio: its "still sounding" peak read 0.028 against 0.05 under load),
which then passed 3 of 3 alone. pytest with `CI=1`: the parallel and serial sets pass.
clang-tidy on the changed files clean in Debug and Release; clang-format, ruff, ruff format
and `mypy --strict` clean. Launcher: `check_launcher: adX.cmd opened adX 0.1.0 - 24
panels, audio device open`; played `single_tone.adx`, 48,896 frames, 0 xruns, quit cleanly.
**Not yet in CI:** the RtAudioBackend fix needs a push.

## 11. Corrections made while executing this plan

1. **The event rings in the shared region are plain arrays** (§4.3 sketched SpscRings).
   The region is exchanged a whole block at a time under the request/reply pair, and a
   host that finds the child still on the previous block writes nothing at all, so the
   two sides never touch it at once: arrays guarded by the sequence are the same safety
   for less.
2. **"The host never blocks on the child" became "never waits past a deadline"** (§4.3).
   Reading only what is ready would cost a whole block of latency on every plugin; the
   audio thread instead waits on the child's event, bounded - half the block in realtime
   (the graph needs the rest), ten seconds offline - in 20 ms slices that end early when
   the watchdog has marked the child dead. Rule 1's "no unbounded wait" holds. Sub-
   millisecond deadlines use a high-resolution waitable timer made on the main thread
   with the event.
3. **Control is the child's stdin and stdout, not a named pipe** (§4.3). Anonymous
   pipes polled with `PeekNamedPipe` give the host a timeout without overlapped I/O; the
   child takes its own duplicates of both handles before pointing the CRT's at stderr.
   (Redirecting first closed the handle under the protocol - every scan's output was
   empty until this was found.)
4. **Load-time misbehaviour is selected by an environment variable** (§4.8 said "by
   parameter"). Nothing can set a parameter before the plugin loads; `ADX_TEST_PLUGIN`
   (crash, hang[:s], dialog), inherited by the scanner child, can. Run-time modes are the
   `mode` parameter as planned, and the mode is not part of the fixture's state: a
   project reopened does not crash again by itself.
5. **Plugin parameters are declared by the node, not the TypeCatalog.** A slot or
   instrument of type `clap`/`vst3` lays out its slice from the cached descriptor
   (`DeclaresParams`), and an unnamed parameter's fallback is NaN, "leave it" - the
   only way a saved state and the project's named values can coexist without the
   graph's defaults overwriting the state. The layout comparison now compares fallbacks
   bitwise (NaN != NaN rebuilt every graph until it did).
6. **`PluginEffect` is a `SlotNode`, not an `effects::Effect`.** `Effect` fixes its
   latency at prepare; a plugin can change its own (§4.5), so the node keeps a dry delay
   that follows the plugin's live latency and PDC catches up on the rebuild the notice
   triggers.
7. **A plugin instrument needed a hook in `ChannelNode`** (`renderWhole`, `wholeSounding`):
   `process` is final and voice-based, and a plugin has voices of its own. Departure and
   handover release it and render a two-second tail.
8. **The VST3 SDK is two repositories, not one, and not its CMake** (§9's anticipated
   escape hatch, used without vendoring): `vst3_pluginterfaces` and `vst3_public_sdk`,
   side by side under one root, hosting sources compiled as `adx_vst3sdk` in C++17 -
   `module_win32.cpp` passes `u8string()` where C++20 makes it `std::u8string`. The
   fixture's VST3 build uses `pluginterfaces` alone (`funknownimpl.h`), which needs the
   factory to list `IPluginFactory` as an indirect interface or the SDK's own loader
   rejects it.
9. **ASIO needs no SDK path** (§4.9). RtAudio ships the ASIO host files itself; the work
   was making the backend name its API, because RtAudio's own default is the first
   compiled API in its list and ASIO precedes WASAPI there.
10. **Plugin state is quoted in the file.** base64's `=` padding read as a key=value in
    a bare token and was lost on load; `STATE "base64:..."`.
11. **File times go into the scan cache as text.** A Windows file time is past 2^53 and
    did not survive JSON's double, so no cached file was ever "fresh".
12. **A notice could be lost.** A NaN mute or late bypass right before an instance was
    destroyed - the end of a short render - never reached the watchdog; the instance now
    reports an unreported state change as it goes. Found by `plugin_nan_is_contained`
    failing only in Release.
13. **MIDI routes and learned bindings are part of the snapshot,** not EngineCore state:
    they swap in exactly when the graph they target does, and a binding to a deleted
    parameter is simply not compiled (it stays in the file).
14. **The two fixture builds need different output names.** One name for both made their
    import libraries and debug databases collide; which build won depended on order.
15. **Automatic clang-tidy fixes renamed the VST3 namespace aliases but not their uses**
    (and only `--fix-errors` applies anything under warnings-as-errors). Reviewed by hand,
    as the Phase 8 note on macro fixes advised.
16. **A duplex RtAudio stream reported twice its round trip.** `getStreamLatency()` is
    input plus output once an input is open, and `RtAudioBackend` used that sum as the
    output latency and copied it to the input as well. Recording without a calibration
    would have been compensated by a whole extra round trip. The sum is now split: from
    the driver itself on ASIO (`ASIOGetLatencies`), evenly otherwise. Found by the
    P9-4 run, where adX showed 8448 samples against FlexASIO's 4224.
17. **A virtual loopback is not steady.** FlexASIO over VB-Cable moves its delay while
    the stream runs, so `tools/measure_latency.py` calibrates after each take too and
    retries a take whose loopback moved (`--attempts`), rather than reading the move as
    an alignment error. The tool also gained `--asio`, and `Engine` its `asio` argument.
18. **There was no MIDI note recording.** P9-6 asked for bend "on input recording"; a
    keyboard could play but nothing kept what it played. MIDI takes (`record/MidiTake`)
    log input at arrangement ticks on the audio thread and become a pattern through the
    SMF importer, so a take bends exactly as an imported file does.
19. **Clock pulses are placed by samples, not by interpolated ticks.** At 120 bpm a tick
    is six frames, so interpolating the block's tick range put pulses two frames out at
    block 64 (`midi_clock_out_through_the_engine`). They are placed through the tempo map.
20. **A plugin's echo needs to say whose it is.** Without a gesture flag, a freshly loaded
    plugin's value report and a user's turn of its knob looked alike (P10.5-2). The wire's
    `WireParam` gained `flags`; CLAP gestures and VST3 `performEdit` set it.
21. **A larger `EngineCore` overflowed a test's stack.** `RenderEngine` holds the core by
    value and `decode_async_does_not_block` puts one on the stack; an 8192-entry take log
    (128 KB) was too much in Debug. The log is 2048 entries, drained at UI rate.

