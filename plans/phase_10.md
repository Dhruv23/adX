# Phase 10 — Analysis, visualization, polish · L

| | |
|---|---|
| **Status** | Not started |
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
| `key_detector_corpus` | correct key on a 40-track corpus with known keys, > 85 % exact / > 95 % including relative |
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

- [ ] `bench_full_project` meets all eight targets. **Phase 11 is blocked until
      it does** — FINAL_PLAN §7 makes optimization a prerequisite, and this is
      where that is enforced.
- [ ] `golden_unchanged_by_optimization` passes, with any re-blessed hash
      reviewed and justified in the commit.
- [ ] Every §5.9 and §5.10 item exists and is tested.
- [ ] The full suite passes with `ADX_ENABLE_ONNX=OFF`, and a clean clone builds
      in under 5 minutes (FINAL_PLAN §8.2's actual requirement).
- [ ] `docs/` is complete and its examples execute in CI.
- [ ] The installer produces a working MSI, verified on a clean VM.
- [ ] `bench_one_hour_soak` passes overnight.
- [ ] FINAL_PLAN.md §10 Phase 10 row updated.

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
