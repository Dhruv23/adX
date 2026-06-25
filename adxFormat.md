# ADX File Format

The ADX file format is used for defining audio projects in the adX application. It consists of several sections, each with specific parameters and values.

## GLOBAL Section
- **BPM**: Beats per minute, controlling the tempo.
  - Example: `BPM=120.0`
- **MASTER_VOL**: Master volume level (0.0 to 1.0).
  - Example: `MASTER_VOL=0.75`
- **TUNING**: Tuning frequency (e.g., A4 = 440 Hz).
  - Example: `TUNING=440.0`

## PATCH Section
- **ENVELOPE**: ADSR envelope values for attack, decay, sustain, and release (0.0 to 1.0).
  - Format: `ENVELOPE=Attack, Decay, Sustain, Release`
  - Example: `ENVELOPE=0.01, 0.4, 0.6, 0.3`
- **HARMONICS**: Amplitudes of overtones (16 values).
  - Format: `HARMONICS=Value1, Value2, ..., Value16`
  - Example: `HARMONICS=1.0, 0.5, 0.33, 0.25, 0.1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0`

## TRACK Section
- **Note Entries**: Define musical notes with specific parameters.
  - Format: `Note StartBeat Duration Velocity`
  - Example:
    ```
    C4  0.0  1.0  0.9
    E4  1.0  1.0  0.8
    G4  2.0  1.0  0.8
    C5  3.0  2.0  1.0
    ```

This format allows for detailed configuration of audio projects, including instrument patches and musical tracks.