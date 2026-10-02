# Phase 4 — Instruments & effects · XL

| | |
|---|---|
| **Status** | In progress (2026-10-01) |
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
  voice/VoiceInstrument.*           UTAU-style sung vocals (§4.13)
  voice/Voicebank.*                 oto.ini + character.txt parse, alias resolution, encoding detect
  voice/WorldAnalysis.*             WORLD F0/spectral-envelope/aperiodicity, cached as .adxfrq
  voice/UtauResampler.*             off-thread note render: pitch, length, flags, bend
  voice/VoiceRenderCache.*          content-addressed rendered notes in the SamplePool

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
  Overdrive.*                   oversampled drive stage + tone, soft/tube/hard characters (§4.9)
  Saturation.*                  tube/tape/transformer models + oversampling
  Vocoder.*                     16-32 band analysis/synthesis, sidechain carrier (§4.9)
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

**Tranche A0 — amendments to Phases 2 and 3 (§4.0).** Additive note/clip fields,
the 32-byte `ScheduledEvent`, `PitchGlide`/`Lyric`/`ParamRamp` events, rate classes.
*Exit:* the Phase 2 and Phase 3 gates re-run unmodified and green, plus A0's own
tests. Small, and first, because everything below reads these events.

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

**Tranche D — Voice.** `engine/instruments/voice/` (§4.13), plus `Overdrive` if
not already pulled forward for the STAKILLAZ pack. Depends on A (Sampler, SamplePool,
`Resample.h`) and on A0's `Lyric`/`PitchGlide` events (§4.0); independent of B and C.

Tranche A is the load-bearing one: it is where the primitives are proven and
where the porting judgment is spent. B and C are largely *applications* of
tranche A's primitives, which is why they are cheaper than their item count
suggests.

Phase 5 may begin once Tranche A is complete — the frontend needs something that
makes sound, not everything that makes sound. B and C are not on Phase 5's
critical path, and running them in parallel is the intended schedule.

---

## 4. Design

### 4.0 Amendments to the Phase 2 model and Phase 3 scheduler — done first

Phases 2 and 3 are closed. Slide notes, lyrics, clip envelopes and sample-accurate
automation need changes to both, and those changes are **this phase's first
tranche (A0)**, landing before any instrument depends on them. Each is additive:
absent fields serialize to nothing, so existing `.adx` files and every committed golden
hash are unchanged, and the Phase 2/3 gates re-run green as A0's exit criterion.

**Phase 2 model additions** (`engine/project/Channel.h`, `Pattern.h`, `PlaylistItem`):

```cpp
// On Note — all default to "absent".
std::optional<NoteSlide> slide;        // glide from this note's pitch to a target
std::vector<PitchPoint>  pitchCurve;   // freeform cents-vs-time, note-relative ticks
std::string              lyric;        // Voice instrument; empty = none
struct NoteSlide  { int16_t targetCents; core::Ticks start, length; CurveShape shape; };
struct PitchPoint { core::Ticks t; int16_t cents; CurveShape shape; };

// On PlaylistItem — automation that belongs to THIS item.
std::vector<ClipEnvelope> envelopes;
struct ClipEnvelope { ParamTarget target; std::vector<Breakpoint> points; };
```

`slide` is the editor's one-gesture case (A → C); `pitchCurve` is the general case.
They compose: slide first, curve added on top. A slide whose target is a following
note's pitch is stored as cents, not as a link, so moving or deleting the next note
never silently retargets it. Envelope breakpoint times are item-relative (0 =
`item.start`) and clamped to `item.length`: trimming the tail drops points, trimming
the head shifts them. `target` is a clip-local parameter (gain, pan, pitchCents) or any
`ParamRef` the item's lane feeds, in which case the envelope applies only while the
item plays and the parameter returns to its base value afterwards. Split divides an
envelope at the cut and inserts a boundary point so the value is continuous across it.
The `.adx` syntax is in [adx-format-v2.md](../docs/adx-format-v2.md) (planned note
extensions); the commands (`SetNoteSlide`, `SetPitchCurve`, `SetLyric`,
`SetClipEnvelope`) follow the one-gesture-one-command rule of Phase 2 §4.9.

**Phase 3 scheduler additions** (`engine/project/EventTrack.h`, `SnapshotBuilder`,
`ParamRegistry`):

