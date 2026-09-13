# Phase 4 — Instruments & effects · XL

| | |
|---|---|
| **Status** | Not started |
| **Governs** | `engine/dsp/`, `engine/instruments/`, `engine/effects/`, `engine/format/audio/` (decode), the preset system |
| **FINAL_PLAN refs** | §3.1 (synthesis half, effects, SimpleFFT), §5.3 in full, §5.4 in full, §5.2 per-insert processing, §7 Phase 4 |
| **Entry criteria** | [phase_3.md](phase_3.md) §7 complete |
| **Next** | [phase_5.md](phase_5.md) |
| **§5 coverage owned** | §5.3 **all** · §5.4 **all** · §5.2 per-insert EQ/gain/pan/polarity/stereo-separation, metering DSP, master limiter · §5.5 multi-format decode only · §5.9 the C418 and STAKILLAZ *preset packs* (their generators are Phase 10) |

---

## 1. Objective

Fill the graph with sound. Phase 3 delivered a correct but silent machine whose
only instrument is a test tone; this phase delivers every instrument and every
effect in FINAL_PLAN §5.3 and §5.4, plus the preset system that makes them
usable.

**Scope note.** FINAL_PLAN §7's Phase 4 sentence names the first tranche only
("the additive and VA synths and all five archived effects... Sampler, slicer,
drum synth. Compressor, limiter, delay, parametric EQ, gate"). This file owns
**all** of §5.3 and §5.4, because they are pure DSP against a stable `Node`
interface with no UI dependency, and splitting a DSP library across phases
means two rounds of the same review, the same test scaffolding and the same
golden-hash churn. That is why this phase is sized **XL**, not L. FINAL_PLAN §7
and §10 are updated to match.

The phase is ordered in three tranches (§3). Tranche A alone satisfies
FINAL_PLAN's stated Phase 4 "done when": `suffocation.adx` renders and is
A/B-comparable to the archived build.

---

## 2. Deliverables — exact file manifest

```
engine/dsp/                     shared primitives; every instrument/effect builds on these
  Oscillator.h/.cpp             polyBLEP saw/square/tri/pulse, sine, phase accumulator
  WaveTable.h/.cpp              mipmapped band-limited tables + interpolation
  Envelope.h/.cpp               multi-stage ADSR with per-stage core::Curve
  Lfo.h/.cpp                    free/sync/retrig, 6 shapes, phase offset
  SvFilter.h/.cpp               TPT state-variable (LP/BP/HP/notch/peak), zero-delay feedback
  RbjFilter.h/.cpp              biquad cookbook: shelves, peaking, allpass
  LadderFilter.h/.cpp           4-pole ZDF ladder with saturation
  FormantBank.h/.cpp            5-band vowel formant bank + morph
  Noise.h/.cpp                  white + pink (Voss-McCartney), seeded xorshift32
  Saturate.h                    tanh, soft clip, asym tube, hard clip — branchless
  Interpolate.h                 linear, cubic Hermite, 8-point windowed sinc
  Resample.h/.cpp               polyphase SRC for sample playback and decode
  Fft.h/.cpp                    ported SimpleFFT: radix-2 + Hann + log-magnitude
  Convolve.h/.cpp               uniform-partition FFT convolution (for reverb IRs)
  Window.h                      Hann, Hamming, Blackman-Harris, Tukey
  Pan.h                         constant-power, linear and -3/-4.5/-6 dB laws
  Smooth.h                      one-pole parameter smoothing; every audible param uses it
  Db.h                          dB<->linear, with a fast path

engine/instruments/
  Instrument.h                  Node subclass adding voice semantics (§4.2)
  Voice.h                       per-voice state base
  additive/AdditiveInstrument.*     ported + extended past 16 harmonics
  va/VaInstrument.*                 ported polyBLEP/unison/ladder subtractive
  sampler/Sampler.*                 multi-layer: zones, velocity layers, round-robin
  sampler/SampleZone.h              loop modes, root key, key/vel ranges, crossfade
  slicer/Slicer.*                   beat-sliced audio -> pads, BPM-aware
  drumsynth/DrumSynth.*             kick/snare/hat/clap/tom models
  drumsynth/HardstyleKick.*         absorbs the archived hardstyle kick generator
  granular/GranularInstrument.*     grain scheduler, spray, density, pitch, window
  fm/FmInstrument.*                 6-operator, 32 algorithms, feedback, ratio/fixed
  wavetable/WavetableInstrument.*   position morph, 2D tables, WT import
  pool/SamplePoolChannel.*          plain one-shot pool playback channel

engine/effects/
  Effect.h                      Node subclass + wet/dry + bypass + latency
  Reverb.*          (ported)    Freeverb: 8 combs + 4 allpasses, 23-sample spread
  Distortion.*      (ported)    tanh drive
  Bitcrush.*        (ported)    bit depth + sample-rate reduction
  Chorus.*          (ported)    modulated delay
  Eq3.*             (ported)    3-band RBJ shelving/peaking
  Delay.*                       ping-pong, tempo-sync, filtered feedback
  Compressor.*                  RMS/peak, knee, lookahead, external sidechain
  Ducker.*                      explicit sidechain ducker
  Limiter.*                     true-peak lookahead brickwall (master limiter)
  MultibandComp.*               3-6 band Linkwitz-Riley crossover + per-band comp
  Gate.*                        gate/expander with hysteresis + lookahead
  TransientShaper.*             attack/sustain envelope differencing
  ParametricEq.*                8-band fully parametric + spectrum tap for the UI
  Flanger.*  Phaser.*  Tremolo.*
  Convolution.*                 IR loading + uniform-partition FFT convolution
  Saturation.*                  tube/tape/transformer models + oversampling
  Vocoder.*                     16-32 band analysis/synthesis with carrier input
  PitchShifter.*                phase-vocoder / RubberBand realtime mode
  FormantFilter.*               vowel morphing as an insert
  StereoImager.*                M/S width, per-band widening
  FrequencyShifter.*            Hilbert-transform true frequency shift
  RingMod.*
  GrossBeat.*                   time + volume manipulation over a bar grid
  SpectralFreeze.*              FFT magnitude hold with phase randomization

engine/mixer/                   the per-insert processing chain (FINAL_PLAN §5.2)
  InsertStrip.h/.cpp            gain, pan, polarity, stereo separation, mute/solo
  Metering.h/.cpp               peak, RMS, LUFS (BS.1770-4 K-weighting + gating)

engine/format/audio/
  AudioFileLoader.h/.cpp  (ported)  miniaudio decode: WAV/MP3/FLAC/OGG/AIFF
  SampleBuffer.h                    interleaved/planar float storage + metadata
  SamplePool.h/.cpp                 refcounted, deduplicated by content hash

engine/preset/
  Preset.h                      instrument or effect state as named params + refs
  PresetLibrary.h/.cpp  (ported PatchLibrary)  scan, load, save, tag, search
  packs/c418/*.adxpreset
  packs/stakillaz/*.adxpreset

tests/cpp/dsp/                  one file per primitive
tests/cpp/instruments/          one file per instrument
tests/cpp/effects/              one file per effect
tests/golden/                   grows substantially in this phase
```

---

## 3. Tranches — the order of work

**Tranche A — parity with iteration one.** `engine/dsp/` primitives, Additive,
VA, Sampler, Reverb, Distortion, Bitcrush, Chorus, Eq3, Delay, Compressor,
Limiter, ParametricEq, Gate, decode, presets, the C418 and STAKILLAZ packs.

*Exit:* `suffocation.adx` renders through the v1 shim and is A/B-comparable to
the archived build's output. **This is FINAL_PLAN's stated Phase 4 gate.**

**Tranche B — the rest of §5.3.** Slicer, DrumSynth (+ HardstyleKick), Granular,
FM, Wavetable, SamplePoolChannel.

**Tranche C — the rest of §5.4.** Ducker, MultibandComp, TransientShaper,
Flanger, Phaser, Tremolo, Convolution, Saturation, Vocoder, PitchShifter,
FormantFilter, StereoImager, FrequencyShifter, RingMod, GrossBeat,
SpectralFreeze.

Tranche A is the load-bearing one: it is where the primitives are proven and
where the porting judgment is spent. B and C are largely *applications* of
tranche A's primitives, which is why they are cheaper than their item count
suggests.

Phase 5 may begin once Tranche A is complete — the frontend needs something that
makes sound, not everything that makes sound. B and C are not on Phase 5's
critical path, and running them in parallel is the intended schedule.

---

## 4. Design

### 4.1 `engine/dsp/` is written first and tested numerically

Every primitive gets a numerical test before any instrument uses it
(FINAL_PLAN §9: *"Every DSP primitive has a numerical test — impulse response,
frequency response, or known-value comparison"*). This is not ceremony: a
subtly wrong filter is nearly undetectable inside a synth and trivially
detectable in isolation.

Standard test shapes, applied uniformly:

- **Filters:** white-noise-in → FFT → compare magnitude response against the
  analytic transfer function, ±0.5 dB across 20 Hz–20 kHz, at Q = 0.5, 1, 4, 16.
  Plus a stability sweep: cutoff from 10 Hz to Nyquist×0.99 at every Q, asserting
  no NaN, no denormal, no output above +6 dB.
- **Oscillators:** FFT of one second; assert harmonic amplitudes follow the ideal
  series and that aliasing (energy at non-harmonic bins) is below −60 dBc at
  fundamental = 40 Hz, 440 Hz, 4 kHz and 10 kHz. **The 10 kHz case is the one
  that catches a bad polyBLEP**, which is exactly where naive implementations
  pass casual listening and fail measurement.
- **Envelopes:** sample-exact stage boundaries at 5 sample rates; every
  `CurveKind` hits 0 and 1 at its endpoints; retrigger from a non-zero level
  never discontinues.
- **Delay-based effects:** impulse in, assert taps land at the exact expected
  sample.

`Smooth.h` matters more than it looks: every audible parameter is smoothed with
a one-pole at ~10 ms. Un-smoothed parameter changes are zipper noise, and
iteration one had it on every knob.

### 4.2 `Instrument` and `Effect` on top of Phase 3's `Node`

```cpp
class Instrument : public graph::Node {
public:
    void process(ProcessContext& ctx) noexcept final;   // final: the loop is shared
protected:
    // Implemented per instrument. One voice, one block, additive into out.
    virtual void renderVoice(Voice&, std::span<float> l, std::span<float> r,
                             uint32_t frames, const ProcessContext&) noexcept = 0;
    virtual void startVoice(Voice&, const ScheduledEvent&) noexcept = 0;
    virtual void stopVoice(Voice&, uint8_t releaseVelocity) noexcept = 0;
    virtual size_t voiceStateBytes() const noexcept = 0;   // pool sizing
};
```

`process()` is `final` and lives in the base: it consumes `ctx.events`, drives
`VoicePool` allocation and release (Phase 3 §4.7), calls `renderVoice` for every
active voice, and retires finished voices. An instrument author writes DSP, not
voice bookkeeping — which is how twelve instruments stay maintainable and how
they all get identical, correct note-off and steal behavior for free.

```cpp
class Effect : public graph::Node {
public:
    void process(ProcessContext& ctx) noexcept final;   // handles wet/dry + bypass
protected:
    virtual void processWet(std::span<float> l, std::span<float> r,
                            uint32_t frames, const ProcessContext&) noexcept = 0;
};
```

Wet/dry crossfade and bypass are in the base, once, correctly (equal-power
crossfade, bypass ramped over 5 ms rather than switched, so a bypass toggle is
not a click).

**Two disciplines ported verbatim from `_archive/src-cpp/include/AudioEffect.h`,
both of which that file got right:**

- `clone()` producing a fresh instance with the same parameters and pristine DSP
  state, so offline render never processes the same instance the audio thread is
  processing. FINAL_PLAN §3.1 singles this out; it stays.
- `isEquivalent()` for hot-reload diffing, so a reparsed file that produces an
  identical effect does not restart its reverb tail. Phase 7 depends on this.

What changes: parameters are no longer `std::atomic<float>` members polled by
the audio thread. They are indices into `ProcessContext::params`, resolved by
Phase 2's `ParamRegistry` and updated through Phase 3's event queue. The atomics
worked, but they scatter the parameter model across every effect and make
automation and undo special cases.

### 4.3 Additive — the port

Source: the synthesis half of `_archive/src-cpp/src/AudioEngine.cpp`.
FINAL_PLAN §3.1: *"The DSP math is sound. The scheduling half around it is
not."* So the extraction is surgical — take the per-voice math, leave everything
that touches scheduling, buses or the 16-track array behind.

The archived `Patch` (`_archive/src-cpp/include/AudioData.h`) maps to named
parameters like this — this table is also the v1 shim's target (Phase 2 §4.12):

| v1 key | v1 fields | v2 parameter names |
|---|---|---|
| `HARMONICS` | amplitude list | `additive.harmonic[i].level` (extended past 16) |
| `ENVELOPE` | a, d, s, r (seconds) | `env.attack/.decay/.sustain/.release` (+ per-stage `curve`) |
| `DRIVE` | drive | `drive.amount` |
| `FILTER` | cutoffHz, lfoRateHz, lfoDepth | `filter.cutoff`, `filter.lfo.rate`, `filter.lfo.depth` |
| `SUB` | level, wave, dropSemitones, dropMs | `sub.level`, `sub.wave`, `sub.drop.semitones`, `sub.drop.time` |
| `NOISE` | level, type | `noise.level`, `noise.type` |
| `RESFILTER` | type, cutoff, resonance, envAmount, keyTrack | `resfilter.*` |
| `FILTERENV` | a, d, s, r | `filterenv.*` (+ curves) |
| `FORMANT` | vowelA, vowelB, morph, amount | `formant.vowelA/.vowelB/.morph/.amount` |
| `VIBRATO` | rateHz, depthCents, delayMs | `vibrato.rate/.depth/.delay` |
| `GLIDE` | ms | `glide.time` |
| `OSC` | wave, unisonVoices, detuneCents, pulseWidth | `osc.wave/.unison/.detune/.pulseWidth` |

**Extensions this phase adds:** harmonic count raised from 16 to 64 with
per-harmonic detune and phase; harmonic-series presets (saw/square/organ/bell);
and a spectral-morph parameter between two harmonic sets.

**Regression discipline for the port.** The archived build renders
`suffocation.adx` to WAV; that WAV is committed under `tests/golden/reference/`
as the *listening* reference. The port is not expected to be bit-identical to it
— the filter topology and voice handling legitimately changed — so the test is
a spectral-distance assertion (per-band RMS within 1.5 dB over 31 third-octave
bands) plus a mandatory human A/B. Claiming bit-identity here would be a lie,
and a test that asserts a lie gets disabled within a month.

### 4.4 VA (subtractive)

polyBLEP oscillator bank, unison with detune and stereo spread, ZDF ladder and
SVF filters, dual envelopes, 2 LFOs with full routing. The archived polyBLEP and
unison code is good and ports directly; the filter is upgraded from the archived
TPT SVF to a selectable SVF/ladder pair because a ladder is what a subtractive
synth is *for*.

### 4.5 Sampler

The instrument with the most surface area and no archived predecessor.

```cpp
struct SampleZone {
    SampleRef sample;
    uint8_t keyLow, keyHigh, rootKey;
    uint8_t velLow, velHigh;
    uint32_t loopStart, loopEnd;
    LoopMode mode;              // Off | Forward | PingPong | Sustain | Release
    uint32_t crossfadeSamples;  // loop crossfade
    float tune, gain, pan;
    uint8_t roundRobinGroup, roundRobinIndex;
};
```

Playback uses `Interpolate.h`'s 8-point windowed sinc when the pitch ratio is
not 1.0 and a direct copy when it is — the direct-copy fast path matters because
drum one-shots at their root key are the common case and interpolating them is
both slower and slightly lossy.

Round-robin index advances per zone group and is stored in the snapshot as a
seeded counter, not a `rand()` — otherwise offline and realtime diverge
(Phase 3 §4.10 condition 6).

### 4.6 Slicer

Beat-sliced audio → playable pads. Slice points come from Phase 10's BPM/onset
detector when available and from an even division of the clip length otherwise,
and are always editable and stored in the project. Each slice is a zone in a
Sampler under the hood — the Slicer is a *front end* over the Sampler, not a
second playback engine. BPM-aware: changing project tempo restretches slice
spacing without re-slicing.

### 4.7 DrumSynth

Five synthesis models rather than one parametric compromise: kick (pitch-swept
sine + click + body), snare (tone pair + filtered noise + transient), hat
(6-square metallic cluster + bandpass + VCA), clap (3 filtered noise bursts with
spread + reverb tail), tom (pitch-swept sine + noise).

`HardstyleKick` absorbs the archived generator as a *preset lineage* of the kick
model — pitch-drop, distortion stage, sub reinforcement — rather than as its own
node, which is what it was in iteration one.

### 4.8 Granular, FM, Wavetable

- **Granular:** grain scheduler with density, size, spray, pitch jitter, window
  shape, stereo spread; a fixed preallocated grain pool (256), identical in
  principle to `_archive/src-cpp/src/ParticleVisualizer.cpp`'s preallocated
  particle pool, whose no-per-frame-allocation discipline FINAL_PLAN §3.1
  explicitly credits.
- **FM:** 6 operators, 32 algorithms (a routing matrix, not 32 hardcoded
  graphs), per-operator ratio/fixed frequency, feedback, per-operator envelope.
- **Wavetable:** 2D tables (position × frame) with mipmapping from
  `WaveTable.h`, linear and spectral morph between positions, `.wav`-based
  wavetable import (2048-sample frames, the de-facto convention).

### 4.9 Effects — the ones with non-obvious requirements

Most of §5.4 is straightforward against `engine/dsp/`. These four are not:

- **Limiter / Compressor lookahead.** Both declare `latencySamples()` and let
  Phase 3's PDC compensate. Getting this wrong is the classic "my master bus is
  8 ms late" bug, and it is *already solved* — declare the latency honestly and
  it disappears.
- **Convolution.** Uniform-partition FFT convolution with the first partition
  processed directly, so reported latency is one partition, not the whole IR.
  IRs load on the main thread and are refcounted through `SamplePool`.
- **PitchShifter.** Realtime mode uses RubberBand (already a dependency,
  FINAL_PLAN §8); the phase-vocoder path is the fallback for low-latency use.
  Its latency is declared.
- **GrossBeat.** Time and volume manipulation over a bar grid. It needs a
  rolling buffer of the last N bars plus a position-remapping curve, and it is
  the one effect that reads `ctx.time` — which works precisely because Phase 3
  passes a `TimeSource` rather than assuming a global (Phase 3 §2). It also
  interacts with Phase 11: a GrossBeat on a launched clip follows *that clip's*
  time source.

### 4.10 Metering

Peak and RMS per insert; LUFS per BS.1770-4 (K-weighting filter pair, 400 ms
blocks, 75 % overlap, gated integrated loudness, short-term and momentary). All
computed on the audio thread into Phase 1 `OverwriteRing`s; the UI reads the
latest window at 60 Hz in one call (FINAL_PLAN §2.2, "one FFI call per frame,
total").

True-peak (4× oversampled) is computed for the limiter and for the master meter
only — it is not free, and per-insert true-peak is not worth it.

### 4.11 Decode and the sample pool

Ported from `_archive/src-cpp/src/AudioFileLoader.cpp`, which already wraps
miniaudio. Additions:

- **Content-hash deduplication.** Two clips referencing the same file share one
  `SampleBuffer`. Iteration one loaded a copy per `CLIP` line.
- **Refcounting + `Reaper` integration.** A sample whose last reference is
  removed during playback is freed on the main thread, never on the audio
  thread.
- **Async decode.** A decode thread pool; the node plays silence and reports
  "loading" until ready. A 200 MB sample must not block the UI or the audio
  thread.
- **Project-relative path resolution** with a search path, so moving a project
  folder does not break it. (Collect-and-save is Phase 8.)

### 4.12 Presets

```
[PRESET]
NAME="Cave Pad"
TYPE=additive
PACK=c418
TAGS=pad, ambient, soft
[PARAMS]
env.attack=0.8 curve=bezier(0.1,0.0,0.4,1.0)
filter.cutoff=800
additive.harmonic[1].level=1.0
```

Same lexer, same writer, same diagnostics as `.adx` (Phase 2) — a preset is a
parameter block, and reusing the format machinery means presets get
round-tripping, unknown-key tolerance and line/column diagnostics for free.

`PresetLibrary` ports `_archive/src-cpp/src/PatchLibrary.cpp`: scan directories,
index by tag, search, save user presets to a user directory separate from the
shipped packs.

**The C418 and STAKILLAZ packs** are authored from `docs/C418.md` and
`docs/STAKILLAZ.md`, which FINAL_PLAN §3.1 records as still-current sound-design
references. Each documented sound becomes a named preset. The *generators*
described in those documents are Phase 10; Phase 4 ships the patches.

---

## 5. Port map

| Archive source | Destination | Fidelity |
|---|---|---|
| `src/AudioEngine.cpp` synthesis half | `engine/instruments/additive/`, `engine/instruments/va/`, `engine/dsp/` | High. Voice DSP extracted; all scheduling, bus and 16-track code left behind |
| `include/AudioEffect.h` + `src/AudioEffects.cpp` | `engine/effects/` | High. Algorithms verbatim; `clone()` and `isEquivalent()` kept; atomic params replaced by `ParamRef` indices |
| `ReverbEffect::processWetOnly` | `Reverb` send mode | Kept — a send bus must not bleed dry signal, which this correctly handles |
| `src/SimpleFFT.cpp` | `engine/dsp/Fft.cpp` | Verbatim. FINAL_PLAN §8: sufficient for UI-rate analysis; swap for pffft only if profiling demands it |
| `src/AudioFileLoader.cpp` | `engine/format/audio/` | Structure kept; pooling, refcounting and async decode added |
| `src/PatchLibrary.cpp` | `engine/preset/PresetLibrary.cpp` | Structure kept; retargeted at the v2 preset format |
| hardstyle kick generator (in `src/main.cpp`) | `engine/instruments/drumsynth/HardstyleKick.*` | Extracted from UI code into a preset lineage of the kick model |
| `src/AudioEngine.cpp` global 64-voice pool | — | **Deleted.** Phase 3's per-channel `VoicePool` replaces it |
| `_archive/src-cpp/CMakeLists.txt` miniaudio, rubberband, shine, flac tags | `cmake/AdxDependencies.cmake` | Same tags; declared in this phase (Phase 0 §3.4 rule) |

---

## 6. Tests

Beyond the per-primitive numerical tests in §4.1:

| Test | Asserts |
|---|---|
| `dsp_polyblep_aliasing` | aliasing < −60 dBc at 40 Hz, 440 Hz, 4 kHz, 10 kHz for saw/square/pulse |
| `dsp_filter_response_matches_analytic` | ±0.5 dB vs. the transfer function, 4 Q values, all filter types |
| `dsp_filter_stability_sweep` | no NaN/denormal/overshoot across the full cutoff×Q space |
| `dsp_envelope_stage_timing` | sample-exact boundaries at 5 sample rates; all `CurveKind`s |
| `dsp_resample_thd` | THD+N < −80 dB for a 1 kHz sine resampled 44.1↔48↔96 kHz |
| `dsp_fft_roundtrip` | `ifft(fft(x)) == x` within 1e-6 |
| `instrument_voice_lifecycle` | note-on/off/steal across every instrument via one table-driven harness |
| `instrument_no_alloc_in_process` | every instrument rendered under the Phase 1 allocator hook: 0 violations |
| `instrument_silence_when_idle` | with no voices, output is exactly 0.0 — not −120 dB of denormal hiss |
| `sampler_loop_modes` | each `LoopMode` produces the expected sample sequence, hand-verified |
| `sampler_roundrobin_deterministic` | identical across offline and realtime renders |
| `sampler_root_key_fast_path` | at ratio 1.0, output is bit-identical to the source samples |
| `slicer_tempo_change` | changing project tempo respaces slices without re-slicing or re-pitching |
| `effect_bypass_is_click_free` | no sample-to-sample discontinuity > 0.01 on bypass toggle |
| `effect_wetdry_equal_power` | wet+dry energy is constant across the crossfade |
| `effect_clone_independence` | processing a clone does not perturb the original's DSP state |
| `effect_is_equivalent` | same params → equivalent; one differing param → not |
| `effect_declares_latency` | every lookahead effect's declared latency matches its measured impulse delay exactly |
| `pdc_compensates_limiter` | a limiter on one parallel path stays sample-aligned with a dry path |
| `convolution_matches_direct` | FFT convolution matches naive time-domain convolution within 1e-5 for a 1 s IR |
| `meter_lufs_matches_reference` | integrated LUFS within 0.1 LU of the EBU TECH 3341 compliance signals |
| `meter_true_peak_catches_intersample` | a signal with a known +1.2 dBTP inter-sample peak is detected |
| `decode_all_formats` | WAV 8/16/24/32f, MP3, FLAC, OGG, AIFF all decode to the expected length and RMS |
| `decode_dedup` | two clips on one file share one buffer; refcount reaches 0 exactly once |
| `decode_async_does_not_block` | a 200 MB file decodes without a callback exceeding its deadline |
| `preset_roundtrip` | every shipped preset loads, saves and reloads identically |
| `preset_packs_load` | all C418 and STAKILLAZ presets load with zero diagnostics |
| **`suffocation_spectral_match`** | **Tranche A gate.** Per-band RMS within 1.5 dB of the archived reference render across 31 third-octave bands |
| `golden_all_instruments` | one golden hash per instrument, per preset pack |
| `golden_all_effects` | one golden hash per effect at 3 parameter settings |

The golden corpus grows from Phase 3's 5 fixtures to roughly 80 here. That is
the point: after this phase, any unintended DSP change anywhere is caught by a
hash within one CI run.

---

## 7. Definition of done

- [ ] Every §5.3 instrument and every §5.4 effect exists, is tested, and has a
      golden hash.
- [ ] `suffocation_spectral_match` passes **and** a human A/B has been done and
      recorded.
- [ ] `instrument_no_alloc_in_process` passes for all twelve instruments.
- [ ] `effect_declares_latency` passes for every effect with lookahead, and
      `pdc_compensates_limiter` confirms PDC handles it.
- [ ] Every DSP primitive in `engine/dsp/` has a numerical test (FINAL_PLAN §9).
- [ ] LUFS metering validated against EBU TECH 3341.
- [ ] C418 and STAKILLAZ preset packs ship and load clean.
- [ ] `adx info suffocation.adx` reports the real instruments, not test tones.
- [ ] FINAL_PLAN.md §10 Phase 4 row updated.

---

## 8. Out of scope (and who owns it)

| Thing | Owner |
|---|---|
| Any UI for any instrument or effect | Phase 6 |
| Mini-notation compilation | Phase 7 wires it; the compiler is ported there |
| Clip time-stretch/warp editing (RubberBand *offline*) | Phase 8 |
| Recording | Phase 8 |
| VST/CLAP-hosted instruments and effects | Phase 9 |
| Spectrum/waterfall/particle *visualizers* (the FFT they use lands here) | Phase 10 |
| C418/STAKILLAZ *generators* (the presets land here) | Phase 10 |

---

## 9. Handoff to Phase 5

Phase 5 inherits a complete sound engine and, specifically:

- A stable `ParamDescriptor` table per instrument and effect — **the UI can
  generate a usable parameter editor generically**, which is what makes a
  twelve-instrument, thirty-effect UI feasible in Phase 6 at all.
- `PresetLibrary` — the browser panel has real content to browse on day one.
- Metering rings already being written by the audio thread, so the Phase 5
  60 Hz timer has something to read before any mixer UI exists.
- `Fft` and the spectrum tap on `ParametricEq`, ready for the Phase 6 EQ
  display and the Phase 10 analyzers.
- A project that actually sounds like music, which is what makes the Phase 5
  piano roll testable by ear rather than only by assertion.

---

## 10. Risks

| Risk | Mitigation |
|---|---|
| The additive port sounds subtly different and nobody notices until much later | Spectral-distance test against the archived reference render, plus a required human A/B recorded in the phase log. Bit-identity is explicitly *not* claimed (§4.3) |
| Thirty effects is a long tail that never finishes | Tranches (§3). Tranche A is the gate; B and C are parallel to Phase 5 and each item is independently testable and shippable |
| Convolution and vocoder latency is mis-declared, producing subtle misalignment | `effect_declares_latency` measures the impulse delay rather than trusting the declaration |
| Async decode introduces a race on `SampleBuffer` lifetime | Refcount + `Reaper`; a node holds a borrowed span valid for the snapshot's lifetime, and the snapshot pins the sample |
| RubberBand's realtime mode allocates | Verified under the allocator hook; if it allocates, it is used offline-only and the phase-vocoder path becomes the realtime one. Decide by measurement, not assumption |
| Golden hashes churn constantly during active DSP work | Golden tests run as a separate ctest label; regenerating is one command and every regeneration is a reviewed commit |
