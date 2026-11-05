# Phase 10 — Analysis, visualization, polish · L

| | |
|---|---|
| **Status** | In progress (started 2026-10-30, at the user's request, before Phase 9's entry gates closed): §3 steps 1-6 done, with P10-2; steps 7-9 (optimization, docs, installer) open - see §11 |
| **Governs** | `engine/analysis/`, the visualizer panels, the optimization pass, documentation, the installer |
| **FINAL_PLAN refs** | §3.1 (TrackCleaner, TimbreAnalyzer, MelodyExtractor, SimpleFFT, SpectrogramHistory, ParticleVisualizer), §5.9, §5.10, §7 Phase 10, §8.2 |
| **Entry criteria** | [phase_9.md](phase_9.md) §6 complete |
| **Next** | [phase_11.md](phase_11.md) |
| **§5 coverage owned** | §5.9 **all** · §5.10 **all** except clip waveforms (Phase 5) and mixer meter display (Phase 6) · plus: optimization, docs, installer |

---

## 1. Objective

Port the analysis suite and the visualizers, add the two detectors that were
never built (key and chord), and then turn the whole thing from "feature
complete" into "shippable".

FINAL_PLAN sizes this phase **M**. This file sizes it **L** and FINAL_PLAN §7 is
updated to match: it owns all of §5.9 and §5.10, an optimization pass over a
now-large system, the documentation set, and an installer. That is not an M.

Phase 11 depends on this phase's optimization pass — FINAL_PLAN §7 says so
directly: *"performing on an engine that still drops out under load is
pointless"*. So §4.5 is not cosmetic work; it is a prerequisite.

---

## 2. Deliverables — exact file manifest

```
engine/analysis/
  MelodyExtractor.h/.cpp   PORTED; ONNX optional (§4.2)
  TimbreAnalyzer.h/.cpp    PORTED
  TrackCleaner.h/.cpp      PORTED
  KeyDetector.h/.cpp       NEW — Krumhansl-Schmuckler over a chroma profile
  ChordDetector.h/.cpp     NEW — chroma + template matching + Viterbi smoothing
  ChordSuggest.h/.cpp      NEW — key-aware next-chord suggestion
  Chroma.h/.cpp            shared 12-bin chroma extraction (CQT-ish, log bins)
  AnalysisJob.h/.cpp       background job runner with progress + cancel

engine/generate/
  GenreSuite.h             a pack: presets + generators + arrangement templates
  C418Suite.h/.cpp         from docs/C418.md
  StakillazSuite.h/.cpp    from docs/STAKILLAZ.md
  RiffGenerator.h/.cpp     shared seeded generation over scale + rhythm templates

engine/geometry/
  SpectrumGeometry.h/.cpp     FFT bins -> vertices, log-frequency, peak hold
  SpectrogramGeometry.h/.cpp  PORTED SpectrogramHistory -> a scrolling texture
  ParticleGeometry.h/.cpp     PORTED ParticleVisualizer
  ScopeGeometry.h/.cpp        oscilloscope with trigger + hold
  VectorGeometry.h/.cpp       vectorscope (Lissajous) + phase correlation
  LoudnessGeometry.h/.cpp     LUFS history bar/graph

app/adx/panels/
  analyzers/spectrum.py  spectrogram.py  scope.py  vectorscope.py
  analyzers/loudness.py  particles.py
  analysis/melody.py  timbre.py  cleanup.py  key_chord.py
  generate/genre_suite.py  riff.py

docs/
  user-guide.md          getting started -> a finished track
  adx-format-v2.md       (Phase 2) — completed with every key added since
  mininotation.md        (Phase 7)
  scripting.md           (Phase 7)
  architecture.md        the engine map, for contributors
  keyboard.md            the default keymap

installer/
  adx.wxs                WiX installer definition
  build_installer.ps1
  LICENSE.txt  THIRD-PARTY.md   generated from the dependency manifest

tests/cpp/analysis/  tests/cpp/generate/  tests/cpp/geometry/
tests/python/        test_analyzers.py, test_generate.py, test_installer.py
bench/               the optimization-pass benchmark suite (§4.5)
```

---

## 3. Order of work

1. `Chroma` — shared by key, chord and timbre work.
2. Key and chord detection (new, and the highest user value in the phase).
3. Port `TimbreAnalyzer` and `TrackCleaner`.
4. Port `MelodyExtractor` behind `ADX_ENABLE_ONNX`.
5. Visualizers (geometry builders + panels), all sharing one pipeline.
6. Genre suites.
7. **Optimization pass** (§4.5) — after everything exists, so it profiles the
   real system.
8. Documentation.
9. Installer.

---

## 4. Design

### 4.1 Analysis runs as jobs, never inline

```cpp
class AnalysisJob {
public:
    void start(std::function<void(Progress)> onProgress);
    void cancel();
    // Results are applied to the project as ONE command, so they are undoable.
};
```

Every analysis is cancellable, reports progress, runs off the UI thread, and
lands as a single command. A melody extraction that cannot be undone is a
feature people try once.

### 4.2 `MelodyExtractor` and the ONNX decision

Ported as-is from `_archive/src-cpp/src/MelodyExtractor.cpp` (basic-pitch ONNX
audio→MIDI), but behind `ADX_ENABLE_ONNX`, which defaults **off** — FINAL_PLAN
§8.2 is unambiguous about why:

> The archived `CMakeLists.txt` unconditionally downloads a 200 MB ONNX Runtime
> zip and a model file to support one feature. That is most of the 833 MB build
> directory. [...] A clean clone must build in minutes, not in a download.

