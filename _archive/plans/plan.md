# Project Implementation Plan: adX Native Song Reconstruction

**Project Context:** The previous `plan.md` (Phases 1-5: audio playback, hot-reload,
RubberBand time-stretch, ONNX melody extraction, and the DSP/sample-browser
remixer workflow) is complete and merged. This plan is the **next** arc: closing
the gap between the current engine and what's needed to reconstruct an entire
multi-timbre electronic song (case study: Crystal Castles' "Suffocation", see
`suffocation.adx` at the repo root) **entirely natively** — every voice
synthesized, vocals included as a note track, no audio samples.
**Host Environment:** 64GB DDR5 system RAM. Aggressive parallel compilation
(`cmake --build . -j`) is safe; so is loading large ML models into memory.
**Architectural Strictness (unchanged from the previous plan, still binding):**
1. **The Audio Thread is Sacred:** `AudioEngine::process` runs on a high-priority
   real-time thread. **DO NOT** introduce `std::mutex`, `std::string`
   allocations, memory allocations (`new`/`malloc`), or file I/O inside this loop.
2. **Lock-Free Communication:** All state changes from the ImGui (Main) thread
   must be passed to the Audio thread via the existing
   `moodycamel::ReaderWriterQueue<AudioEvent>`.
3. **Heavy Processing:** File decoding, ONNX inference, and `.adx` parsing
   happen on the Main Thread (or a background `std::thread`).

**Reference blueprint:** `suffocation.adx` (repo root) is the target file this
plan builds toward. It is written in the *extended* `.adx` format described
below, with every new key tagged `[P1]`..`[P5]` for the phase that makes it
audible. Until a phase lands, the parser must ignore keys it doesn't
recognize yet, so the file keeps loading (as a degraded, additive-only mix)
at every stage of this plan.

---

## Phase 1: Per-Track Patches & Per-Track Mix
**Goal:** Break the single shared-patch constraint (`AudioEngine.h` /
`m_activePatch`) so every `Track` plays through its own synth patch
simultaneously — the #1 blocker for any multi-timbre song.

1. **Data:** Add `float volume = 1.0f, pan = 0.0f;` to `Track` in `AudioData.h`.
2. **Engine:** Remove the single `m_activePatch` resolution path. At
   `DispatchSequenceUpdate` time, resolve each `Track::patchName` against the
   patch registry shipped with the sequence update and stamp the resolved
   `const Patch*` onto every `NoteOn` scheduled for that track (the `Voice`
   struct already carries a per-voice `patch` pointer — this just makes sure
   it's *this track's* patch, not the one global active patch).
3. **Engine:** Apply per-track `volume`/`pan` on the track's bus sum before
   mixing into the master (same point where per-track `effects` are applied).
4. **Parser:** Add `MIX=Volume,Pan` as a `TRACK`-scope key. Strip inline
   `#`-to-end-of-line comments on every value line (needed because
   `suffocation.adx` documents units inline, e.g. `SUB=1.0, 0, 36, 55  # comment`).
5. **UI:** Per-track patch-select dropdown and volume/pan controls in the
   track header (`SequencerUI.cpp`).
6. **Acceptance:** Loading `suffocation.adx` plays Kick/SubBass/Lead/Pad/
   Vocal/Snare/Hat simultaneously with seven audibly distinct timbres.

---

## Phase 2: Automation Engine & Arrangement Markers
**Goal:** Give the engine a timeline of *changing* parameters instead of only
static, UI-set atomics — the #2 blocker, since a song's arrangement is
largely expressed as automation (filter sweeps, volume swells, sidechain
on/off per section, reverb throws).

1. **Data:** `struct Breakpoint { float beat, value; Curve curve; };` and
   `struct AutomationLane { std::string trackTarget, paramTarget; std::vector<Breakpoint> points; };`
   held on `SequencerState`. `Curve` = `Linear | Exponential | Step | Smooth`.
2. **Data:** Introduce a small "live params" block of `std::atomic<float>` per
   track/patch instance (filter cutoff, resonance, formant morph, effect wet
   mix, etc.) that voices/effects read each block and automation lanes write
   into — this is the plumbing Phases 3-5 hang their modulation targets on.
3. **Engine:** Each process block, evaluate every active lane at the current
   `playheadPositionBeats` and write the interpolated value to its target's
   atomic. Target resolution is a small string-path dispatch:
   `patch.<field>`, `mix.<field>`, `effect.<TypeName>.<field>`,
   `master.<field>` (target `MASTER` as the track name).
4. **Parser:** `MARKER=Beat,Name` (`GLOBAL` scope) and
   `[AUTOMATION <Track> <target>]` sections, each line `beat value curve`.
5. **UI:** Automation lane view under the timeline (draggable breakpoints per
   track/param) and marker labels on the timeline ruler.
6. **Acceptance:** `suffocation.adx`'s Lead filter sweep (closed intro → open
   chorus), Pad volume swell, section-gated `SIDECHAIN`, and the breakdown
   reverb throw all audibly follow the automation lanes.

---

## Phase 3: Noise Oscillator, Resonant Filter & Filter Envelope
**Goal:** Give the voice engine (`AudioEngine.cpp`, additive stack at
line ~323) the two primitives it's missing for *any* native percussion and
for real subtractive-synth motion: a noise source and a proper resonant,
envelope-modulated filter (today's per-voice filter is a fixed one-pole LPF
with only an LFO, no resonance and no envelope).

1. **Voice:** Add a per-voice noise generator (xorshift PRNG, no heap
   allocation) with white/pink modes, summed into the oscillator stage
   alongside the harmonic stack and sub-oscillator.
