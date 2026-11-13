# Building a sound pack

How the packs of Phases 13 to 19 are made: what a pack is, the vocabulary its sounds
are written in, and the tools that build, check and measure them. The file formats are
specified in [adx-format-v2.md](adx-format-v2.md) (§7.7 macros, §7.8 racks, §13 presets,
SFZ, `lib://` and `pack.adxpack`); this page is the workflow.

## 1. What goes in a pack

A pack is a folder under `engine/preset/packs/<pack>/`:

```
engine/preset/packs/hardcore/
  pack.adxpack               what it is, which libraries it needs, its assets' licences
  kicks/euphoric.adxrack     racks: layered sounds with a chain, sends and macros
  leads/screech.adxpreset    presets: one instrument or one effect
  ...
```

Use a **preset** for one instrument or one effect. Use a **rack** when a sound is more
than one: a kick of three layers into an eight-slot chain, a supersaw with a reverb send,
a vocal chain. A rack's layer may name a preset (`PRESET=hardcore/Euphoric Tail`), so a
sound is defined once and reused.

Every preset and rack names a `CATEGORY` from the fixed list in `app/adx/taxonomy.py`
(`Drums/Kick`, `Bass/808`, `Lead`, `Pad`, `Keys/Mallet`, `Chain/Space`, ...). The
category decides three things:

- where it appears in the browser, across every pack;
- what its macros mean. Every `Lead`'s Macro 1 is Brightness, 2 Detune, 3 Vibrato,
  4 Drive, 5 Attack, 6 Release, 7 Space, 8 Width (`taxonomy.MACRO_SLOTS`). A rack exposes
  at most eight, named as its category names them, in slot order;
- how loud its audition must be (`taxonomy.LOUDNESS_TARGETS`, within ±2 LU).

## 2. The instruments and effects the packs are written in

| Need | Use |
|---|---|
| a layered kick, a kit, a split, a velocity layer | a rack; `KEYS=` and `VELOCITY=` on each layer |
| one knob over many parameters | a `[MACRO]`: `TARGET path range=lo..hi curve=...` |
| a pitch dive into each note (hoover, laser, kick tail) | `pitchEnv.amount` and `pitchEnv.time` on va, wavetable, fm, additive, physical |
| brightness that follows velocity, vibrato that arrives late | the modulation matrix: `mod.N.source/dest/amount`; `lfo1.delay`, `lfo1.fade` |
| tempo-locked wobble | `lfo1.sync` (1/64 .. 4 bars), or the `VolumeShaper` effect |
| mono lines, glides, TB-style slides | `voice.mode=1` (mono) or `2` (legato), `glide`, `glide.mode` |
| plucked or bowed strings, mallets, bells, pans, flutes, reeds | `physical` (model pluck, bow, modal, tube) |
| 808/909/707 drums, cowbell, rim, clave, shaker, conga | `drumsynth`, `model` 0..11, `character` |
| clip, fold, shape | `Clipper` (16x oversampled), `Shaper` (a drawn seven-knot curve) |
| OTT | `OTT`: three bands, upward and downward |
| long or shimmering space, frozen pads | `Hall` (0.1-60 s, shimmer, freeze) |
| tape, chorus, Leslie | `Tape`, `Ensemble`, `Rotary` |
| hard-tune | `PitchCorrect` (`retune=0` is the snap) |
| sampled orchestral, piano, world instruments | a sampler preset imported from a library's SFZ |

## 3. Samples

Sample libraries are data, not dependencies. Fetch them once:

```
python tools/packs/fetch_libraries.py list
python tools/packs/fetch_libraries.py fetch vsco2ce vcsl salamander generaluser
```

Each is pinned to a commit and checked file by file against
`engine/preset/libraries/<id>.sha256`. A preset names a library file as
`lib://<id>/<path>`, never by where the store is.

Import an SFZ mapping as a preset rather than writing zones by hand:

```
python tools/packs/import_sfz.py "%LOCALAPPDATA%\adX\libraries\vsco2ce@6dd651d55dde\ViolinSect-KS.sfz" \
    --pack classical --category Strings --out engine/preset/packs/classical/strings
```

It writes the preset and an `.import.txt` beside it listing every SFZ opcode that was
not applied, with its line. Read it: an *approximated* opcode (a per-region envelope, a
controller-driven volume) plays differently from the SFZ player the library was made
for. Exit 1 means an opcode was unknown or a sample missing.

Give every sampled preset a `FALLBACK=` naming a synthesized preset in the same pack:
on a machine without the library, the browser shows it as needing one and plays the
fallback, with a diagnostic, rather than silence.

## 4. Rendering

- `tools/packs/render_multisample.py PRESET` renders any preset or rack every 3
  semitones at three velocities and two round robins into `lib://rendered/<pack>/...`
  and writes the sampler preset that plays them. Use it for a physical model too costly
  at full polyphony, or to ship an "adX-rendered" variant reproducible from source.
- `tools/packs/kick_lab.py KICK.adxrack --passes 2` is the hardstyle resample loop: the
  kick rendered at every key from E1 to E2 and each render run back through the rack's
  own chain, into a sampler with one zone per key. Every tail is in tune by
  construction.
- `tools/packs/gen_wavetables.py` and `tools/packs/gen_irs.py` regenerate the committed
  tables and impulse responses in `docs/examples/generated/`.

## 5. Checking

```
python tools/packs/audition.py engine/preset/packs/hardcore/**/*.adx*
```

plays each sound with its category's phrase - a melody for a lead, a held chord for a
pad, four on the floor moving root for a kick - at its `TEMPO_HINT` and `ROOT_HINT`,
and writes the WAV, a spectrum PNG and a JSON with integrated LUFS, true peak, and
whether it is inside its category's window. Adjust `level` (or the rack's chain output)
until every sound is: browsing must never jump by 15 dB. The browser plays these cached
WAVs on click without loading anything.

Before a pack is offered to the user, every rack and preset must load without errors,
render without `adx glitch` findings, and appear in the browser. Then the user plays the
pack and says, in their own words, whether it is ready.

## 6. Licences

`pack.adxpack` lists every asset that is not adX's own, with its licence and whether it
may be shipped:

```
[ASSETS]
ASSET "lib://vsco2ce/" licence=CC0-1.0 redistributable=yes credit="Versilian Studios"
```

`tools/gen_third_party.py` puts them in `installer/THIRD-PARTY.md`, and
`tools/packs/licence_report.py --gate STAGE` fails the installer build if a file from an
asset marked `redistributable=no` - or a copy of the Windows GM bank - is in it.
