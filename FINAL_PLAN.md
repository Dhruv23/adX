# adX — FINAL PLAN

**This file is the sole source of truth for what adX is and where it is going.**
Any other document in this repository is either reference data (`docs/`) or a
historical artifact of the first iteration (`_archive/`). If this file and any
other file disagree, this file wins. If a decision is not written here, it has
not been made.

---

## 1. What we are building

**adX is a music production environment with the feature surface of FL Studio,
built from scratch, whose project format is a human-readable, hot-reloadable,
git-diffable text file.**

Two halves, and the tension between them is the whole point of the project:

- **A complete DAW.** Channel rack, patterns, playlist, piano roll, a real
  mixer with arbitrary routing, native instruments, native effects, automation,
  audio recording, time-stretching, offline export. Mouse-first. Fast.
- **A text-first project format (`.adx`).** Every project is a plain text file
  you can open in an editor, diff in git, generate from a script, hot-reload
  while it plays, and write in a Tidal-style mini-notation instead of clicking
  notes. Nothing you can do in the GUI is unrepresentable in text, and nothing
  you can write in text is invisible to the GUI.

No DAW does the second half properly. That is the reason for adX to exist. The
first half is the price of admission — a text format is worthless if the tool
around it cannot actually make music.

### Non-goals

Written down so they never get relitigated:

- **Not a DAW clone in appearance.** FL Studio's *capability* is the target,
  not its skin, its keyboard map, or its project compatibility.
- **Not a plugin.** adX is a host, never a VST/AU/CLAP guest.
- **Not cross-platform on day one.** Windows x64 first. The engine stays
  portable C++20 with no Win32 in it, so macOS/Linux is later work, not a
  rewrite. The audio backend abstraction exists from day one for this reason.
- **Not collaborative/multi-user.** Single user, local files.

Live performance is **in** scope — see §5.11 and Phase 11. It is deliberately
the last thing built, and §7's Phase 3 carries a checkpoint so the transport
does not foreclose it.

---

## 2. Architecture

### 2.1 The split

**Engine: C++20. Frontend: Python 3.12 + PySide6 (Qt 6). Bridge: pybind11.**

```
┌──────────────────────────────────────────────────────────────┐
│  app/   Python + PySide6/QML                                 │
│         window layout, menus, dialogs, tool logic,           │
│         browser, undo UI, preferences, scripting console     │
│                                                              │
│    ┌──────────────────────────────────────────────────┐      │
│    │  hot surfaces (piano roll, playlist, mixer,      │      │
│    │  waveforms, meters, scope, spectrum)             │      │
│    │  → geometry built in C++, uploaded once,         │      │
│    │    drawn by Qt's scene graph on the GPU          │      │
│    └──────────────────────────────────────────────────┘      │
└────────────────────────────┬─────────────────────────────────┘
                             │  pybind11 · GIL released in every
                             │  call · zero-copy numpy views
┌────────────────────────────┴─────────────────────────────────┐
│  bindings/   adx_engine (native extension module)            │
├──────────────────────────────────────────────────────────────┤
│  engine/     C++20 static lib — no Qt, no Python, no UI      │
│                                                              │
│    main thread          │  audio thread (RtAudio, ~5 ms)     │
│    project model        │  graph render · voices · DSP       │
│    commands/undo        │  ZERO allocation, ZERO locks,      │
│    .adx parse/write     │  ZERO Python, ZERO exceptions      │
│         └── lock-free SPSC queues + shared ring buffers ─────┤
└──────────────────────────────────────────────────────────────┘
```

### 2.2 Why this split, and the three rules that make it survivable

Python + Qt was chosen for iteration speed on a UI surface that will be large
and will change constantly, and for native access to numpy/scipy/ML tooling in
the analysis features. It is the right call **only if** the following three
rules are treated as non-negotiable invariants, because a DAW UI is the one
application class where Python's weaknesses bite hardest.

> **Rule 1 — The audio thread never touches Python.**
> No callbacks into Python from `process()`. No Python object lifetime
> reachable from the audio thread. The GIL does not exist below the binding
> layer. A violation here is an audible dropout, not a slow frame.

> **Rule 2 — The FFI boundary is crossed O(interactions), never O(notes) or
> O(frames×notes).**
> Dragging a note is *one* call. Drawing 10,000 notes is *one* call returning a
> zero-copy `numpy` view over a C++-owned vertex buffer — not 10,000 Python
> objects. Any API that returns a `list` of per-note Python objects is a bug.

> **Rule 3 — Every engine call that does real work releases the GIL.**
> `py::gil_scoped_release` around parsing, rendering, decoding, analysis,
> geometry building. Python must never block the Qt event loop waiting on C++.

The practical consequence for rendering: **Python owns layout, C++ owns
geometry.** The piano roll is a `QQuickItem` whose `QSGGeometryNode` vertex
buffer is filled by the engine. Panning and zooming are transform changes on
retained GPU geometry — zero repaint cost, zero Python per frame. Python only
re-asks the engine for geometry when the *data* changes, and handles hit
testing, selection, and tool behavior at interaction rate (tens of events per
second, not tens of thousands).