2. **Voice:** Add a TPT (topology-preserving-transform) state-variable filter
   with switchable LP/BP/HP mode and a resonance parameter, replacing/
   augmenting the existing one-pole stage. Add a dedicated filter ADSR
   (`Attack/Decay/Sustain/Release` + `envAmount`) plus optional key-tracking,
   independent of the amplitude envelope.
3. **Patch/live-params:** `noiseLevel, noiseType, resFilterType,
   resFilterCutoff, resFilterResonance, filterEnvAmount, keyTrack`, plus the
   filter-envelope times.
4. **Parser:** `NOISE=Level,Type`, `RESFILTER=Type,Cutoff,Resonance,EnvAmt,KeyTrack`,
   `FILTERENV=Attack,Decay,Sustain,Release` (`PATCH` scope).
5. **Acceptance:** `suffocation.adx`'s Kick gets a synthesized beater-click
   transient, Snare/Hat are fully synthesized from noise (no samples), and
   SubBass/Lead's resonant sweeps have audible resonance/squelch, not just a
   dull low-pass.

---

## Phase 4: Formant Synthesis, Vibrato & Glide (the Vocal Track)
**Goal:** Make a note-based track actually read as a sung voice rather than
an organ — the feature that unlocks "vocals as a track of notes."

1. **Voice:** Add a parallel 3-band resonant bandpass "formant bank" on top
   of a harmonically-rich source (additive stack or Phase-5 unison saw).
   Each band has a vowel-preset center frequency; a `morph` parameter
   crossfades between two vowel presets (e.g. `Ah` → `Oo`).
2. **Voice:** Add a pitch-LFO **vibrato** (rate, depth in cents, onset delay)
   applied to the fundamental (and therefore to the harmonic stack, sub-osc,
   and formant bank all together).
3. **Voice:** Add **glide/portamento** — when a new note starts while the
   voice's oscillator is already running (or within a short legato window),
   slide the previous frequency to the new one over `glideMs` instead of
   jumping, producing the sliding "wail" between notes.
4. **Patch/live-params:** `FORMANT=VowelA,VowelB,Morph,Amount`,
   `VIBRATO=RateHz,DepthCents,DelayMs`, `GLIDE=Ms`; `formantMorph` exposed as
   a live-param so Phase 2 automation can animate it.
5. **Acceptance:** The `Vocal` track in `suffocation.adx` is recognizable as a
   voice-like timbre, its formant morph audibly shifts per section via
   automation, and note-to-note glide is audible on the melody.

---

## Phase 5: Unison Oscillators, Lo-Fi FX & Aux Sends
**Goal:** The finishing character pass — width, grit, and flexible routing.

1. **Voice:** Add a band-limited (polyBLEP) virtual-analog oscillator stage
   (saw/square/triangle) as an alternative/complement to the additive sine
   stack, with **unison**: N detuned copies spread across the stereo field
   (for the supersaw bass/lead/pad character).
2. **Effects (`AudioEffect.h`/`AudioEffects.cpp`):** Add `BitcrushEffect`
   (bit-depth quantization + sample-rate decimation), `ChorusEffect`
   (modulated delay lines), `EQEffect` (3-band shelf/peak). Register each in
   the parser's `EFFECT` dispatch (`AdxParser.cpp` load + save paths).
3. **Routing:** Add per-track `SEND=BusName,Amount` (`Delay`/`Reverb`) so a
   track can throw into a shared master aux without needing its own reverb
   insert instance.
4. **Patch:** `OSC=Wave,UnisonVoices,DetuneCents,PulseWidth` (`PATCH` scope).
5. **Acceptance:** `suffocation.adx` renders (via `ExportRenderer`, WAV/MP3/
   FLAC) with the wide detuned bass/lead/pad, bitcrushed vocal/hat grit, and
   delay/reverb sends all active — the full native reconstruction, playable
   end-to-end from `.adx` through the existing export pipeline.

---

## Extended `.adx` Format Reference (new keys introduced by this plan)

| Key | Scope | Phase | Meaning |
|---|---|---|---|
| `MIX=Volume,Pan` | `TRACK` | 1 | Per-track level + pan |
| `MARKER=Beat,Name` | `GLOBAL` | 2 | Arrangement/section markers |
| `[AUTOMATION Track Target]` + `Beat Value Curve` lines | top-level section | 2 | Breakpoint automation; `Track=MASTER` targets `master.*` |
| `NOISE=Level,Type` | `PATCH` | 3 | White/pink noise source level |
| `RESFILTER=Type,Cutoff,Resonance,EnvAmt,KeyTrack` | `PATCH` | 3 | Resonant multimode filter (LP/BP/HP) |
| `FILTERENV=Attack,Decay,Sustain,Release` | `PATCH` | 3 | Dedicated filter envelope |
| `FORMANT=VowelA,VowelB,Morph,Amount` | `PATCH` | 4 | Vocal formant bank |
| `VIBRATO=RateHz,DepthCents,DelayMs` | `PATCH` | 4 | Pitch LFO |
| `GLIDE=Ms` | `PATCH` | 4 | Portamento between notes |
| `OSC=Wave,UnisonVoices,DetuneCents,PulseWidth` | `PATCH` | 5 | Virtual-analog oscillator + unison |
| `EFFECT Bitcrush BitDepth RateHz Mix` | `TRACK` | 5 | Bit/rate reduction insert |
| `EFFECT Chorus Rate Depth Mix` | `TRACK` | 5 | Modulated delay-line chorus insert |
| `EFFECT EQ LowGain MidGain HighGain` | `TRACK` | 5 | 3-band EQ insert |
| `SEND=BusName,Amount` | `TRACK` | 5 | Aux send to a shared master bus |

Unrecognized keys/sections must be skipped (not fatal) at every phase so
`suffocation.adx` continues to load — with progressively more of the song
audible — as each phase lands.