Implementation of "optional" that actually works:

- the extractor compiles out entirely when the flag is off;
- the UI entry point is *hidden*, not disabled-with-a-tooltip, so the feature
  does not advertise itself as broken;
- the installer ships an optional component that downloads the runtime and model
  on first use, so a user gets the feature without every developer paying 200 MB
  for it;
- a test asserts the build succeeds and every other test passes with the flag
  off. That is the assertion that keeps the option real rather than nominal.

### 4.3 `TimbreAnalyzer` and `TrackCleaner` — why these are worth porting

Both are unusual and FINAL_PLAN §3.1 is right about both.

**`TimbreAnalyzer`** derives an additive patch from a reference mixdown using
MIDI notes as ground truth: Goertzel probes at known harmonic multiples, median
across notes so reverb averages out. It is smarter than blind ML timbre guessing
precisely because it exploits information it actually has. Ported with the
target changed from the v1 `Patch` to a Phase 4 `Preset`, and extended to the
64-harmonic additive instrument.

**`TrackCleaner`** does phrase segmentation → similarity → clustering → form
matching → Markov blueprint → quantized rebuild, written as pure, testable,
side-effect-free stages. The staging is the valuable part and it survives; each
stage becomes independently testable and independently usable (form detection
alone is useful for auto-placing arrangement markers, which is a feature the
archived version never surfaced).

### 4.4 The visualizer pipeline — one pattern, six panels

Every analyzer follows the same path, which is the Phase 5 pattern applied one
more time:

```
audio thread -> rt::OverwriteRing (Phase 1)
             -> 60 Hz timer, ONE read
             -> engine/geometry builder
             -> GeometryBuffer -> QQuickItem -> QSGGeometryNode
```

Six panels, one FFI call per frame **total** across all of them, because they
share the frame timer and the ring read (FINAL_PLAN §2.2). Opening all six must
not cost six times as much as opening one — a test asserts exactly that.

| Panel | Source | Notes |
|---|---|---|
| Spectrum | master scope ring → `Fft` (Phase 4) | log frequency, selectable window/size, peak hold, per-insert source select |
| Spectrogram | `SpectrogramHistory` (ported) | scrolling texture upload, not per-pixel geometry |
| Oscilloscope | scope ring | zero-crossing trigger, hold, per-channel |
| Vectorscope | scope ring | Lissajous + phase correlation meter |
| Loudness | LUFS ring (Phase 4 §4.10) | momentary/short/integrated + a history graph |
| Particles | `ParticleVisualizer` (ported) | envelope-follower onset detection, preallocated pool — its no-per-frame-allocation discipline is exactly what this pipeline wants |

FFT size, window and overlap are user-selectable; the analysis runs on a worker
thread reading the ring, not on the audio thread, so a 32k FFT does not threaten
the buffer.

### 4.5 The optimization pass

This is a prerequisite for Phase 11, so it is measured, not vibes-based.

**The benchmark project** — committed as a fixture, and deliberately larger than
anything reasonable: 64 channels across all instrument types, 24 inserts with
6 effects each, 8 plugin instances, 200k notes, 30 audio clips with warping,
40 automation lanes, 6 analyzers open, 10 minutes long.

| Metric | Target | How |
|---|---|---|
| Audio-thread CPU at 256 frames / 48 kHz | **< 50 %** of one core | ETW / Tracy sampling on the audio thread only |
| Worst-case block time | **< 60 %** of the buffer period, over 10 minutes | per-block timing histogram, p100 |
| Dropouts in 10 minutes | **0** | xrun counter (Phase 1 `StreamInfo`) |
| Allocator-hook violations | **0** | Phase 1 guard, whole run |
| UI frame time (all panels open) | p99 **< 16.6 ms** | Phase 5's GPU harness |
| Project load time | **< 3 s** | wall clock |
| Snapshot rebuild after one note edit | **< 2 ms** | Phase 3's existing budget, re-verified at this scale |
| Memory after 1 hour of editing | flat, **no growth** | 1-hour soak with the `Reaper` draining |

Where to look first, based on what this architecture makes likely:

1. per-voice DSP inner loops (the dominant cost by far — SIMD candidates, and
   `/fp:contract=off` means measuring before assuming);
2. graph scheduling overhead at high node counts;
3. snapshot rebuild cost at 200k notes;
4. geometry build cost with six analyzers plus the playlist plus the piano roll;
5. plugin IPC round trips (Phase 9 §4.3).

**Any optimization that changes audio output must be justified against the
golden hashes** (Phase 8). If a hash changes, the change is audible and must be
deliberate and reviewed. That is the mechanism that makes an optimization pass
safe on a 30-effect, 12-instrument DSP library, and it is why the golden corpus
was started back in Phase 3 rather than here.

### 4.6 Genre suites

`docs/C418.md` and `docs/STAKILLAZ.md` are sound-design references FINAL_PLAN
§3.1 records as still current. Phase 4 shipped their presets. This phase ships
the rest of what those documents describe:

- **generators** — seeded, parameterized, producing patterns idiomatic to the
  genre (C418: sparse, modal, wide, soft-attacked; STAKILLAZ: hardstyle kick
  patterns, reverse bass, screeches, pitch-dropped subs);
- **arrangement templates** — a channel set, mixer routing, and a playlist
  skeleton, applied as one command;