Meters, playhead, scope, and spectrum are read on a 60 Hz `QTimer` as a single
zero-copy array read from the engine's shared ring buffers. One FFI call per
frame, total.

### 2.3 Repository layout

```
adX/
├── FINAL_PLAN.md              ← this file; the source of truth
├── README.md                  ← build/run instructions (created in Phase 0)
├── plans/                     ← per-phase execution plans, phase_0..phase_11 (§9)
├── engine/                    ← C++20 static lib. No Qt. No Python. No UI.
│   ├── core/                  ids, time, tempo map, fixed-capacity containers
│   ├── rt/                    lock-free queues, arenas, RT-safety assertions
│   ├── dsp/                   oscillators, filters, envelopes, FFT, resampling
│   ├── instruments/           additive, sampler, VA, drumsynth, granular
│   ├── effects/               the insert-effect library
│   ├── graph/                 node graph, mixer routing, PDC
│   ├── transport/             playhead, loop, scheduling, sync
│   ├── project/               Project/Channel/Pattern/Playlist/Mixer + commands
│   ├── format/                .adx read/write, MIDI I/O, audio file I/O
│   ├── analysis/              melody extraction, timbre fit, BPM, form cleanup
│   ├── render/                offline render, stems, encoders
│   └── geometry/              UI geometry builders (Rule 2 lives here)
├── bindings/                  pybind11 → adx_engine
├── app/                       Python + PySide6/QML frontend (source root)
│   └── adx/                   the importable package
│       ├── panels/            piano roll, playlist, mixer, rack, browser, …
│       ├── widgets/           knobs, faders, meters, scope
│       ├── theme/             QML styling
│       └── scripting/         user-facing Python API + console
├── tests/
│   ├── cpp/                   Catch2 — DSP, parser, scheduler, commands
│   ├── python/                pytest — bindings, UI logic, round-trips
│   └── golden/                render-hash regression corpus
├── docs/
│   ├── adxFormat.md           ← v1 spec (HISTORICAL — see §6)
│   ├── C418.md                ← sound-design reference, still current
│   ├── STAKILLAZ.md           ← sound-design reference, still current
│   └── examples/              *.adx corpus — the parser's regression fixtures
└── _archive/                  ← first iteration, read-only reference
    ├── src-cpp/               all previous C++ + its CMakeLists
    ├── plans/                 plan.md, live-PLAN.md, UI-Refactor.md, …
    ├── assets/                samples/, reference mp3s, reference .mid
    ├── scratch/               loose junk from the old root
    └── build/                 old CMake cache (gitignored; safe to delete)
```

---

## 3. What survives from iteration one

The first iteration is in `_archive/src-cpp/`. It is a reference to port *from*,
never a tree to build. Nothing links against it. Every item below is a
deliberate port with a stated reason, not a copy-paste.

### 3.1 Port with high fidelity — this code is genuinely good

| From `_archive/src-cpp/` | Goes to | Why it survives |
|---|---|---|
| `src/AdxParser.cpp`, `include/AdxParser.h` | `engine/format/adx/` | The reason the project exists. Comment-stripping that correctly distinguishes `# comment` from `F#5`, unknown-key tolerance, deferred `PATTERN=` compilation so section order doesn't matter. Rewrite the *grammar* for v2 (§6); keep the *lexing discipline* exactly. |
| `include/AudioEffect.h`, `src/AudioEffects.cpp` | `engine/effects/` | Freeverb, tanh distortion, bitcrush, chorus, 3-band RBJ EQ. Clean interface: `processSample` / `typeName` / `isEquivalent` / `clone`. The `clone()`-for-offline-render discipline is exactly right and gets kept. Atomic params, main-thread-only construction. |
| `src/PatternCompiler.cpp`, `include/PatternCompiler.h` | `engine/format/mininotation/` | 762 lines of working Tidal/Strudel mini-notation: subdivision, alternation, `*n` `/n` `!n` `@n`, Euclidean `(k,n,r)`, stacking, seeded-PRNG probability, scale degrees, `rev`/`fast`/`every` transforms. Deterministic by design so hot-reload and offline export agree. This is a differentiator; do not rewrite it from scratch. |
| `src/AudioEngine.cpp` — the *synthesis* half | `engine/instruments/additive/` | polyBLEP VA oscillators, unison/detune/spread, TPT state-variable filter, formant bank, pink noise, pitch-drop sub, vibrato, glide, dual ADSR. The DSP math is sound. The *scheduling* half around it is not — see §3.3. |
| `include/AudioEngine.h::AudioTap` | `engine/rt/` | Correct overwrite-style SPSC ring: release-store cursor, acquire-load read, no dequeue, drops rather than blocks. Generalize to N typed rings (meters, scope, spectrum, per-track peaks). |
| `src/ExportRenderer.cpp` | `engine/render/` | Renders through a *fresh engine instance on the same DSP path* as realtime — the only way offline and realtime stay identical. WAV16/24/f32, MP3 (shine), FLAC. Keep the architecture verbatim; extend to stems. |
| `src/SimpleFFT.cpp`, `include/SpectrogramHistory.h` | `engine/dsp/` | Self-contained radix-2 FFT + Hann + log-magnitude, and a clean fixed-capacity spectrogram ring. No dependency needed. |
| `src/ParticleVisualizer.cpp` | `engine/geometry/` | Pre-allocated particle pool, envelope-follower onset detection, no per-frame allocation. Becomes a geometry builder feeding a QML scene-graph node. |
| `src/ClipPeakCache.cpp` | `engine/geometry/` | Mipmap-tiered min/max waveform peaks with correct invalidation on `(path, pitch, stretch, reversed)`. Exactly the right structure for zoomable waveforms; feeds Rule 2's vertex buffers directly. |
| `src/TrackCleaner.cpp` | `engine/analysis/` | Phrase segmentation → similarity → clustering → form matching → Markov blueprint → quantized rebuild. Deliberately written as pure, testable, side-effect-free stages. Unusual and worth keeping. |
| `src/TimbreAnalyzer.cpp` | `engine/analysis/` | Derives an additive patch from a reference mixdown using MIDI notes as ground truth — Goertzel probes at known harmonic multiples, median across notes so reverb averages out. Smarter than blind ML timbre guessing. |
| `src/MidiImporter.cpp` | `engine/format/midi/` | Dependency-free SMF parser, exact tick→beat conversion, per-channel splitting. Needs a writer added. |
| `src/MelodyExtractor.cpp` | `engine/analysis/` | basic-pitch ONNX audio→MIDI. Port as-is but make ONNX **optional** at build time (§8.2). |
| `src/BpmDetector.cpp`, `src/AudioFileLoader.cpp`, `src/AudioClipProcessor.cpp`, `src/PatchLibrary.cpp` | `engine/analysis/`, `engine/format/audio/` | Small, correct, no reason to rewrite. RubberBand offline pitch/stretch stays. |

