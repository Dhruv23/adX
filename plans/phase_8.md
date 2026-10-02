# Phase 8 — Audio & export · M

| | |
|---|---|
| **Status** | Not started |
| **Governs** | recording, clip editing, time-stretch/warp, the full export matrix, stems, bundling, MIDI I/O, `adx render` |
| **FINAL_PLAN refs** | §3.1 (ExportRenderer, MidiImporter, AudioClipProcessor, BpmDetector), §5.5, §5.7, §5.6 MIDI file I/O, §7 Phase 8, §9 golden hashes |
| **Entry criteria** | [phase_7.md](phase_7.md) §7 complete |
| **Next** | [phase_9.md](phase_9.md) |
| **§5 coverage owned** | §5.5 **all** except multi-format decode (Phase 4) · §5.7 **all** · §5.6 MIDI file import and export only · §5.8 `adx render` |

---

## 1. Objective

Close the loop between audio going in and audio coming out. Until this phase,
adX can only synthesize; after it, adX can record, edit and deliver.

The gate is a regression gate, deliberately:

> **Done when** golden-hash tests cover every export format and the corpus
> renders identically across three consecutive builds.

That is the point at which "did I break the sound?" stops being a question
anyone has to answer by listening.

---

## 2. Deliverables — exact file manifest

```
engine/record/
  InputCapture.h/.cpp       audio-thread input -> lock-free ring
  DiskWriter.h/.cpp         writer thread; streaming WAV, preallocated buffers
  RecordSession.h/.cpp      arm, punch-in/out, takes, comping lanes
  Monitoring.h/.cpp         direct/software monitoring + latency compensation
  CountIn.h                 metronome + pre-roll

engine/clip/
  ClipEdit.h/.cpp           slip, trim, fade, crossfade — non-destructive
  WarpMarkers.h/.cpp        marker-based time map over an audio clip
  Stretch.h/.cpp            RubberBand offline; PORTED from AudioClipProcessor
  Destructive.h/.cpp        reverse, normalize, DC removal, gain, silence trim
  Freeze.h/.cpp             render a channel to audio; swap in place, reversible

engine/analysis/
  BpmDetector.h/.cpp        PORTED
  OnsetDetector.h/.cpp      spectral flux -> transients; feeds warp + slicer

engine/render/
  ExportJob.h/.cpp          one render request; runs off the UI thread
  Encoders.h                the encoder interface
  WavEncoder.h/.cpp         16/24/32f, RF64 for >4 GB
  Mp3Encoder.h/.cpp         shine; PORTED
  FlacEncoder.h/.cpp        libFLAC; PORTED
  OggEncoder.h/.cpp         libvorbis — NEW dependency (§4.6)
  Dither.h/.cpp             TPDF + noise shaping
  StemRender.h/.cpp         per channel / per insert / per pattern
  Bundle.h/.cpp             collect-and-save; rewrites paths via Phase 7's Patch

engine/format/midi/
  MidiImporter.h/.cpp       PORTED
  MidiExporter.h/.cpp       NEW — SMF type 1 writer
  MidiFile.h                shared chunk/varint plumbing

app/adx/panels/
  export/dialog.py          the full matrix (§4.5)
  record/toolbar.py         arm, input select, monitor, punch, count-in
  clip_editor/panel.py      waveform editing, warp markers, destructive ops
app/adx/cli.py              `adx render` implemented

tests/cpp/record/  tests/cpp/clip/  tests/cpp/render/  tests/cpp/midi/
tests/golden/      grows to full coverage: every format × every fixture
tests/python/      test_export_matrix.py, test_record.py, test_bundle.py
```

---

## 3. Design

### 3.1 Recording

The audio thread's only recording job is to copy input frames into a ring. It
never touches a file.

```cpp
class InputCapture {
public:
    // AUDIO THREAD. noexcept. Copies `frames` of `in` into the ring.
    void capture(const float* in, uint32_t frames, uint32_t channels) noexcept;
private:
    rt::SpscRing<float, 1 << 20> m_ring;     // ~5.5 s stereo at 96 kHz
    std::atomic<uint64_t> m_overrunCount;
};
```

`DiskWriter` is a dedicated thread that drains the ring into a streaming WAV
with preallocated buffers, `FILE_FLAG_SEQUENTIAL_SCAN`, and a periodic header
update so a crash mid-take still leaves a playable file. An overrun (writer fell
behind) is counted and surfaced, never silently dropped — a recording with a
silent gap is worse than a recording that stopped.