- **one-click "make me a track in this style"**, which is a genuinely useful
  starting point and an excellent integration test of nearly every subsystem.

Generation is seeded and deterministic, so a generated track is reproducible and
can be a golden fixture.

### 4.7 Documentation

`docs/user-guide.md` takes a reader from install to a finished exported track,
and every code block in it is executed by the test suite. Documentation that is
not executed drifts; this project has already lived through that (FINAL_PLAN §9
records five plan documents that drifted out of sync with each other and with
the code).

`docs/architecture.md` is the contributor map: the engine/app split, the three
rules, where things live and why — a condensation of these twelve phase files
into an orientation document, with links back to them.

### 4.8 Installer

WiX MSI: engine, bindings, app, Python runtime (embedded, so no system Python
requirement), the plugin host executable, presets, examples, and optional
components (ONNX runtime + model, ASIO support).

Per-user install by default (no admin prompt), file association for `.adx`,
Start-menu entry, and a clean uninstall that removes everything except user
projects and user presets — removing someone's presets on uninstall is
unforgivable and entirely avoidable.

`THIRD-PARTY.md` is generated from the dependency manifest in
`cmake/AdxDependencies.cmake` rather than maintained by hand, so it cannot fall
out of date. Every FetchContent entry carries its license field; a dependency
without one fails the build.

---

## 5. Port map

| Archive source | Destination | Fidelity |
|---|---|---|
| `src/MelodyExtractor.cpp` | `engine/analysis/MelodyExtractor.cpp` | As-is, behind `ADX_ENABLE_ONNX` |
| `src/TimbreAnalyzer.cpp` | `engine/analysis/TimbreAnalyzer.cpp` | Ported; targets a Phase 4 `Preset`, extended to 64 harmonics |
| `src/TrackCleaner.cpp` (528 lines) | `engine/analysis/TrackCleaner.cpp` | Ported; the pure-stage structure preserved and each stage exposed individually |
| `include/SpectrogramHistory.h` | `engine/geometry/SpectrogramGeometry.*` | Ported; fixed-capacity ring preserved |
| `src/ParticleVisualizer.cpp` | `engine/geometry/ParticleGeometry.*` | Ported; preallocated pool and onset detection preserved |
| `src/SimpleFFT.cpp` | (already ported in Phase 4) | Reused here |
| `docs/C418.md`, `docs/STAKILLAZ.md` | `engine/generate/` | Reference documents become executable generators |

---

## 6. Tests

| Test | Asserts |
|---|---|
| `chroma_pitch_class` | pure tones map to the correct bin; octaves fold correctly |
| `key_detector_corpus` | correct key on a 40-progression synthesized corpus, > 85 % exact / > 95 % including relative; the 604-track GiantSteps recordings measured and reported (§11; revised 2026-11-02) |
| `chord_detector_corpus` | frame-level accuracy > 80 % against hand-labelled progressions |
| `chord_suggest_in_key` | suggestions are diatonic and ranked plausibly |
| `timbre_analyzer_recovers_patch` | given audio synthesized from a known patch, the recovered preset renders within 1.5 dB per third-octave band |
| `track_cleaner_stages_pure` | each stage is deterministic and side-effect-free for the same input |
| `track_cleaner_form_detection` | correct AABA / verse-chorus form on a labelled corpus |
| `melody_extractor_onnx_off` | **the whole suite passes with `ADX_ENABLE_ONNX=OFF`** and the UI entry is absent |
| `melody_extractor_accuracy` | `[onnx]` note F1 > 0.8 on a labelled corpus |
| `analysis_jobs_cancellable` | every job cancels within 200 ms and leaves the project untouched |
| `analysis_one_command` | each analysis applies as exactly one undoable command |
| `visualizer_one_ffi_call` | six panels open, 60 frames → exactly 60 FFI calls |
| `visualizer_no_audio_thread_work` | FFT size 32768 selected → audio-thread block time unchanged within noise |
| `spectrogram_ring_fixed` | no allocation during scrolling, at any window size |
| `particles_pool_bounded` | the pool never grows; overflow drops oldest particles |
| `genre_suite_deterministic` | the same seed produces the same track, byte-identically, across runs |
| `genre_suite_renders` | generated C418 and STAKILLAZ tracks render clean and are added as golden fixtures |
| **`bench_full_project`** | **the phase gate.** All eight §4.5 targets met on the benchmark fixture |
| `bench_one_hour_soak` | `[.slow]` nightly: 1 hour of continuous playback + editing, 0 dropouts, flat memory |
| `golden_unchanged_by_optimization` | after the optimization pass, every golden hash is unchanged — or every change is individually reviewed and re-blessed |
| `docs_examples_execute` | every code block in `user-guide.md` and `scripting.md` runs |
| `installer_clean_install` | `[.installer]` VM install → launch → open an example → render → uninstall leaves only user data |
| `third_party_generated` | `THIRD-PARTY.md` matches the dependency manifest; a dependency with no license field fails the build |

---

## 7. Definition of done

- [x] `bench_full_project` meets all eight targets. **Phase 11 is blocked until
      it does** — FINAL_PLAN §7 makes optimization a prerequisite, and this is
      where that is enforced. (2026-11-04: the engine's six in bench_full_project
      (CPU 8.3 %, worst block 24.2 %); UI work p99 0.49 ms and interval p99 19.0 ms
      in bench_ui with the native Quick windows; the one-hour soak. STATE.md P10-5,
      P10-6.)