### 3.2 Port the *ideas*, discard the code

| Concept | Where it came from | What changes |
|---|---|---|
| Lock-free UI→audio event queue | `AudioData.h::AudioEvent` | The `trivially_copyable` + `static_assert` discipline is right and stays. The event set is rebuilt for the new project model. |
| GC bin for retired snapshots | `AudioEngine::m_sequenceGarbageBin` | Right instinct (audio thread never calls `delete`), wrong mechanism. Becomes a proper deferred-reclaim queue drained by a reaper on the main thread. |
| Breakpoint automation w/ shared evaluator | `EvaluateAutomationLane` | The "one evaluator shared by engine and UI so they cannot disagree" rule is excellent and becomes a general principle. The dotted-path string targets (`"effect.Reverb.mix"`) get replaced by resolved integer parameter IDs — string comparison per block is not acceptable. |
| Hot reload | `main.cpp::CheckForHotReload` | Keep the behavior, drop the 500 ms `stat()` poll for a real filesystem watcher, and make reload *diff-and-patch* rather than *rebuild-and-replace* so playing voices are not cut. |
| In-app live-code editor | `LiveCodeEditor.cpp` | The concept is core to adX. The implementation (ImGui text box scanning for `PATTERN=` lines by string prefix) is replaced by a proper editor panel with real syntax highlighting over an engine-side incremental parser. |

### 3.3 Do not port — these are the reasons the rewrite is happening

Each of these is a real defect in the archived engine, not a stylistic
complaint. They are listed so the rewrite is measured against them.

1. **The audio thread allocates.** `AudioEngine::process()` constructs
   `std::vector<ScheduledEvent> scheduledEvents` and
   `std::vector<ActiveClipRef> activeClips` on **every callback**. This is a
   malloc on the realtime thread and it is the single worst defect in the
   codebase. → Fixed-capacity RT arenas; a debug allocator hook that asserts
   if the audio thread allocates at all.

2. **Event dispatch is O(frames × events).** For every one of 512 frames, the
   code linearly rescans the entire `scheduledEvents` vector looking for
   `sampleOffset == i`. → Events sorted by offset once, consumed by a single
   advancing cursor.

3. **Note scheduling is O(all notes in the project) per block.** Every track's
   entire note vector is scanned every callback to find the handful that start
   in this block. At 86 callbacks/sec against a full arrangement this does not
   scale. → Per-track events pre-sorted with a resume cursor, invalidated only
   on seek.

4. **Note-off matches by pitch alone.** `handleNoteOff` searches voices by MIDI
   pitch with no track or note identity, so two tracks playing the same pitch
   cross-release each other's voices. The archived code documents this as an
   "accepted consequence" — it is not acceptable in a DAW. → Voices keyed by
   `(channelId, noteId)`.

5. **Hard 16-track cap.** `kMaxEngineTracks = 16`; track 17+ is silently folded
   onto bus 16. → Dynamic bus count, sized on the main thread at commit time.

6. **One global 64-voice pool shared by all instruments.** A busy pad steals
   the kick's voice. → Per-channel polyphony limits and per-instrument pools.

7. **The project model conflates everything.** `Track { patchName, notes,
   audioClips, effects, arp, volume, pan, sends }` is simultaneously an
   instrument, a pattern, a playlist lane, and a mixer strip. This is the
   structural reason the app cannot grow into a DAW. → §4.

8. **No undo. Anywhere.** Table stakes, entirely absent. → Command pattern over
   the project model, from commit one.