**Latency compensation** is the part that is usually wrong. A recorded take must
land where the performer heard it, which means shifting it back by
(input latency + output latency + PDC-reported graph latency). Phase 1's
`StreamInfo` and Phase 3's `Pdc` both already report their numbers, so this is
arithmetic rather than guesswork — and there is a calibration routine (loopback
a click through the interface, measure the true round trip) for the cases where
the driver lies, which some do.

**Punch-in/out** records only within a region, with pre-roll and post-roll.
**Takes** stack as lanes on one playlist track; comping selects ranges across
lanes into a comp. Each take is an ordinary audio clip, so nothing downstream
needs to know a comp is a comp.

**Monitoring:** direct (interface-level, zero latency, adX just does not
duplicate it) or software (through the channel's effect chain, at the cost of
round-trip latency, which is displayed so the choice is informed).

### 3.2 Clip editing

Non-destructive edits are clip *properties*, already in the Phase 2 model
(`sourceOffset`, `length`, stretch, pitch, reverse):

- **Slip** changes `sourceOffset` only.
- **Trim** changes `start`/`length`.
- **Fades** are in/out curves using Phase 2's `Curve` — the same evaluator again.
- **Crossfade** between overlapping clips on one lane, equal-power or linear.
- **Clip envelopes** (model and scheduling land in Phase 4 §4.0; this phase owns the
  clip-editing behavior): gain, pan and pitch envelopes
  that belong to the clip, plus envelopes on an effect parameter that apply only while
  the clip plays. Fades are the special case of a gain envelope's first and last
  segments; they stay separate in the UI but compile through the same path (Phase 4
  §4.0 `ParamRamp`). Slip moves the audio under the envelope, not the envelope; trim clamps
  it. Tests: `clip_envelope_travels_on_move`, `clip_envelope_split_is_continuous`,
  `clip_envelope_survives_slip`, `clip_fade_equals_gain_envelope`.

Destructive edits (reverse, normalize, DC removal, gain, silence trim) write a
new `SampleBuffer` and leave the original file untouched, with the edit recorded
in the project so it survives a reload. The original is never modified on disk;
"destructive" here means destructive to the *cached buffer*, which is
recoverable and undoable.

### 3.3 Warp markers and stretching

`WarpMarkers` is a piecewise-linear map from clip-source time to project time,
anchored at markers. Markers are seeded by `OnsetDetector` and `BpmDetector`,
then hand-editable — automatic detection that cannot be corrected is worse than
none.

Stretching uses RubberBand offline (`_archive/src-cpp/src/AudioClipProcessor.cpp`
already wraps it; ported). Offline because quality matters more than latency
here, and because a warped clip's stretched buffer is cached and keyed the same
way `ClipPeakCache` is keyed (Phase 5 §4.5) — so the waveform display and the
audio never disagree about what is playing.

Pitch and time are independent, as FINAL_PLAN §5.5 requires.

### 3.4 Freeze

Render a channel's output to an audio clip, replace the channel with it, keep
the original reversibly. It is a substantial CPU win on heavy channels and it is
cheap to build here because offline render already exists (Phase 3 §4.10) and
runs through the identical path — so a frozen channel sounds *exactly* like the
unfrozen one, which is the whole reason freeze is trustworthy.

### 3.5 The export matrix

One dialog, one `ExportJob`, every combination:

| Axis | Options |
|---|---|
| Format | WAV 16 / 24 / 32f · MP3 (CBR/VBR, bitrate) · FLAC (level) · OGG (quality) |
| Source | master · stems per channel · stems per insert · stems per pattern · selection |
| Range | whole project · loop region only · between markers · split at markers |
| Tail | fixed seconds, or "until silence" with a threshold |
| Processing | dither (TPDF, noise-shaped, off) · normalize (peak / LUFS target) |
| Sample rate | project rate, or resample on export |
| Extras | also write MIDI · also write a project bundle |

`ExportJob` runs off the UI thread with progress and cancel. It uses Phase 1's
`OfflineBackend`, so it is the same DSP path as realtime — the architecture
FINAL_PLAN §3.1 credits `ExportRenderer.cpp` with, preserved.

**Stem coherence is the non-obvious requirement.** Stems must sum to the master.
That means each stem renders with the full graph intact and everything else
muted, *not* by soloing a subgraph — because sends, sidechains and bus
processing make a soloed subgraph a different signal. A test asserts
`sum(stems) == master` within 1e-6.

**Dither goes last**, after any normalization, and only when reducing bit depth.
Dithering a 32f export is a bug.

### 3.6 `adx render`