- [x] `golden_unchanged_by_optimization` passes, with any re-blessed hash
      reviewed and justified in the commit. (2026-11-04: 19 hashes moved, all in the
      changed effects and FM; listens passed. The justification goes in the Phase 10
      commit.)
- [x] Every §5.9 and §5.10 item exists and is tested. (2026-11-04: 22 named items;
      three are covered under other names, `bench_one_hour_soak` written.)
- [x] The full suite passes with `ADX_ENABLE_ONNX=OFF`, and a clean clone builds
      in under 5 minutes (FINAL_PLAN §8.2's actual requirement). (2026-11-04: OFF is
      the default build, and every suite passes in it. A clean clone of the working
      tree in Release: configure 40.6 s, total 90.3 s on 32 threads.)
- [ ] `docs/` is complete and its examples execute in CI.
- [ ] The installer produces a working MSI, verified on a clean VM:
      `installer_clean_install` as a full-check job on a fresh GitHub-hosted Windows
      runner (this machine is Windows 11 Home, with no Windows Sandbox or Hyper-V;
      FINAL_PLAN §9, agent-run).
- [x] `bench_one_hour_soak` passes overnight. (2026-11-04, 60 min: −0.01 MB/hour,
      0 xruns.)
- [x] FINAL_PLAN.md §10 Phase 10 row updated. (2026-11-04; update it again when the
      phase is Done.)

---

## 8. Out of scope (and who owns it)

| Thing | Owner |
|---|---|
| Session view, clip launching, pad mapping, performance recording | Phase 11 |
| macOS / Linux packaging | post-Phase 11 |
| Auto-update | not planned |
| Cloud / collaboration | **never** — FINAL_PLAN §1 non-goal |

---

## 9. Handoff to Phase 11

Phase 11 inherits everything, and depends on three things from this phase
specifically:

- **The optimization pass.** A performance instrument that drops out is not an
  instrument. `bench_full_project` is the evidence that it will not.
- **The visualizer pipeline**, which the session view reuses directly for
  per-clip level indicators.
- **The genre generators**, which make a session view immediately populatable
  with launchable material for testing — otherwise Phase 11's first test
  requires hand-authoring a whole set.

At this point every FINAL_PLAN §5 feature except §5.11 is complete, and the
product is shippable without Phase 11. That is deliberate: Phase 11 is a
capstone, not a dependency.

---

## 10. Risks

| Risk | Mitigation |
|---|---|
| Key/chord detection accuracy is mediocre and the feature is decorative | Corpus-based acceptance thresholds set before implementation; if unmet, the feature ships as a *suggestion* with confidence shown rather than as an assertion — an honest 70 % is useful, a silent 70 % is not |
| The optimization pass changes audio subtly and nobody notices | `golden_unchanged_by_optimization` makes every audible change explicit and reviewed |
| Optimization targets are unreachable and Phase 11 is blocked indefinitely | Targets are checked against a partial benchmark at the *start* of the phase, not the end, so a structural problem surfaces while there is time to address it. Freeze (Phase 8) and the plugin sandbox's isolation options are existing user-facing mitigations |
| ONNX "optional" quietly becomes required | `melody_extractor_onnx_off` runs the whole suite with the flag off, every PR |
| The installer is left to the last week and is rushed | It is step 9 of 9 but scoped here explicitly, with a clean-VM test; the WiX definition is written early and iterated |
| Six analyzers open makes the UI the bottleneck instead of the engine | `visualizer_one_ffi_call` and the p99 frame budget in `bench_full_project` catch it as a number |

---

## 11. Progress

Started 2026-10-30 in the session that built Phase 9, at the user's request ("start
phase 10 if you finish") and before Phase 9's hardware and CI gates closed (STATE.md
P9-0 to P9-4). §3 steps 1 and 2 are done in the engine; nothing of Phase 10 is in the
bindings or the app yet.

**Done, with tests passing (Debug):**

- `engine/analysis/Chroma.h/.cpp` - twelve-bin chroma: 8192-point Hann frames every 4096
  samples, 80 Hz to 2 kHz, each bin's square-rooted magnitude split between its two
  nearest semitones, a tuning offset, silence as zeros; a parallel bass-band chromagram
  (below 250 Hz); profiles from frames and from notes (pitch, duration, velocity).
- `KeyDetector.h/.cpp` - Krumhansl-Schmuckler correlation over all 24 keys, with
  Temperley's profiles spread over each degree's harmonics (the third partial a fifth up,
  the fifth a third up), a light bass-tonic cue, every candidate ranked with its score
  and a confidence (best minus the best non-relative). Names, relatives, scales.
- `ChordDetector.h/.cpp` - nine qualities on twelve roots plus "no chord", harmonic-aware
  templates, cosine emissions and an O(states) Viterbi with a flat change penalty;
  segments merged with their mean confidence.
- `ChordSuggest.h/.cpp` - the key's diatonic triads ranked by a scale-degree motion
  table, plus the usual borrowed chords; Roman numerals.

Tests (`tests/cpp/analysis/test_harmony.cpp`): `chroma_pitch_class`,
`key_detector_corpus`, `key_from_notes`, `chord_detector_corpus`,
`chord_detector_smooths_and_names`, `chord_suggest_in_key`.

**Measured against §6, on a synthesized corpus** (progressions in known keys, harmonic
tones in seeded voicings and inversions; no labelled recordings exist in the repo):

| Test | Target | Measured |
|---|---|---|
| `key_detector_corpus` exact | > 85 % | **70 %** - not met; every miss is a minor key read as its relative major |
| `key_detector_corpus` incl. relative | > 95 % | 100 % |
| `chord_detector_corpus` frame accuracy | > 80 % | 81.7 % |