9. **No tests. Zero.** 9,205 lines of DSP and parsing with no test of any kind.
   → §9.

10. **UI is three monolithic files.** `DrawSequencerUI()` is one 1,219-line
    function; `main.cpp` is 1,871 lines of mutable globals
    (`attackMs`, `draftPatch`, `g_melodyJob`, …) interleaved with ImGui calls.
    → Panels as classes, state in the engine, no module-level mutable globals.

11. **`std::stof` + `try/catch` per token in the parser.** Throwing on every
    malformed field is slow and loses column information. →
    `std::from_chars`, structured diagnostics with line *and* column.

12. **Patch envelope curve shape is unrepresentable on disk.** The Bézier
    handles in the patch editor are UI-only; `.adx` stores only ADSR
    milliseconds, so saving and reloading silently flattens every curve to a
    linear ramp. → Curves are first-class in the format (§6).

---

## 4. The project model

FL Studio's power comes from one specific decomposition. adX adopts the
decomposition (not the UI), because the archived flat `Track` is precisely what
blocks every feature below.

```
Project
├── TempoMap            tempo + time-signature changes over time
├── Channel[]           an instrument instance + its settings  (FL: Channel Rack)
│   ├── Instrument      additive / sampler / VA / drumsynth / granular
│   └── routing         → which mixer insert it feeds
├── Pattern[]           a named, reusable bundle of musical data (FL: Patterns)
│   ├── NoteClip[]      notes for one channel        (edited in the Piano Roll)
│   └── AutomationClip[] breakpoint curves for one parameter
├── Playlist            the arrangement (FL: Playlist)
│   └── PlaylistTrack[]
│       └── Item[]      a placed PatternRef | AudioClip | AutomationClip
├── Mixer
│   ├── Insert[]        strip: gain, pan, mute, solo, effect slots, sends
│   │   └── Slot[]      one effect + wet/dry + on/off
│   └── Route[]         insert → insert, arbitrary DAG (not just → master)
└── Resources           sample pool, patch library, plugin scan cache
```

**Why each separation earns its place:**

- **Channel ≠ Pattern.** One instrument can appear in fifty patterns. The
  archived model forced a 1:1 binding, which is why you cannot reuse a riff.
- **Pattern ≠ Playlist item.** A pattern placed eight times in the playlist is
  one piece of data, and editing it updates all eight. This is FL's central
  ergonomic advantage and it is impossible without the split.
- **Channel ≠ Mixer insert.** Many channels routing into one insert is how
  drum-bus processing and grouped sidechaining work.
- **Route[] as a DAG.** Arbitrary insert→insert routing (with cycle detection)
  is what makes real submixing and parallel processing possible. The archived
  fixed `send → master delay/reverb` pair is a special case of this.

**Every mutation goes through a command.** `Command { apply(Project&),
revert(Project&), coalesceWith(Command&) }`. Undo/redo is a stack of these; a
note drag coalesces into one entry. The `.adx` writer, the GUI, and the Python
scripting API all mutate the project through the *same* command set — which is
what guarantees they can never drift apart.

**Commit protocol (main → audio thread).** A command mutates the main-thread
project, then publishes an immutable render snapshot to the audio thread via
the lock-free queue. The audio thread swaps the pointer between blocks and
hands the retired snapshot to the reaper. The audio thread never allocates,
never frees, never locks, never waits.

---

## 5. Feature target — FL Studio parity, mapped

The complete surface, grouped by area. §7 sequences it; this section is the
definition of "done".

### 5.1 Sequencing & editing
Channel rack with step sequencer · piano roll (draw/paint/slice/glue/strum,
velocity + per-note pan/cutoff/resonance/pitch lanes, ghost notes, scale
highlighting + snapping, chord and arp tools, quantize/humanize, riff
generator) · playlist with pattern/audio/automation clips, per-track mute,
grouping, time markers, arrangement snapshots · automation clips, envelope
controllers, LFO tool, peak controller, parameter linking with MIDI learn ·
multi-level undo with a visible history list.

### 5.2 Mixer & routing
Unlimited insert tracks · effect slots per insert with drag-reorder and
per-slot wet/dry + bypass · arbitrary insert→insert routing DAG · sends with
independent levels · sidechain routing as an explicit connection ·
automatic plugin delay compensation · per-insert EQ, gain, pan, polarity,
stereo separation · solo/mute/record-arm · per-insert and master metering
(peak, RMS, LUFS) · master limiter.

### 5.3 Native instruments
Additive (ported, extended past 16 harmonics) · multi-layer sampler with
loop modes, zones, velocity layers, round-robin · slicer (beat-sliced audio →
playable pads, BPM-aware) · virtual-analog subtractive (polyBLEP osc bank,
unison, filters — ported) · drum synth (kick/snare/hat/clap/tom models;
absorbs the archived hardstyle kick generator) · granular · FM operator synth ·
wavetable synth · sample-pool playback channel.

### 5.4 Native effects
Ported: reverb, distortion, bitcrush, chorus, 3-band EQ, ping-pong delay,
compressor, sidechain ducker.
New: parametric EQ with spectrum overlay · multiband compressor · limiter ·
gate/expander · transient shaper · flanger · phaser · tremolo/auto-pan ·
convolution reverb (IR loading) · saturation/tube models · vocoder · pitch
shifter · formant filter · stereo imager · frequency shifter · ring modulator ·
"gross beat"-style time/volume manipulator · spectral freeze.

