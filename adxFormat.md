# ADX File Format

The ADX file format is used for defining audio projects in the adX application. It consists of several sections, each with specific parameters and values.

## GLOBAL Section
- **BPM**: Beats per minute, controlling the tempo.
  - Example: `BPM=120.0`
- **MASTER_VOL**: Master volume level (0.0 to 1.0).
  - Example: `MASTER_VOL=0.75`
- **TUNING**: Tuning frequency (e.g., A4 = 440 Hz).
  - Example: `TUNING=440.0`
- **DELAY** *(optional)*: Master stereo ping-pong delay: `DELAY=TimeMs, Feedback, Mix` (Mix 0.0 = bypass).
  - Example: `DELAY=350, 0.35, 0.25`
- **REVERB** *(optional)*: Master algorithmic reverb: `REVERB=RoomSize, Damping, Mix` (all 0.0–1.0; Mix 0.0 = bypass).
  - Example: `REVERB=0.8, 0.5, 0.3`
- **SIDECHAIN** *(optional)*: Track 1's bus ducks the master: `SIDECHAIN=Enabled, Amount, ReleaseMs` (Enabled 0/1).
  - Example: `SIDECHAIN=1, 0.6, 120`
- **MASTER_DRIVE** *(optional)*: Final tanh saturation before the compressor (0.0 = bypass, up to 30.0).
  - Example: `MASTER_DRIVE=8.0`
- Omitted master-FX keys reset to their defaults (all bypassed) on load.

## PATCH Section
- **ENVELOPE**: ADSR envelope values for attack, decay, sustain, and release (0.0 to 1.0).
  - Format: `ENVELOPE=Attack, Decay, Sustain, Release`
  - Example: `ENVELOPE=0.01, 0.4, 0.6, 0.3`
- **HARMONICS**: Amplitudes of overtones (16 values).
  - Format: `HARMONICS=Value1, Value2, ..., Value16`
  - Example: `HARMONICS=1.0, 0.5, 0.33, 0.25, 0.1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0`
- **DRIVE** *(optional)*: Per-voice tanh waveshaper drive (0.0 = clean/bypass, up to 30.0).
  - Example: `DRIVE=12.0`
- **FILTER** *(optional)*: Per-voice one-pole low-pass with LFO cutoff modulation: `FILTER=CutoffHz, LfoRateHz, LfoDepth`. Cutoffs ≥ 19000 Hz bypass the filter.
  - Example: `FILTER=2500, 0.1, 0.4`
- **SUB** *(optional)*: 808-style sub oscillator with pitch-drop transient: `SUB=Level, Wave, DropSemitones, DropMs` (Wave 0 = sine, 1 = triangle; DropSemitones 0 = no drop).
  - Example: `SUB=0.9, 0, 36, 45`

## TRACK Section
- **Arp Entry** *(optional, at most one per track)*: Track-level arpeggiator. When enabled, the track's held/overlapping notes are not played directly — the chord sounding at each grid step becomes one cascading staccato note.
  - Format: `ARP Mode RateBeats Octaves Gate` (Mode 0 = off, 1 = up, 2 = down, 3 = up-down; RateBeats e.g. `0.25` for 16ths; Octaves 1–4; Gate 0.1–0.95 fraction of the step).
  - Example: `ARP 1 0.25 2 0.8`
- **Note Entries**: Define musical notes with specific parameters.
  - Format: `Note StartBeat Duration Velocity`
  - Example:
    ```
    C4  0.0  1.0  0.9
    E4  1.0  1.0  0.8
    G4  2.0  1.0  0.8
    C5  3.0  2.0  1.0
    ```
- **Clip Entries**: Reference a decoded audio file (WAV/MP3) placed on the timeline.
  - Format: `CLIP FilePath StartTimeSeconds [PitchShiftSemitones TimeStretchFactor [R]]` (the file path must not contain spaces; a trailing `R` plays the clip reversed)
  - Position is in absolute seconds from the start of playback, independent of BPM (unlike notes, which are positioned in beats).
  - The trailing pitch/stretch pair is optional on load (older files without it default to `0.0` semitones / `1.0` speed, i.e. unshifted); newly saved files always include it.
  - `PitchShiftSemitones`: semitones to shift the clip's pitch (0 = unshifted, e.g. `12` = one octave up).
  - `TimeStretchFactor`: playback-speed multiplier independent of pitch (`1.0` = original speed, `2.0` = twice as long/half speed).
  - Example:
    ```
    CLIP samples/kick.wav 4.000 0.0 1.0
    CLIP samples/vocal_chop.wav 8.000 -3.0 1.5
    ```

- **Effect Entries**: Per-track insert effects, applied in file order to the track's bus before it is mixed into the master (Phase 5).
  - Formats:
    - `EFFECT Reverb Mix RoomSize Damping` — Freeverb-style algorithmic reverb. `Mix` 0.0 (dry) to 1.0 (wet), `RoomSize` 0.0–1.0, `Damping` 0.0–1.0.
    - `EFFECT Distortion Drive Mix` — tanh waveshaper. `Drive` 1.0 (clean-ish) to 30.0 (blown out), `Mix` 0.0–1.0.
  - Example:
    ```
    EFFECT Reverb 0.350 0.800 0.500
    EFFECT Distortion 8.000 1.000
    ```

This format allows for detailed configuration of audio projects, including instrument patches, musical tracks, audio clips, and per-track effects.