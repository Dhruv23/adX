# STAKILLAZ Suite — Implementation Plan

Trashwave / Phonk / Experimental-Hardstyle generation suite for the adX
engine (branch: `feature/experimentation-genres`).

## 1. Research Summary

STAKILLAZ operates in the SoundCloud-underground cluster of **trashwave,
phonk, jumpstyle/hardstyle and yabujincore** (their own Spotify playlist is
literally titled "jumpstyle/hardstyle/yabujincore"; releases like "LEVELS
PHONK" and "russian phonk" collabs sit squarely in Russian-phonk territory).
The signature ingredients:

- **Blown-out distortion** — everything from the kick to the lead is driven
  into saturation; overtones matter more than the clean source.
- **Four-on-the-floor hardstyle kicks** — a clicky transient welded to a
  pitched, distorted sub tail; the tail's pitch follows the bassline root
  (this is what Phase 5's kick generator + the pitched `samples/kick_*.wav`
  set already provide).
- **Heavy sidechain pumping** — the whole bus ducks hard on every kick;
  in phonk/trashwave the pump IS the groove.
- **808 sub-bass with pitch-drop transients** — sine/triangle sub with a fast
  exponential pitch envelope (start +2..3 octaves up, fall to root in
  30–80 ms) — simultaneously the hardstyle-kick "tok" and the 808 boom.
- **Chopped, repitched samples** — vocal chops and drum loops pitched extreme
  distances (hyperpop-style), i.e. RubberBand with big semitone offsets.
- **Dark atmosphere** — Memphis-tape hiss, minor-key cowbell melodies, lo-fi
  reverbs (covered by Phase 5's per-track Reverb + Distortion inserts).

## 2. Mapping to the adX Engine

### 2.1 Distortion & Saturation (drive in the Patch)
- `Patch` gains `drive` (0 = clean/bypass, up to ~30). Applied at the voice
  output as `tanh(x·(1+drive))/tanh(1+drive)` — a waveshaping stage in
  `AudioEngine::process()` upstream of the master volume clamp.
- A master `MASTER DRIVE` parameter (in the MASTER FX window, via
  `EngineParam`) provides the final hard-clip/limiter character stage before
  the compressor for the "blown out" glue.
- Persistence: `DRIVE=x` line in `[PATCH]`, `MASTER_DRIVE=` in `[GLOBAL]`.

### 2.2 Heavy Sidechain Compression
- `CompressorState` gains sidechain fields: `sidechainEnabled`,
  `sidechainAmount` (0..1), `sidechainReleaseMs`, source fixed to **Track 1's
  bus** (the kick track by convention, matching the plan).
- In `process()`: an envelope follower tracks Track 1's post-effect bus
  amplitude; master gain is multiplied by
  `1 − amount · env` (clamped ≥ 0) before the peak compressor — classic
  pump keyed by the kick, not by the master peak.
- Controls in the MASTER FX window; `SIDECHAIN=enabled, amount, releaseMs`
  in `[GLOBAL]`.

### 2.3 808-Style Sub Oscillator + Pitch Drop
- `Patch` gains `subOscLevel` (0..1), `subOscWave` (0 = sine, 1 = triangle),
  `pitchDropSemitones` (0 = off; e.g. 36 = start 3 octaves up),
  `pitchDropMs` (decay time constant).
- `Voice` gains `subPhase` and `ageSamples`. The pitch-drop factor
  `2^(drop·e^(−age/τ)/12)` multiplies the fundamental frequency of BOTH the
  harmonic stack and the sub — one knob turns any patch into a hardstyle
  kick / 808.
- The sub bypasses the 16-harmonic array entirely (pure sine/triangle at the
  fundamental) and is summed after the harmonic oscillator, before drive.
- Persistence: `SUB=level, wave, dropSemitones, dropMs` in `[PATCH]`.
- Factory patches (in `PatchLibrary`, shared with the C418 suite):
  **Hardstyle Kick** (drive ~12, drop 36 st/45 ms, sub 0.9),
  **808 Sub** (sub 1.0, drop 12 st/60 ms, long release),
  **Screech Lead** (odd-harmonic stack, drive ~18),
  **Phonk Bass** (low harmonics, drive 6, moderate LPF).

### 2.4 Clip Pitch-Shifting Panel (hyperpop sampling)
- Phase 3's `AudioClipProcessor::ReprocessClip` already covers ±24 st and
  0.25–4× stretch from the Arranger's per-clip panel. Extend the panel with:
  - extreme-range slider (−48..+48 st) and stretch 0.1–8×,
  - quick-chop buttons (+12 / −12 / +7 / formant-style "chipmunk" +19),
  - a "REVERSE" toggle (reverse `originalPcmData` before RubberBand).
- All processing stays on the Main Thread (RubberBand offline engine).

## 3. Order of Work
1. `Patch` drive/sub/pitch-drop fields + voice DSP + parser + patch-editor UI.
2. Sidechain fields in `CompressorState` + envelope follower + MASTER FX UI.
3. Master drive stage.
4. Extended clip panel (range, chop presets, reverse).
5. Factory patches + a demo `stakillaz_demo.adx` (kick track + 808 + screech).

## 4. Reference material
- STAKILLAZ on SoundCloud (soundcloud.com/stakillaz), "LEVELS PHONK" on
  Spotify; genre cluster per their "jumpstyle/hardstyle/yabujincore" playlist.
- Kick style anchors requested for the sample set: prodArvee, Hixxy, Yosuf
  (see `samples/README.md` for the downloaded pitched kick pack).