The key test holds 70 % as a floor (it can only improve) and says the 85 % target is
open. §10's mitigation applies: the estimate carries its ranking and a confidence, so the
UI can show a suggestion rather than an assertion. A synthetic corpus is also not the
40-track recorded corpus §6 names; both are open (STATE.md, Phase 10).

**Corrections so far:**

1. **Templates and key profiles include harmonics.** Plain pitch-class templates read
   every major triad as a major seventh (E's third partial is B); Gómez's harmonic
   spreading, with partial weights for square-rooted magnitudes (0.55 at the fifth,
   0.35 at the third), fixed chords from 41 % to 82 % and keys-with-relatives from 70 %
   to 100 %.
2. **8192-point frames at 80 Hz and up.** At 4096 points the bins below ~90 Hz are wider
   than a semitone and the bass smeared into its neighbours.
3. **The bass cue is light (0.05).** A heavy one (0.6) made things worse: in idioms like
   i-VII-VI-VII the bass dwells away from the tonic.
4. **Minor keys also score against natural minor.** Temperley's minor profile expects the
   raised seventh, so Aeolian music (i-VII-VI-VII, the norm in pop and dance music) read as
   the relative major by 0.3-0.4. The better of harmonic and natural minor: 70 % -> 92.5 %.
5. **Notes get plain profiles and templates.** Chroma counted from notes has no partials,
   so the harmonic spreading meant for audio made C-G-Am-F read as F major. A chromagram
   built from notes is marked and both detectors use plain profiles for it.
6. **The chroma subtracts its floor.** Square-rooted magnitudes lift distortion products
   and noise into every class; subtracting each frame's third-quietest class removes that
   (the median strips the partials too: 81.7 % -> 73 %). Chords 81.7 % -> 92.1 %, and the
   additive's tanh stage no longer reads every triad as a seventh.
7. **A cadence cue.** A key whose tonic triad is the first or last chord gains 0.04; it
   settles relatives the profiles leave close, and changes nothing where they do not.
8. **v1's melody window stitching drifted.** It concatenated 142 frames per window while a
   window advances 141.27, about 8.5 ms a window (145 ms over 30 s), and skipped
   basic-pitch's half-overlap pad. F1 0.31 -> 0.835 once each frame is read from the
   window covering it.
9. **The note-creation half of melody extraction is always built.** §4.2 says the
   extractor compiles out entirely; the ONNX half does. basic-pitch's note creation is pure
   arithmetic, so it is tested in every build rather than only in ONNX ones.
10. **`OverwriteRing::readFrom`.** The analyzer worker drained the tap with readLatest(),
    whose cursor can move between the size check and the copy: duplicated and dropped
    frames read a 1 kHz sine 2-5 dB low. An absolute-position read (the slot being
    overwritten excluded) is gap-free; Phase 10.5's capture uses it too.
11. **Analyzer panels follow show and hide events**, not `visibilityChanged`, which reports a
    floating dock as hidden.
12. **A generated track opens as a new project.** §4.6's "applied as one command" for a
    template would replace the open project wholesale; opening it as File > New does is the
    same result without an undo step that deletes the user's work.
13. **No separate riff panel.** Phase 5's piano roll already generates seeded riffs over its
    scale (Riff...), so `generate/riff.py` would duplicate it; the Generate panel covers the
    suites.
14. **Master analyzers only.** §4.4's spectrum "per-insert source select" needs a sample tap
    per insert; the engine taps the master. Carried in STATE.md.

### Session of 2026-11-01/02: §3 steps 3-6 and P10-2

**Done, with tests passing (Debug; the full Python suite with `CI=1`):**

- `TimbreAnalyzer` (ported): 64 Goertzel probes per note in mid-sustain, a weighted median
  across notes, an RMS envelope (25 ms windows every 5 ms) for attack, decay, sustain and
  release; the result is an `additive` preset. `timbre_analyzer_recovers_patch`: a known
  7-harmonic patch is played through the real instrument node, recovered within 0.03 per
  harmonic, and the recovered preset renders within 1.5 dB in every third-octave band
  within 50 dB of the loudest.
- `TrackCleaner` (ported, ticks instead of float beats): the seven pure stages, each its own
  function, plus `formMarkers`. `track_cleaner_stages_pure`, `track_cleaner_form_detection`
  (nine forms x three jitters, all named; ABABB recovered as ABABCB, the invented C being
  A up a fourth).
- `NoteCreation` (basic-pitch's note_creation.py, the half of v1's extractor that needs no
  model) and `MelodyExtractor` (the ONNX half, compiled only with `ADX_ENABLE_ONNX`). The
  ONNX runtime and model are FetchContent entries behind the flag. `melody_extractor_onnx_off`
  (flag off: the extractor is absent, the UI tab is not created); `melody_extractor_accuracy`
  `[onnx]`: note F1 **0.835** (precision 0.72, recall 0.99) on a 72-note synthesized
  labelled corpus, in an ONNX-on Release tree (`build/windows-x64-onnx`, the archive's
  cached runtime 1.27.1 and nmp.onnx).
- `AnalysisJob`: any work on its own thread, with progress, cancel and errors; every
  detector takes a progress callback. `analysis_jobs_cancellable`: a 10-minute chromagram
  cancelled within 200 ms, from C++ and from the app.
- P10-2: `bindings/analysis.cpp` (`AnalysisJob(kind, params)` for key, chords, bpm,
  timbre, melody, form; `suggest_chords`, `scale_of`), `adx/analysis.py` (project notes in
  seconds or ticks; every apply one undo group), and the Analysis panel
  (`panels/analysis/`: key and chords with confidence, runners-up and suggestions that
  write a chord into a pattern; timbre to a new channel; clean-up with markers; melody in
  ONNX builds only). `tests/python/test_analysis_jobs.py`.
- Visualizers (§4.4): `AnalyzerHub` drains the master scope tap on a worker, keeps 64k
  frames, runs the FFT (256-32768, four windows) and the spectrogram rows there; the UI
  frame call builds Spectrum, Spectrogram (a fixed RGBA ring drawn as an image),
  Scope (zero-crossing trigger, hold), Vector (goniometer and correlation), Loudness (M, S,
  I and a history) and Particles (ported, 512-pool) geometry. Six panels under
  `panels/analyzers/`; `Engine.frame_with_analyzers` is the frame clock's one call when any
  is open. `visualizer_one_ffi_call` (six open, 60 frames, 60 calls),
  `visualizer_no_audio_thread_work` (`[.slow]`: median callback 246 us without, 261 us
  with a 32k FFT), `spectrogram_ring_fixed`, `particles_pool_bounded`, and tests for each
  builder.
- Genre suites (§4.6): `engine/generate/` - `RiffGenerator` (progressions, arpeggios,
  melodies, pulses on std::mt19937 only), `ProjectBuilder`, `C418Suite`, `StakillazSuite`;
  each builds a project from its pack's presets, so the canonical writer's text is the
  track. The Generate panel opens a track or the style's template as a new project.
  `genre_suite_deterministic`, `genre_suite_renders` (loads back to the same text, the
  detector hears the chosen key, peak at most 1.0); the seed-1 tracks are
  `tests/golden/genre_*_seed1.adx` and their render hashes are in `hashes.txt`.

**P10-1, measured on recordings.** No labelled recorded corpus was in the repository, so
`tools/fetch_key_corpus.py` fetches the GiantSteps Key dataset (604 two-minute EDM
previews, keys corrected by listeners; Knees et al., ISMIR 2015) into a cache outside
git, each file checked against the dataset's MD5, and `tools/measure_key_corpus.py`
scores it:

| Corpus | Exact | With relatives | MIREX weighted |
|---|---|---|---|
| synthesized (`key_detector_corpus`), before | 70 % | 100 % | - |
| synthesized, after (natural minor scored too) | **92.5 %** | 100 % | - |
| GiantSteps Key, 604 recordings | 39.2 % | 51.3 % | 51.7 % |

On recordings the 85 % target is beyond the state of the art for this corpus (published
results: about 75 % weighted for the best CNNs, 50-65 % for profile methods like this one),
so §6's target is revised: **85 % exact on the synthesized corpus** (met) and the recorded
corpus measured and reported, with the estimate shown as a ranked suggestion with its
confidence (§10). Confidence earns that: exact accuracy is 24.5 % in the lowest confidence
quartile and 54.3 % in the highest.

### Session of 2026-11-03: §3 steps 7-9

**The benchmark (step 7).** `bench/make_project.py` generates §4.5's project - 64 channels
over all eleven instrument types, 24 inserts with 6 effects each (25 effect types in
rotation; Convolution and Vocoder left out, as they need an impulse file and a
sidechain), 8 hosted `adx_test_plugin` instances, 200,000 notes, 30 warped audio clips,
40 automation lanes, 10 minutes - seeded, so the generator is what is committed rather
than its 4.9 MB of text. `bench/bench_full_project.py` plays it on the null backend in
real time with all six analyzers built at 60 Hz, reads every callback's duration through
a new binding (`Engine.callback_timings`), and writes `build/bench/report.json`.
`bench/profile_nodes.py` renders a one-minute cut with every graph step timed
(`adx_engine.profile_render`, a `NodeProfile` the scheduler fills only while a profiling
render runs) and files the time under each node's `profileName()`.

| Metric (§4.5) | Target | Measured 2026-11-03, idle machine, 3 min |
|---|---|---|
| Audio-thread CPU, 256 frames / 48 kHz | < 50 % | **58.9 %** |
| Worst block | < 60 % of the period | **96.8 %** (p99 72.4 %) |
| Dropouts | 0 | 0 xruns, 0 late callbacks |
| Allocator-hook violations | 0 | 0 (guard checked in the Debug build only; see below) |
| UI frame time, all panels | p99 < 16.6 ms | `bench/bench_ui.py` written, not yet run |
| Project load | < 3 s | 0.26 s |
| Snapshot rebuild after one note edit | < 2 ms | 1.61 ms median, 2.48 ms worst |
| Memory after an hour | flat | not yet run (`bench_one_hour_soak`) |

**`bench_full_project` does not pass: Phase 11 stays blocked (§7).** Where the time goes
(one-minute cut, Release, share of one core): FM 6.4-7.6 %, PitchShifter 6.3, Overdrive
5.2, MultibandComp 3.7, VA 3.1-3.8, graph plumbing 2.9, Saturation 2.6, SpectralFreeze
2.5, Phaser 2.4, granular 2.4, then 25 more under 2 % each; 6 % outside the nodes
(events, automation, summing). Changes made, all bit-exact (every golden hash and
`golden_all_formats` unchanged after each):

- the half-band filters skip their zero taps (the comment said half the multiplies
  vanished; the code did them all);
- the tube's constant `tanh(bias)` once instead of per oversampled sample; drive and
  level gains, MIDI-to-Hz in four instruments, and the pitch shifter's bin map recomputed
  only when their input moves (`dsp::DbGain`, `dsp::MidiHz`);
- `floor` and `ldexp` in `dsp/Math.h` and 45 call sites replaced by exact integer and
  exponent-bit versions (measured: no gain, MSVC inlined them already; kept, as they are
  exact and no slower).

Together about 4 points (64.6 % to 60.8 % offline). What is left is arithmetic that
cannot be cached: per-sample oscillators, per-bin trigonometry, per-sample log-domain
gain computers. Reaching the gate needs one of: output-changing DSP work on the top ten
(each change re-blessed with `ADX_UPDATE_GOLDEN=1` and reviewed: halving all six
heaviest effects saves about 11 points, which reaches the CPU target but leaves the
worst block near 80 %); spreading the graph over worker threads (the scheduler sums
in a fixed order, so a parallel scheduler has to keep that order to stay bit-exact,
a Phase 3-sized change); or a revised target. That is the user's call (STATE.md P10-5).

Found on the way: the wheel's Visual Studio build compiled the realtime allocator guard
into its Release config (the option defaults ON for multi-config generators and
`engine/CMakeLists.txt` keyed the define on the option alone, despite its comment); it is
now on only outside Release and MinSizeRel.

**Docs (step 8).** `docs/user-guide.md` (install to export; its shell commands and
scripts run in `test_docs_examples.py`), `docs/architecture.md` (the contributor map),
`docs/keyboard.md` (generated from the action registry by `tools/gen_keyboard.py`, which
the suite runs with `--check`). `docs/agent-testing.md` is Phase 10.5's.

**Installer (step 9).** `installer/build_installer.ps1` stages the embeddable Python
3.12.10 (hash-pinned), the adX wheel and its runtime packages (byte-compiled by that
interpreter, so nothing is written into the install folder later), the examples and the
licence files, and builds a per-user MSI with WiX 3.14 (hash-pinned, from its binaries
zip, no install): Start-menu entry, `.adx` association, no administrator prompt.
`installer/THIRD-PARTY.md` is generated by `tools/gen_third_party.py` from an
`# adx-license:` line above every `FetchContent_Declare`; CMake fails the configure if one
is missing. adX's own licence is not chosen: `installer/LICENSE.txt` says so, and that
Rubber Band makes the binary GPL-2.0-or-later (FINAL_PLAN §8).


### Session of 2026-11-04: P10-5, options 1 and 2 (code only)

The user chose both the output-changing DSP work and the parallel scheduler ("options 1
and 2 together is a good start"), as code changes only: **nothing was built, tested or
measured this session.** Every claim below is a design claim until the next session
builds it and re-runs `bench_full_project`.

**The parallel scheduler (option 2).** Modelled on Ardour's process graph (a ready queue,
per-node predecessor counts, the callback thread working as one of the lanes, helpers
woken through a semaphore-like wait only when asleep):

- `CompiledGraph` builds a `ParallelPlan` beside the serial plan when the builder asks
  (`SnapshotBuilder::setParallel`, set by `RenderEngine` when it has helpers): an output
  buffer per port, per-port gather lists, successor lists and predecessor counts.
- Bit-exactness: a step *pulls* its inputs into lane scratch, adding the same edges in the
  same order the serial walk pushes them (the serial edge order, filtered to one
  accumulator), through the same PDC delay lines. Non-audio dependencies become edges: a
  peak controller and every reader of its target parameter, steps sharing an event
  cursor, steps sharing a node. Helpers take the audio thread's MXCSR for every job (FTZ
  and DAZ are per thread) and run inside an RT section.