### 5.5 Audio
Multi-format decode (WAV/MP3/FLAC/OGG/AIFF) · audio recording with
monitoring and punch-in · clip time-stretch and pitch-shift, independent
(RubberBand, ported) · beat detection and warp markers · slip editing,
fades, crossfades · reverse, normalize, trim, DC offset removal · destructive
clip editor · sample pool management with project-relative paths and
collect-and-save.

### 5.6 Plugins & I/O
VST3 host (scan, sandbox, parameter automation, state save/load) · CLAP host ·
ASIO/WASAPI with device + buffer selection and a latency readout · MIDI input
with device routing, channel filtering, and MIDI learn · MIDI file import
(ported) and export · MIDI clock/MTC sync.

### 5.7 Export
WAV 16/24/32f · MP3 · FLAC · OGG · stem export per channel/insert/pattern ·
MIDI export · project bundle (collect all samples) · tail length, dithering,
normalization, loop-region-only, split-at-markers · offline render through the
identical DSP path as realtime (ported), verified by golden-hash tests.

### 5.8 Text-first — adX's differentiator
`.adx` v2 as the canonical format (§6) · bidirectional GUI↔text (edit either,
both update) · filesystem-watch hot reload that patches rather than rebuilds ·
in-app code editor with syntax highlighting, inline diagnostics, and
playhead-synced step highlighting · mini-notation patterns (ported) ·
a documented Python scripting API over the same command set the GUI uses ·
headless CLI (`adx render`, `adx validate`, `adx fmt`, `adx diff`).

### 5.9 Analysis & generation
Audio→MIDI melody extraction (ported) · timbre analysis → patch (ported) ·
BPM and key detection · form analysis and structural cleanup (ported) ·
chord detection and suggestion · the C418 and STAKILLAZ genre suites
(`docs/C418.md`, `docs/STAKILLAZ.md`) as first-class preset + generator packs.

### 5.10 Visualization
Per-track and master metering · oscilloscope · spectrum analyzer (ported) ·
spectrogram waterfall (ported) · transient particle visualizer (ported) ·
zoomable clip waveforms from mipmapped peaks (ported) · loudness/LUFS meter ·
stereo vectorscope · phase correlation.

### 5.11 Live performance
Session view — a clip-launching grid alongside the arrangement · per-clip
independent playback positions, lengths and loop states · quantized launch
(next beat / bar / N bars / instant) with a queued-next indicator · scene
rows that fire a column of clips together · clip follow-actions (stop, next,
random, repeat N) · MIDI-controller pad mapping with velocity-sensitive
triggering and LED feedback where the device supports it · performance
recording that captures a live session back into the arrangement as ordinary
playlist items · tempo nudge and tap tempo · live mini-notation patterns as
launchable clips, which is where this joins §5.8 rather than duplicating it.

---

## 6. The `.adx` format, v2

`docs/adxFormat.md` documents **v1, and does so incompletely** — it predates
`NOISE`, `RESFILTER`, `FILTERENV`, `FORMANT`, `VIBRATO`, `GLIDE`, `OSC`, `MIX`,
`SEND`, `PATTERN`, `LOOP`, `MARKER`, the `[AUTOMATION]` section, and the
Bitcrush/Chorus/EQ effects, all of which the archived parser actually accepts.
It is retained as a historical reference. **The v2 specification will be written
as `docs/adx-format-v2.md` as the first deliverable of Phase 2 and is the only
normative spec thereafter.**

### Design rules

1. **Text is canonical.** Not an export target, not a sidecar. The `.adx` file
   *is* the project. Binary blobs (recorded audio, IRs) live beside it in a
   project folder and are referenced by relative path.
2. **Round-trip is lossless and stable.** Parse → write → parse is byte-identical
   for a canonically-formatted file. Enforced by a property test. This is what
   makes git diffs meaningful and is why §3.3.12 (unrepresentable Bézier curves)
   must be fixed.
3. **Diff-friendly.** Stable ordering, one musical event per line, no
   reflowing on save. Adding a note changes one line.
4. **Forward-tolerant.** Unknown keys and sections are preserved verbatim
   through a load/save cycle and reported as warnings — never dropped. A newer
   adX's file degrades gracefully in an older one, exactly as v1 intended.
5. **Diagnostics carry line and column.** Every warning and error is
   addressable by an editor.
6. **v1 loads.** A compatibility shim reads every v1 file, including
   `docs/examples/suffocation.adx` (551 lines exercising nearly the whole
   extended grammar), and upgrades it to the v2 model in memory. Those four
   example files are permanent parser regression fixtures.

### Structural shape

v2 mirrors the §4 model. New sections — `[CHANNEL]`, `[PATTERN]`,
`[PLAYLIST]`, `[MIXER]`, `[INSERT]`, `[ROUTE]`, `[TEMPO]` — join the existing
`[GLOBAL]`, `[PATCH]`, `[TRACK]`, `[AUTOMATION]`. Automation targets become
stable named parameter paths that resolve to integer IDs at load, so nothing
compares strings at block rate. The full grammar, with a normative EBNF, is the
Phase 2 deliverable.