```
adx render project.adx -o out.wav [--format wav24] [--range loop]
                                  [--stems channels] [--tail 4s]
                                  [--normalize -14LUFS] [--block 512]
```

Headless, no Qt import, usable in CI and from scripts. It is a thin wrapper over
Phase 7's scripting API plus `ExportJob`.

`--block` exists because Phase 3's bit-identity guarantee is per-block-size, and
reproducing a specific render requires specifying it.

### 3.7 Project bundle

Collect every referenced sample into `<project>/samples/`, rewrite the paths in
the `.adx`, and zip it. Path rewriting goes through Phase 7's `Patch` so the
rest of the file — comments, formatting, unknown keys — is untouched. A bundle
is a normal folder with a normal text file in it; that is a property worth
protecting.

This also resolves the outstanding practical wrinkle that
`docs/examples/stakillaz_demo.adx` and `docs/examples/suffocation.adx` reference
`samples/kick_*.wav` paths that now live under `_archive/assets/samples/`.
Phase 8 adds a `samples/` search path in project settings and a
"locate missing samples" flow; the example corpus is bundled so it is
self-contained and the fixtures stop depending on the archive.

### 3.8 MIDI file I/O

Import is ported (`_archive/src-cpp/src/MidiImporter.cpp` — dependency-free SMF
parser, exact tick→beat conversion, per-channel splitting). FINAL_PLAN §3.1
notes it "needs a writer added"; this phase adds it.

Export writes SMF type 1: one track per channel, tempo and time-signature meta
events from the `TempoMap`, note on/off with exact ticks, CC from automation
lanes where a lane maps to a CC, and markers as meta events.

Round-trip property: import→export→import produces identical note data.

### 3.9 Golden hashes — completing FINAL_PLAN §9

The corpus grows to full coverage: every fixture × every export format ×
{master, stems}. WAV is hashed directly. Lossy formats (MP3, OGG) are **decoded
and compared spectrally**, not byte-hashed — encoder output is not guaranteed
bit-stable across builds of the encoder, and a test that pretends otherwise
fails for the wrong reason and gets disabled. FLAC is lossless, so its *decoded*
output is hashed.

The "three consecutive builds" gate runs as a CI job that builds from clean
three times and compares.

---

## 4. Port map

| Archive source | Destination | Fidelity |
|---|---|---|
| `src/ExportRenderer.cpp` | `engine/render/ExportJob.cpp` + encoders | Architecture already adopted in Phase 3; this phase adds the format matrix, stems and tail handling |
| `src/AudioClipProcessor.cpp` (RubberBand) | `engine/clip/Stretch.cpp` | Ported; caching and warp-marker integration added |
| `src/BpmDetector.cpp` | `engine/analysis/BpmDetector.cpp` | Ported as-is |
| `src/MidiImporter.cpp` | `engine/format/midi/MidiImporter.cpp` | Ported as-is |
| shine / libFLAC integration in `CMakeLists.txt` | `cmake/AdxDependencies.cmake` | Same tags |
| — | `engine/format/midi/MidiExporter.cpp` | New |
| — | `engine/render/OggEncoder.cpp` | New; libvorbis + libogg (§4.6 below) |

### 4.6 New dependency justification

FINAL_PLAN §9 requires every new dependency be justified in that file before it
is added. **libvorbis + libogg**, for OGG export (§5.7). Justification: OGG is
explicitly in the feature target; writing a Vorbis encoder is out of the
question; the reference encoder is BSD-licensed, small, and has no transitive
dependencies. FINAL_PLAN §8's table gains a row when this phase starts.

---

## 5. Tests

