# Phase 9 — Plugins & MIDI · L

| | |
|---|---|
| **Status** | Not started |
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

- [ ] `midi_roundtrip_latency` measured under 10 ms on real hardware, recorded.
- [ ] A VST3 instrument and a VST3 effect load, automate, save and reload
      (FINAL_PLAN's stated gate), verified with at least three real third-party
      plugins as well as the fixture.
- [ ] A CLAP instrument and effect do the same.
- [ ] Every `adx_test_plugin` misbehavior mode is handled without taking down the
      host, and each has a passing test.
- [ ] A crashing plugin during playback does not interrupt playback of the rest
      of the project — demonstrated live, not only in a test.
- [ ] ASIO works with a real interface, with the latency readout matching a
      measured loopback.
- [ ] MIDI learn works from the Phase 6 generic editor on both native and plugin
      parameters.
- [ ] `headless_render_with_plugins` passes in CI.
- [ ] FINAL_PLAN.md §8 gains rows for the VST3 SDK, CLAP and RtMidi; §10 Phase 9
      row updated, and its "may need its own plan document" note is resolved by
      pointing at this file.

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