---

## 7. Roadmap

Twelve phases, 0 through 11. Every phase ends in something runnable and tested;
no phase is a pure refactor with nothing to show. Phase numbering is the order
of execution.

Sizes: **S** ≈ days · **M** ≈ 1–2 weeks · **L** ≈ 3–6 weeks · **XL** ≈ months.
These are relative, not commitments.

Each phase below is a summary. Its **execution plan** — file manifest, design,
port map, named tests, definition of done, handoff — is `plans/phase_N.md`,
linked from each heading. Those files are subordinate to this one (§9): this
file owns *what* and *why*, they own *how*. Between them they cover every item
in §5 exactly once, so completing all twelve completes the repository.

### Phase 0 — Foundation · S · [plan](plans/phase_0.md)
Build system (CMake + `vcpkg`/`FetchContent`, one `cmake --preset` command),
Python packaging (`pyproject.toml`, `scikit-build-core`), CI on Windows x64,
Catch2 + pytest wired, `clang-format`/`clang-tidy`/`ruff`/`mypy` enforced.
**Done when** an empty engine builds, imports as `adx_engine` in Python, and a
trivial test passes in CI.

### Phase 1 — RT core · M · [plan](plans/phase_1.md)
`engine/rt/`: lock-free SPSC queue, typed shared rings (generalized `AudioTap`),
fixed-capacity RT containers, deferred-reclaim reaper, and a debug allocator
hook that **fails the test suite** if the audio thread allocates. RtAudio
backend abstraction; device enumeration; a stream that outputs silence.
**Done when** an RT-safety test runs a 60-second stream under the allocator
hook with zero violations and zero dropouts.

### Phase 2 — Project model, commands, `.adx` v2 · L · [plan](plans/phase_2.md)
The §4 model. Command pattern with undo/redo and coalescing. `docs/adx-format-v2.md`.
The v2 parser/writer (`from_chars`, line+column diagnostics, unknown-key
preservation) and the v1 compatibility shim. Headless CLI: `adx validate`,
`adx fmt`, `adx diff`.
**Done when** all four `docs/examples/*.adx` load, round-trip losslessly, and
survive a randomized command sequence with undo-to-empty restoring byte-identical
output.

### Phase 3 — Audio graph & scheduling · L · [plan](plans/phase_3.md)
Node graph with topological scheduling and PDC. The mixer: inserts, slots,
sends, arbitrary routing DAG with cycle detection. Sample-accurate transport
with a tempo map, loop, and seek. **Scheduling rewritten to fix §3.3 items
1–6**: sorted per-track events with resume cursors, cursor-consumed dispatch,
`(channelId, noteId)` voice identity, dynamic bus count, per-channel voice
pools.

> **Checkpoint — do not foreclose Phase 11.** Performance Mode (§5.11) needs
> *N* independent playback positions, not one. The archived engine has a single
> `double m_currentSamplePosition` and a single loop region, and that scalar is
> exactly what makes clip launching impossible to add later without a transport
> rewrite. So Phase 3 must model time as a **set of time sources** — the
> arrangement playhead is simply the first one — even though Phases 3–10 only
> ever instantiate one. Concretely: no code outside `engine/transport/` may
> assume a global "current position"; scheduling takes its time source as a
> parameter. Enforced by a test that runs two independent time sources through
> the scheduler at different positions and tempos. This costs very little now
> and is the whole cost of Phase 11 if skipped.

**Done when** a synthetic 200-track / 100k-note project renders offline
bit-identically to its realtime capture with zero audio-thread allocations,
and the two-time-source scheduler test passes.

### Phase 4 — Instruments & effects · XL · [plan](plans/phase_4.md)
`engine/dsp/` primitives, then **all of §5.3 and all of §5.4** onto the new
graph, plus multi-format decode, the sample pool, per-insert processing and
metering, and the patch/preset system with the C418 and STAKILLAZ packs. Run in
three tranches (see the plan): A is parity with iteration one — additive, VA,
sampler, the five archived effects, delay, compressor, limiter, parametric EQ,
gate; B is the rest of §5.3; C is the rest of §5.4. The whole DSP library lives
in one phase because it is pure DSP against a stable `Node` interface with no UI
dependency, and splitting it costs two rounds of the same review and test
scaffolding. Phase 5 may begin once tranche A is done; B and C run in parallel.
**Done when** (tranche A) `suffocation.adx` (v1, via the shim) renders and is
A/B-comparable to the archived build's output, and (full phase) every §5.3 and
§5.4 item exists with a numerical test and a golden hash.

### Phase 5 — Frontend foundation · L · [plan](plans/phase_5.md)
`app/` skeleton: PySide6 shell, dockable panels, theme, transport bar, browser.
The Rule-2 geometry pipeline end to end: `engine/geometry/` → pybind11
zero-copy buffers → `QSGGeometryNode`. First hot surface: the **piano roll**,
with full editing, and the mipmapped waveform renderer.
**Done when** the piano roll holds 60 fps while panning and zooming a
10,000-note pattern, measured, with a CI performance gate.