| Test | Asserts |
|---|---|
| `capture_no_alloc` | `InputCapture::capture` under the allocator hook: 0 violations over 60 s |
| `capture_overrun_counted` | a deliberately stalled writer increments the overrun counter and the take is marked, not silently gapped |
| `record_latency_alignment` | a loopback click records at the sample position it was heard, within ±1 sample, at 3 buffer sizes |
| `record_punch_region` | audio exists only inside the punch region, with correct pre/post-roll |
| `record_take_comping` | a comp built from 3 takes renders the expected sample sequence at each boundary |
| `clip_slip_preserves_length` | slip changes `sourceOffset` only |
| `clip_fade_uses_engine_curve` | fade shape matches `Curve::evaluate` exactly |
| `clip_crossfade_equal_power` | summed energy is constant through the crossfade |
| `clip_destructive_is_undoable` | reverse/normalize/DC/gain each undo to the original buffer, bit-identically |
| `clip_destructive_never_writes_source` | the source file's mtime and hash are unchanged |
| `warp_markers_piecewise_map` | source→project mapping matches hand-computed values at and between markers |
| `warp_cache_key` | changing any warp parameter invalidates the stretched buffer *and* the peak cache together |
| `stretch_quality` | a 1 kHz sine stretched 2× stays 1 kHz within 1 cent |
| `bpm_detector_corpus` | detected BPM within 1 % on a 20-file corpus with known tempos |
| `onset_detector_corpus` | onsets within 10 ms of hand-labelled positions, F1 > 0.9 |
| `freeze_is_transparent` | a frozen channel's render is **bit-identical** to the unfrozen channel's |
| `freeze_is_reversible` | unfreeze restores the original channel exactly |
| **`stems_sum_to_master`** | for every fixture, summed stems equal the master render within 1e-6 — including projects with sends and sidechains |
| `export_all_formats` | every format × bit depth produces a file that decodes to the expected length, rate and channel count |
| `export_dither_only_on_reduction` | no dither noise is present in a 32f export |
| `export_normalize_lufs` | LUFS-normalized output measures within 0.1 LU of the target |
| `export_tail_until_silence` | a 6 s reverb tail is fully captured; the file does not end mid-decay |
| `export_split_at_markers` | N markers produce N+1 files with correct boundaries and no dropped samples |
| `export_range_loop_only` | the loop region exports sample-exactly |
| `midi_import_roundtrip` | import→export→import yields identical note data across a 20-file corpus |
| `midi_export_tempo_meta` | tempo and meter changes appear as correct meta events |
| `bundle_collects_and_rewrites` | every sample is copied, every path rewritten, and the bundle opens on a machine with no access to the originals |
| `bundle_preserves_formatting` | everything in the `.adx` except the rewritten paths is byte-identical |
| `render_cli` | `adx render` produces a file identical to the GUI export with the same settings |
| **`golden_all_formats`** | **the phase gate.** Every fixture × every format; WAV/FLAC by hash, MP3/OGG by spectral comparison |
| **`three_clean_builds_identical`** | **the phase gate.** Build from clean three times; corpus hashes identical all three times |

---

## 6. Definition of done

- [ ] `golden_all_formats` passes.
- [ ] `three_clean_builds_identical` passes.
- [ ] `stems_sum_to_master` passes, including on a fixture with sidechains.
- [ ] `record_latency_alignment` passes on real hardware at 3 buffer sizes, with
      the measured offsets recorded in the phase log.
- [ ] `freeze_is_transparent` passes bit-identically.
- [ ] `adx render` works headless in CI with no Qt import.
- [ ] The `docs/examples/` corpus is self-contained — no sample reference
      resolves into `_archive/`.
- [ ] libvorbis justified in FINAL_PLAN §8 before it is added.
- [ ] FINAL_PLAN.md §10 Phase 8 row updated.

---

## 7. Out of scope (and who owns it)

| Thing | Owner |
|---|---|
| MIDI *device* input, MIDI learn, clock sync | Phase 9 |
| VST/CLAP | Phase 9 |
| Key/chord detection, melody extraction, form cleanup | Phase 10 |
| Performance recording (capturing a live session back into the arrangement) | Phase 11 — it reuses this phase's take/comp machinery |

---

## 8. Handoff to Phase 9

Phase 9 inherits:

- A proven input path — Phase 9's MIDI input is a second, far simpler input
  stream through the same arm/monitor/punch machinery.
- Measured, calibrated latency numbers, which is exactly what Phase 9's "under
  10 ms round trip" gate must be measured against.
- `ExportJob` running off the UI thread with cancel — the model Phase 9's
  out-of-process plugin scanner follows.
- Freeze, which is the standard mitigation for an expensive or unstable plugin
  and is therefore worth having *before* plugins exist rather than after.

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| Recorded takes land in the wrong place because a driver misreports latency | A loopback calibration routine measures the true round trip; the measured value overrides the reported one and is stored per device |
| Lossy-format golden tests flake across encoder builds | Spectral comparison with a stated tolerance, never byte hashing (§3.9) |
| Stems do not sum because of sends or sidechains | Stems render with the full graph and everything else muted, never by soloing a subgraph (§3.5); tested on a sidechain fixture specifically |
| RubberBand offline stretching is slow enough to block the UI on long clips | `ExportJob`-style background job with progress; the clip plays unstretched until ready |
| Disk writer falls behind on slow media and silently gaps a take | Overrun counter surfaced in the UI and recorded in the take's metadata |
| The example corpus keeps depending on archived samples | Explicit done-criterion; the corpus is bundled in this phase |