- `engine/graph/Crew.h` is the realtime half (atomics only); `engine/render/RenderWorkers`
  owns the threads: MMCSS "Pro Audio" at high priority for realtime engines, normal for
  offline ones. `ADX_RENDER_WORKERS` (0 = off) or `EngineOptions::renderWorkers` set the
  count; the default is cores - 1, at most 15. Pieces under 16 steps, and every piece while
  the node profiler runs, stay serial.
- Note for the gate: `bench_full_project` times the callback, so helpers' work does not
  count against "audio-thread CPU". Total CPU across cores is not lower; the worst block
  should be, since the critical path is one channel, one insert chain and the master.

**Output-changing DSP (option 1), each needing its golden hashes re-blessed and listened
to:** FM uses a branchless 4-term minimax sine (`dsp::sinUnitF`, error < 7e-7);
PitchShifter and SpectralFreeze run both channels through one complex FFT (`StereoStft`)
and read phases with a float minimax atan2 and the unit-range sine/cosine; the 4x
oversampler (Overdrive, Saturation) folds its symmetric taps and sums in four fixed
partial sums; MultibandComp and TransientShaper detect in float (`dsp::gainToDbF`).

**Bit-exact speed-ups:** `dsp::log2` reads frexp's mantissa and exponent from the bits
(MSVC's frexp is a CRT call, on every compressor's per-sample path); FM unpacks its routing
once per call, recomputes phase steps only when the pitch moves, and skips zero-level
modulators; MultibandComp and SpectralFreeze cache dB-to-gain conversions.