### Phase 6 — The DAW proper · XL · [plan](plans/phase_6.md)
Playlist, channel rack, mixer UI with live meters, plugin/instrument editor
panels, automation editing, browser with drag-and-drop, undo history panel,
preferences. The bulk of the GUI work.
**Done when** a complete track can be written, arranged, mixed and exported
without ever opening a text editor.

### Phase 7 — Text-first layer · M · [plan](plans/phase_7.md)
Bidirectional GUI↔text sync. Filesystem-watch hot reload that diffs and patches
(playing voices survive). In-app code editor with highlighting, diagnostics,
and playhead-synced step highlighting. Mini-notation ported and wired live.
Python scripting API + console over the same command set.
**Done when** a project can be edited in the GUI and an external editor
simultaneously, with changes flowing both ways and no dropouts on reload.

### Phase 8 — Audio & export · M · [plan](plans/phase_8.md)
Recording with monitoring and punch-in. Clip editing: stretch, warp markers,
slip, fades, reverse, normalize. Full export matrix, stems, project bundling,
MIDI export.
**Done when** golden-hash tests cover every export format and the corpus
renders identically across three consecutive builds.

### Phase 9 — Plugins & MIDI · L · [plan](plans/phase_9.md)
VST3 host (out-of-process scanning, sandboxed instantiation, parameter
automation, state persistence). CLAP host. MIDI input, routing, MIDI learn,
clock sync. ASIO.
**Done when** a VST3 instrument and effect can be loaded, automated, saved,
and reloaded, and a hardware MIDI keyboard plays a channel with measured
round-trip latency under 10 ms.

### Phase 10 — Analysis, visualization, polish · L · [plan](plans/phase_10.md)
Port the analysis suite (melody extraction, timbre fit, BPM, form cleanup) and
the visualizers (spectrum, waterfall, particles, meters, vectorscope, LUFS).
Key and chord detection, chord suggestion, the C418/STAKILLAZ generators.
Optimization pass, docs, installer.

**Done when** the benchmark project (64 channels, 200k notes, 8 plugins, all
analyzers open) holds every performance target with zero dropouts and zero
allocator-hook violations, every golden hash is unchanged by the optimization
pass, the full suite passes with `ADX_ENABLE_ONNX=OFF`, and the installer is
verified on a clean VM. **The performance targets are a hard entry criterion for
Phase 11** — §7's Phase 11 depends on them.

### Phase 11 — Live performance · L — **the final stage** · [plan](plans/phase_11.md)
Everything in §5.11. Last on purpose: it is the only feature that depends on
*all* of the others being finished and stable. Session view needs the playlist
(6) to have something to launch and to record back into; pad triggering needs
MIDI in and MIDI learn (9); launchable mini-notation clips need the text layer
(7); and performing on an engine that still drops out under load is pointless,
so it needs Phase 10's optimization pass behind it.

Because Phase 3's checkpoint already made time a set of sources, this phase is
mostly composition rather than surgery: a launch scheduler that quantizes
trigger requests to musical boundaries, a per-clip state machine (stopped →
queued → playing → queued-stop, with follow-actions), the session-view UI, and
a performance recorder that writes fired clips back as ordinary playlist items.
The one genuinely new engine concept is voice and effect-tail handling across a
clip stop — a launched clip that stops must release rather than cut, which the
archived engine's note-off handling already gets right and can be reused.

**Done when** a set can be performed end to end from a hardware pad controller
— clips launched and stopped on quantized boundaries, scenes fired, follow-
actions honoured — with no dropouts under the Phase 1 allocator hook, and the
recorded performance reopens as an editable arrangement that renders identically
to what was heard.

---

## 8. Technology decisions

| Concern | Decision | Reasoning |
|---|---|---|
| Engine language | C++20 | Ported code is C++20; concepts/ranges/`from_chars` all earn their place. |
| Frontend | Python 3.12 + PySide6 (Qt 6) | Chosen for UI iteration speed and the numpy/ML ecosystem. Viable **only** under §2.2's three rules. |
| Bridge | pybind11 | Mature, header-only, first-class numpy buffer protocol — which is what Rule 2 is built on. |
| Hot-surface rendering | QML + custom `QSGGeometryNode` | Retained GPU geometry. Pan/zoom is a transform, not a repaint. The only approach that survives Python at 60 fps. |
| Audio I/O | RtAudio (ported) + ASIO in Phase 9 | Already in use and working; abstracted behind an interface so it is replaceable. |
| Build | CMake + presets, `scikit-build-core` | One `pip install -e .` builds engine, bindings, and app together. |
| C++ tests | Catch2 | Fast, header-light, good matchers for float comparison. |
| Python tests | pytest + pytest-qt | Standard; pytest-qt covers the panel logic. |
| Time-stretch | RubberBand (ported) | Already integrated and working. |
| MP3 / FLAC | shine, libFLAC (ported) | Already integrated; permissive licensing. |
| Decode | miniaudio (ported) | Single header, decode-only, no device layer. |
| FFT | `SimpleFFT` (ported) | Sufficient for UI-rate analysis. Swap for pffft only if profiling demands it — not preemptively. |
| ONNX | **Optional** build flag | See §8.2. |
| Plugin hosting | VST3 SDK + CLAP | Phase 9. Out-of-process scanning so a bad plugin cannot take down the host. |