- `ScheduledEvent` grows from 24 to 32 bytes: `float value` plus `uint32_t duration`
  and `uint8_t shape`. The `static_assert` moves with it; the per-block walk is
  unchanged. New kinds: `PitchGlide`, `Lyric`, `ParamRamp`.
- **Slides and lyrics compile to events**, not voice-side lookups. `Note.slide` and
  `Note.pitchCurve` become `PitchGlide` events keyed by `noteId` at the glide's start
  tick (N curve points → N−1 chained glides). `Note.lyric` becomes a `Lyric` event
  carrying an index into a snapshot-owned lyric table; strings never ride in an event.
- **Automation is delivered as ramps, sample-accurately.** An `AutomationClip` and a
  clip envelope (offset by its item's start) compile to chains of `ParamRamp` events:
  `value` = target, `duration`/`shape` as above, the parameter slot index in the field
  `noteId` occupies for note events. A parameter holds `(current, target, remaining
  samples)`; the audio thread advances it and does not wait for the next block. Curved
  segments (Bezier, exponential, smooth) are subdivided into linear ramps at compile
  time until the error against `Curve::evaluate` is under 0.1 % of the parameter
  range (capped at 32 pieces) — a deterministic function of the curve, so offline and
  realtime build identical chains.
- **Rate classes.** `ParamRegistry` gives every parameter `Block` (constant within a
  block; ramps interpolate at block edges), `Sample` (the node receives a per-sample
  value span: cutoff, drive, mix, gain, pan) or `Baked` (§4.13).
- **Seek** into the middle of a glide or ramp resolves the in-flight value from the
  resume cursor (Phase 3 §4.11), not by replaying from the start.

**A0 tests:** `slide_seek_midway_matches_continuous`,
`automation_sweep_block_size_independent` (bit-identical at 64/512/1024),
`automation_ramp_matches_curve_evaluate`, `automation_seek_midramp_continuous`,
`clip_envelope_applies_only_within_item`, `new_note_fields_absent_roundtrip_unchanged`
(every existing golden file round-trips byte-identically), and the Phase 2 and Phase 3
phase gates re-run unmodified.

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

**Pitch glide is in the base, not per instrument.** `Voice` carries a `pitchCents`
offset that `Instrument::process` advances from `PitchGlide` events (§4.0)
using the event's duration and curve shape, sample-accurately, once. `renderVoice`
reads `voice.pitchRatio()` and nothing else, so slide notes and pitch curves work on
every instrument for free. This is distinct from the ported `GLIDE` parameter, which
is channel-level portamento: it triggers only when a new note starts while another
is sounding (mono/legato), glides over `glide.time`, and is overridden by an
explicit `slide` on the note. Precedence: base pitch + `fineTuneCents` + channel
`pitchOffsetCents` + portamento + slide + `pitchCurve` + vibrato, summed in cents
and converted to a ratio once per block with per-sample linear interpolation of the
cents value (so a slide is smooth, not stair-stepped at block boundaries). A
`PitchGlide` for a `noteId` with no live voice (already stolen/released) is dropped.
Tests: `slide_reaches_target_exactly`, `slide_block_size_independent`,
`slide_plus_vibrato_sums_in_cents`, `portamento_overridden_by_explicit_slide`.

**Automated parameters at the right rate.** `Effect::processWet` and
`renderVoice` receive, for each `Sample`-class parameter, a per-sample value span
(§4.0) — constant when nothing is ramping, so the common case costs one
pointer check — and a plain float for `Block`-class parameters. Parameters whose
cost makes per-sample evaluation wasteful (filter coefficient computation) declare
`Block` and are interpolated at sub-block granularity (every 32 samples) instead;
the declaration is part of the parameter's metadata, checked by a test that sweeps
every automatable parameter of every effect and instrument and asserts no
discontinuity above the smoothing floor. `Smooth.h` still applies to manual knob
turns; it is bypassed for ramp events, which are already smooth by construction.
Wet/dry and bypass are automatable like any other parameter.

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

**Overdrive.** Distinct from the ported `Distortion` (a bare tanh) and from
`Saturation` (tube/tape/transformer models). Signal path: input gain → pre-emphasis
high-pass (tightens the low end before clipping, the classic overdrive-pedal
move) → waveshaper → post low-pass `tone` → output level, all inside 4× oversampling
with a polyphase half-band pair so the clipper does not alias. Characters (`mode`):
`soft` (tanh-ish), `tube` (asymmetric, even harmonics), `hard` (clamp). Parameters:
`drive`, `tone`, `tightness` (pre-HPF corner), `mode`, `level`, `mix`. Declares
the oversampler's latency honestly. `Saturate.h` supplies the shapers, so this is
mostly wiring; the work is the oversampler and the aliasing test.
Tests: `overdrive_alias_below_-60dB` (swept sine, aliased energy measured),
`overdrive_declares_latency`, `overdrive_mode_switch_no_click`.

**Vocoder.** 16–32 bands (`bands`), analysis filterbank on the modulator, synthesis
filterbank on the carrier, per-band envelope followers (`attack`/`release`).
Carrier is a second input: a mixer **sidechain connection** (Phase 3's explicit
sidechain edge, the same mechanism as Compressor/Ducker), or a built-in carrier
(`saw` bank tracking MIDI via the host channel, `noise`, or `saw+noise`) so it works
on one track. Intelligibility features that matter more than band count: `formantShift`,
`bandwidth`, an unvoiced/sibilance path (high-passed modulator passed through
or used to gate a noise carrier above ~5 kHz, `sibilance` mix), and
`bandFreeze`. This is a channel vocoder; the phase-vocoder in `PitchShifter` is a
different technique despite the shared name. Latency is declared from the filterbank
group delay and *measured* by `effect_declares_latency`. Tests:
`vocoder_flat_carrier_reproduces_envelope`, `vocoder_sibilance_passthrough`,
`vocoder_sidechain_latency_aligned` (PDC aligns modulator and carrier).

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

### 4.13 Voice — UTAU voicebanks

**What a UTAU voicebank is.** A folder of WAV recordings of syllables plus
`oto.ini`, which maps each *alias* (a lyric such as "ka" or "- a") to a WAV and five
timings: offset, consonant, cutoff, preutterance, overlap. `character.txt` names it.
There is no synthesis in the bank. The synthesis is done by a **resampler** that
re-pitches and re-times the syllable to the note. adX therefore ships its own
resampler rather than "loading" a bank into something that already sings.

**Shape.** Offline-quality synthesis cannot run on the audio thread, so Voice is a
*renderer plus a sampler*:

```
Note(lyric, pitch, length, slide/pitchCurve, flags)
   -> VoiceRenderKey = hash(voicebank content hash, resolved alias, pitch, length,
                            bend, flags, resampler version)
   -> VoiceRenderCache hit?  yes: play the cached buffer as a Sampler voice
                             no : worker thread renders -> SamplePool -> snapshot swap
```

- **Analysis** (`WorldAnalysis`): F0 from the bank's own UTAU `.frq` when present
  and valid (Teto ships one per WAV), else WORLD Harvest/DIO; CheapTrick envelope and
  D4C aperiodicity always from WORLD. Cached next to the project as `.adxfrq` keyed by
  file hash. Heavy; runs on a worker with progress, never on first play.
- **Render** (`UtauResampler`): resolve alias (with the bank's prefix/suffix map and
  "CV/VCV" fallbacks), take the oto segment, shift F0 to the note pitch (plus
  `slide`/`pitchCurve`/vibrato, evaluated per frame), time-stretch only the vowel
  tail so the consonant keeps its natural length, apply UTAU flags (`g` gender,
  `B` breathiness, `t` tuning, `P` peak compression — the common subset), resynthesize.
- **Join**: preutterance and overlap give each note its early start and the
  crossfade into the previous note, so consecutive notes on a track are rendered as a
  *phrase*, not as isolated one-shots. The cache key therefore includes the
  neighboring lyric when overlap is non-zero.
- **Playback** is ordinary Sampler playback of the cached buffer, so it is
  allocation-free and obeys the Phase 3 voice rules. A note whose render is not ready
  plays silence plus a "rendering…" marker in the UI; it never blocks the callback.
- **Offline == realtime**: export and golden renders *wait* for the cache to be
  complete (the render is deterministic: WORLD, fixed seeds, no wall-clock), so the
  determinism gate of Phase 3 §4.10 still holds.

#### Reference voicebank: Kasane Teto (重音テト)

Voice is **built and accepted against Kasane Teto**, not against an abstract UTAU
spec. Source: the official site (kasaneteto.jp/utau/), `TETO-tougou-110401.zip`
("Solo + Continuous", 95 MB), downloaded to `third_party_assets/voicebanks/teto/`
(git-ignored). It is surveyed here from the real files; these facts, not the generic
description above, are the design inputs:

| Fact (measured) | Consequence for the design |
|---|---|
| Three sub-banks in one install: `単独音` solo/CV (142 WAVs, 319 oto lines), `連続音` continuous/VCV (249 WAVs, 887 lines), `エクストラ` extra (16 WAVs, 39 lines) | `Voicebank` loads a **tree of sub-banks** with a search order (VCV → CV → extra), not one flat `oto.ini` |
| `oto.ini`, `character.txt`, `readme.txt` are Shift-JIS; WAV filenames are Japanese (`_あ.wav`) | Shift-JIS decode is mandatory, not an edge case; filenames resolve through the zip/OS encoding, never assumed ASCII |
| One WAV carries **many oto lines** (`_あ.wav` has `あ`, `- あ` and `* あ`; a VCV file has five: `a い`, `i う`, `e お`, …) | The alias table is `alias → (wav, 5 timings)`, many-to-one on the file; analysis is cached per WAV, not per alias |
| CV aliases: `あ` plain, `- あ` (after silence), `* あ` (breath-in/alt). VCV aliases: `<prev vowel> <kana>` (`a い`) | Alias resolution is driven by the **previous note's ending vowel**: lyric `い` after `あ` → `a い`; after a rest → `- い`; otherwise plain `い`. Rule lives in `AliasResolver`, with the bank's own fallbacks |
| Timings are fractional ms (`542.228`) and **cutoff is negative** in VCV lines (`-809.68`: measured back from the end of the file) | Parse as `double`; negative cutoff means "length = fileEnd − |cutoff| − offset". A common cause of garbled consonants if mishandled; gets its own test |
| 44.1 kHz, mono, 16-bit WAV | Resampler runs at 44.1 kHz internally; project-rate conversion happens at the cache boundary (`Resample.h`), once |
| Recorded at **mono-pitch ≈ D#4** (UTAU's own `.frq` reports an average F0 of 309.8 Hz) | Quality degrades far from D#4. Define a supported range (target ±7 semitones clean, wider with a UI warning), and have the gate test the extremes |
| Every WAV ships a **`.frq`** (UTAU `FREQ0003`: hop 256, average F0, then N × (f0, amplitude) doubles) | Import the `.frq` as the F0 track and **skip Harvest/DIO** when it validates; fall back to analysis only when missing/corrupt. Faster first load and it matches what the bank author tuned |
| `character.txt` + `readme.txt` bundled | Surfaced in the instrument panel (name, image `teto.bmp`, credits) |

**Licensing, as shipped with this bank (readme.txt).** Free for **non-commercial**
use (including non-profit doujin); work using it may be published without notifying
the author; **commercial use needs separate permission** (Crypton Future Media — the
official terms page says the UTAU/VOICEPEAK banks are licensed through them), and
**distributing the library without consent is forbidden**. Consequences, binding on
the plan:

- the bank is **never committed** (`third_party_assets/` is in `.gitignore`), never
  in CI artifacts, never in the installer, never in a "demo project" bundle;
- project bundles ("collect and save") **exclude voicebanks by default** and record
  only path + content hash; including one needs an explicit user action with a
  warning;
- the first-load dialog shows the bank's readme text and a non-commercial notice;
- the export dialog shows a one-line reminder when a Voice channel is present.
  adX informs; it does not police.

**What is tested on Teto, and what cannot be.** Because the bank cannot enter the
repo or CI, tests split in two:

- **CI tests** run on the synthetic generated bank (`tests/data/voicebank_synth/`),
  which is built to *reproduce Teto's structural quirks*: Shift-JIS `oto.ini`, many
  aliases per WAV, VCV `<vowel> <kana>` aliases, negative cutoffs, fractional
  timings, a `.frq`. Every parser and resolver rule above has a CI test there.
- **Local acceptance tests** (`tests/local/teto/`) run only when `ADX_TETO_DIR`
  points at the extracted bank, and are skipped (not failed) otherwise. They assert:
  `teto_loads_all_three_subbanks` (counts 319/887/39 lines; every referenced WAV
  exists), `teto_frq_matches_analysis` (imported F0 within 30 cents of WORLD's on
  voiced frames), `teto_vcv_phrase_resolves` ("あ い う え お" resolves to
  `- あ`, `a い`, `i う`, `u え`, `e お`), `teto_pitch_sweep_no_artifacts` (render
  D#4 ±12 semitones, no clipping, no NaN, F0 within ±10 cents of target), and
  `teto_offline_equals_realtime`.
- **The human gate.** A committed score — `examples/teto_demo.adx`, lyrics and notes
  only, no audio — renders a ~20 s phrase using a slide, a vibrato and a
  Vocoder-free dry vocal. It must be intelligible Japanese by ear. Like the Phase 4
  `suffocation.adx` A/B, this is a listening check recorded in `plans/STATE.md`; it
  cannot be automated and is not pretended to be.

**Automation of Voice parameters.** Voice has two kinds of parameter, and the
registry marks them differently:

- **Post-render** (`Sample`/`Block` class): volume, pan, filter, envelope, and every
  insert effect. Automate freely; nothing re-renders.
- **Baked** (`Baked` class): `gender`, `breathiness`, `tuning`, `formantShift`, the
  resampler flags. These change the *synthesis*, so they live in the cached render.
  They are automatable, evaluated per WORLD frame (5 ms) over the note's span at render
  time, and the sampled curve (not the lane) is hashed into `VoiceRenderKey`.
  Consequences: editing a baked lane re-renders only the notes the edit overlaps;
  the changed notes play silent-with-marker until ready (never blocking, §4.13 above);
  the parameter's knob and lane are drawn with a "re-renders" badge; and a baked lane
  is time-continuous within a note but **not across a phrase's join**, where the next
  note's render starts from its own first value.

Tests: `voice_baked_lane_changes_key_only_where_overlapping`,
`voice_baked_automation_deterministic`, `voice_post_render_automation_no_rerender`.

**Encodings and edge cases.** `oto.ini` and `character.txt` are very commonly
Shift-JIS (Teto's are); detect and transcode to UTF-8, store the original bytes, and never rewrite
a user's bank. Banks with missing WAVs or out-of-range oto timings load partially
with diagnostics (`ADX4300` range), not as a failure of the whole instrument.
Romaji/hiragana alias sets are both supported; a note's lyric is matched against the
alias table, with a per-channel `lyricFallback`.

**Optional external resampler adapter.** UTAU resamplers (moresampler, fresamp, …)
share a fixed 12-argument command line. A user may point a channel at one;
it runs sandboxed exactly like Phase 9's plugin scanner (separate process, timeout,
no project access beyond the temp WAV) and its output goes into the same
`VoiceRenderCache`. Off by default; the built-in WORLD resampler is the supported path.

**Licensing.** Voicebanks carry their own terms, often non-commercial or
character-specific. adX bundles **none**. The UI shows `character.txt`/readme
text on first load, and the export dialog does not enforce licenses (that is the
user's responsibility), but the project stores only a path + content hash, so
sharing a project never redistributes a bank.

**Import.** UST (UTAU sequence) and USTX (OpenUtau) import via Phase 9's import
path: notes, lyrics, and per-note pitch bends map to `Note`, `Note.lyric`, and
`Note.pitchCurve`. Export back to UST is out of scope.

**Not covered here.** *Vocaloid* is proprietary and has no third-party SDK, so
there is no native Vocaloid instrument. It is reachable only through Phase 9's VST3
host if the user has a VST3 build of a voice product. *DiffSinger* (open neural
singing synthesis, ONNX) is a plausible later `VoiceInstrument` backend behind
`ADX_ENABLE_ONNX` (FINAL_PLAN §8.2); the render-cache design above is deliberately
backend-agnostic for that reason. Record as a Phase 10+ candidate, not a deliverable.

**Tests.** `oto_parse_shiftjis`, `alias_resolution_cv_vcv`,
`world_analysis_deterministic` (bit-identical across runs),
`resampler_pitch_accuracy` (±5 cents on a synthetic bank),
`resampler_consonant_length_preserved`, `voice_cache_key_stable_and_sensitive`
(any input change changes the key), `voice_note_not_ready_is_silent_not_blocking`
(allocator hook + no wait), `voice_export_waits_for_cache`,
`voice_offline_equals_realtime` golden hash. A tiny synthetic voicebank
(`tests/data/voicebank_synth/`) is generated by script mirroring Teto's structure
(see the reference-voicebank table) — no real banks in the repo. Teto-specific tests
are local-only (above).

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