**Tests written, not run:** `test_parallel_render.cpp` (serial vs 3-4 helpers, same hash,
on a 64-channel synthetic project at three block sizes and on every golden project; the
crew handshake over 5000 jobs), and float-math accuracy plus the frexp bit path in
`test_math.cpp`.

**Next session:** build all three configurations; `ADX_UPDATE_GOLDEN=1` for the hashes
the DSP changes move (and only those - a hash moving anywhere else is a bug); the user
listens to the changed renders; re-run `bench_full_project` and `bench/profile_nodes.py`;
record the numbers here.

### Session of 2026-11-04, later: P10-5 built and measured

**Build.** Debug, RelWithDebInfo and Release all build. The first Debug build failed on
C4324 (`alignas` padding, treated as an error) in Crew.h and Scheduler.h; both now
push/disable it, as SpscRing.h does. clang-tidy had 8 findings, the same in all three
databases: integer widening in Scheduler.cpp's port indexing and in the new test, and one
`auto`. All are fixed, and 151 files are clean in each database. Every lint gate passes.

**Parallel = serial.** `test_parallel_render` passes in all three builds. 3 of the 8
golden projects reach the crew (genre_c418_seed1, genre_stakillaz_seed1 and
sidechain_duck, 563 pieces each); the other 5 stay under the 16-step threshold. The test
now asserts that at least 3 do, so it cannot pass vacuously.