### 8.2 ONNX is optional

The archived `CMakeLists.txt` unconditionally downloads a 200 MB ONNX Runtime
zip and a model file to support one feature (audio→MIDI melody extraction).
That is most of the 833 MB build directory now sitting in `_archive/build/`.
In the rewrite, `ADX_ENABLE_ONNX` defaults **off**; the melody extractor is
compiled out and the UI entry point is hidden when it is off. A clean clone
must build in minutes, not in a download.

---

## 9. Engineering standards

These exist because iteration one had none of them, and that — more than any
individual defect — is why it stopped being pleasant to work in.

**Realtime safety.** The audio thread must not allocate, free, lock, block,
throw, log, touch Python, or call anything unbounded. Enforced by a debug
allocator hook that fails CI, not by convention.

**Testing.**
- Every DSP primitive has a numerical test (impulse response, frequency
  response, or known-value comparison).
- The parser has round-trip property tests and a fixture corpus
  (`docs/examples/`) that must never regress.
- The command system has a randomized-sequence test: apply N random commands,
  undo all N, assert the project is byte-identical to its initial state.
- Rendering has **golden-hash tests**: the corpus renders to a stable hash, so
  any unintended DSP change is caught immediately.
- The scheduler has sample-accuracy tests against hand-computed offsets.
- Performance gates in CI: the Phase-5 piano-roll frame budget is a *test*,
  not an aspiration.

**Code organization.**
- No module-level mutable globals. Ever. (`main.cpp` had ~15.)
- No function over ~150 lines. (`DrawSequencerUI` was 1,219.)
- One class or one coherent free-function group per file.
- Headers declare; implementation files implement. No 500-line headers.
- Comments explain *why*, never *what*. The archived code did this well and it
  is the single best thing about it — keep the habit.

**Dependencies.** Every new dependency must be justified in this file before it
is added. Prefer a small, well-understood implementation over a large,
general one.

**Documentation.** This file is updated whenever the plan changes. Phase
completion is recorded in §10.

There is exactly one permitted family of planning documents besides this one:
`plans/phase_0.md` … `plans/phase_11.md`, one per §7 phase. They are
**subordinate and non-overlapping** — this file owns *what* and *why*, each
phase file owns *how* for its phase alone, and where they disagree this file
wins. Every §5 feature is assigned to exactly one phase file, so the twelve
together are a complete specification of the remaining work. A phase file is
updated only by the phase that owns it; scope moving between phases is a change
to *this* file first.

No other parallel plan documents. The first iteration accumulated five
(`plan.md`, `live-PLAN.md`, `UI-Refactor.md`, `INTERLEAVED-PLAN.md`,
`context.md`) with overlapping scope and no ownership rule, and they drifted out
of sync with each other and with the code.

---

## 10. Status

| Phase | State | Notes |
|---|---|---|
| Analysis & archive | **Done** | 2026-09-10. Iteration one analyzed; keepers identified (§3.1); tree moved to `_archive/`; `docs/` established. |
| Phase plans | **Done** | 2026-09-10. `plans/phase_0.md` … `plans/phase_11.md` written; §5 fully assigned, no gaps. |
| [0 — Foundation](plans/phase_0.md) | Not started | |
| [1 — RT core](plans/phase_1.md) | Not started | The allocator hook's positive-control test gates everything after it. |
| [2 — Project model, commands, `.adx` v2](plans/phase_2.md) | Not started | |
| [3 — Audio graph & scheduling](plans/phase_3.md) | Not started | Carries the Phase 11 transport checkpoint. |
| [4 — Instruments & effects](plans/phase_4.md) | Not started | Owns all of §5.3 and §5.4; three tranches. |
| [5 — Frontend foundation](plans/phase_5.md) | Not started | Where the §2.2 bet is proven or disproven. |
| [6 — The DAW proper](plans/phase_6.md) | Not started | |
| [7 — Text-first layer](plans/phase_7.md) | Not started | The half no other DAW has (§1). |
| [8 — Audio & export](plans/phase_8.md) | Not started | |
| [9 — Plugins & MIDI](plans/phase_9.md) | Not started | Largest risk; out-of-process plugin sandbox. |
| [10 — Analysis, visualization, polish](plans/phase_10.md) | Not started | Its performance targets gate Phase 11. |
| [11 — Live performance](plans/phase_11.md) | Not started | Final stage. Transport prerequisite lands in Phase 3. |

### Honest assessment of scale

FL Studio is roughly twenty-five years of work by a funded team. Phases 0–6
produce a genuinely usable DAW with a text-first workflow that nothing else
has. Phases 7–10 close the gap on parity. Phase 11 is the capstone and is last
because it depends on every phase before it being finished and stable.

Phase 9 (plugin hosting) is the single largest risk, because it is the only
phase whose failure modes are controlled by third-party code. Its plan
(`plans/phase_9.md`) is correspondingly the most detailed, and it is the
document this file previously anticipated Phase 9 would need.

The scope is enormous. Sequencing it so there is always something that plays
music is how it stays worth doing.