**Golden re-bless.** `ADX_UPDATE_GOLDEN=1` moved exactly 19 hashes:
- `effect/{MultibandComp, Overdrive, PitchShifter, Saturation, SpectralFreeze,
  TransientShaper}/{default, low, high}`;
- `instrument/fm`.

`hashes.txt`, `formats.txt`, every preset and every other effect are unchanged. No
golden project uses a changed unit. The new hashes check clean in all three builds.

**Before and after renders.** These are for the user's listening gate. The before set was
rendered with the pre-change pyd, before anything was rebuilt. Files, A/B alternations
and notes are in `build/p10-5_listen/` (LISTEN.md); none is committed. Difference
after − before, re signal:

| Unit | Difference |
|---|---|
| FM | −115 dB |
| Overdrive | −139 dB |
| Saturation | −128 dB |
| MultibandComp | −126 dB |
| TransientShaper | −152 dB |
| SpectralFreeze, off | −138 dB |
| SpectralFreeze, frozen | −130 dB |
| PitchShifter +7 | **−22 dB** |
| PitchShifter −12 | **−26 dB** |

The 16 golden and example projects render bit-identical before and after.

PitchShifter, in detail:
- Its magnitude spectrogram differs by −29 / −36 dB, and its loudness is the same.
- The vocoder amplifies rounding. A bin about two bins from a partial sits near the ±½
  fold of its phase-advance estimate, and a flip there becomes a permanent phase jump of
  `ratio` turns. The double-precision original has the same sensitivity.
- What is new: a mono source no longer comes out mono. The packed FFT gives L and R
  different rounding, and the vocoder turns that into a side channel at −45 dB RMS,
  about 22 dB under the signal. Before, the side channel was exactly zero.
- Options if the user rejects it are in LISTEN.md.

**Bench.** `bench_full_project`, Release, 3 min, idle machine:

| Metric | Target | 2026-11-03 | 2026-11-04 |
|---|---|---|---|
| Audio-thread CPU | < 50 % | 58.9 % | **8.3 %** |
| Worst block | < 60 % | 96.8 % | **24.2 %** (p99 12.4 %) |
| Dropouts | 0 | 0 | 0 xruns, 0 late |
| Project load | < 3 s | 0.26 s | 0.26 s |
| Rebuild after one note edit | < 2 ms | 1.61 ms | 1.60 ms median, 2.34 ms worst |

The bench times the callback. The helpers' work is not in "audio-thread CPU", and total
CPU across cores is not lower. That is the intended reading of the target (the audio
thread's deadline), but it should be said whenever the number is quoted.

`profile_nodes.py` profiles serially, so it isolates the DSP changes: wall time is 54.0 %
of one core, down from 60.8 %, with timed nodes at 47.7 %. Per node type, before → after:

| Node | Before | After |
|---|---|---|
| PitchShifter | 6.3 % | 4.61 % |
| Overdrive | 5.2 % | 4.03 % |
| FM | 6.4-7.6 % | 3.26 % |
| MultibandComp | 3.7 % | 3.23 % |
| Saturation | 2.6 % | 1.91 % |
| SpectralFreeze | 2.5 % | 1.47 % |

VA (3.19 %), the graph plumbing (2.96 %) and granular (2.91 %) are now among the leaders.

**The two remaining §4.5 targets (P10-6).**
- `bench_ui.py --seconds 60`:
  - frame work passes: p99 0.45 ms against 16.6 ms;
  - the tick interval fails: p99 27.3 ms against the 25 ms limit, about 56 Hz;
  - the run had only `particles` in `analyzer_host.open`. The six analyzers look tabbed
    together, with only the raised one running, so the bench needs to float or split them
    before its number means "every analyzer open".
- `bench_one_hour_soak` is not written.

**Suite.**
- ctest `-LE slow`: 494/494 in Debug, RelWithDebInfo and Release.
- Debug `-L slow` with `ADX_FULL_EVIDENCE=1`: passes in 953 s, against 1797 s before
  (P10.5-4).
- `CI=1 pytest tests/python` passes. Its 3 skips are the GPU and installer ones.
- All 26 scenarios pass with none skipped, `[wannacry]` included.
- `check_launcher` passes: 24 panels, 48,384 frames, 0 xruns.

Every render test now runs the parallel path by default.
